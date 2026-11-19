/*
 *  proto_map.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief FPGetUserInfo, FPMapID and FPMapName: user and group IDs and names
 */

#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

/*! @brief afp_getuserinfo()'s bitmap, and the IDs its reply parser takes */
struct uidgid {
    unsigned short bitmap;
    unsigned int uid;
    unsigned int gid;
};

/*!
 * @brief Ask the server for a user's ID and primary group's ID
 *
 * @param[in]  server    the server
 * @param[in]  thisuser  1 for the session's user, 0 for userid
 * @param[in]  userid    the user to ask about when thisuser is 0
 * @param[in]  bitmap    kFPGetUserInfo_USER_ID, kFPGetUserInfo_PRI_GROUPID,
 *                       or both
 * @param[out] newuid    receives the user's ID when the bitmap asks for it
 *                       and the request succeeds; left as it is otherwise
 * @param[out] newgid    receives the group's ID likewise
 * @returns 0, the server's AFP result, -1 for a reply the parser refuses,
 *          or afpc_dsi_send()'s error when the request fails
 */
int afp_getuserinfo(struct afp_server * server, int thisuser,
                    unsigned int userid, unsigned short bitmap,
                    unsigned int *newuid, unsigned int *newgid)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t thisuser;
        uint32_t userid __attribute__((__packed__));
        uint16_t bitmap __attribute__((__packed__));
    }  __attribute__((__packed__)) request;
    struct uidgid uidgid;
    int ret;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpGetUserInfo;
    request.thisuser = (thisuser != 0);
    request.userid = htonl(userid);
    request.bitmap = htons(bitmap);
    uidgid.bitmap = bitmap;
    ret = afpc_dsi_send(server, (char *) &request, sizeof(request),
                        DSI_DEFAULT_TIMEOUT, afpGetUserInfo, (void *) &uidgid);

    if (ret != 0) {
        return ret;
    }

    if (bitmap & kFPGetUserInfo_USER_ID) {
        *newuid = uidgid.uid;
    }

    if (bitmap & kFPGetUserInfo_PRI_GROUPID) {
        *newgid = uidgid.gid;
    }

    return 0;
}


/*!
 * @brief Take the IDs an FPGetUserInfo reply carries
 *
 * Each ID the bitmap names follows it, the user's before the group's; one
 * the bitmap leaves out takes no room.
 *
 * @param[in]     server  unused
 * @param[in]     buf     the reply
 * @param[in]     size    its length
 * @param[in,out] other   the struct uidgid, whose bitmap names the IDs asked
 *                        for and which receives the IDs, 0 for one the reply
 *                        does not carry
 * @returns 0, or -1 for an error reply, one without an ID asked for, or
 *          one too short for its bitmap's IDs
 */
int afp_getuserinfo_reply(struct afp_server *server _U_,
                          char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    struct uidgid *uidgid = other;
    unsigned int offset = sizeof(*reply);
    unsigned short bitmap;
    uint32_t id;
    uidgid->uid = 0;
    uidgid->gid = 0;

    if (size < sizeof(*reply)
            || reply->header.return_code.error_code != kFPNoErr) {
        return -1;
    }

    bitmap = ntohs(reply->bitmap);

    if ((bitmap & uidgid->bitmap) != uidgid->bitmap) {
        return -1;
    }

    if (bitmap & kFPGetUserInfo_USER_ID) {
        if (size - offset < sizeof(id)) {
            return -1;
        }

        memcpy(&id, buf + offset, sizeof(id));
        uidgid->uid = ntohl(id);
        offset += sizeof(id);
    }

    if (bitmap & kFPGetUserInfo_PRI_GROUPID) {
        if (size - offset < sizeof(id)) {
            return -1;
        }

        memcpy(&id, buf + offset, sizeof(id));
        uidgid->gid = ntohl(id);
    }

    return 0;
}


/*! @brief An FPMapID request's subfunction, and the caller's name buffer */
struct mapid_name {
    unsigned char subfunction;
    char *name;
};

