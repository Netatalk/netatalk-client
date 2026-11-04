/*
 *  proto_replyblock.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

#include <string.h>

#include "afp_internal.h"
#include "afp_replies.h"
#include "codepage.h"
#include "utils.h"

/*!
 * @brief Whether a block holds len more bytes at p
 *
 * @param[in] p    the read position
 * @param[in] end  the block's end
 * @param[in] len  bytes wanted
 * @returns 1 when they fit, 0 otherwise
 */
static int need_bytes(const char *p, const char *end, size_t len)
{
    return p <= end && (size_t)(end - p) >= len;
}

/*!
 * @brief Locate a Pascal string in a parameter block
 *
 * @param[in]  base    the block
 * @param[in]  size    bytes of the block
 * @param[in]  offset  offset of the string's length byte in the block
 * @param[out] src     receives the string's first byte
 * @param[out] len     receives its length
 * @returns 0, or -1 for a string that runs past the block
 */
static int locate_pascal(const char *base, size_t size,
                         unsigned int offset, const char **src, size_t *len)
{
    const char *end = base + size;

    if (offset >= size) {
        return -1;
    }

    *len = (unsigned char) base[offset];

    if (!need_bytes(base + offset + 1, end, *len)) {
        return -1;
    }

    *src = base + offset + 1;
    return 0;
}

/*!
 * @brief Locate an AFP name in a parameter block
 *
 * The name's block is a four-byte text encoding hint, a two-byte length
 * and the bytes.
 *
 * @param[in]  base    the block
 * @param[in]  size    bytes of the block
 * @param[in]  offset  offset of the name's block in the block
 * @param[out] src     receives the name's first byte
 * @param[out] len     receives its length
 * @returns 0, or -1 for a name that runs past the block
 */
static int locate_afpname(const char *base, size_t size,
                          unsigned int offset, const char **src, size_t *len)
{
    const char *end = base + size;
    uint16_t len16;

    if (offset > size || size - offset < 6) {
        return -1;
    }

    memcpy(&len16, base + offset + 4, sizeof(len16));
    *len = ntohs(len16);

    if (!need_bytes(base + offset + 6, end, *len)) {
        return -1;
    }

    *src = base + offset + 6;
    return 0;
}

/*!
 * @brief Convert a located name into the caller's buffer
 *
 * @param[in]  encoding  kFPLongName or kFPUTF8Name, by the bit that named
 *                       the name
 * @param[in]  src       the name's wire bytes
 * @param[in]  len       bytes of src
 * @param[out] name      receives the UNIX form, or NULL to skip the name
 * @param[in]  name_len  capacity of name
 * @returns 0, or -1 for a name the buffer does not hold
 */
static int store_name(char encoding, const char *src, size_t len,
                      char *name, size_t name_len)
{
    if (name == NULL) {
        return 0;
    }

    return convert_name_to_unix(encoding, src, len, name, name_len);
}

/*!
 * @brief Parse a date parameter
 *
 * @param[in]     server  server of the session, for its time offset
 * @param[in,out] p       the parameter, advanced past it
 * @param[in]     end     end of the block
 * @param[in]     bit     the date's bitmap bit
 * @param[out]    ns      receives the date, unless it is the unset sentinel
 * @param[in,out] valid   gets bit for a date that is set
 * @returns 0, or -1 for a block too short for the date
 */
static int parse_date(const struct afp_server *server, const char **p,
                      const char *end, uint32_t bit, int64_t *ns,
                      uint32_t *valid)
{
    uint32_t date;

    if (!need_bytes(*p, end, 4)) {
        return -1;
    }

    memcpy(&date, *p, sizeof(date));

    if (ntohl(date) != UINT32_C(0x80000000)) {
        *ns = afp_ns_from_sec(afp_date_to_unix(server, date));
        *valid |= bit;
    }

    *p += 4;
    return 0;
}

/*!
 * @brief Parse a 16-bit parameter
 *
 * @param[in,out] p      the parameter, advanced past it
 * @param[in]     end    end of the block
 * @param[in]     bit    the parameter's bitmap bit
 * @param[out]    value  receives the parameter
 * @param[in,out] valid  gets bit
 * @returns 0, or -1 for a block too short for the parameter
 */
