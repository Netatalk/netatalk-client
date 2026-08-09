/*
 *  proto_desktop.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "afp_replies.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

/* closedt, addicon, geticoninfo, addappl, removeappl */

int afp_geticon(struct afp_volume * volume, unsigned int filecreator,
                unsigned int filetype, unsigned char icontype,
                unsigned short length, struct afp_icon * icon)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad1;
        uint16_t dtrefnum ;
        uint32_t filecreator;
        uint32_t filetype ;
        uint8_t icontype ;
        uint8_t pad2;
        uint16_t length ;
    } __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = afpGetIcon;
    request_packet.pad1 = 0;
    /* I'm not entirely sure these two should be translated */
    request_packet.dtrefnum = htons(volume->dtrefnum);
    request_packet.filecreator = htonl(filecreator);
    request_packet.filetype = htonl(filetype);
    request_packet.icontype = icontype;
    request_packet.pad2 = 0;
    request_packet.length = htons(length);
    return afpc_dsi_send(volume->server, (char *)&request_packet,
                         sizeof(request_packet), DSI_DEFAULT_TIMEOUT,
                         afpGetIcon, (void *) icon);
}

int afp_geticon_reply(struct afp_server *server _U_,
                      char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
    } * reply_packet = (void *) buf;
    struct afp_icon * icon = other;
    unsigned int len;

    if (!icon || (icon->maxsize != 0 && !icon->data)
            || size < sizeof(*reply_packet)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "geticon response is malformed (%u bytes)", size);
        return -1;
    }

    if (reply_packet->header.return_code.error_code != kFPNoErr) {
        return reply_packet->header.return_code.error_code;
    }

    len = size - sizeof(*reply_packet);

    /* AFP explicitly permits a reply shorter than the requested length. */
    if (len > icon->maxsize) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "geticon response exceeds capacity (%u > %u)", len,
                       icon->maxsize);
        return -1;
    }

    icon->size = len;
    memcpy(icon->data, buf + sizeof(*reply_packet), len);
    return 0;
}


int afp_addcomment_sized(struct afp_volume *volume, unsigned int did,
                         const char *pathname, const void *comment,
                         size_t comment_size)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t dtrefnum ;
        uint32_t dirid ;
    } __attribute__((__packed__)) * request_packet;
    size_t path_size;
    size_t len;
    char *msg, *p;
    int rc;

    if (!volume || !pathname || (!comment && comment_size != 0)
            || comment_size > UINT8_MAX) {
        return -1;
    }

    path_size = strlen(pathname);

    if (path_size > INT_MAX - sizeof(*request_packet)
            - sizeof_path_header(volume) - 2U - comment_size) {
        return -1;
    }

    len = sizeof(*request_packet) + sizeof_path_header(volume)
          + path_size;

    if (len & 1U) {
        len++;
    }

    len += 1U + comment_size;
    msg = calloc(1, len);

    if (!msg) {
        return -1;
    }

    p = msg + (sizeof(*request_packet));
    request_packet = (void *) msg;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet->command = afpAddComment;
    request_packet->pad = 0;
    request_packet->dtrefnum = htons(volume->dtrefnum);
    request_packet->dirid = htonl(did);
    copy_path(volume, p, pathname, path_size);
    unixpath_to_afppath(volume, p);
    p = msg + sizeof(*request_packet) + sizeof_path_header(volume)
        + path_size;

    if (((uintptr_t) p) & 0x1) {
        /* Make sure we're on an even boundary */
        p++;
    }

    *p++ = (uint8_t)comment_size;

    if (comment_size) {
        memcpy(p, comment, comment_size);
    }

    rc = afpc_dsi_send(volume->server, msg, (int)len,
                       DSI_DEFAULT_TIMEOUT,
                       afpAddComment, NULL);
    free(msg);
    return rc;
}

int afp_addcomment(struct afp_volume *volume, unsigned int did,
                   const char *pathname, char *comment, uint64_t *size)
{
    size_t comment_size;

    if (!comment) {
        return -1;
    }

    comment_size = strlen(comment);

    if (size) {
        *size = comment_size;
    }

    return afp_addcomment_sized(volume, did, pathname, comment, comment_size);
}

int afp_getcomment(struct afp_volume *volume, unsigned int did,
                   const char *pathname, struct afp_comment * comment)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t dtrefnum ;
        uint32_t dirid ;
    } __attribute__((__packed__)) * request_packet;
    unsigned int len = sizeof(*request_packet) +
                       sizeof_path_header(volume) + strlen(pathname);
    char *msg, *path;
    int rc;
    msg = malloc(len);
    path = msg + (sizeof(*request_packet));
    request_packet = (void *) msg;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet->command = afpGetComment;
    request_packet->pad = 0;
    request_packet->dtrefnum = htons(volume->dtrefnum);
    request_packet->dirid = htonl(did);
    copy_path(volume, path, pathname, strlen(pathname));
    unixpath_to_afppath(volume, path);
    rc = afpc_dsi_send(volume->server, (char *)msg, len, DSI_DEFAULT_TIMEOUT,
                       afpGetComment, (void *) comment);
    free(msg);
    return rc;
}

