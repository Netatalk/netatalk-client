/*
 * test_write_frames.c: the frames the AFP commands hand the DSI send
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of the frames the AFP commands hand the DSI send
 */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/utils.h"
#include "tap.h"

/* The lib/proto_*.c sources compiled into this test send through the stubs
 * below, which lib/dsi.c's definitions document. */
static unsigned char sent_head[64];
static int sent_size;
static const char *sent_data;
static size_t sent_data_len;
static unsigned char sent_subcommand;
static int sent_wait;

/*!
 * @brief Keep what a send was given: its head's start, data, wait and command
 *
 * @param[in] msg         the head
 * @param[in] size        bytes of msg
 * @param[in] data        the data, or NULL
 * @param[in] data_len    bytes of data
 * @param[in] wait        the send's wait
 * @param[in] subcommand  the AFP command
 * @returns 0, the send's success
 */
static int keep_send(const char *msg, int size, const char *data,
                     size_t data_len, int wait, unsigned char subcommand)
{
    memset(sent_head, 0, sizeof(sent_head));
    memcpy(sent_head, msg, min((size_t) size, sizeof(sent_head)));
    sent_size = size;
    sent_data = data;
    sent_data_len = data_len;
    sent_wait = wait;
    sent_subcommand = subcommand;
    return 0;
}

/* Take a frame sent in one buffer. */
int afpc_dsi_send(struct afp_server *server, char *msg, int size, int wait,
                  unsigned char subcommand, void *other)
{
    (void) server;
    (void) other;
    return keep_send(msg, size, NULL, 0, wait, subcommand);
}

/* Take a frame's head and its data apart. */
int afpc_dsi_send_data(struct afp_server *server, char *msg, int size,
                       const char *data, size_t data_len, int wait,
                       unsigned char subcommand, void *other)
{
    (void) server;
    (void) other;
    return keep_send(msg, size, data, data_len, wait, subcommand);
}

/*!
 * @brief Tell whether the kept head is a DSIWrite whose data follow parameters
 *
 * @param[in] parameters  bytes of the command's parameters
 * @returns 1 when the header says so, 0 otherwise
 */
static int write_header(uint32_t parameters)
{
    struct dsi_header header;
    memcpy(&header, sent_head, sizeof(header));
    return header.command == DSI_DSIWrite
           && ntohl(header.return_code.data_offset) == parameters;
}

