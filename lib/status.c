/*
 *  status.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The status text of a server, its session, and its attached volumes
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/socket.h>

#include "afp_internal.h"
#include "dsi.h"
#include "mapping.h"

int afp_status_header(char *text, int *len)
{
    int pos;

    if (*len <= 0) {
        return -1;
    }

    memset(text, 0, *len);
    pos = snprintf(text, *len, "Netatalk Client Version: %s\n"
                               "Client UAMs: %s\n",
                   NETATALK_CLIENT_VERSION,
                   get_uam_names_list());

    if ((pos < 0) || (pos >= *len)) {
        *len = 0;
        return -1;
    }

    *len -= pos;
    return pos;
}

/*!
 * @brief Move the write position past the line snprintf() wrote
 *
 * A line that did not fit is cut at the buffer's end, and *pos stops at
 * the terminator's place, so a later snprintf() has one byte, for its
 * terminator, and writes nothing; it never sees a negative size.
 *
 * @param[in,out] pos      the write position, below len on return
 * @param[in]     len      capacity of the buffer
 * @param[in]     written  what snprintf() returned for the line
 */
static void status_advance(int *pos, int len, int written)
{
    if (written < 0) {
        return;
    }

    *pos = (written >= len - *pos) ? len - 1 : *pos + written;
}

/*!
 * @brief Append a volume's lines to a server's status text
 *
 * @param[in]     v      the volume
 * @param[in]     s      its server
 * @param[out]    text   the buffer
 * @param[in,out] pos_p  the write position
 * @param[in]     len    capacity of text
 */
static void print_volume_status(struct afp_volume *v, struct afp_server *s,
                                char *text, int *pos_p, int *len)
{
    int mounted = v->mounted == AFP_VOLUME_MOUNTED;
    int n;
    n = snprintf(text + *pos_p, (size_t)(*len - *pos_p),
                 "Volume \"%s\"\n    id: %d\n    attribs: 0x%x\n"
                 "    mounted: %s%s\n",
                 v->volume_name_printable, v->volid, v->attributes,
                 mounted ? v->mountpoint : "No",
                 mounted && volume_is_readonly(v) ? " (read only)" : "");
    status_advance(pos_p, *len, n);

    if (v->attached == AFP_VOLUME_ATTACHED) {
        n = snprintf(text + *pos_p, (size_t)(*len - *pos_p),
                     "    did cache stats: %" PRIu64 " miss, %" PRIu64
                     " hit, %" PRIu64 " expired, %" PRIu64 " force removal\n"
                     "    uid/gid mapping: %s (%d/%d)\n",
                     v->did_cache_stats.misses, v->did_cache_stats.hits,
                     v->did_cache_stats.expired,
                     v->did_cache_stats.force_removed,
                     get_mapping_name(v),
                     s->server_uid, s->server_gid);
        status_advance(pos_p, *len, n);
        n = snprintf(text + *pos_p, (size_t)(*len - *pos_p),
                     "    Unix permissions: %s",
                     (v->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) ?
                     "Yes\n" : "No\n");
        status_advance(pos_p, *len, n);
    }
}

/*!
 * @brief Write a server's status text
 *
 * Lists the connection, session, transfer counts, and request counts by
 * command, then the attached volumes; every line stops at len.
 *
 * @param[in]     s     server to describe, or NULL
 * @param[out]    text  receives the text
 * @param[in,out] len   capacity of text on entry, what remains of it on
 *                      return
 * @returns the bytes written
 */
