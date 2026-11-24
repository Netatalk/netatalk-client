/*
 *  proto_reply.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  The reply table and afp_reply() come from afp.c.
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The table that gives a reply to its AFP command's parser
 *
 * Part of the protocol layer: the reply table, and the call that reads it.
 * Lanes: data, metadata, directory, session.
 * Southbound: the DSI layer, which hands a reply up through that call.
 */

#include "afp_internal.h"
#include "afp_replies.h"
#include "client.h"
#include "compat.h"
#include "dsi_protocol.h"

/*!
 * @brief Take the result of a reply that carries nothing more
 *
 * @param[in] server   unused
 * @param[in] buf      the reply
 * @param[in] size     unused
 * @param[in] ignored  unused
 * @returns the reply's error code
 */
static int afp_blank_reply(
    struct afp_server *server _U_,
    char *buf,
    unsigned int size _U_,
    void *ignored _U_
)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
    } * reply = (const void *) buf;
    return reply->header.return_code.error_code;
}

int (*afp_replies[])(struct afp_server * server, char *buf, unsigned int len,
                     void *other) = {
    NULL, afp_byterangelock_reply, afp_blank_reply, NULL,
    afp_blank_reply, afp_blank_reply, afp_createdir_reply, afp_blank_reply, /* 0 - 7 */
    afp_blank_reply, afp_enumerate_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, NULL, NULL,                       /* 8 - 15 */
    afp_getsrvrparms_reply, afp_getvolparms_reply, afp_login_reply, afp_login_reply,
    afp_blank_reply, afp_mapid_reply, afp_mapname_reply, afp_blank_reply,  /*16 - 23 */
    afp_volopen_reply, NULL, afp_openfork_reply, afp_read_reply,
    afp_blank_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,    /*24 - 31 */
    NULL, afp_write_reply, afp_getfiledirparms_reply, afp_blank_reply,
    afp_changepassword_reply, afp_getuserinfo_reply, afp_getsrvrmsg_reply, NULL,      /*32 - 39 */

    NULL, NULL, afp_blank_reply, NULL,
    NULL, NULL, NULL, NULL,                       /*40 - 47 */
    afp_opendt_reply, afp_blank_reply, NULL, afp_geticon_reply,
    afp_geticoninfo_reply, NULL, NULL, afp_getappl_reply, /*48 - 55 */
    afp_blank_reply, NULL, afp_getcomment_reply, afp_byterangelockext_reply,
    afp_readext_reply, afp_writeext_reply,
    NULL, afp_login_reply,            /*56 - 63 */
    /* FPEnumerateExt's replies share FPEnumerateExt2's layout, a 16-bit length
     * before each entry; the requests differ in index and size widths alone. */
    afp_getsessiontoken_reply, afp_blank_reply, afp_enumerateext2_reply, NULL,
    afp_enumerateext2_reply, afp_getextattr_reply, afp_blank_reply, afp_blank_reply, /*64 - 71 */
    afp_listextattrs_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, afp_blank_reply, afp_blank_reply,                       /*72 - 79 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 80 - 99 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 100 - 119 */

    NULL, NULL, afp_blank_reply, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 120 - 139 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

};

/*!
 * @brief Hand a reply to its command's parser
 *
 * @param[in]     subcommand  the AFP command the reply answers
 * @param[in,out] server      the server it came from
 * @param[in,out] other       the request's caller data
 * @returns the parser's result, 0 without a parser, or -1 for a reply
 *          shorter than a DSI header
 */
int afp_reply(unsigned short subcommand, struct afp_server * server,
              void *other)
{
    int ret = 0;

    /* No AFP packet is valid if it is smaller than a DSI header. */

    if ((unsigned long) server->data_read < sizeof(struct dsi_header)) {
        return -1;
    }

    if (afp_replies[subcommand]) {
        ret = (*afp_replies[subcommand])(server,
                                         server->incoming_buffer,
                                         server->data_read, other);
    } else {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "AFP subcommand %d not supported", subcommand);
    }

    return ret;
}
