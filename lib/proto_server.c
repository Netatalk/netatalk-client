/*
 *  proto_server.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief AFP server parameter, message, and sleep requests, and their replies
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

static int server_uses_local_time(const struct afp_server *server)
{
    return server && server->using_version
           && server->using_version->av_number < 30;
}

time_t afp_date_to_unix(const struct afp_server *server, uint32_t date)
{
    uint32_t wire_seconds = ntohl(date);
    int64_t seconds = wire_seconds;

    /* AFP defines this field as a signed long.  Do the sign extension before
     * applying the 2000 epoch; 0x80000000 is the protocol's unset-date
     * sentinel, not a date in 2068. */
    if (wire_seconds & UINT32_C(0x80000000)) {
        seconds -= INT64_C(0x100000000);
    }

    seconds += AD_DATE_DELTA;

    if (server_uses_local_time(server)) {
        seconds -= server->time_offset;
    }

    return (time_t)seconds;
}

/*!
 * @brief Convert a time to an AFP date
 *
 * A time past what AFP's signed 32 bits from 2000 hold takes the nearest
 * date it holds; the lowest is the protocol's unset date.
 *
 * @param[in] server  the server, whose AFP 2 dates are its local time, or
 *                    NULL
 * @param[in] date    the time
 * @returns the date in network order
 */
uint32_t afp_date_from_unix(const struct afp_server *server, time_t date)
{
    int64_t seconds = (int64_t)date - AD_DATE_DELTA;

    if (server_uses_local_time(server)) {
        seconds += server->time_offset;
    }

    if (seconds > INT32_MAX) {
        seconds = INT32_MAX;
    } else if (seconds <= INT32_MIN) {
        seconds = INT32_MIN + 1;
    }

    return htonl((uint32_t)seconds);
}

int afp_getsrvrparms(struct afp_server *server)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
    }  __attribute__((__packed__)) afp_getsrvrparms_request;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&afp_getsrvrparms_request.dsi_header, &hdr, sizeof(struct dsi_header));
    afp_getsrvrparms_request.command = afpGetSrvrParms;
    return afpc_dsi_send(server, (char *) &afp_getsrvrparms_request,
                         sizeof(afp_getsrvrparms_request),
                         DSI_DEFAULT_TIMEOUT, afpGetSrvrParms, NULL);
}


/*!
 * @brief Parse an FPGetSrvrParms reply into the server's time and volumes
 *
 * Builds the volume list, each volume's locks initialized, and hands it to
 * the server under the list lock, which walkers of every server's volumes
 * hold. An AFP 2 server's local time sets the offset that corrects later
 * dates.
 *
 * @param[in,out] server  server of the session, which receives the volumes
 * @param[in]     msg     the reply, DSI header first
 * @param[in]     size    bytes of msg
 * @param[in]     ignore  unused
 * @returns 0, or -1 for a reply cut short or a volume list not allocated
 */