static int parse_u16(const char **p, const char *end, uint32_t bit,
                     uint16_t *value, uint32_t *valid)
{
    uint16_t v;

    if (!need_bytes(*p, end, 2)) {
        return -1;
    }

    memcpy(&v, *p, sizeof(v));
    *value = ntohs(v);
    *valid |= bit;
    *p += 2;
    return 0;
}

/*!
 * @brief Parse a 32-bit parameter
 *
 * @param[in,out] p      the parameter, advanced past it
 * @param[in]     end    end of the block
 * @param[in]     bit    the parameter's bitmap bit
 * @param[out]    value  receives the parameter
 * @param[in,out] valid  gets bit
 * @returns 0, or -1 for a block too short for the parameter
 */
static int parse_u32(const char **p, const char *end, uint32_t bit,
                     uint32_t *value, uint32_t *valid)
{
    uint32_t v;

    if (!need_bytes(*p, end, 4)) {
        return -1;
    }

    memcpy(&v, *p, sizeof(v));
    *value = ntohl(v);
    *valid |= bit;
    *p += 4;
    return 0;
}

/*!
 * @brief Parse a 64-bit parameter
 *
 * @param[in,out] p      the parameter, advanced past it
 * @param[in]     end    end of the block
 * @param[in]     bit    the parameter's bitmap bit
 * @param[out]    value  receives the parameter
 * @param[in,out] valid  gets bit
 * @returns 0, or -1 for a block too short for the parameter
 */
static int parse_u64(const char **p, const char *end, uint32_t bit,
                     uint64_t *value, uint32_t *valid)
{
    uint64_t v;

    if (!need_bytes(*p, end, 8)) {
        return -1;
    }

    memcpy(&v, *p, sizeof(v));
    *value = ntoh64(v);
    *valid |= bit;
    *p += 8;
    return 0;
}

/*!
 * @brief Skip a parameter the record does not carry
 *
 * @param[in,out] p    the parameter, advanced past it
 * @param[in]     end  end of the block
 * @param[in]     len  the parameter's bytes
 * @returns 0, or -1 for a block too short for the parameter
 */
static int skip_bytes(const char **p, const char *end, size_t len)
{
    if (!need_bytes(*p, end, len)) {
        return -1;
    }

    *p += len;
    return 0;
}

/*!
 * @brief Parse a reply's parameter block into a record
 *
 * Each stored member sets its bit in valid; a name converts to the UNIX form
 * into the caller's buffer, the UTF-8 name winning over the long name.
 *
 * @param[in]  server      server of the session, for its time offset
 * @param[in]  buf         the parameter block
 * @param[in]  size        bytes of buf
 * @param[in]  isdir       nonzero for a directory's parameters, as afpd's
 *                         0x80, 0 for a file's
 * @param[in]  filebitmap  the file bitmap of the request
 * @param[in]  dirbitmap   the directory bitmap of the request
 * @param[out] e           receives the parameters, cleared first
 * @param[out] name        receives the name, empty when the block carries
 *                         none, or NULL to skip it
 * @param[in]  name_len    capacity of name
 * @returns 0, or -1 for a block too short for its bitmap or a name the
 *          buffer does not hold
 */
int parse_reply_block(struct afp_server *server, const char *buf,
                      size_t size, unsigned char isdir,
                      unsigned int filebitmap, unsigned int dirbitmap,
                      struct afp_dirent *e, char *name, size_t name_len)
{
    unsigned int bitmap = isdir ? dirbitmap : filebitmap;
    const char *p2 = buf;
    const char *end = buf + size;
    const char *src = NULL;
    size_t len = 0;
    memset(e, 0, sizeof(*e));
    e->isdir = isdir;

    if (name != NULL && name_len > 0) {
        name[0] = '\0';
    }

    if ((bitmap & kFPAttributeBit)
            && parse_u16(&p2, end, kFPAttributeBit, &e->attributes,
                         &e->valid) < 0) {
        return -1;
    }

    if ((bitmap & kFPParentDirIDBit)
            && parse_u32(&p2, end, kFPParentDirIDBit, &e->parent_did,
                         &e->valid) < 0) {
        return -1;
    }

    if ((bitmap & kFPCreateDateBit)
            && parse_date(server, &p2, end, kFPCreateDateBit, &e->create_ns,
                          &e->valid) < 0) {
        return -1;
    }

