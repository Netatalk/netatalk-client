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
 * vfs_speedtest issues the afp_speedtest transfers as POSIX file operations
 * against a directory: Read, Write and Copy move the data through user space
 * in chunks, ServerCopy hands a whole file to copy_file_range(), which an SMB
 * or NFS server and a FUSE daemon with that operation execute without the
 * data crossing the mount. Runs, warmup, statistics and the CSV layout are
 * those of afp_speedtest, which plot_speedtest.pl reads. Every data file
 * opens with O_DIRECT; -D keeps the page cache for a plain filesystem.
 */

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfs_common.h"

#define MAX_ITERATIONS 1000
#define MAX_SIZES 32

enum {
    OP_READ,
    OP_WRITE,
    OP_COPY,
    OP_SERVERCOPY,
    NUM_OPS
};

/*! Operation names, in the order the summary blocks print */
static const char *op_names[NUM_OPS] = {"Read", "Write", "Copy", "ServerCopy"};

struct timing {
    uint64_t values[MAX_ITERATIONS];
    int count;
};

struct stats {
    double mean;
    double median;
    double stddev;
    uint64_t min;
    uint64_t max;
};

struct summary {
    off_t size;
    struct stats st;
};

static bool csv_output;
static bool csv_header_done;
static bool sweep;
static int count = 1;
static int warmup = 1;
static off_t size = 64 * (off_t)MIB;
static size_t chunk = MIB;
static off_t sizes[MAX_SIZES];
static int num_sizes;
static int op_order[NUM_OPS];
static int num_run_ops;
static const char *basedir;
static char *buf;
static char opdir[PATH_MAX];
static bool opdir_created;
static bool cleaning;
static struct timing timing;
static struct summary summaries[NUM_OPS][MAX_SIZES];
static int num_summaries[NUM_OPS];

/*!
 * @brief MiB per second for one transfer of the current size
 *
 * @param[in] bytes  bytes moved
 * @param[in] us     microseconds taken
 *
 * @returns the throughput, 0 for a run too fast to time
 */
