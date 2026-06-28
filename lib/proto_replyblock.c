/*
 *  proto_replyblock.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <string.h>
#include "dsi.h"
#include "afp.h"
#include "compat.h"
#include "utils.h"
#include "afp_internal.h"
#include "afp_replies.h"


static int need_bytes(const char *p, const char *end, size_t len)
{
    return p <= end && (size_t)(end - p) >= len;
}

static int copy_pascal_bounded(char *dest, size_t dest_len,
                               const char *base, unsigned int size,
                               unsigned int offset)
{
    const char *src;
    const char *end = base + size;
    unsigned char len;

    if (offset >= size) {
        return -1;
    }

    src = base + offset;

    if (!need_bytes(src, end, 1)) {
        return -1;
    }

    len = (unsigned char) * src;

    if (!need_bytes(src + 1, end, len)) {
        return -1;
    }

    if (dest_len == 0) {
        return 0;
    }

    if ((size_t)len >= dest_len) {
        return -1;
    }

    memset(dest, 0, dest_len);
    memcpy(dest, src + 1, len);
    return 0;
}

static int copy_afpname_bounded(char *dest, size_t dest_len,
                                const char *base, unsigned int size,
                                unsigned int offset)
{
    const char *src;
    const char *end = base + size;
    unsigned short len;
    const unsigned short *len_p;

    if (offset > size || size - offset < 6) {
        return -1;
    }

    src = base + offset + 4;

    if (!need_bytes(src, end, 2)) {
        return -1;
    }

    len_p = (const void *) src;
    len = ntohs(*len_p);

    if (!need_bytes(src + 2, end, len)) {
        return -1;
    }

    if (dest_len == 0) {
        return 0;
    }

    if ((size_t)len >= dest_len) {
        return -1;
    }

    memset(dest, 0, dest_len);
    memcpy(dest, src + 2, len);
    return 0;
}

int parse_reply_block(struct afp_server *server _U_,
                      const char *buf,
                      unsigned int size, unsigned char isdir,
                      unsigned int filebitmap,
                      unsigned int dirbitmap,
                      struct afp_file_info * filecur)
{
    unsigned short bitmap;
    const char *p2, *end;
    memset(filecur, 0, sizeof(struct afp_file_info));
    filecur->isdir = isdir;
    p2 = buf;
    end = buf + size;

    if (isdir) {
        bitmap = dirbitmap ;
    } else {
        bitmap = filebitmap;
    }

    if (bitmap & kFPAttributeBit) {
        const unsigned short *attr = (const void *) p2;

        if (!need_bytes(p2, end, 2)) {
            return -1;
        }

        filecur->attributes = ntohs(*attr);
        p2 += 2;
    }

    if (bitmap & kFPParentDirIDBit) {
        const unsigned int *did = (const void *) p2;

        if (!need_bytes(p2, end, 4)) {
            return -1;
        }

        filecur->did = ntohl(*did);
        p2 += 4;
    }

    if (bitmap & kFPCreateDateBit) {
        const unsigned int *date = (const void *) p2;

        if (!need_bytes(p2, end, 4)) {
            return -1;
        }

        filecur->creation_date = AD_DATE_TO_UNIX(*date);
        p2 += 4;
    }

    if (bitmap & kFPModDateBit) {
        const unsigned int *date = (const void *) p2;

        if (!need_bytes(p2, end, 4)) {
            return -1;
        }

        filecur->modification_date = AD_DATE_TO_UNIX(*date);
        p2 += 4;
    }

    if (bitmap & kFPBackupDateBit) {
        const unsigned int *date = (const void *) p2;

        if (!need_bytes(p2, end, 4)) {
            return -1;
        }

        filecur->backup_date = AD_DATE_TO_UNIX(*date);
        p2 += 4;
    }

    if (bitmap & kFPFinderInfoBit) {
        if (!need_bytes(p2, end, 32)) {
            return -1;
        }

        memcpy(filecur->finderinfo, p2, 32);
        p2 += 32;
    }

    if (bitmap & kFPLongNameBit) {
        const unsigned short *offset = (const void *) p2;

        if (!need_bytes(p2, end, 2)
                || copy_pascal_bounded(filecur->name, sizeof(filecur->name),
                                       buf, size, ntohs(*offset)) < 0) {
            return -1;
        }

        p2 += 2;
    }

    if (bitmap & kFPShortNameBit) {
        if (!need_bytes(p2, end, 2)) {
            return -1;
        }

        p2 += 2;
    }

    if (bitmap & kFPNodeIDBit) {
        const unsigned int *id = (const void *) p2;

        if (!need_bytes(p2, end, 4)) {
            return -1;
        }

        filecur->fileid = ntohl(*id);
        p2 += 4;
    }

    if (isdir) {
        if (bitmap & kFPOffspringCountBit) {
            const unsigned short *offspring = (const void *) p2;

            if (!need_bytes(p2, end, 2)) {
                return -1;
            }

            filecur->offspring = ntohs(*offspring);
            p2 += 2;
        }

        if (bitmap & kFPOwnerIDBit) {
            const unsigned int *owner = (const void *) p2;

            if (!need_bytes(p2, end, 4)) {
                return -1;
            }

            filecur->unixprivs.uid = ntohl(*owner);
            p2 += 4;
        }

        if (bitmap & kFPGroupIDBit) {
            const unsigned int *group = (const void *) p2;

            if (!need_bytes(p2, end, 4)) {
                return -1;
            }

            filecur->unixprivs.gid = ntohl(*group);
            p2 += 4;
        }

        if (bitmap & kFPAccessRightsBit) {
            const unsigned int *access = (const void *) p2;

            if (!need_bytes(p2, end, 4)) {
                return -1;
            }

            filecur->accessrights = ntohl(*access);
            p2 += 4;
        }
    } else {
        if (bitmap & kFPDataForkLenBit) {
            const unsigned int *len = (const void *) p2;

            if (!need_bytes(p2, end, 4)) {
                return -1;
            }

            filecur->size = ntohl(*len);
            p2 += 4;
        }

        if (bitmap & kFPRsrcForkLenBit) {
            const unsigned int *rsrc_len = (const void *) p2;

            if (!need_bytes(p2, end, 4)) {
                return -1;
            }

            filecur->resourcesize = ntohl(*rsrc_len);
            p2 += 4;
        }

        if (bitmap & kFPExtDataForkLenBit) {
            const unsigned long long *len = (const void *) p2;

            if (!need_bytes(p2, end, 8)) {
                return -1;
            }

            filecur->size = ntoh64(*len);
            p2 += 8;
        }

        if (bitmap & kFPLaunchLimitBit) {
            if (!need_bytes(p2, end, 2)) {
                return -1;
            }

            p2 += 2;
        }
    }

    if (bitmap & kFPUTF8NameBit) {
        const unsigned short *offset = (const void *) p2;

        if (!need_bytes(p2, end, 6)
                || copy_afpname_bounded(filecur->name, sizeof(filecur->name),
                                        buf, size, ntohs(*offset)) < 0) {
            return -1;
        }

        p2 += 2;
        p2 += 4;
    }

    if (bitmap & kFPExtRsrcForkLenBit) {
        const unsigned long long *rsrc_len = (const void *) p2;

        if (!need_bytes(p2, end, 8)) {
            return -1;
        }

        filecur->resourcesize = ntoh64(*rsrc_len);
        p2 += 8;
    }

    if (bitmap & kFPUnixPrivsBit) {
        const struct afp_unixprivs *unixpriv = (const void *) p2;

        if (!need_bytes(p2, end, sizeof(*unixpriv))) {
            return -1;
        }

        filecur->unixprivs.uid = ntohl(unixpriv->uid);
        filecur->unixprivs.gid = ntohl(unixpriv->gid);
        filecur->unixprivs.permissions = ntohl(unixpriv->permissions);
        filecur->unixprivs.ua_permissions = ntohl(unixpriv->ua_permissions);
        p2 += sizeof(*unixpriv);
    }

    return 0;
}
