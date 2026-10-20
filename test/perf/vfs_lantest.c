/*
 * Copyright (c) 2026 Andy Lemin (andylemin)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * vfs_lantest issues the afp_lantest workload as POSIX file operations
 * against a directory, so a mounted filesystem is measured end to end: the
 * kernel, the filesystem client (afpfsd and libafpclient on a FUSE mount, the
 * CIFS client on an SMB mount) and the server. Test set, order, statistics
 * and CSV layout are those of afp_lantest, which the Netatalk chart and
 * dashboard scripts read. Every data read and write opens with O_DIRECT so
 * the kernel page cache never answers for the filesystem; -D keeps the page
 * cache for a plain filesystem, which rejects O_DIRECT for the 1024-byte
 * transfers.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfs_common.h"

#define SMALL_IO 1024
#define NUM_FILES 2000
#define NUM_COPIES 1000
#define NUM_LOCKS 2000
#define LOCK_RANGE 10
#define TREE_I 20
#define TREE_J 9
#define TREE_K 10
#define TREE_DIRS (TREE_I + TREE_I * TREE_J + TREE_I * TREE_J * TREE_K)
#define CACHE_DIRS 20
#define CACHE_FILES_PER_DIR 100
#define MIXED_FILES 500
#define MIXED_ENUM_EVERY 10
#define DEEP_LEVELS 20
#define DEEP_WALKS 100
/* "/deep_NN" per level: the longest entry path under the test directory */
#define DEEP_PATH_LEN (DEEP_LEVELS * 8)
#define VALIDATION_FILES 500
#define MAX_ITERATIONS 1000
#define NAME_WIDTH 66
#define DASHES "------------------------------------------------------------------"

enum {
    TEST_WRITE_BIG,
    TEST_READ_BIG,
    TEST_CREATE_FILES,
    TEST_DIR_TREE,
    TEST_WRITE_FILES,
    TEST_READ_FILES,
    TEST_COPY_FILES,
    TEST_SERVER_COPY_FILES,
    TEST_STAT_FILES,
    TEST_ENUM_FILES,
    TEST_LOCK_FILES,
    TEST_DELETE_FILES,
    TEST_BYTE_LOCKS,
    TEST_CACHE_HITS,
    TEST_MIXED_CACHE,
    TEST_DEEP_PATH,
    TEST_CACHE_VALIDATION,
    NUM_TESTS
};

/*! The afp_lantest name of each test, then the calls in its timed region */
static const char *test_names[NUM_TESTS] = {
    "Writing one large file [pwrite per chunk, close]",
    "Reading one large file [pread per chunk, close]",
    "Creating 2000 files [mknod, stat x2000]",
    "Create 2000 dirs tree (20×9×10) [mkdir x2000]",
    "Open, write 1024 bytes, close 2000 files [open, pwrite, close x2000]",
    "Open, read 1024 bytes, close 2000 files [open, pread, close x2000]",
    "Copying 1000 files client-side (R+W) [open, pread, close, open, pwrite, close x1000]",
    "Copying 2000 files server-side [open x2, copy_file_range, close x2, x2000]",
    "Stat (lookup+getparams) 2000 files [stat x2000]",
    "Enumerate dir with 2000 files [opendir, readdir, fstatat x2000, closedir]",
    "Lock then unlock 2000 open forks [fcntl lock, unlock x2000]",
    "Deleting 2000 files [unlink x2000]",
    "Byte-range lock/unlock 2000 ranges in one fork [fcntl lock, unlock x2000]",
    "Directory cache hits (20 dirs x 100 files) [stat x2020]",
    "Mixed cache operations (create/stat/enum/delete) on 500 files [mknod, stat, stat, unlink x500, readdir x50]",
    "Deep path traversal (20 levels x 100 walks) [stat x2000]",
    "Cache validation (500 files x 4 lookups) [stat x2000]",
};

/*! Timed calls per test; the large-file entries follow the chunk count */
static int test_ops[NUM_TESTS] = {
    0,
    0,
    2 * NUM_FILES,
    TREE_DIRS,
    3 * NUM_FILES,
    3 * NUM_FILES,
    6 * NUM_COPIES,
    5 * NUM_FILES,
    NUM_FILES,
    NUM_FILES + 2,
    2 * NUM_FILES,
    NUM_FILES,
    2 * NUM_LOCKS,
    CACHE_DIRS + CACHE_DIRS * CACHE_FILES_PER_DIR,
    4 * MIXED_FILES + MIXED_FILES / MIXED_ENUM_EVERY,
    DEEP_LEVELS * DEEP_WALKS,
    4 * VALIDATION_FILES,
};

