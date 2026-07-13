/*
 * Protocol-neutral Unix socket lifecycle helpers for AFP daemons.
 *
 * Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "daemon_socket.h"

static int socket_address(struct sockaddr_un *address, const char *socket_path,
                          socklen_t *address_len)
{
    size_t path_len;

    if (!address || !socket_path || !address_len) {
        errno = EINVAL;
        return -1;
    }

    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    path_len = strlcpy(address->sun_path, socket_path,
                       sizeof(address->sun_path));

    if (path_len >= sizeof(address->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    *address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + path_len + 1U);
    return 0;
}

int daemon_socket_create(const char *socket_path, int backlog)
{
    struct sockaddr_un address;
    socklen_t address_len;
    int command_fd;

    if (socket_address(&address, socket_path, &address_len) != 0) {
        perror("daemon socket path");
        return -1;
    }

    command_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (command_fd < 0) {
        perror("socket");
        return -1;
    }

    if (bind(command_fd, (struct sockaddr *)&address, address_len) != 0) {
        if (errno == EADDRINUSE) {
            if (daemon_socket_cleanup_stale(socket_path) == 0
                    && bind(command_fd, (struct sockaddr *)&address,
                            address_len) == 0) {
                goto bound;
            }

            fprintf(stderr, "Daemon socket path is already in use\n");
        }

        perror("bind");
        close(command_fd);
        return -1;
    }

bound:

    if (listen(command_fd, backlog) != 0) {
        perror("listen");
        unlink(socket_path);
        close(command_fd);
        return -1;
    }

    return command_fd;
}

int daemon_socket_cleanup_stale(const char *socket_path)
{
    struct sockaddr_un address;
    struct stat socket_stat;
    socklen_t address_len;
    int connect_error;
    int fd;
    int flags;

    if (socket_address(&address, socket_path, &address_len) != 0) {
        return -1;
    }

    if (lstat(socket_path, &socket_stat) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISSOCK(socket_stat.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        connect_error = errno;
        close(fd);
        errno = connect_error;
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&address, address_len) == 0) {
        close(fd);
        errno = EADDRINUSE;
        return -1;
    }

    connect_error = errno;
    close(fd);

    /* Only connection refusal proves that a socket pathname has no listener.
     * An in-progress nonblocking connection, permission error, resource
     * exhaustion, or any other result must never cause an endpoint owned by a
     * live daemon to be unlinked. */
    if (connect_error != ECONNREFUSED) {
        errno = connect_error;
        return -1;
    }

    if (unlink(socket_path) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
}

void daemon_socket_close(int fd, const char *socket_path)
{
    if (fd >= 0) {
        close(fd);
    }

    if (socket_path && unlink(socket_path) != 0 && errno != ENOENT) {
        perror("unlink daemon socket");
    }
}