/*!
 * @brief Check the frames and waits the AFP commands hand the DSI send
 *
 * A trapping build stops at a misaligned store and at a null memcpy() source.
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    static char data[1U << 20];
    /* FPWriteExt of fork 7 from the end, at 0x0102030405060708, 1 MiB. */
    static const unsigned char writeext_parameters[20] = {
        afpWriteExt, AFP_WRITE_FROM_END, 0, 7, 1, 2, 3, 4, 5, 6, 7, 8,
        0, 0, 0, 0, 0, 0x10, 0, 0,
    };
    /* FPWrite of fork 7 at 0x01020304, 4 KiB. */
    static const unsigned char write_parameters[12] = {
        afpWrite, 0, 0, 7, 1, 2, 3, 4, 0, 0, 0x10, 0,
    };
    /* FPLogin with the guest UAM, which has no info. */
    static const unsigned char login_parameters[24] = {
        afpLogin, 6, 'A', 'F', 'P', '3', '.', '4',
        15, 'N', 'o', ' ', 'U', 's', 'e', 'r', ' ',
        'A', 'u', 't', 'h', 'e', 'n', 't',
    };
    /* FPLoginExt for DHX2 as user "abc": both lengths lie at odd offsets,
     * and a pad byte precedes the info. */
    static const unsigned char loginext_odd_parameters[28] = {
        afpLoginExt, 0, 0, 0, 6, 'A', 'F', 'P', '3', '.', '4',
        4, 'D', 'H', 'X', '2', 3, 0, 3, 'a', 'b', 'c', 3, 0, 0, 0, 1, 2,
    };
    /* FPLoginExt for SRP as user "test": the empty path's length lies at an
     * odd offset, and a pad byte precedes the info. */
    static const unsigned char loginext_parameters[28] = {
        afpLoginExt, 0, 0, 0, 6, 'A', 'F', 'P', '3', '.', '4',
        3, 'S', 'R', 'P', 3, 0, 4, 't', 'e', 's', 't', 3, 0, 0, 0, 1, 2,
    };
    /* FPSetExtAttr of "n" on "f" in directory 2, 1 MiB: the name leaves
     * the value's size at an odd offset. */
    static const unsigned char setextattr_parameters[33] = {
        afpSetExtAttr, 0, 0, 0, 0, 0, 0, 2, 0, kXAttrCreate,
        0, 0, 0, 0, 0, 0, 0, 0,
        3, 0x08, 0, 1, 3, 0, 1, 'f',
        0, 1, 'n', 0, 0x10, 0, 0,
    };
    static struct afp_versions afp34 = { "AFP3.4", 34 };
    char auth_info[2] = { 1, 2 };
    struct afp_volume volume;
    test_tap_init(argc, argv);
    memset(&volume, 0, sizeof(volume));
    volume.server = afp_server_init(NULL);
    CHECK(volume.server != NULL && volume.server->dsi_default_timeout == 5);
    CHECK(afp_writeext(&volume, 7, AFP_WRITE_FROM_END,
                       UINT64_C(0x0102030405060708), sizeof(data), data,
                       NULL) == 0);
    CHECK(sent_data == data && sent_data_len == sizeof(data));
    CHECK(sent_size == 36 && write_header(20));
    CHECK(memcmp(sent_head + 16, writeext_parameters, 20) == 0);
    CHECK(sent_subcommand == afpWriteExt
          && sent_wait == volume.server->dsi_default_timeout + 32);
    CHECK(afp_write(&volume, 7, 0, 0x01020304, 4096, data, NULL) == 0);
    CHECK(sent_data == data && sent_data_len == 4096);
    CHECK(sent_size == 28 && write_header(12));
    CHECK(memcmp(sent_head + 16, write_parameters, 12) == 0);
    CHECK(sent_subcommand == afpWrite
          && sent_wait == volume.server->dsi_default_timeout);
    volume.path_encoding = kFPUTF8Name;
    CHECK(afp_setextattr(&volume, 2, kXAttrCreate, 0, "f", 1, "n",
                         sizeof(data), data) == 0);
    CHECK(sent_data == data && sent_data_len == sizeof(data));
    CHECK(sent_size == 49 && sent_subcommand == afpSetExtAttr
          && sent_wait == volume.server->dsi_default_timeout + 32);
    CHECK(memcmp(sent_head + 16, setextattr_parameters, 33) == 0);
    sent_subcommand = 0;
    CHECK(afp_setextattr(&volume, 2, 0, 0, "f", 1, "n", 8, NULL) == -1
          && sent_subcommand == 0);
    volume.server->using_version = &afp34;
    CHECK(afp_login(volume.server, "No User Authent", NULL, 0, NULL) == 0);
    CHECK(sent_subcommand == afpLogin && sent_size == 40);
    CHECK(memcmp(sent_head + 16, login_parameters, 24) == 0);
    CHECK(afp_loginext(volume.server, "DHX2", "abc", auth_info,
                       sizeof(auth_info), NULL) == 0);
    CHECK(sent_subcommand == afpLoginExt && sent_size == 44
          && memcmp(sent_head + 16, loginext_odd_parameters, 28) == 0);
    CHECK(afp_loginext(volume.server, "SRP", "test", auth_info,
                       sizeof(auth_info), NULL) == 0);
    CHECK(sent_subcommand == afpLoginExt && sent_size == 44);
    CHECK(memcmp(sent_head + 16, loginext_parameters, 28) == 0);
    /* A wait goes to the send as given, the sentinel of the default too. */
    CHECK(afp_getsrvrmsg(volume.server, AFPMESG_LOGIN, 1, DSI_DEFAULT_TIMEOUT,
                         NULL) == 0 && sent_subcommand == afpGetSrvrMsg
          && sent_wait == DSI_DEFAULT_TIMEOUT);
    CHECK(afp_logout(volume.server, DSI_DEFAULT_TIMEOUT) == 0
          && sent_subcommand == afpLogout && sent_wait == DSI_DEFAULT_TIMEOUT);
    afp_server_release(volume.server);
    return test_tap_finish();
}
