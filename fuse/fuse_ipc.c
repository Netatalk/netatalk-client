/*
 *  fuse_ipc.c - FUSE manager IPC helpers
 *
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 */

#include <errno.h>
#include <unistd.h>

#include "fuse_ipc.h"

int afpfsd_ipc_write_all(int fd, const void *buffer, size_t length)
{
    const char *cursor = buffer;

    while (length != 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return -1;
        }

        cursor += written;
        length -= (size_t)written;
    }

    return 0;
}

int afpfsd_ipc_read_all(int fd, void *buffer, size_t length)
{
    char *cursor = buffer;

    while (length != 0) {
        ssize_t received = read(fd, cursor, length);

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            return -1;
        }

        cursor += received;
        length -= (size_t)received;
    }

    return 0;
}