int afp_getcomment_reply(struct afp_server *server _U_,
                         char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t commentlen;
    } __attribute__((__packed__)) * reply_packet = (void *) buf;
    struct afp_comment * comment = other;
    unsigned int len;

    if (!comment || !comment->data || size < sizeof(*reply_packet)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getcomment response is too short (%u bytes)", size);
        return -1;
    }

    if (reply_packet->header.return_code.error_code != kFPNoErr) {
        return reply_packet->header.return_code.error_code;
    }

    len = reply_packet->commentlen;

    if (len > size - sizeof(*reply_packet) || len > comment->maxsize) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getcomment response length is invalid (%u bytes)", len);
        return -1;
    }

    memcpy(comment->data, buf + sizeof(*reply_packet), len);
    comment->size = len;
    return 0;
}

int afp_closedt(struct afp_server * server, unsigned short refnum)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t refnum ;
    } __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = afpCloseDT;
    request_packet.pad = 0;
    request_packet.refnum = htons(refnum);
    return afpc_dsi_send(server, (char *) &request_packet,
                         sizeof(request_packet), DSI_DEFAULT_TIMEOUT, afpCloseDT, NULL);
}



int afp_opendt(struct afp_volume *volume, unsigned short * refnum)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t volid ;
    } __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = afpOpenDT;
    request_packet.pad = 0;
    request_packet.volid = htons(volume->volid);
    return afpc_dsi_send(volume->server, (char *) &request_packet,
                         sizeof(request_packet), DSI_DEFAULT_TIMEOUT, afpOpenDT,
                         (void *) refnum);
}


int afp_opendt_reply(struct afp_server *server _U_,
                     char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t refnum ;
    } __attribute__((__packed__)) * reply_packet = (void *) buf;
    unsigned short *refnum = other;

    if (!refnum || size < sizeof(*reply_packet)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "opendt response is too short (%u bytes)", size);
        return -1;
    }

    if (reply_packet->header.return_code.error_code != kFPNoErr) {
        return reply_packet->header.return_code.error_code;
    }

    *refnum = ntohs(reply_packet->refnum);
    return 0;
}

int afp_geticoninfo(struct afp_volume *volume, unsigned int filecreator,
                    unsigned short index, struct afp_icon_info *info)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t dtrefnum;
        uint32_t filecreator;
        uint16_t index;
    } __attribute__((__packed__)) request;
    struct dsi_header hdr;

    if (!volume || !info) {
        return -1;
    }

    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(hdr));
    request.command = afpGetIconInfo;
    request.pad = 0;
    request.dtrefnum = htons(volume->dtrefnum);
    request.filecreator = htonl(filecreator);
    request.index = htons(index);
    return afpc_dsi_send(volume->server, (char *)&request, sizeof(request),
                         DSI_DEFAULT_TIMEOUT, afpGetIconInfo, info);
}

int afp_geticoninfo_reply(struct afp_server *server _U_, char *buf,
                          unsigned int size, void *other)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
        uint32_t tag;
        uint32_t filetype;
        uint8_t icontype;
        uint8_t pad;
        uint16_t icon_size;
    } __attribute__((__packed__)) *reply = (const void *)buf;
    struct afp_icon_info *info = other;

    if (!info || size < sizeof(*reply)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "geticoninfo response is too short (%u bytes)", size);
        return -1;
    }

    if (reply->header.return_code.error_code != kFPNoErr) {
        return reply->header.return_code.error_code;
    }

    info->tag = ntohl(reply->tag);
    info->filetype = ntohl(reply->filetype);
    info->icontype = reply->icontype;
    info->size = ntohs(reply->icon_size);
    return 0;
}

int afp_getappl(struct afp_volume *volume, unsigned int filecreator,
                unsigned short index, unsigned short bitmap,
                struct afp_appl *appl)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t dtrefnum;
        uint32_t filecreator;
        uint16_t index;
        uint16_t bitmap;
    } __attribute__((__packed__)) request;
    struct dsi_header hdr;

    if (!volume || !appl) {
        return -1;
    }

    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(hdr));
    request.command = afpGetAPPL;
    request.pad = 0;
    request.dtrefnum = htons(volume->dtrefnum);
    request.filecreator = htonl(filecreator);
    request.index = htons(index);
    request.bitmap = htons(bitmap);
    return afpc_dsi_send(volume->server, (char *)&request, sizeof(request),
                         DSI_DEFAULT_TIMEOUT, afpGetAPPL, appl);
}

int afp_getappl_reply(struct afp_server *server, char *buf,
                      unsigned int size, void *other)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint32_t tag;
    } __attribute__((__packed__)) *reply = (const void *)buf;
    struct afp_appl *appl = other;
    unsigned int bitmap;

    if (!appl || size < sizeof(*reply)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getappl response is too short (%u bytes)", size);
        return -1;
    }

    if (reply->header.return_code.error_code != kFPNoErr) {
        return reply->header.return_code.error_code;
    }

    bitmap = ntohs(reply->bitmap);

    if (parse_reply_block(server, buf + sizeof(*reply), size - sizeof(*reply),
                          0, bitmap, 0, &appl->file) < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "getappl response has malformed file parameters");
        return -1;
    }

    appl->bitmap = (uint16_t)bitmap;
    appl->tag = ntohl(reply->tag);
    return 0;
}
