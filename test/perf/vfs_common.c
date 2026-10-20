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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "vfs_common.h"

const char *vfs_progname = "vfs_bench";
int vfs_data_flags = O_DIRECT;
void (*vfs_cleanup)(void);
static struct timespec t_start;

/*!
 * @brief Report a failed call, run the cleanup hook and exit
 *
 * @param[in] what  the call that failed
 * @param[in] path  its path argument
 */
_Noreturn void vfs_fatal(const char *what, const char *path)
{
    int err = errno;
    fprintf(stderr, "%s: %s '%s': %s\n", vfs_progname, what, path,
            strerror(err));

    if (err == EINVAL && vfs_data_flags != 0) {
        fprintf(stderr, "%s: O_DIRECT refused here; use -D for buffered I/O\n",
                vfs_progname);
    }

    if (vfs_cleanup != NULL) {
        vfs_cleanup();
    }

    exit(EXIT_TESTFAIL);
}

/*!
 * @brief Report a failure that has no errno, run the cleanup hook and exit
 *
 * @param[in] msg  what went wrong
 */
_Noreturn void vfs_fatal_msg(const char *msg)
{
    fprintf(stderr, "%s: %s\n", vfs_progname, msg);

    if (vfs_cleanup != NULL) {
        vfs_cleanup();
    }

    exit(EXIT_TESTFAIL);
}

/*!
 * @brief Parse a decimal option value within [lo, hi] or exit through usage
 *
 * @param[in] arg    the option argument
 * @param[in] lo     smallest accepted value
 * @param[in] hi     largest accepted value
 * @param[in] usage  prints the usage text and exits
 *
 * @returns the value
 */
long vfs_opt_number(const char *arg, long lo, long hi, void (*usage)(void))
{
    char *end;
    long v;
    errno = 0;
    v = strtol(arg, &end, 10);

    if (errno != 0 || *arg == '\0' || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "%s: option value '%s' is not a number in [%ld, %ld]\n",
                vfs_progname, arg, lo, hi);
        usage();
    }

    return v;
}

/*!
 * @brief Allocate a page-aligned zeroed buffer for O_DIRECT transfers
 *
 * @param[in] len  bytes
 *
 * @returns the buffer
 */
char *vfs_aligned_zeroed(size_t len)
{
    void *p;

    if (posix_memalign(&p, 4096, len) != 0) {
        vfs_fatal_msg("cannot allocate a data buffer");
    }

    memset(p, 0, len);
    return p;
}

/*!
 * @brief Join a directory and an entry name into a PATH_MAX buffer
 *
 * @param[out] dst   buffer of PATH_MAX bytes
 * @param[in]  dir   directory
 * @param[in]  name  entry name, with or without subdirectories
 */
void vfs_join(char *dst, const char *dir, const char *name)
{
    if (snprintf(dst, PATH_MAX, "%s/%s", dir, name) >= PATH_MAX) {
        vfs_fatal_msg("path too long");
    }
}

/*!
 * @brief open() that exits on failure
 *
 * @param[in] path   file to open
 * @param[in] flags  open flags; FILE_MODE applies with O_CREAT
 *
 * @returns the descriptor
 */
int vfs_open(const char *path, int flags)
{
    int fd = open(path, flags, FILE_MODE);

    if (fd < 0) {
        vfs_fatal("open", path);
    }

    return fd;
}

/*!
 * @brief close() that exits on failure
 */
void vfs_close(int fd, const char *path)
{
    if (close(fd) != 0) {
        vfs_fatal("close", path);
    }
}

/*!
 * @brief mkdir() that exits on failure
 */
void vfs_mkdir(const char *path)
{
    if (mkdir(path, DIR_MODE) != 0) {
        vfs_fatal("mkdir", path);
    }
}

/*!
 * @brief rmdir() that exits on failure
 */
void vfs_rmdir(const char *path)
{
    if (rmdir(path) != 0) {
        vfs_fatal("rmdir", path);
    }
}

/*!
 * @brief unlink() that exits on failure
 */
void vfs_unlink(const char *path)
{
    if (unlink(path) != 0) {
        vfs_fatal("unlink", path);
    }
}

/*!
 * @brief Create an empty regular file without opening it, as FPCreateFile
 */
void vfs_mknod(const char *path)
{
    if (mknod(path, S_IFREG | FILE_MODE, 0) != 0) {
        vfs_fatal("mknod", path);
    }
}

/*!
 * @brief stat() that exits on failure
 */
void vfs_stat(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        vfs_fatal("stat", path);
    }
}

/*!
 * @brief Write the whole buffer at an offset, resuming after a short write
 *
 * @param[in] fd    open descriptor
 * @param[in] buf   data
 * @param[in] len   bytes to write
 * @param[in] off   file offset of the first byte
 * @param[in] path  for the error message
 */
void vfs_pwrite(int fd, const char *buf, size_t len, off_t off,
                const char *path)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, buf, len, off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            vfs_fatal("write", path);
        }

        if (n == 0) {
            errno = EIO;
            vfs_fatal("short write", path);
        }

        buf += n;
        off += n;
        len -= (size_t)n;
    }
}

/*!
 * @brief Read exactly len bytes from an offset, resuming after a short read
 *
 * @param[in]  fd    open descriptor
 * @param[out] buf   destination
 * @param[in]  len   bytes to read
 * @param[in]  off   file offset of the first byte
 * @param[in]  path  for the error message
 */
void vfs_pread(int fd, char *buf, size_t len, off_t off, const char *path)
{
    while (len > 0) {
        ssize_t n = pread(fd, buf, len, off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            vfs_fatal("read", path);
        }

        if (n == 0) {
            errno = EIO;
            vfs_fatal("short read", path);
        }

        buf += n;
        off += n;
        len -= (size_t)n;
    }
}

/*!
 * @brief Copy len bytes between two open files with copy_file_range()
 *
 * The kernel or the filesystem moves the data: an SMB or NFS server as a
 * server-side copy, a FUSE daemon through its copy_file_range operation,
 * otherwise the kernel itself.
 *
 * @param[in] in    source descriptor at its current offset
 * @param[in] out   destination descriptor at its current offset
 * @param[in] len   bytes to copy
 * @param[in] path  for the error message
 */
void vfs_copy_range(int in, int out, size_t len, const char *path)
{
    while (len > 0) {
        ssize_t n = copy_file_range(in, NULL, out, NULL, len, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            vfs_fatal("copy_file_range", path);
        }

        if (n == 0) {
            errno = EIO;
            vfs_fatal("copy_file_range stopped early", path);
        }

        len -= (size_t)n;
    }
}

/*!
 * @brief Start the timed region
 */
void vfs_timer_start(void)
{
    clock_gettime(CLOCK_MONOTONIC, &t_start);
}

/*!
 * @brief Microseconds since vfs_timer_start()
 *
 * @returns elapsed whole microseconds
 */
uint64_t vfs_timer_us(void)
{
    struct timespec now;
    int64_t ns;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = (int64_t)(now.tv_sec - t_start.tv_sec) * 1000000000LL
         + (now.tv_nsec - t_start.tv_nsec);
    return (uint64_t)(ns / 1000);
}