int afp_status_server(struct afp_server * s, char *text, int *len)
{
    unsigned int j;
    struct afp_volume *v;
    char signature_string[AFP_SIGNATURE_LEN * 2 + 1];
    int pos = 0;
    int firsttime = 0;
    struct dsi_request * request;
    char ip_addr[64];
    int port = 0;
    int n;
    memset(text, 0, *len);

    if (s == NULL) {
        n = snprintf(text + pos, (size_t)(*len - pos),
                     "Not connected to any servers\n");
        status_advance(&pos, *len, n);
        goto out;
    }

    /* A server with no address in use or no version, one whose first DSI
     * session has not opened among them, is listed by its name and state. */
    if (s->used_address == NULL || s->using_version == NULL) {
        const char *state = "(disconnected)";

        if (s->connect_state == SERVER_STATE_CONNECTING) {
            state = "(connecting)";
        } else if (s->connect_state == SERVER_STATE_CONNECTED) {
            state = "(active)";
        }

        n = snprintf(text + pos, (size_t)(*len - pos),
                     "Server \"%s\"\n    connection: %s\n",
                     s->server_name_printable, state);
        status_advance(&pos, *len, n);
        goto out;
    }

    for (j = 0; j < AFP_SIGNATURE_LEN; j++)
        snprintf(signature_string + (j * 2),
                 sizeof(signature_string) - (j * 2), "%02x",
                 (unsigned int)((unsigned char) s->signature[j]));

    switch (s->used_address->ai_family) {
    case AF_INET6: {
        const struct sockaddr_in6 *sin6 =
            (const void *) s->used_address->ai_addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip_addr, INET6_ADDRSTRLEN);
        port = ntohs(sin6->sin6_port);
        break;
    }

    case AF_INET: {
        const struct sockaddr_in *sin =
            (const void *) s->used_address->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip_addr, INET6_ADDRSTRLEN);
        port = ntohs(sin->sin_port);
        break;
    }

    default:
        snprintf(ip_addr, 23, "unknown address family");
        break;
    }

    ip_addr[63] = '\0';
    n = snprintf(text + pos, (size_t)(*len - pos),
                 "Server \"%s\"\n"
                 "    connection: %s:%d %s\n"
                 "    using AFP version: %s\n",
                 s->server_name_printable,
                 ip_addr, port,
                 (s->connect_state == SERVER_STATE_DISCONNECTED ?
                  "(disconnected)" : "(active)"),
                 s->using_version->av_name);
    status_advance(&pos, *len, n);
    n = snprintf(text + pos, (size_t)(*len - pos),
                 "    server UAMs: ");
    status_advance(&pos, *len, n);

    for (j = 1; j < 0x200; j <<= 1) {
        if (j & s->supported_uams) {
            if (firsttime != 0) {
                n = snprintf(text + pos, (size_t)(*len - pos),
                             ", ");
                status_advance(&pos, *len, n);
            }

            if (j == s->using_uam) {
                n = snprintf(text + pos, (size_t)(*len - pos),
                             "%s (used)",
                             uam_bitmap_to_string(j));
                status_advance(&pos, *len, n);
            } else {
                n = snprintf(text + pos, (size_t)(*len - pos),
                             "%s",
                             uam_bitmap_to_string(j));
                status_advance(&pos, *len, n);
            }

            firsttime = 1;
        }
    }

    n = snprintf(text + pos, (size_t)(*len - pos),
                 "\n    login message: %s\n"
                 "    type: %s",
                 s->loginmesg, s->machine_type);
    status_advance(&pos, *len, n);
    n = snprintf(text + pos, (size_t)(*len - pos),
                 "\n"
                 "    signature: %s\n"
                 "    transmit delay: %ums\n"
                 "    quantum: %u\n"
                 "    last request id: %d in queue: %" PRIu64 "\n",
                 signature_string,
                 s->tx_delay,
                 s->quantum,
                 s->lastrequestid, s->stats.requests_pending);
    status_advance(&pos, *len, n);
    pthread_mutex_lock(&s->request_queue_mutex);

    for (request = s->command_requests; request; request = request->next) {
        n = snprintf(text + pos, (size_t)(*len - pos),
                     "         request %d, %s\n",
                     request->requestid,
                     afp_get_command_name(request->subcommand));
        status_advance(&pos, *len, n);
    }

    pthread_mutex_unlock(&s->request_queue_mutex);
    n = snprintf(text + pos, (size_t)(*len - pos),
                 "    transfer: %" PRIu64 "(rx) %" PRIu64 "(tx)\n"
                 "    runt packets: %" PRIu64 "\n",
                 s->stats.rx_bytes, s->stats.tx_bytes,
                 s->stats.runt_packets);
    status_advance(&pos, *len, n);

    for (j = 0; j <= UINT8_MAX; j++) {
        if (s->stats.commands[j]) {
            n = snprintf(text + pos, (size_t)(*len - pos),
                         "    %s: %" PRIu64 "\n",
                         afp_get_command_name((unsigned char) j),
                         s->stats.commands[j]);
            status_advance(&pos, *len, n);
        }
    }

    for (j = 0; j < s->num_volumes; j++) {
        v = &s->volumes[j];

        /* Only show status for attached volumes */
        if (v->attached == AFP_VOLUME_ATTACHED) {
            print_volume_status(v, s, text, &pos, len);
        }
    }

out:
    *len -= pos;
    return pos;
}