int afp_getsrvrparms_reply(struct afp_server *server, char *msg,
                           unsigned int size, void *ignore _U_)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
        uint32_t time __attribute__((__packed__));
        uint8_t numvolumes;
    }  __attribute__((__packed__)) *afp_getsrvparm_reply = (void *) msg;
    char *p;
    const char *msg_end;
    struct afp_volume * newvolumes;
    size_t header_size = sizeof(struct dsi_header) + sizeof(uint32_t) + sizeof(
                             uint8_t);

    if (size < header_size) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getsrvparm_reply response too short (%u bytes)", size);
        return -1;
    }

    server->connect_time = afp_date_to_unix(NULL, afp_getsrvparm_reply->time);

    if (server_uses_local_time(server)) {
        /* AFP 2.x dates use the server's local clock.  Express subsequent
         * dates on the client's Unix time base, using the server time sampled
         * in this reply. */
        server->time_offset = server->connect_time - time(NULL);
        server->connect_time -= server->time_offset;
    }

    newvolumes = calloc(afp_getsrvparm_reply->numvolumes,
                        sizeof(struct afp_volume));

    if (newvolumes == NULL && afp_getsrvparm_reply->numvolumes > 0) {
        return -1;
    }

    p = msg + header_size;
    msg_end = msg + size;

    for (int i = 0; i < afp_getsrvparm_reply->numvolumes; i++) {
        if (p >= msg_end) {
            free(newvolumes);
            return -1;
        }

        struct afp_volume * vol;

        vol = &newvolumes[i];
        vol->flags = *p;
        vol->server = server;
        pthread_mutex_init(&vol->did_cache_mutex, NULL);
        pthread_mutex_init(&vol->open_forks_mutex, NULL);
        pthread_rwlock_init(&vol->open_lock, NULL);
        p++;
        p += copy_from_pascal(vol->volume_name, p,
                              sizeof(vol->volume_name)) + 1;

        /* Here's the logic; until we call openvol, we should
         * first use the AFP version to figure out what the
         * format of the volume name is.  In openvol, we use
         * the volume attributes bit (since we have it then).
         */

        if (server->using_version->av_number < 30)
            convert_mac_roman_to_utf8(vol->volume_name,
                                      strnlen(vol->volume_name, sizeof(vol->volume_name)),
                                      vol->volume_name_printable,
                                      AFP_VOLUME_NAME_UTF8_LEN);
        else
            convert_utf8dec_to_utf8pre(vol->volume_name,
                                       strnlen(vol->volume_name, sizeof(vol->volume_name)),
                                       vol->volume_name_printable,
                                       AFP_VOLUME_NAME_UTF8_LEN);
    }

    afp_lock_server_list();
    server->num_volumes = afp_getsrvparm_reply->numvolumes;
    server->volumes = newvolumes;
    afp_unlock_server_list();
    return 0;
}


int afp_getsrvrmsg_reply(struct afp_server *server _U_,
                         char *buf, unsigned int size, void *other)
{
    struct afp_getsrvrmsg_reply_packet {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint16_t messagetype;
        uint16_t messagebitmap;
    }  __attribute__((__packed__)) * afp_getsrvrmsg_reply = (void *) buf;
    static char mesg[AFP_LOGINMESG_LEN];
    char *mesg_ptr = other;
    char *src;

    if (size < sizeof(struct afp_getsrvrmsg_reply_packet)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getsrvrmsg response too short (%u bytes)", size);
        return -1;
    }

    src = buf + (sizeof(struct afp_getsrvrmsg_reply_packet));

    if (ntohs(afp_getsrvrmsg_reply->messagebitmap) & AFP_GETSRVRMSG_UTF8) {
        copy_from_pascal_two(mesg, src, sizeof(mesg));
    } else {
        copy_from_pascal(mesg, src, sizeof(mesg));
    }

    if (mesg_ptr != NULL) {
        memcpy(mesg_ptr, mesg, AFP_LOGINMESG_LEN);
    }

    return 0;
}

int afp_getsrvrmsg(struct afp_server *server, unsigned short messagetype,
                   unsigned char utf8, unsigned char block, char *mesg)
{
    int rc;
    struct afp_getsrvrmsg_request_packet {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t messagetype __attribute__((__packed__));
        uint16_t messagebitmap __attribute__((__packed__));
    }  __attribute__((__packed__)) afp_getsrvrmsg_request;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&afp_getsrvrmsg_request.dsi_header, &hdr, sizeof(struct dsi_header));
    afp_getsrvrmsg_request.command = afpGetSrvrMsg;
    afp_getsrvrmsg_request.pad = 0;
    afp_getsrvrmsg_request.messagetype = htons(messagetype);
    afp_getsrvrmsg_request.messagebitmap =
        htons(AFP_GETSRVRMSG_GETMSG | (utf8 ? AFP_GETSRVRMSG_UTF8 : 0));
    /* Get the message, and yes, we support UTF8 */
    rc = afpc_dsi_send(server, (char *) &afp_getsrvrmsg_request,
                       sizeof(afp_getsrvrmsg_request), block, afpGetSrvrMsg, (void *) mesg);
    return rc;
}

int afp_zzzzz(struct afp_server *server)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint32_t reserved;
    }  __attribute__((__packed__)) request;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpZzzzz;
    request.pad = 0;
    request.reserved = 0;
    return afpc_dsi_send(server, (char *) &request,
                         sizeof(request), DSI_DEFAULT_TIMEOUT, afpZzzzz, NULL);
}