static double throughput(off_t bytes, double us)
{
    return us > 0 ? (double)bytes * 1000000.0 / ((double)MIB * us) : 0.0;
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
 * @brief Mean, median, population standard deviation and spread, as
 * afp_speedtest computes them
 *
 * @param[in]  t   the recorded runs
 * @param[out] st  the statistics
 */
static void calc_stats(const struct timing *t, struct stats *st)
{
    uint64_t sorted[MAX_ITERATIONS];
    double sum = 0.0;
    double sq = 0.0;
    int lo;
    int hi;
    memset(st, 0, sizeof(*st));

    if (t->count < 1) {
        return;
    }

    for (int i = 0; i < t->count; i++) {
        sum += (double)t->values[i];
    }

    st->mean = sum / t->count;

    for (int i = 0; i < t->count; i++) {
        double diff = (double)t->values[i] - st->mean;
        sq += diff * diff;
    }

    st->stddev = sqrt(sq / t->count);
    memcpy(sorted, t->values, (size_t)t->count * sizeof(sorted[0]));
    qsort(sorted, (size_t)t->count, sizeof(sorted[0]), cmp_u64);
    st->min = sorted[0];
    st->max = sorted[t->count - 1];
    /* The two middle runs; one and the same for an odd count. */
    lo = (t->count - 1) / 2;
    hi = t->count / 2;
    st->median = ((double)sorted[lo] + (double)sorted[hi]) / 2.0;
}

/*!
 * @brief Print the size as KiB below 1 MiB, MiB otherwise
 */
static void size_label(char *dst, size_t len, off_t bytes)
{
    if (bytes < MIB) {
        snprintf(dst, len, "%jd KiB", (intmax_t)(bytes / KIB));
    } else {
        snprintf(dst, len, "%jd MiB", (intmax_t)(bytes / MIB));
    }
}

/*!
 * @brief Begin an operation: reset the runs; print the CSV column names
 * once, or the text banner of this operation and size
 */
static void begin_op(int op)
{
    char label[32];
    timing.count = 0;

    if (csv_output) {
        if (!csv_header_done) {
            printf("test_name,iteration,file_size_mb,microseconds,"
                   "throughput_mbs\n");
            csv_header_done = true;
        }

        return;
    }

    size_label(label, sizeof(label), size);
    printf("\n===== Test Passes for %s =====\n", op_names[op]);
    printf("%s chunk %zu KiB, size %s\n", op_names[op], chunk / KIB, label);
    printf("Warmup: %d runs, Measured: %d runs\n", warmup, count);
    printf("run\t %9s\t%6s\n", "microsec", "MB/s");
}

/*!
 * @brief Stop the timer of run i of an operation; record and print it
 *
 * @param[in] op  operation index
 * @param[in] i   run number from 1; the warmup runs are not recorded
 */
static void finish_run(int op, int i)
{
    uint64_t us = vfs_timer_us();
    bool measured = i > warmup;
    int shown = measured ? i - warmup : i;

    if (measured && timing.count < MAX_ITERATIONS) {
        timing.values[timing.count++] = us;
    }

    if (csv_output) {
        if (measured) {
            printf("%s,%d,%.3f,%" PRIu64 ",%.2f\n", op_names[op], shown,
                   (double)size / MIB, us, throughput(size, (double)us));
        }

        return;
    }

    if (measured) {
        printf("%d\t", shown);
    } else {
        printf("[W%d]\t", shown);
    }

    if (us > 0) {
        printf("%9" PRIu64 "\t%.2f\n", us, throughput(size, (double)us));
    } else {
        printf("       <1\t(too fast)\n");
    }
}

/*!
 * @brief End an operation: print its statistics and keep its summary row
 */
static void end_op(int op)
{
    struct stats st;
    double mb = (double)size / MIB;

    if (timing.count == 0) {
        return;
    }

    calc_stats(&timing, &st);

    if (sweep && num_summaries[op] < MAX_SIZES) {
        summaries[op][num_summaries[op]].size = size;
        summaries[op][num_summaries[op]].st = st;
        num_summaries[op]++;
    }

    if (timing.count < 2) {
        return;
    }

    if (csv_output) {
        printf("%s_mean,,%.3f,%" PRIu64 ",%.2f\n", op_names[op], mb,
               (uint64_t)st.mean, throughput(size, st.mean));
        printf("%s_median,,%.3f,%" PRIu64 ",%.2f\n", op_names[op], mb,
               (uint64_t)st.median, throughput(size, st.median));
        printf("%s_stddev,,%.3f,%" PRIu64 ",\n", op_names[op], mb,
               (uint64_t)st.stddev);
        printf("%s_min,,%.3f,%" PRIu64 ",%.2f\n", op_names[op], mb, st.min,
               throughput(size, (double)st.min));
        printf("%s_max,,%.3f,%" PRIu64 ",%.2f\n", op_names[op], mb, st.max,
               throughput(size, (double)st.max));
        return;
    }

    printf("\n===== Statistics for %s =====\n", op_names[op]);
    printf("Iterations:     %d\n", timing.count);
    printf("Mean:           %.0f us (%.2f MB/s)\n", st.mean,
           throughput(size, st.mean));
    printf("Median:         %.0f us (%.2f MB/s)\n", st.median,
           throughput(size, st.median));
    printf("Std Dev:        %.0f us (%.2f%%)\n", st.stddev,
           st.mean > 0 ? 100.0 * st.stddev / st.mean : 0.0);
    printf("Min:            %" PRIu64 " us (%.2f MB/s)\n", st.min,
           throughput(size, (double)st.min));
    printf("Max:            %" PRIu64 " us (%.2f MB/s)\n", st.max,
           throughput(size, (double)st.max));
}

/*!
 * @brief Print one operation's per-size summary table
 */
static void print_summary(int op)
{
    if (num_summaries[op] == 0) {
        return;
    }

    if (csv_output) {
        printf("\n# File Size Performance Summary for %s\n", op_names[op]);
        printf("file_size_bytes,file_size_mb,mean_throughput_mbs,"
               "median_throughput_mbs,min_throughput_mbs,max_throughput_mbs,"
               "stddev_ms,mean_time_ms\n");
    } else {
        printf("\n# Summary for %s\n", op_names[op]);
        printf("%10s %11s %13s %10s %10s %11s %9s\n", "Size", "Mean(MB/s)",
               "Median(MB/s)", "Min(MB/s)", "Max(MB/s)", "StdDev(ms)",
               "Mean(ms)");
    }

    for (int i = 0; i < num_summaries[op]; i++) {
        const struct summary *s = &summaries[op][i];
        double mean_tput = throughput(s->size, s->st.mean);
        double median_tput = throughput(s->size, s->st.median);
        double min_tput = throughput(s->size, (double)s->st.max);
        double max_tput = throughput(s->size, (double)s->st.min);

        if (csv_output) {
            printf("%jd,%.3f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f\n",
                   (intmax_t)s->size, (double)s->size / MIB, mean_tput,
                   median_tput, min_tput, max_tput, s->st.stddev / 1000.0,
                   s->st.mean / 1000.0);
        } else {
            char label[32];
            size_label(label, sizeof(label), s->size);
            printf("%10s %11.2f %13.2f %10.2f %10.2f %11.1f %9.1f\n", label,
                   mean_tput, median_tput, min_tput, max_tput,
                   s->st.stddev / 1000.0, s->st.mean / 1000.0);
        }
    }
}

/*!
 * @brief Write the whole size in chunks from offset 0
 */
static void write_all(int fd, const char *path)
{
    off_t left = size;
    off_t off = 0;

    while (left > 0) {
        size_t n = left < (off_t)chunk ? (size_t)left : chunk;
        vfs_pwrite(fd, buf, n, off, path);
        left -= (off_t)n;
        off += (off_t)n;
    }
}

/*!
 * @brief Read the whole size in chunks from offset 0
 */
static void read_all(int fd, const char *path)
{
    off_t left = size;
    off_t off = 0;

    while (left > 0) {
        size_t n = left < (off_t)chunk ? (size_t)left : chunk;
        vfs_pread(fd, buf, n, off, path);
        left -= (off_t)n;
        off += (off_t)n;
    }
}

/*!
 * @brief Read the source and write the destination, one chunk at a time
 */
static void copy_all(int in, int out, const char *src, const char *dst)
{
    off_t left = size;
    off_t off = 0;

    while (left > 0) {
        size_t n = left < (off_t)chunk ? (size_t)left : chunk;
        vfs_pread(in, buf, n, off, src);
        vfs_pwrite(out, buf, n, off, dst);
        left -= (off_t)n;
        off += (off_t)n;
    }
}

/*!
 * @brief Create this operation's directory under the base directory
 */
static void enter_opdir(const char *name)
{
    char dirname[64];
    snprintf(dirname, sizeof(dirname), "%sTest-%ld", name, (long)getpid());
    vfs_join(opdir, basedir, dirname);
    vfs_mkdir(opdir);
    opdir_created = true;
}

/*!
 * @brief Remove this operation's directory
 */
static void leave_opdir(void)
{
    vfs_rmdir(opdir);
    opdir_created = false;
}

/*!
 * @brief Remove the files and directory of the operation under way,
 * ignoring errors
 */
static void cleanup(void)
{
    static const char *const names[] = {"File", "Source", "Destination"};
    char path[PATH_MAX];

    if (!opdir_created || cleaning) {
        return;
    }

    cleaning = true;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        vfs_join(path, opdir, names[i]);
        (void)unlink(path);
    }

    (void)rmdir(opdir);
}

