/*
 * afpsl_probe.c: a libafpsl client for the integration suites, which times
 * one volume list, or one read, on the user's live afpsld session, or stops
 * partway through a write's data
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Time a volume list or a read on the live afpsld session of a URL, or
 *        stall a write on it
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "daemon/stateless_ipc.h"
#include "netatalk-client/afpsl.h"

/* The bytes of a timed read or a stalled write, a Netatalk quantum */
#define PROBE_BYTES (1024U * 1024U)

/*!
 * @brief Give the seconds from one time to another
 *
 * @param[in] start  the earlier time
 * @param[in] end    the later time
 * @returns the seconds between them
 */
static double seconds_between(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec)
           + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/*!
 * @brief Open a file of the URL's volume, and once standard input gives a
 *        line, time a read of a quantum of it
 *
 * The suite stops the session's server between the two, so the read waits
 * for it. The volume stays attached for the session's owner.
 *
 * @param[in]     server  the resumed session
 * @param[in,out] url     the session's URL, which names the volume
 * @param[in]     path    the file's path on the volume
 * @returns 0 after printing the read's result and its time, or 3 when the
 *          file does not open
 */
static int timed_read(afpc_server_t server, struct afpc_url *url,
                      const char *path)
{
    static char data[PROBE_BYTES];
    enum afp_sl_attach_status status;
    afpc_volume_t volume = NULL;
    unsigned int fileid = 0;
    unsigned int received = 0;
    unsigned int eof = 0;
    struct timespec start;
    struct timespec end;
    char line[8];
    int ret;
    ret = afp_sl_attach(server, url, AFP_SL_VOLUME_NO_LOCKING, &volume,
                        &status);

    if (ret == 0) {
        ret = afp_sl_open(&volume, path, NULL, &fileid, O_RDONLY);
    }

    printf("open %d\n", ret);
    fflush(stdout);

    if (ret != 0) {
        return 3;
    }

    if (fgets(line, sizeof(line), stdin)) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        ret = afp_sl_read(&volume, fileid, 0, 0, sizeof(data), &received,
                          &eof, data);
        clock_gettime(CLOCK_MONOTONIC, &end);
        printf("read %d in %.1f s, %u bytes\n", ret,
               seconds_between(&start, &end), received);
    }

    afp_sl_close(&volume, fileid);
    return 0;
}

/*!
 * @brief Send a write of a quantum to the URL's volume on a connection of
 *        the probe's own, stop after its first byte of data, and wait for a
 *        line on standard input
 *
 * The daemon reads the data under the session's lock, which the stall holds
 * until afpsld gives up on the silent connection.
 *
 * @param[in]     server  the resumed session
 * @param[in,out] url     the session's URL, which names the volume
 * @returns 0 after printing whether the write's start went out, or 3 when
 *          the volume does not attach
 */
static int stalled_write(afpc_server_t server, struct afpc_url *url)
{
    struct afpsl_ipc_hello_request hello;
    struct afpsl_ipc_write_request request;
    struct sockaddr_un addr;
    enum afp_sl_attach_status status;
    afpc_volume_t volume = NULL;
    char answer[256];
    char line[8];
    int ret;
    int fd;
    ret = afp_sl_attach(server, url, AFP_SL_VOLUME_NO_LOCKING, &volume,
                        &status);

    if (ret != 0) {
        printf("attach %d\n", ret);
        return 3;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s-%d",
             AFPSL_IPC_SOCKET_PATH, (int) geteuid());
    memset(&hello, 0, sizeof(hello));
    hello.header.command = AFPSL_IPC_COMMAND_HELLO;
    hello.header.len = sizeof(hello);
    hello.magic = AFPSL_IPC_PROTOCOL_MAGIC;
    hello.major = AFPSL_IPC_PROTOCOL_MAJOR;
    hello.minor = AFPSL_IPC_PROTOCOL_MINOR;
    memset(&request, 0, sizeof(request));
    request.header.command = AFPSL_IPC_COMMAND_WRITE;
    request.header.len = sizeof(request);
    request.volumeid = volume;
    request.size = PROBE_BYTES;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ret = fd >= 0
          && connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0
          && write(fd, &hello, sizeof(hello)) == (ssize_t) sizeof(hello)
          && read(fd, answer, sizeof(answer)) > 0
          && write(fd, &request, sizeof(request)) == (ssize_t) sizeof(request)
          && write(fd, "x", 1) == 1 ? 0 : -1;
    printf("stalled %d\n", ret);
    fflush(stdout);

    if (fgets(line, sizeof(line), stdin)) {
        printf("released\n");
    }

    if (fd >= 0) {
        close(fd);
    }

    return 0;
}

/*!
 * @brief Resume a URL's live session, and time a volume list or a read
 *
 * The daemon answers the list from memory, under the session's lock. A
 * resume takes no password, and the probe leaves the session to its owner.
 * The result lines go to standard output, the library's log lines to
 * standard error.
 *
 * @param[in] argc  argument count
 * @param[in] argv  the program's name and the session's URL, then "read"
 *                  and a file's path for a read, or "stall"
 * @returns 0 after printing the result and its time, 1 for arguments that
 *          do not parse, 2 for a session that does not resume, or 3 for a
 *          file that does not open or a volume that does not attach
 */
int main(int argc, char **argv)
{
    struct afpc_url url;
    struct afpc_volume_info vols[16];
    afpc_server_t server = NULL;
    unsigned int num = 0;
    struct timespec start;
    struct timespec end;
    int ret;
    afp_sl_url_init(&url);

    if ((argc != 2 && (argc != 4 || strcmp(argv[2], "read") != 0)
            && (argc != 3 || strcmp(argv[2], "stall") != 0))
            || afp_sl_url_parse(&url, argv[1]) != 0) {
        fprintf(stderr, "usage: afpsl_probe URL [read PATH | stall]\n");
        afp_sl_url_clear(&url);
        return 1;
    }

    memset(url.password, 0, sizeof(url.password));
    ret = afp_sl_resume(&url, 0, &server, NULL);

    if (ret != 0) {
        printf("resume %d\n", ret);
        afp_sl_url_clear(&url);
        return 2;
    }

    if (argc > 2) {
        ret = argc == 4 ? timed_read(server, &url, argv[3])
              : stalled_write(server, &url);
        afp_sl_url_clear(&url);
        return ret;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    ret = afp_sl_getvols(server, &url, 0, sizeof(vols) / sizeof(vols[0]),
                         &num, vols);
    clock_gettime(CLOCK_MONOTONIC, &end);
    afp_sl_url_clear(&url);
    printf("getvols %d in %.1f s, %u volumes\n", ret,
           seconds_between(&start, &end), num);
    return 0;
}