    if ((bitmap & kFPModDateBit)
            && parse_date(server, &p2, end, kFPModDateBit, &e->mod_ns,
                          &e->valid) < 0) {
        return -1;
    }

    if ((bitmap & kFPBackupDateBit) && skip_bytes(&p2, end, 4) < 0) {
        return -1;
    }

    if (bitmap & kFPFinderInfoBit) {
        if (!need_bytes(p2, end, 32)) {
            return -1;
        }

        memcpy(e->finderinfo, p2, 32);
        e->valid |= kFPFinderInfoBit;
        p2 += 32;
    }

    if (bitmap & kFPLongNameBit) {
        uint16_t offset;

        if (parse_u16(&p2, end, kFPLongNameBit, &offset, &e->valid) < 0
                || locate_pascal(buf, size, offset, &src, &len) < 0
                || store_name(kFPLongName, src, len, name, name_len) < 0) {
            return -1;
        }
    }

    if ((bitmap & kFPShortNameBit) && skip_bytes(&p2, end, 2) < 0) {
        return -1;
    }

    if ((bitmap & kFPNodeIDBit)
            && parse_u32(&p2, end, kFPNodeIDBit, &e->cnid, &e->valid) < 0) {
        return -1;
    }

    if (isdir) {
        if ((bitmap & kFPOffspringCountBit)
                && parse_u16(&p2, end, kFPOffspringCountBit, &e->offspring,
                             &e->valid) < 0) {
            return -1;
        }

        if ((bitmap & kFPOwnerIDBit)
                && parse_u32(&p2, end, kFPOwnerIDBit, &e->unixprivs.uid,
                             &e->valid) < 0) {
            return -1;
        }

        if ((bitmap & kFPGroupIDBit)
                && parse_u32(&p2, end, kFPGroupIDBit, &e->unixprivs.gid,
                             &e->valid) < 0) {
            return -1;
        }

        if ((bitmap & kFPAccessRightsBit) && skip_bytes(&p2, end, 4) < 0) {
            return -1;
        }
    } else {
        if (bitmap & kFPDataForkLenBit) {
            uint32_t len32;

            if (parse_u32(&p2, end, kFPDataForkLenBit, &len32, &e->valid) < 0) {
                return -1;
            }

            e->size = len32;
        }

        if (bitmap & kFPRsrcForkLenBit) {
            uint32_t len32;

            if (parse_u32(&p2, end, kFPRsrcForkLenBit, &len32, &e->valid) < 0) {
                return -1;
            }

            e->resourcesize = len32;
        }

        if ((bitmap & kFPExtDataForkLenBit)
                && parse_u64(&p2, end, kFPExtDataForkLenBit, &e->size,
                             &e->valid) < 0) {
            return -1;
        }

        if ((bitmap & kFPLaunchLimitBit) && skip_bytes(&p2, end, 2) < 0) {
            return -1;
        }
    }

    if (bitmap & kFPUTF8NameBit) {
        uint16_t offset;

        /* The offset, then the four-byte text encoding hint. */
        if (parse_u16(&p2, end, kFPUTF8NameBit, &offset, &e->valid) < 0
                || skip_bytes(&p2, end, 4) < 0
                || locate_afpname(buf, size, offset, &src, &len) < 0
                || store_name(kFPUTF8Name, src, len, name, name_len) < 0) {
            return -1;
        }
    }

    if ((bitmap & kFPExtRsrcForkLenBit)
            && parse_u64(&p2, end, kFPExtRsrcForkLenBit, &e->resourcesize,
                         &e->valid) < 0) {
        return -1;
    }

    if (bitmap & kFPUnixPrivsBit) {
        struct afpc_unix_privileges unixpriv;

        if (!need_bytes(p2, end, sizeof(unixpriv))) {
            return -1;
        }

        memcpy(&unixpriv, p2, sizeof(unixpriv));
        e->unixprivs.uid = ntohl(unixpriv.uid);
        e->unixprivs.gid = ntohl(unixpriv.gid);
        e->unixprivs.permissions = ntohl(unixpriv.permissions);
        e->unixprivs.ua_permissions = ntohl(unixpriv.ua_permissions);
        e->valid |= kFPUnixPrivsBit;
        p2 += sizeof(unixpriv);
    }

    return 0;
}
