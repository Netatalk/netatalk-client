/*
 *  fuse_ipc.c - FUSE manager IPC helpers
 *
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief afpfsd's IPC messages: whole reads and writes, and a command's check
 *
 * Common to the FUSE front end, and outside AFP: IPC messages and their check.
 * Lane: control.
 * Northbound: afpfsd's processes and their command line.
 * Southbound: none.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "lib/mapping.h"

#include "fuse_ipc.h"

/*!
 * @brief Write a whole IPC message, retrying interrupted writes
 *
 * @param[in] fd      the connection
 * @param[in] buffer  the message
 * @param[in] length  its bytes
 * @returns 0, or -1 when a write fails or writes nothing
 */
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

/*!
 * @brief Read a whole IPC message, retrying interrupted reads
 *
 * @param[in]  fd      the connection
 * @param[out] buffer  receives the message
 * @param[in]  length  its bytes
 * @returns 0, or -1 when a read fails or the connection ends first
 */
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

/*!
 * @brief Test whether a mount daemon's whole command holds values it can use
 *
 * Each text field of the request ends within its size, and a mount's
 * mapping is one of AFP_MAPPING_*.
 *
 * @param[in] command  the command byte, then the whole request
 * @returns 1 for a command the daemon can use, 0 otherwise
 */
int afpfsd_ipc_command_valid(const char *command)
{
    switch ((unsigned char) command[0]) {
    case AFPFSD_IPC_COMMAND_MOUNT: {
        struct afpfsd_ipc_mount_request r;
        memcpy(&r, command + 1, sizeof(r));
        return r.map <= AFP_MAPPING_NAME
               && AFPFSD_IPC_FIELD_ENDS(r.mountpoint)
               && AFPFSD_IPC_FIELD_ENDS(r.fuse_options)
               && AFPFSD_IPC_FIELD_ENDS(r.url.username)
               && AFPFSD_IPC_FIELD_ENDS(r.url.uamname)
               && AFPFSD_IPC_FIELD_ENDS(r.url.password)
               && AFPFSD_IPC_FIELD_ENDS(r.url.servername)
               && AFPFSD_IPC_FIELD_ENDS(r.url.volumename)
               && AFPFSD_IPC_FIELD_ENDS(r.url.zone)
               && AFPFSD_IPC_FIELD_ENDS(r.url.volpassword);
    }

    case AFPFSD_IPC_COMMAND_STATUS: {
        struct afpfsd_ipc_status_request r;
        memcpy(&r, command + 1, sizeof(r));
        return AFPFSD_IPC_FIELD_ENDS(r.volumename)
               && AFPFSD_IPC_FIELD_ENDS(r.servername)
               && AFPFSD_IPC_FIELD_ENDS(r.mountpoint);
    }

    case AFPFSD_IPC_COMMAND_UNMOUNT: {
        struct afpfsd_ipc_unmount_request r;
        memcpy(&r, command + 1, sizeof(r));
        return AFPFSD_IPC_FIELD_ENDS(r.mountpoint);
    }

    case AFPFSD_IPC_COMMAND_SUSPEND: {
        struct afpfsd_ipc_suspend_request r;
        memcpy(&r, command + 1, sizeof(r));
        return AFPFSD_IPC_FIELD_ENDS(r.mountpoint);
    }

    case AFPFSD_IPC_COMMAND_RESUME: {
        struct afpfsd_ipc_resume_request r;
        memcpy(&r, command + 1, sizeof(r));
        return AFPFSD_IPC_FIELD_ENDS(r.mountpoint);
    }

    default:
        return 1;
    }
}