struct stats {
    uint64_t mean;
    double stddev;
    uint64_t min;
    uint64_t max;
    uint64_t median;
    int count;
};

static bool csv_output;
static bool run[NUM_TESTS];
static int iterations = 2;
static off_t rwsize = 100 * (off_t)MIB;
static size_t chunk = MIB;
static char testdir[PATH_MAX];
static bool testdir_created;
static bool removing;
static char *big_buf;
static char *small_buf;
/*! samples[iteration * NUM_TESTS + test], in ms, valid when valid[] is set */
static uint64_t *samples;
static bool *valid;

/*!
 * @brief Path of a numbered entry in the test directory
 *
 * @param[out] dst     buffer of PATH_MAX bytes
 * @param[in]  prefix  name prefix
 * @param[in]  i       number appended to the prefix
 */
static void numbered_path(char *dst, const char *prefix, int i)
{
    char name[64];
    snprintf(name, sizeof(name), "%s%d", prefix, i);
    vfs_join(dst, testdir, name);
}

/*!
 * @brief Path of a tree directory; j or k of 0 stop at the level above
 *
 * @param[out] dst  buffer of PATH_MAX bytes
 * @param[in]  i    outer level, from 1
 * @param[in]  j    middle level, from 1, or 0
 * @param[in]  k    inner level, from 1, or 0
 */
static void tree_path(char *dst, int i, int j, int k)
{
    char name[64];

    if (k > 0) {
        snprintf(name, sizeof(name), "dir%02d/dir%02d/dir%02d", i, j, k);
    } else if (j > 0) {
        snprintf(name, sizeof(name), "dir%02d/dir%02d", i, j);
    } else {
        snprintf(name, sizeof(name), "dir%02d", i);
    }

    vfs_join(dst, testdir, name);
}

/*!
 * @brief Path of cache directory i, or of its file j when j is not negative
 *
 * @param[out] dst  buffer of PATH_MAX bytes
 * @param[in]  i    directory number
 * @param[in]  j    file number, or -1 for the directory itself
 */
static void cache_path(char *dst, int i, int j)
{
    char name[64];

    if (j < 0) {
        snprintf(name, sizeof(name), "cache_dir_%d", i);
    } else {
        snprintf(name, sizeof(name), "cache_dir_%d/cache_file_%d_%d",
                 i, i, j);
    }

    vfs_join(dst, testdir, name);
}

/*!
 * @brief Path of the deep chain down to the given level
 *
 * @param[out] dst    buffer of PATH_MAX bytes
 * @param[in]  depth  last level included, from 0
 */
static void deep_path(char *dst, int depth)
{
    char name[PATH_MAX];
    size_t len = 0;

    for (int i = 0; i <= depth; i++) {
        int n = snprintf(name + len, sizeof(name) - len, "%sdeep_%02d",
                         i > 0 ? "/" : "", i);

        if (n < 0 || (size_t)n >= sizeof(name) - len) {
            vfs_fatal_msg("test path too long");
        }

        len += (size_t)n;
    }

    vfs_join(dst, testdir, name);
}

/*!
 * @brief Set or clear an advisory lock on a byte range
 *
 * @param[in] fd     open descriptor
 * @param[in] type   F_WRLCK or F_UNLCK
 * @param[in] start  first byte of the range
 * @param[in] len    length of the range
 * @param[in] path   for the error message
 */
static void range_lock(int fd, short type, off_t start, off_t len,
                       const char *path)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;

    if (fcntl(fd, F_SETLK, &fl) != 0) {
        vfs_fatal(type == F_UNLCK ? "fcntl unlock" : "fcntl lock", path);
    }
}

/*!
 * @brief Read every name in a directory, as a names-only enumerate does
 */
static void list_names(const char *path)
{
    DIR *dir = opendir(path);
    const struct dirent *ent;

    if (dir == NULL) {
        vfs_fatal("opendir", path);
    }

    do {
        errno = 0;
        ent = readdir(dir);
    } while (ent != NULL);

    if (errno != 0) {
        vfs_fatal("readdir", path);
    }

    if (closedir(dir) != 0) {
        vfs_fatal("closedir", path);
    }
}

/*!
 * @brief Store the timed region of one test and print its progress line
 *
 * @param[in] test       test index
 * @param[in] iteration  iteration index, from 0
 */