/*!
 * @brief Write test: File rewritten in place, the writes alone timed
 */
static void run_write(void)
{
    char file[PATH_MAX];
    enter_opdir("Write");
    vfs_join(file, opdir, "File");
    vfs_mknod(file);
    begin_op(OP_WRITE);

    for (int i = 1; i <= warmup + count; i++) {
        int fd = vfs_open(file, O_RDWR | vfs_data_flags);
        vfs_timer_start();
        write_all(fd, file);
        finish_run(OP_WRITE, i);
        vfs_close(fd, file);
    }

    end_op(OP_WRITE);
    vfs_unlink(file);
    leave_opdir();
}

/*!
 * @brief Read test: File filled during the first run, the reads alone timed
 */
static void run_read(void)
{
    char file[PATH_MAX];
    enter_opdir("Read");
    vfs_join(file, opdir, "File");
    vfs_mknod(file);
    begin_op(OP_READ);

    for (int i = 1; i <= warmup + count; i++) {
        int fd = vfs_open(file, O_RDWR | vfs_data_flags);

        if (i == 1) {
            write_all(fd, file);
        }

        vfs_timer_start();
        read_all(fd, file);
        finish_run(OP_READ, i);
        vfs_close(fd, file);
    }

    end_op(OP_READ);
    vfs_unlink(file);
    leave_opdir();
}