/*!
 * @brief Ask the server for the name of a user or group ID with FPMapID
 *
 * @param[in]  server       the logged-in server
 * @param[in]  subfunction  kUserIDToName, kGroupIDToName, kUserIDToUTF8Name
 *                          or kGroupIDToUTF8Name
 * @param[in]  id           the ID
 * @param[out] name         receives AFPC_MAX_NAME_BYTES bytes, the name
 *                          NUL-terminated, or NULL
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
int afp_mapid(struct afp_server * server, unsigned char subfunction,
              unsigned int id, char *name)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t subfunction;
        uint32_t id;
    }  __attribute__((__packed__)) request;
    struct mapid_name mapped = { .subfunction = subfunction, .name = name };
    int ret;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpMapID;
    request.subfunction = subfunction;
    request.id = htonl(id);
    ret = afpc_dsi_send(server, (char *) &request, sizeof(request),
                        DSI_DEFAULT_TIMEOUT, afpMapID, &mapped);
    return ret;
}

/*!
 * @brief Take the name an FPMapID reply carries
 *
 * The name follows the DSI header, its length in two bytes for a UTF-8
 * name and in one for a Mac Roman name.
 *
 * @param[in]     server  unused
 * @param[in]     buf     the reply, DSI header first
 * @param[in]     size    bytes of buf
 * @param[in,out] other   the request's subfunction, and the caller's buffer
 *                        of AFPC_MAX_NAME_BYTES for the name, which may be
 *                        NULL
 * @returns 0, or -1 for a reply too short for its name's length, one whose
 *          name runs past its end, or one that answers an error
 */
int afp_mapid_reply(struct afp_server *server _U_,
                    char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        char name[];
    }  __attribute__((__packed__)) * reply = (void *) buf;
    const struct mapid_name *mapped = other;
    int utf8 = mapped->subfunction == kUserIDToUTF8Name
               || mapped->subfunction == kGroupIDToUTF8Name;
    int roman = mapped->subfunction == kUserIDToName
                || mapped->subfunction == kGroupIDToName;
    uint16_t len;

    if (size < sizeof(struct dsi_header) + (utf8 ? 2U : 1U)
            || reply->header.return_code.error_code != kFPNoErr) {
        return -1;
    }

    if (utf8) {
        memcpy(&len, reply->name, sizeof(len));

        if (2U + ntohs(len) > size - sizeof(struct dsi_header)) {
            return -1;
        }
    } else if (roman && 1U + (unsigned char) reply->name[0]
               > size - sizeof(struct dsi_header)) {
        return -1;
    }

    if (mapped->name == NULL) {
        return 0;
    }

    memset(mapped->name, 0, AFPC_MAX_NAME_BYTES);

    if (utf8) {
        copy_from_pascal_two(mapped->name, reply->name, AFPC_MAX_NAME_BYTES);
    } else if (roman) {
        copy_from_pascal(mapped->name, reply->name, AFPC_MAX_NAME_BYTES);
    }

    return 0;
}

int afp_mapname(struct afp_server * server, unsigned char subfunction,
                char *name, unsigned int *id)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t subfunction;
    }  __attribute__((__packed__)) * request;
    unsigned int len = sizeof(*request) +
                       1 + strlen(name);
    char *msg, *nameptr;
    int ret;

    if ((msg = malloc(len)) == NULL) {
        return -1;
    }

    memset(msg, 0x4b, len);
    nameptr = msg + sizeof(*request);
    request = (void *) msg;
    copy_to_pascal(nameptr, name);
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->dsi_header, &hdr, sizeof(struct dsi_header));
    request->command = afpMapName;
    request->subfunction = subfunction;
    ret = afpc_dsi_send(server, (char *) request, len,
                        DSI_DEFAULT_TIMEOUT, afpMapName, (void *) id);
    free(msg);
    return ret;
}


int afp_mapname_reply(struct afp_server *server _U_,
                      char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint32_t id;
    }  __attribute__((__packed__))* reply = (void *) buf;
    unsigned int *id = (void *) other;

    /* RJVB: there should be at least 2 bytes after the dsi_header */
    if (size < (sizeof(unsigned short) + sizeof(struct dsi_header))) {
        return -1;
    }

    *id = ntohl(reply->id);
    return 0;
}