static void record(int test, int iteration)
{
    uint64_t ms = vfs_timer_us() / 1000;
    samples[iteration * NUM_TESTS + test] = ms;
    valid[iteration * NUM_TESTS + test] = true;
    fprintf(stderr, "Run %d => %-*.*s %6" PRIu64 " ms", iteration + 1,
            NAME_WIDTH, NAME_WIDTH, test_names[test], ms);

    if (test == TEST_WRITE_BIG || test == TEST_READ_BIG) {
        uintmax_t mib = (uintmax_t)(rwsize / MIB);

        if (ms > 0) {
            fprintf(stderr, " for %ju MiB (avg. %ju MiB/s)", mib,
                    (uintmax_t)(mib * 1000 / ms));
        } else {
            fprintf(stderr, " for %ju MiB", mib);
        }
    }

    fprintf(stderr, "\n");
}

/*!
 * @brief Write the large file in chunks and close it, timed
 */
static void write_big(int iteration)
{
    char path[PATH_MAX];
    uint64_t numrw = (uint64_t)(rwsize / (off_t)chunk);
    int fd;
    vfs_join(path, testdir, "File.big");
    fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC | vfs_data_flags);
    vfs_timer_start();

    for (uint64_t i = 0; i < numrw; i++) {
        vfs_pwrite(fd, big_buf, chunk, (off_t)(i * chunk), path);
    }

    vfs_close(fd, path);
    record(TEST_WRITE_BIG, iteration);

    if (!run[TEST_READ_BIG]) {
        vfs_unlink(path);
    }
}

/*!
 * @brief Read the large file back in chunks and close it, timed
 */
static void read_big(int iteration)
{
    char path[PATH_MAX];
    uint64_t numrw = (uint64_t)(rwsize / (off_t)chunk);
    int fd;
    vfs_join(path, testdir, "File.big");
    fd = vfs_open(path, O_RDONLY | vfs_data_flags);
    vfs_timer_start();

    for (uint64_t i = 0; i < numrw; i++) {
        vfs_pread(fd, big_buf, chunk, (off_t)(i * chunk), path);
    }

    vfs_close(fd, path);
    record(TEST_READ_BIG, iteration);
    vfs_unlink(path);
}

/*!
 * @brief Create the shared 2000 files and stat each, timed
 */
static void create_files(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        numbered_path(path, "File.0k", i);
        vfs_mknod(path);
        vfs_stat(path);
    }

    record(TEST_CREATE_FILES, iteration);
}

/*!
 * @brief Create the 20x9x10 directory tree, timed; remove it untimed
 */
static void dir_tree(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= TREE_I; i++) {
        tree_path(path, i, 0, 0);
        vfs_mkdir(path);

        for (int j = 1; j <= TREE_J; j++) {
            tree_path(path, i, j, 0);
            vfs_mkdir(path);

            for (int k = 1; k <= TREE_K; k++) {
                tree_path(path, i, j, k);
                vfs_mkdir(path);
            }
        }
    }

    record(TEST_DIR_TREE, iteration);

    for (int i = 1; i <= TREE_I; i++) {
        for (int j = 1; j <= TREE_J; j++) {
            for (int k = 1; k <= TREE_K; k++) {
                tree_path(path, i, j, k);
                vfs_rmdir(path);
            }

            tree_path(path, i, j, 0);
            vfs_rmdir(path);
        }

        tree_path(path, i, 0, 0);
        vfs_rmdir(path);
    }
}

/*!
 * @brief Open, write 1024 bytes and close each shared file, timed
 */
static void write_files(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        int fd;
        numbered_path(path, "File.0k", i);
        fd = vfs_open(path, O_WRONLY | vfs_data_flags);
        vfs_pwrite(fd, small_buf, SMALL_IO, 0, path);
        vfs_close(fd, path);
    }

    record(TEST_WRITE_FILES, iteration);
}

/*!
 * @brief Open, read the 1024 bytes back and close each shared file, timed
 */
static void read_files(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        int fd;
        numbered_path(path, "File.0k", i);
        fd = vfs_open(path, O_RDONLY | vfs_data_flags);
        vfs_pread(fd, small_buf, SMALL_IO, 0, path);
        vfs_close(fd, path);
    }

    record(TEST_READ_FILES, iteration);
}

/*!
 * @brief Copy 1000 files through user space: read each, write a new file
 */