/*!
 * @brief Copy test: Source filled during the first run, then read into
 * Destination chunk by chunk, the transfers alone timed
 */
static void run_copy(void)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];
    enter_opdir("Copy");
    vfs_join(src, opdir, "Source");
    vfs_join(dst, opdir, "Destination");
    vfs_mknod(src);
    vfs_mknod(dst);
    begin_op(OP_COPY);

    for (int i = 1; i <= warmup + count; i++) {
        int in = vfs_open(src, O_RDWR | vfs_data_flags);
        int out = vfs_open(dst, O_RDWR | vfs_data_flags);

        if (i == 1) {
            write_all(in, src);
        }

        vfs_timer_start();
        copy_all(in, out, src, dst);
        finish_run(OP_COPY, i);
        vfs_close(in, src);
        vfs_close(out, dst);
    }

    end_op(OP_COPY);
    vfs_unlink(src);
    vfs_unlink(dst);
    leave_opdir();
}

/*!
 * @brief ServerCopy test: one copy_file_range() of Source into a fresh
 * Destination per run, the call alone timed
 */
static void run_servercopy(void)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];
    int in;
    enter_opdir("ServerCopy");
    vfs_join(src, opdir, "Source");
    vfs_join(dst, opdir, "Destination");
    vfs_mknod(src);
    in = vfs_open(src, O_RDWR | vfs_data_flags);
    write_all(in, src);
    vfs_close(in, src);
    begin_op(OP_SERVERCOPY);

    for (int i = 1; i <= warmup + count; i++) {
        int out;
        in = vfs_open(src, O_RDONLY | vfs_data_flags);
        out = vfs_open(dst, O_WRONLY | O_CREAT | O_EXCL | vfs_data_flags);
        vfs_timer_start();
        vfs_copy_range(in, out, (size_t)size, dst);
        finish_run(OP_SERVERCOPY, i);
        vfs_close(in, src);
        vfs_close(out, dst);
        vfs_unlink(dst);
    }

    end_op(OP_SERVERCOPY);
    vfs_unlink(src);
    leave_opdir();
}

/*! The operations, indexed as op_names */
static void (*const op_fn[NUM_OPS])(void) = {
    run_read,
    run_write,
    run_copy,
    run_servercopy,
};

/*!
 * @brief Print the usage text and exit
 */
static void usage(void)
{
    fprintf(stderr,
            "usage: vfs_speedtest [-cD] [-d MiB] [-f operations] [-n runs] "
            "[-q KiB] [-W warmup] [-z sizes] -P directory\n"
            "\t-P\tdirectory on the mounted filesystem to run in\n"
            "\t-c\tCSV output\n"
            "\t-D\tbuffered I/O (default: O_DIRECT on every data file)\n"
            "\t-d\tfile size in MiB (default: 64)\n"
            "\t-f\toperations, comma-separated: Read, Write, Copy, "
            "ServerCopy (default: Write)\n"
            "\t-n\tmeasured runs per operation and size (default: 1)\n"
            "\t-q\tchunk size in KiB (default: 1024)\n"
            "\t-W\twarmup runs left out of the statistics (default: 1)\n"
            "\t-z\tfile size sweep, comma-separated MiB, fractions allowed "
            "(4 KiB = 0.00390625)\n");
    exit(EXIT_USAGE);
}

