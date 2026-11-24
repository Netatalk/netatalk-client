/*
 *  proto_volume.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief AFP volume requests: opening and closing volumes, volume parameters
 *
 * Part of the protocol layer (afp_*): one call per AFP command.
 * Lanes: metadata, session.
 * Northbound: the session layer and the mid level.
 * Southbound: the DSI layer.
 */

#include <string.h>
#include <stdlib.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

/*!
 * @brief Take a volume parameter from a reply, at any alignment
 *
 * @param[in,out] p      the parameter, advanced past it
 * @param[in]     end    the reply's end
 * @param[out]    field  receives the parameter's bytes, in network order,
 *                       or NULL for a parameter to step over
 * @param[in]     len    bytes of the parameter
 * @returns 0, or -1 for a reply too short for the parameter
 */
static int take_parameter(const char **p, const char *end, void *field,
                          size_t len)
{
    if ((size_t)(end - *p) < len) {
        return -1;
    }

    if (field != NULL) {
        memcpy(field, *p, len);
    }

    *p += len;
    return 0;
}

/*!
 * @brief Read the volume parameters a bitmap names from a reply
 *
 * Each parameter follows the one before, in the bitmap's order, at whatever
 * offset that leaves it.
 *
 * @param[in]     server  the server, whose AFP 2 dates are its local time
 * @param[in,out] tmpvol  the volume, which receives the parameters
 * @param[in]     bitmap  the parameters the reply carries
 * @param[in]     msg     the reply's parameters, after its bitmap
 * @param[in]     size    bytes of msg
 * @returns 0, or -1 for a reply too short for its bitmap's parameters
 */
static int parse_volbitmap_reply(struct afp_server *server,
                                 struct afp_volume *tmpvol,
                                 unsigned short bitmap, const char *msg,
                                 unsigned int size)
{
    const char *p = msg;
    const char *end = msg + size;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;

    if (bitmap & kFPVolAttributeBit) {
        if (take_parameter(&p, end, &u16, sizeof(u16)) != 0) {
            return -1;
        }

        tmpvol->attributes = ntohs(u16);
    }

    if (bitmap & kFPVolSignatureBit) {
        if (take_parameter(&p, end, &u16, sizeof(u16)) != 0) {
            return -1;
        }

        tmpvol->signature = ntohs(u16);
    }

    if (bitmap & kFPVolCreateDateBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->creation_date = afp_date_to_unix(server, u32);
    }

    if (bitmap & kFPVolModDateBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->modification_date = afp_date_to_unix(server, u32);
    }

    if (bitmap & kFPVolBackupDateBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->backup_date = afp_date_to_unix(server, u32);
    }

    if (bitmap & kFPVolIDBit) {
        if (take_parameter(&p, end, &u16, sizeof(u16)) != 0) {
            return -1;
        }

        tmpvol->volid = ntohs(u16);
    }

    if (bitmap & kFPVolBytesFreeBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->stat.f_bfree = ntohl(u32);
    }

    if (bitmap & kFPVolBytesTotalBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->stat.f_blocks = ntohl(u32);
    }

    /* The caller has set the name, so only its offset is stepped over: the
     * reply gives the name in the server's Mac charset. */
    if ((bitmap & kFPVolNameBit)
            && take_parameter(&p, end, NULL, sizeof(u16)) != 0) {
        return -1;
    }

    if (bitmap & kFPVolExtBytesFreeBit) {
        if (take_parameter(&p, end, &u64, sizeof(u64)) != 0) {
            return -1;
        }

        tmpvol->stat.f_bfree = ntoh64(u64);
    }

    if (bitmap & kFPVolExtBytesTotalBit) {
        if (take_parameter(&p, end, &u64, sizeof(u64)) != 0) {
            return -1;
        }

        tmpvol->stat.f_blocks = ntoh64(u64);
    }

    if (bitmap & kFPVolBlockSizeBit) {
        if (take_parameter(&p, end, &u32, sizeof(u32)) != 0) {
            return -1;
        }

        tmpvol->stat.f_bsize = ntohl(u32);
    }

    return 0;
}

int afp_volclose(struct afp_volume * volume)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t volid;
    }  __attribute__((__packed__)) request;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(hdr));
    request.command = afpCloseVol;
    request.pad = 0;
    request.volid = htons(volume->volid);
    return afpc_dsi_send(volume->server, (char *) &request, sizeof(request),
                         DSI_DEFAULT_TIMEOUT, afpCloseVol, NULL);
}