static void copy_files(int iteration)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_COPIES; i++) {
        int fd;
        numbered_path(src, "File.0k", i);
        numbered_path(dst, "File.cp", i);
        fd = vfs_open(src, O_RDONLY | vfs_data_flags);
        vfs_pread(fd, small_buf, SMALL_IO, 0, src);
        vfs_close(fd, src);
        fd = vfs_open(dst, O_WRONLY | O_CREAT | O_EXCL | vfs_data_flags);
        vfs_pwrite(fd, small_buf, SMALL_IO, 0, dst);
        vfs_close(fd, dst);
    }

    record(TEST_COPY_FILES, iteration);

    for (int i = 1; i <= NUM_COPIES; i++) {
        numbered_path(dst, "File.cp", i);
        vfs_unlink(dst);
    }
}

/*!
 * @brief Copy 2000 files with copy_file_range, the data staying below the
 * mount when the filesystem offloads the copy
 */
static void server_copy_files(int iteration)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        int in;
        int out;
        numbered_path(src, "File.0k", i);
        numbered_path(dst, "File.sc", i);
        in = vfs_open(src, O_RDONLY | vfs_data_flags);
        out = vfs_open(dst, O_WRONLY | O_CREAT | O_EXCL | vfs_data_flags);
        vfs_copy_range(in, out, SMALL_IO, dst);
        vfs_close(in, src);
        vfs_close(out, dst);
    }

    record(TEST_SERVER_COPY_FILES, iteration);

    for (int i = 1; i <= NUM_FILES; i++) {
        numbered_path(dst, "File.sc", i);
        vfs_unlink(dst);
    }
}

/*!
 * @brief stat each shared file, timed
 */
static void stat_files(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        numbered_path(path, "File.0k", i);
        vfs_stat(path);
    }

    record(TEST_STAT_FILES, iteration);
}

/*!
 * @brief List the test directory and stat every entry, as ls -l does
 */