/*!
 * @brief Select the operations of a comma-separated list of names
 *
 * @param[in] list  the -f argument, modified by strtok
 */
static void select_ops(char *list)
{
    const char *tok = strtok(list, ",");
    num_run_ops = 0;

    if (tok == NULL) {
        usage();
    }

    while (tok != NULL) {
        int op = 0;

        while (op < NUM_OPS && strcmp(tok, op_names[op]) != 0) {
            op++;
        }

        if (op == NUM_OPS) {
            fprintf(stderr, "%s: unknown operation '%s'\n", vfs_progname, tok);
            usage();
        }

        if (num_run_ops == NUM_OPS) {
            fprintf(stderr, "%s: more than %d operations\n", vfs_progname,
                    NUM_OPS);
            usage();
        }

        op_order[num_run_ops++] = op;
        tok = strtok(NULL, ",");
    }
}

/*!
 * @brief Parse the -z sweep: sizes in MiB, 1 KiB to 1 GiB each
 *
 * @param[in] list  the -z argument, modified by strtok
 */
static void select_sizes(char *list)
{
    const char *tok = strtok(list, ",");
    num_sizes = 0;

    if (tok == NULL) {
        usage();
    }

    while (tok != NULL) {
        char *end;
        double mb = strtod(tok, &end);

        if (*tok == '\0' || *end != '\0' || !(mb >= 1.0 / KIB && mb <= KIB)
                || num_sizes == MAX_SIZES) {
            fprintf(stderr, "%s: bad sweep size '%s'\n", vfs_progname, tok);
            usage();
        }

        sizes[num_sizes] = (off_t)(mb * MIB);

        if (sizes[num_sizes] % 4096 != 0) {
            fprintf(stderr, "%s: sweep size '%s' is not a multiple of 4 KiB\n",
                    vfs_progname, tok);
            usage();
        }

        num_sizes++;
        tok = strtok(NULL, ",");
    }

    sweep = true;
}

int main(int argc, char **argv)
{
    bool single = false;
    int opt;
    vfs_progname = "vfs_speedtest";
    vfs_cleanup = cleanup;
    op_order[0] = OP_WRITE;
    num_run_ops = 1;

    while ((opt = getopt(argc, argv, "cDd:f:n:P:q:W:z:")) != -1) {
        switch (opt) {
        case 'c':
            csv_output = true;
            break;

        case 'D':
            vfs_data_flags = 0;
            break;

        case 'd':
            size = (off_t)vfs_opt_number(optarg, 1, KIB, usage) * MIB;
            single = true;
            break;

        case 'f':
            select_ops(optarg);
            break;

        case 'n':
            count = (int)vfs_opt_number(optarg, 1, MAX_ITERATIONS, usage);
            break;

        case 'P':
            basedir = optarg;
            break;

        case 'q':
            chunk = (size_t)vfs_opt_number(optarg, 1, 64 * KIB, usage) * KIB;
            break;

        case 'W':
            warmup = (int)vfs_opt_number(optarg, 0, MAX_ITERATIONS, usage);
            break;

        case 'z':
            select_sizes(optarg);
            break;

        default:
            usage();
        }
    }

    if (basedir == NULL || optind != argc || (single && sweep)) {
        usage();
    }

    if (warmup + count > MAX_ITERATIONS) {
        fprintf(stderr, "%s: warmup plus runs exceed %d\n", vfs_progname,
                MAX_ITERATIONS);
        usage();
    }

    if (!sweep) {
        sizes[0] = size;
        num_sizes = 1;
    }

    buf = vfs_aligned_zeroed(chunk);

    for (int s = 0; s < num_sizes; s++) {
        size = sizes[s];

        for (int i = 0; i < num_run_ops; i++) {
            op_fn[op_order[i]]();
        }
    }

    for (int op = 0; op < NUM_OPS; op++) {
        print_summary(op);
    }

    free(buf);
    return 0;
}
