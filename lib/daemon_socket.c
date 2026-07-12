/*
 * daemon_socket.c - Shared socket utilities for AFP daemons
 *
 * Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This file provides common socket management functions used by both
 * afpsld (stateless daemon) and afpfsd (FUSE daemon).
 */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "daemon/server.h"
#include "daemon_socket.h"

/*
 * Create a UNIX domain socket listener
 *
 * socket_path: Path to the UNIX socket file
 * backlog: Maximum number of pending connections
 *
 * Returns: Socket file descriptor on success, -1 on error
 */
int daemon_socket_create(const char *socket_path, int backlog)
{
    int command_fd;
    struct sockaddr_un sa;
    int len;

    if (!socket_path) {
        fprintf(stderr, "Socket path is NULL\n");
        return -1;
    }

    if ((command_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t path_len = strlcpy(sa.sun_path, socket_path,
                              sizeof(sa.sun_path));

    if (path_len >= sizeof(sa.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", socket_path);
        close(command_fd);
        return -1;
    }

    len = sizeof(sa.sun_family) + path_len + 1;

    if (bind(command_fd, (struct sockaddr *)&sa, len) < 0) {
        if (errno == EADDRINUSE) {
            /* Check if a daemon is actually alive, retry bind once */
            if (daemon_socket_cleanup_stale(socket_path) == 0
                    && bind(command_fd, (struct sockaddr *)&sa, len) == 0) {
                goto bound;
            }

            /* Either daemon is alive, or retry also failed */
            fprintf(stderr, "Another daemon is already running\n");
        }

        perror("bind");
        close(command_fd);
        return -1;
    }

bound:

    if (listen(command_fd, backlog) < 0) {
        perror("listen");
        unlink(socket_path);
        close(command_fd);
        return -1;
    }

    return command_fd;
}

/*
 * Check for stale daemon and cleanup if needed
 *
 * Attempts to ping an existing daemon at socket_path. If the daemon
 * doesn't respond, removes the stale socket file.
 *
 * socket_path: Path to the UNIX socket file
 *
 * Returns:
 *   0: Socket doesn't exist or stale socket was removed
 *  -1: Active daemon is running (cannot cleanup)
 */
int daemon_socket_cleanup_stale(const char *socket_path)
{
    int sock;
    struct sockaddr_un servaddr;
    int len = 0, ret;
    char incoming_buffer[MAX_CLIENT_RESPONSE];
    struct timeval tv;
    fd_set rds;
    char outgoing_buffer[1];  /* Just the PING command byte */

    if (!socket_path) {
        return -1;
    }

    /* Check if socket file exists */
    if (access(socket_path, F_OK) != 0) {
        return 0;  /* Doesn't exist, nothing to cleanup */
    }

    /* Try to connect to the socket */
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;

    if (strlcpy(servaddr.sun_path, socket_path,
                sizeof(servaddr.sun_path)) >= sizeof(servaddr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", socket_path);
        close(sock);
        return -1;
    }

    /* Try to connect - if it fails, socket is dead */
    if (connect(sock, (struct sockaddr *)&servaddr,
                sizeof(servaddr.sun_family) + sizeof(servaddr.sun_path)) < 0) {
        goto dead;
    }

    /* Try sending a PING command */
    outgoing_buffer[0] = AFP_SERVER_COMMAND_PING;

    if (write(sock, outgoing_buffer, 1) < 1) {
        goto dead;
    }

    /* Wait for response with timeout */
    memset(incoming_buffer, 0, MAX_CLIENT_RESPONSE);
    FD_ZERO(&rds);
    FD_SET(sock, &rds);
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ret = select(sock + 1, &rds, NULL, NULL, &tv);

    if (ret == 0) {
        goto dead;  /* Timeout - daemon not responding */
    }

    if (ret < 0) {
        close(sock);
        return -1;  /* Select error */
    }

    /* Try to read response */
    len = read(sock, incoming_buffer, MAX_CLIENT_RESPONSE);

    if (len < 1) {
        goto dead;
    }

    /* Daemon is alive and responding */
    close(sock);
    return -1;
dead:
    close(sock);

    /* Remove stale socket file; tolerate ENOENT in case another process
     * (e.g. the just-exiting daemon) already cleaned it up. */
    if (unlink(socket_path) != 0 && errno != ENOENT) {
        perror("unlink");
        return -1;
    }

    return 0;
}

/*
 * Close socket and remove socket file
 *
 * fd: Socket file descriptor to close
 * socket_path: Path to the UNIX socket file to remove
 */
void daemon_socket_close(int fd, const char *socket_path)
{
    if (fd >= 0) {
        close(fd);
    }

    if (socket_path && access(socket_path, F_OK) == 0) {
        unlink(socket_path);
    }
}
