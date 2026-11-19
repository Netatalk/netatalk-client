/*
 * test_unaligned_lengths.c: lengths read and written at odd addresses
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of lengths read and written at odd addresses
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/utils.h"
#include "tap.h"

/*!
 * @brief Send a frame whose head lies at an odd address, as a packed one's may
 *
 * The send writes the length and reads the command in place, and refuses a
 * frame its 32-bit length cannot count.
 */
static void send_odd_frame(void)
{
    static uint32_t frame_storage[8];
    char *frame = (char *) frame_storage + 1;
    char sent[sizeof(struct dsi_header) + 1];
    struct afp_server *server = afp_server_init(NULL);
    /* A read of a frame cut short gives up after 10 s. */
    struct timeval limit = { 10, 0 };
    int fds[2];
    CHECK(server != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0
          && setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &limit,
                        sizeof(limit)) == 0);
    server->fd = fds[0];
    server->connect_state = SERVER_STATE_CONNECTED;
    memset(frame, 0, sizeof(sent));
    frame[offsetof(struct dsi_header, command)] = DSI_DSICommand;
    frame[sizeof(struct dsi_header)] = afpGetSrvrParms;
    CHECK(afpc_dsi_send(server, frame, (int) sizeof(sent), DSI_DONT_WAIT,
                        afpGetSrvrParms, NULL) == 0);
    CHECK(recv(fds[1], sent, sizeof(sent), MSG_WAITALL)
          == (ssize_t) sizeof(sent));
    CHECK(memcmp(sent + offsetof(struct dsi_header, length), "\0\0\0\x01", 4)
          == 0 && sent[sizeof(struct dsi_header)] == afpGetSrvrParms);
    CHECK(server->stats.commands[afpGetSrvrParms] == 1);
    /* With the peer gone, a frame let through would fail with -EIO. */
    close(fds[1]);
    CHECK(afpc_dsi_send_data(server, frame, (int) sizeof(sent), frame,
                             UINT32_MAX, DSI_DONT_WAIT, afpSetExtAttr,
                             NULL) == -EINVAL);
    close(fds[0]);
    afp_server_release(server);
}

/*!
 * @brief Read and write lengths at odd addresses
 *
 * A trapping build stops at a load or a store through a misaligned pointer.
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    /* Two-byte storage: a path's length sits five bytes in, a string's one
     * byte in, both at odd addresses. */
    static uint16_t path_storage[8];
    static uint16_t string_storage[8];
    char *path = (char *) path_storage;
    char *string = (char *) string_storage + 1;
    struct afp_volume volume;
    char dest[8];
    test_tap_init(argc, argv);
    memset(&volume, 0, sizeof(volume));
    volume.path_encoding = kFPUTF8Name;
    /* The encoding, the hint, the length 3, "a/:" and a slash past the
     * length, which the conversion leaves. */
    memcpy(path, "\x03\0\0\0\0\0\x03" "a/:/", 11);
    CHECK(unixpath_to_afppath(&volume, path) == 0);
    CHECK(memcmp(path + 7, "a\0/", 3) == 0 && path[10] == '/');
    memcpy(string, "\0\x02" "hi", 4);
    CHECK(copy_from_pascal_two(dest, string, sizeof(dest)) == 2);
    CHECK(strcmp(dest, "hi") == 0);
    send_odd_frame();
    return test_tap_finish();
}