int afp_volopen_reply(struct afp_server *server, char *buf, unsigned int size,
                      void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
    }  __attribute__((__packed__)) * afp_volopen_reply_packet = (void *) buf;
    unsigned short bitmap;
    struct afp_volume **v_p = (void *) other;
    struct afp_volume * volume = *v_p;

    if (size < sizeof(*afp_volopen_reply_packet)) {
        return -1;
    }

    bitmap = ntohs(afp_volopen_reply_packet->bitmap);

    if (parse_volbitmap_reply(server, volume,
                              bitmap, buf + sizeof(*afp_volopen_reply_packet),
                              size - sizeof(*afp_volopen_reply_packet)) != 0) {
        return -1;
    }

    if (volume->attributes & kSupportsUTF8Names) {
        convert_utf8dec_to_utf8pre(volume->volume_name,
                                   strnlen(volume->volume_name, sizeof(volume->volume_name)),
                                   volume->volume_name_printable,
                                   AFP_VOLUME_NAME_UTF8_LEN);
    } else {
        convert_mac_roman_to_utf8(volume->volume_name,
                                  strnlen(volume->volume_name, sizeof(volume->volume_name)),
                                  volume->volume_name_printable,
                                  AFP_VOLUME_NAME_UTF8_LEN);
    }

    return 0;
}

int afp_volopen(struct afp_volume * volume,
                unsigned short bitmap, char *password)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t bitmap;
    }  __attribute__((__packed__)) * afp_volopen_request;
    char *msg, *volname, *password_ptr;
    unsigned int len = sizeof(*afp_volopen_request) +
                       strnlen(volume->volume_name, sizeof(volume->volume_name)) + 1;
    unsigned char len2;
    int ret;

    if (password) {
        len += AFP_VOLPASS_LEN;

        /* This is undocumented, but put it on
        an even boundary.  This is how netatalk works. */
        if (len & 0x1) {
            len++;
        }
    }

    if ((msg = malloc(len)) == NULL) {
        return -1;
    }

    afp_volopen_request = (void *) msg;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&afp_volopen_request->dsi_header, &hdr, sizeof(hdr));
    afp_volopen_request->command = afpOpenVol;
    afp_volopen_request->pad = 0;
    afp_volopen_request->bitmap = htons(bitmap);
    volname = msg + sizeof(*afp_volopen_request);
    copy_to_pascal(volname, volume->volume_name);

    if (password) {
        password_ptr = msg + len - AFP_VOLPASS_LEN;
        len2 = strnlen(password, AFP_VOLPASS_LEN);

        if (len2 > AFP_VOLPASS_LEN) {
            len2 = AFP_VOLPASS_LEN;
        }

        memset(password_ptr, 0, AFP_VOLPASS_LEN);
        memcpy(password_ptr, password, len2);
    }

    ret = afpc_dsi_send(volume->server, (char *) msg, len,
                        DSI_OPENVOLUME_TIMEOUT, afpOpenVol, (void *) &volume);
    free(msg);
    return ret;
}


int afp_getvolparms_reply(struct afp_server *server, char *buf,
                          unsigned int size, void *other)
{
    unsigned int bitmap;
    struct afp_volume *volume = (void *) other;
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint16_t bitmap;
    }  __attribute__((__packed__)) * reply = (void *) buf;

    if (size < sizeof(*reply)) {
        return -1;
    }

    bitmap = ntohs(reply->bitmap);

    if (!volume) {
        log_for_client(NULL, AFPFSD, LOG_WARNING, "I don't know what volume this is");
        return -1;
    }

    /* Go find the correct volume */

    if (parse_volbitmap_reply(server, volume,
                              bitmap, buf + sizeof(*reply),
                              size - sizeof(*reply)) != 0) {
        return -1;
    }

    return 0;
}

int afp_getvolparms(struct afp_volume * volume, unsigned short bitmap)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t volid __attribute__((__packed__));
        uint16_t bitmap __attribute__((__packed__));
    }  __attribute__((__packed__)) afp_getvolparms_request;
    int ret;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&afp_getvolparms_request.dsi_header, &hdr, sizeof(hdr));
    afp_getvolparms_request.command = afpGetVolParms;
    afp_getvolparms_request.pad = 0;
    afp_getvolparms_request.volid = htons(volume->volid);
    afp_getvolparms_request.bitmap = htons(bitmap);
    ret = afpc_dsi_send(volume->server, (char *) &afp_getvolparms_request,
                        sizeof(afp_getvolparms_request), DSI_DEFAULT_TIMEOUT,
                        afpGetVolParms, (void *) volume);
    return ret;
}