static void enum_files(int iteration)
{
    DIR *dir;
    int dfd;
    const struct dirent *ent;
    struct stat st;
    int count = 0;
    vfs_timer_start();
    dir = opendir(testdir);

    if (dir == NULL) {
        vfs_fatal("opendir", testdir);
    }

    dfd = dirfd(dir);

    if (dfd < 0) {
        vfs_fatal("dirfd", testdir);
    }

    for (;;) {
        errno = 0;
        ent = readdir(dir);

        if (ent == NULL) {
            break;
        }

        if (strcmp(ent->d_name, ".") == 0
                || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        if (fstatat(dfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            vfs_fatal("fstatat", ent->d_name);
        }

        count++;
    }

    if (errno != 0) {
        vfs_fatal("readdir", testdir);
    }

    if (closedir(dir) != 0) {
        vfs_fatal("closedir", testdir);
    }

    record(TEST_ENUM_FILES, iteration);

    if (count != NUM_FILES) {
        fprintf(stderr, "%s: enumerate saw %d entries, expected %d\n",
                vfs_progname, count, NUM_FILES);
        vfs_cleanup();
        exit(EXIT_TESTFAIL);
    }
}

/*!
 * @brief Lock then unlock one byte in each of the 2000 open files, timed
 */
static void lock_files(int iteration)
{
    static int fds[NUM_FILES];
    char path[PATH_MAX];

    for (int i = 0; i < NUM_FILES; i++) {
        numbered_path(path, "File.0k", i + 1);
        fds[i] = vfs_open(path, O_RDWR);
    }

    vfs_timer_start();

    for (int i = 0; i < NUM_FILES; i++) {
        range_lock(fds[i], F_WRLCK, 0, 1, "File.0k*");
    }

    for (int i = 0; i < NUM_FILES; i++) {
        range_lock(fds[i], F_UNLCK, 0, 1, "File.0k*");
    }

    record(TEST_LOCK_FILES, iteration);

    for (int i = 0; i < NUM_FILES; i++) {
        vfs_close(fds[i], "File.0k*");
    }
}

/*!
 * @brief Delete the shared files, timed
 */
static void delete_files(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 1; i <= NUM_FILES; i++) {
        numbered_path(path, "File.0k", i);
        vfs_unlink(path);
    }

    record(TEST_DELETE_FILES, iteration);
}

/*!
 * @brief Hold 2000 distinct ranges in one file, then release them, timed
 */
static void byte_locks(int iteration)
{
    char path[PATH_MAX];
    int fd;
    vfs_join(path, testdir, "File.lock");
    fd = vfs_open(path, O_RDWR | O_CREAT | O_EXCL | vfs_data_flags);

    for (int i = 0; i < NUM_LOCKS * LOCK_RANGE / SMALL_IO + 1; i++) {
        vfs_pwrite(fd, small_buf, SMALL_IO, (off_t)i * SMALL_IO, path);
    }

    vfs_timer_start();

    for (int i = 0; i < NUM_LOCKS; i++) {
        range_lock(fd, F_WRLCK, (off_t)i * LOCK_RANGE, LOCK_RANGE, path);
    }

    for (int i = 0; i < NUM_LOCKS; i++) {
        range_lock(fd, F_UNLCK, (off_t)i * LOCK_RANGE, LOCK_RANGE, path);
    }

    record(TEST_BYTE_LOCKS, iteration);
    vfs_close(fd, path);
    vfs_unlink(path);
}

/*!
 * @brief stat 20 directories and the 100 files of each, timed
 */
static void cache_hits(int iteration)
{
    char path[PATH_MAX];

    for (int i = 0; i < CACHE_DIRS; i++) {
        cache_path(path, i, -1);
        vfs_mkdir(path);

        for (int j = 0; j < CACHE_FILES_PER_DIR; j++) {
            cache_path(path, i, j);
            vfs_mknod(path);
        }
    }

    vfs_timer_start();

    for (int i = 0; i < CACHE_DIRS; i++) {
        cache_path(path, i, -1);
        vfs_stat(path);

        for (int j = 0; j < CACHE_FILES_PER_DIR; j++) {
            cache_path(path, i, j);
            vfs_stat(path);
        }
    }

    record(TEST_CACHE_HITS, iteration);

    for (int i = 0; i < CACHE_DIRS; i++) {
        for (int j = 0; j < CACHE_FILES_PER_DIR; j++) {
            cache_path(path, i, j);
            vfs_unlink(path);
        }

        cache_path(path, i, -1);
        vfs_rmdir(path);
    }
}

/*!
 * @brief Create, stat, stat and delete 500 files, listing every tenth round
 */
static void mixed_cache(int iteration)
{
    char path[PATH_MAX];
    vfs_timer_start();

    for (int i = 0; i < MIXED_FILES; i++) {
        numbered_path(path, "mixed_file_", i);
        vfs_mknod(path);
        vfs_stat(path);

        if (i % MIXED_ENUM_EVERY == MIXED_ENUM_EVERY - 1) {
            list_names(testdir);
        }

        vfs_stat(path);
        vfs_unlink(path);
    }

    record(TEST_MIXED_CACHE, iteration);
}

/*!
 * @brief Walk a 20-level directory chain 100 times, one stat per level
 */
static void deep_traversal(int iteration)
{
    char path[PATH_MAX];

    for (int i = 0; i < DEEP_LEVELS; i++) {
        deep_path(path, i);
        vfs_mkdir(path);
    }

    vfs_timer_start();

    for (int walk = 0; walk < DEEP_WALKS; walk++) {
        for (int i = 0; i < DEEP_LEVELS; i++) {
            deep_path(path, i);
            vfs_stat(path);
        }
    }

    record(TEST_DEEP_PATH, iteration);

    for (int i = DEEP_LEVELS - 1; i >= 0; i--) {
        deep_path(path, i);
        vfs_rmdir(path);
    }
}

/*!
 * @brief stat 500 files four times each, timed
 */
static void cache_validation(int iteration)
{
    char path[PATH_MAX];

    for (int i = 0; i < VALIDATION_FILES; i++) {
        numbered_path(path, "valid_file_", i);
        vfs_mknod(path);
    }

    vfs_timer_start();

    for (int i = 0; i < VALIDATION_FILES; i++) {
        numbered_path(path, "valid_file_", i);

        for (int k = 0; k < 4; k++) {
            vfs_stat(path);
        }
    }

    record(TEST_CACHE_VALIDATION, iteration);

    for (int i = 0; i < VALIDATION_FILES; i++) {
        numbered_path(path, "valid_file_", i);
        vfs_unlink(path);
    }
}

/*! The tests in afp_lantest order */
static void (*const test_fn[NUM_TESTS])(int) = {
    write_big,
    read_big,
    create_files,
    dir_tree,
    write_files,
    read_files,
    copy_files,
    server_copy_files,
    stat_files,
    enum_files,
    lock_files,
    delete_files,
    byte_locks,
    cache_hits,
    mixed_cache,
    deep_traversal,
    cache_validation,
};

/*!
 * @brief Remove whatever a run may have left behind, ignoring errors
 */
static void remove_known(void)
{
    char path[PATH_MAX];

    if (!testdir_created || removing) {
        return;
    }

    removing = true;
    vfs_join(path, testdir, "File.big");
    (void)unlink(path);
    vfs_join(path, testdir, "File.lock");
    (void)unlink(path);

    for (int i = 1; i <= NUM_FILES; i++) {
        numbered_path(path, "File.0k", i);
        (void)unlink(path);
        numbered_path(path, "File.sc", i);
        (void)unlink(path);
    }

    for (int i = 1; i <= NUM_COPIES; i++) {
        numbered_path(path, "File.cp", i);
        (void)unlink(path);
    }

    for (int i = 0; i < MIXED_FILES; i++) {
        numbered_path(path, "mixed_file_", i);
        (void)unlink(path);
    }

    for (int i = 0; i < VALIDATION_FILES; i++) {
        numbered_path(path, "valid_file_", i);
        (void)unlink(path);
    }

    for (int i = 0; i < CACHE_DIRS; i++) {
        for (int j = 0; j < CACHE_FILES_PER_DIR; j++) {
            cache_path(path, i, j);
            (void)unlink(path);
        }

        cache_path(path, i, -1);
        (void)rmdir(path);
    }

    for (int i = 1; i <= TREE_I; i++) {
        for (int j = 1; j <= TREE_J; j++) {
            for (int k = 1; k <= TREE_K; k++) {
                tree_path(path, i, j, k);
                (void)rmdir(path);
            }

            tree_path(path, i, j, 0);
            (void)rmdir(path);
        }

        tree_path(path, i, 0, 0);
        (void)rmdir(path);
    }

    for (int i = DEEP_LEVELS - 1; i >= 0; i--) {
        deep_path(path, i);
        (void)rmdir(path);
    }

    if (rmdir(testdir) != 0) {
        fprintf(stderr, "%s: left '%s' behind: %s\n", vfs_progname, testdir,
                strerror(errno));
    }
}

/*!
 * @brief qsort comparator for uint64_t
 */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/*!
 * @brief Drop the fastest and slowest sample of each test, as afp_lantest
 * does above five iterations
 */
static void drop_outliers(void)
{
    for (int test = 0; test < NUM_TESTS; test++) {
        uint64_t min = UINT64_MAX;
        uint64_t max = 0;
        int min_at = 0;
        int max_at = 0;

        if (!run[test]) {
            continue;
        }

        for (int it = 0; it < iterations; it++) {
            uint64_t v = samples[it * NUM_TESTS + test];

            if (v < min) {
                min = v;
                min_at = it;
            }

            if (v > max) {
                max = v;
                max_at = it;
            }
        }

        valid[min_at * NUM_TESTS + test] = false;
        valid[max_at * NUM_TESTS + test] = false;
    }
}

/*!
 * @brief Mean, standard deviation, spread and median of a test's samples
 *
 * @param[in]  test  test index
 * @param[out] st    the statistics; count is 0 without valid samples
 */
static void test_stats(int test, struct stats *st)
{
    uint64_t *sorted = calloc((size_t)iterations, sizeof(*sorted));
    uint64_t sum = 0;
    double sq = 0.0;
    int lo;
    int hi;
    memset(st, 0, sizeof(*st));

    if (sorted == NULL) {
        vfs_fatal_msg("cannot allocate the sample table");
    }

    /* A 0 ms sample is data: afp_lantest zeroes samples to drop outliers
     * and so has to skip zeros, this tool flags them in valid[]. */
    for (int it = 0; it < iterations; it++) {
        if (valid[it * NUM_TESTS + test]) {
            sorted[st->count++] = samples[it * NUM_TESTS + test];
            sum += samples[it * NUM_TESTS + test];
        }
    }

    if (st->count < 1) {
        free(sorted);
        return;
    }

    st->mean = sum / (uint64_t)st->count;

    for (int i = 0; i < st->count; i++) {
        double diff = (double)sorted[i] - (double)st->mean;
        sq += diff * diff;
    }

    if (st->count > 1) {
        st->stddev = sqrt(sq / (double)(st->count - 1));
    }

    qsort(sorted, (size_t)st->count, sizeof(sorted[0]), cmp_u64);
    st->min = sorted[0];
    st->max = sorted[st->count - 1];
    /* The two middle samples; one and the same for an odd count. */
    lo = (st->count - 1) / 2;
    hi = st->count / 2;
    st->median = (sorted[lo] + sorted[hi]) / 2;
    free(sorted);
}

/*!
 * @brief Print the per-test table, the sum row and the aggregates
 */
static void display_results(void)
{
    double sum_mean = 0.0;
    double sum_median = 0.0;
    int total_ops = 0;

    if (iterations > 5) {
        drop_outliers();
    }

    if (csv_output) {
        printf("Test,Time_ms,Time±,Min_ms,Max_ms,Median_ms,MB/s\n");
    } else {
        printf("\nNetatalk Client Lantest Results (mean and standard deviation "
               "across %d iteration%s)\n", iterations,
               iterations == 1 ? "" : "s");
        printf("%-66s %7s %6s %6s\n", "Test", "Time_ms", "Time±", "MB/s");
        printf("%-66s %7s %6s %6s\n", DASHES, "-------", "------", "------");
    }

    for (int test = 0; test < NUM_TESTS; test++) {
        struct stats st;
        uint64_t mbs = 0;

        if (!run[test]) {
            continue;
        }

        test_stats(test, &st);

        if ((test == TEST_WRITE_BIG || test == TEST_READ_BIG)
                && st.mean > 0) {
            mbs = (uint64_t)(rwsize / MIB) * 1000 / st.mean;
        }

        if (csv_output) {
            printf("%s,%" PRIu64 ",%.1f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 "\n", test_names[test], st.mean, st.stddev,
                   st.min, st.max, st.median, mbs);
        } else {
            printf("%-66s %7" PRIu64 " %6.1f %6" PRIu64 "\n", test_names[test],
                   st.mean, st.stddev, mbs);
        }

        sum_mean += (double)st.mean;
        sum_median += (double)st.median;
        total_ops += test_ops[test];
    }

    if (csv_output) {
        printf("Sum of all file ops = %d,%.0f,,,,,\n", total_ops, sum_mean);
        printf("# Aggregate,Value\n");
        printf("Total file ops,%d\n", total_ops);
        printf("Total median time (ms),%.0f\n", sum_median);
        printf("Avg time per file op (µs),%.0f\n",
               sum_median * 1000.0 / (double)total_ops);
    } else {
        char sum_row[64];
        snprintf(sum_row, sizeof(sum_row), "Sum of all file ops = %d",
                 total_ops);
        printf("%-66s %7s %6s %6s\n", DASHES, "-------", "------", "------");
        printf("%-66s %7.0f\n", sum_row, sum_mean);
        printf("\nAverage time per file op: %.0f µs (from per-test medians)\n",
               sum_median * 1000.0 / (double)total_ops);
    }
}

/*!
 * @brief Print the usage text and exit
 */
static void usage(void)
{
    fprintf(stderr,
            "usage: vfs_lantest [-cDK] [-f tests] [-n iterations] [-q KiB] "
            "[-s MiB] -P directory\n"
            "\t-P\tdirectory on the mounted filesystem to run in\n"
            "\t-c\tCSV output\n"
            "\t-D\tbuffered I/O (default: O_DIRECT on every data read and "
            "write)\n"
            "\t-f\ttests to run, comma-separated numbers 1-%d (default: all)\n"
            "\t-K\tcache-focused tests only (14-%d)\n"
            "\t-n\titerations, 1-%d (default: 2); above 5 the fastest and "
            "slowest\n\t\tsample of each test are dropped\n"
            "\t-q\tchunk size of the large-file tests in KiB (default: 1024)\n"
            "\t-s\tlarge-file size in MiB (default: 100)\n"
            "\tTests:\n", NUM_TESTS, NUM_TESTS, MAX_ITERATIONS);

    for (int i = 0; i < NUM_TESTS; i++) {
        fprintf(stderr, "\t(%d) %s\n", i + 1, test_names[i]);
    }

    exit(EXIT_USAGE);
}

/*!
 * @brief Select the tests of a comma-separated list of numbers
 *
 * @param[in] list  the -f argument, modified by strtok
 */
static void select_tests(char *list)
{
    const char *tok = strtok(list, ",");

    if (tok == NULL) {
        usage();
    }

    while (tok != NULL) {
        run[vfs_opt_number(tok, 1, NUM_TESTS, usage) - 1] = true;
        tok = strtok(NULL, ",");
    }
}

/*!
 * @brief Raise the open file limit to what the fork lock test holds
 */
static void raise_nofile(void)
{
    static const rlim_t need = NUM_FILES + 64;
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        vfs_fatal("getrlimit", "RLIMIT_NOFILE");
    }

    if (rl.rlim_cur < need) {
        rl.rlim_cur = rl.rlim_max < need ? rl.rlim_max : need;

        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            vfs_fatal("setrlimit", "RLIMIT_NOFILE");
        }
    }

    if (rl.rlim_cur < need) {
        vfs_fatal_msg("open file limit below 2064; raise ulimit -n or leave "
                      "test 11 out");
    }
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    bool any = false;
    int opt;
    vfs_progname = "vfs_lantest";
    vfs_cleanup = remove_known;

    while ((opt = getopt(argc, argv, "cDf:Kn:P:q:s:")) != -1) {
        switch (opt) {
        case 'c':
            csv_output = true;
            break;

        case 'D':
            vfs_data_flags = 0;
            break;

        case 'f':
            select_tests(optarg);
            break;

        case 'K':
            run[TEST_CACHE_HITS] = true;
            run[TEST_MIXED_CACHE] = true;
            run[TEST_DEEP_PATH] = true;
            run[TEST_CACHE_VALIDATION] = true;
            break;

        case 'n':
            iterations = (int)vfs_opt_number(optarg, 1, MAX_ITERATIONS, usage);
            break;

        case 'P':
            dir = optarg;
            break;

        case 'q':
            chunk = (size_t)vfs_opt_number(optarg, 1, 64 * KIB, usage) * KIB;
            break;

        case 's':
            rwsize = (off_t)vfs_opt_number(optarg, 1, 100 * KIB, usage) * MIB;
            break;

        default:
            usage();
        }
    }

    if (dir == NULL || optind != argc) {
        usage();
    }

    if (rwsize % (off_t)chunk != 0) {
        fprintf(stderr, "%s: -s must be a multiple of -q\n", vfs_progname);
        usage();
    }

    for (int test = 0; test < NUM_TESTS; test++) {
        any = any || run[test];
    }

    if (!any) {
        for (int test = 0; test < NUM_TESTS; test++) {
            run[test] = true;
        }
    }

    /* The shared 2000 files flow through the pipeline tests as in
     * afp_lantest: the content tests need the writes, the writes and the
     * metadata tests need the files, and the files go at the end of every
     * iteration so the next one starts clean. */
    if (run[TEST_READ_BIG]) {
        run[TEST_WRITE_BIG] = true;
    }

    if (run[TEST_READ_FILES] || run[TEST_COPY_FILES]
            || run[TEST_SERVER_COPY_FILES]) {
        run[TEST_WRITE_FILES] = true;
    }

    if (run[TEST_WRITE_FILES] || run[TEST_LOCK_FILES]
            || run[TEST_STAT_FILES] || run[TEST_ENUM_FILES]
            || run[TEST_DELETE_FILES]) {
        run[TEST_CREATE_FILES] = true;
    }

    if (run[TEST_CREATE_FILES]) {
        run[TEST_DELETE_FILES] = true;
    }

    if (run[TEST_LOCK_FILES]) {
        raise_nofile();
    }

    test_ops[TEST_WRITE_BIG] = (int)(rwsize / (off_t)chunk) + 1;
    test_ops[TEST_READ_BIG] = test_ops[TEST_WRITE_BIG];
    big_buf = vfs_aligned_zeroed(chunk);
    small_buf = vfs_aligned_zeroed(SMALL_IO);
    samples = calloc((size_t)iterations * NUM_TESTS, sizeof(*samples));
    valid = calloc((size_t)iterations * NUM_TESTS, sizeof(*valid));

    if (samples == NULL || valid == NULL) {
        vfs_fatal_msg("cannot allocate the result table");
    }

    if (snprintf(testdir, sizeof(testdir), "%s/LanTest-%ld", dir,
                 (long)getpid()) >= (int)sizeof(testdir) - DEEP_PATH_LEN) {
        vfs_fatal_msg("directory path too long");
    }

    vfs_mkdir(testdir);
    testdir_created = true;

    for (int it = 0; it < iterations; it++) {
        for (int test = 0; test < NUM_TESTS; test++) {
            if (run[test]) {
                test_fn[test](it);
            }
        }
    }

    vfs_rmdir(testdir);
    testdir_created = false;
    display_results();
    free(samples);
    free(valid);
    free(big_buf);
    free(small_buf);
    return 0;
}
