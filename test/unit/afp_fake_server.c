/*
    afp_fake_server.c: the server end of an AFP session for the unit tests

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief The fake AFP server's thread, which answers from the test's knobs
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/did.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/forklist.h"
#include "lib/utils.h"

/* The largest request read; a larger one ends the server. */
#define AFP_FAKE_SERVER_MAX_REQUEST 65536U
/* A reply goes out in pieces no socket's buffer is smaller than. */
#define AFP_FAKE_SERVER_PIECE 4096U
/* A write of one quantum and its command fit the largest request read. */
#define AFP_FAKE_SERVER_QUANTUM (AFP_FAKE_SERVER_MAX_REQUEST - 64U)
/* Waits for a connection or for bytes go in poll() slices, each looking
 * afresh: OpenBSD 8.0's accept() can miss a connection that completed, and
 * macOS's read() its socketpair peer's shutdown. */
#define AFP_FAKE_SERVER_SLICE_MS 50

/*!
 * @brief Read len bytes from a stream that may deliver them in pieces
 *
 * Each piece is waited for in poll() slices, so no read() sleeps.
 *
 * @param[in]  fd   the descriptor
 * @param[out] buf  receives the bytes
 * @param[in]  len  bytes to read
 * @returns 0, or -1 when the stream ends or fails first
 */
static int read_full(int fd, void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        ssize_t n;

        if (poll(&pfd, 1, AFP_FAKE_SERVER_SLICE_MS) <= 0) {
            continue;
        }

        n = read(fd, (char *) buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Write len bytes to a stream that may take them in pieces
 *
 * @param[in] fd   the descriptor
 * @param[in] buf  the bytes
 * @param[in] len  bytes to write
 * @returns 0, or -1 when a write fails
 */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, (const char *) buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Run the receiver on the client's end until it has read len more bytes
 *
 * It runs as the loop thread would.
 *
 * @param[in] fake  the fake server
 * @param[in] len   bytes the receiver is to read
 * @returns 0, or -1 when the receiver fails or the client's descriptor is
 *          gone
 */
static int deliver(const struct afp_fake_server *fake, size_t len)
{
    uint64_t end = fake->server->stats.rx_bytes + len;

    while (fake->server->stats.rx_bytes < end) {
        if (__atomic_load_n(&fake->server->fd, __ATOMIC_ACQUIRE) < 0
                || afpc_dsi_recv(fake->server) < 0) {
            return -1;
        }
    }

    return 0;
}

/*!
 * @brief Write a reply's bytes to the connection it answers
 *
 * The server's thread is the one reader of a socketpair's client end, so a
 * piece must fit the socket's buffer; a listener's client has its own loop.
 *
 * @param[in] fake  the fake server
 * @param[in] fd    the connection
 * @param[in] buf   the reply, DSI header first
 * @param[in] len   bytes of buf
 * @returns 0, or -1 when a write or the receiver fails
 */
static int send_reply(const struct afp_fake_server *fake, int fd,
                      const void *buf, size_t len)
{
    if (fake->listener >= 0) {
        return write_full(fd, buf, len);
    }

    for (size_t done = 0; done < len;) {
        size_t piece = len - done;

        if (piece > AFP_FAKE_SERVER_PIECE) {
            piece = AFP_FAKE_SERVER_PIECE;
        }

        if (write_full(fd, (const char *) buf + done, piece) < 0
                || deliver(fake, piece) < 0) {
            return -1;
        }

        done += piece;
    }

    return 0;
}

/*!
 * @brief Whether an AFP command names an open fork after its pad byte
 *
 * @param[in] command  the AFP command
 * @returns 1 for a fork's command, 0 for another
 */
static int names_a_fork(uint8_t command)
{
    switch (command) {
    case afpByteRangeLock:
    case afpCloseFork:
    case afpFlushFork:
    case afpGetForkParms:
    case afpRead:
    case afpSetForkParms:
    case afpWrite:
    case afpByteRangeLockExt:
    case afpReadExt:
    case afpWriteExt:
    case afpSyncFork:
        return 1;

    default:
        return 0;
    }
}

/*!
 * @brief Whether a read-only filesystem refuses a command
 *
 * afpd refuses a write open, a create, a delete and a directory's mode or
 * owner change there; the fake also refuses a length and a file's parameters.
 *
 * @param[in] command  the AFP command
 * @param[in] access   the access an FPOpenFork asks for
 * @returns 1 for a command refused, 0 for another
 */
static int refused_read_only(uint8_t command, uint16_t access)
{
    switch (command) {
    case afpOpenFork:
        return (access & AFP_OPENFORK_ALLOWWRITE) != 0;

    case afpCreateFile:
    case afpCreateDir:
    case afpDelete:
    case afpSetForkParms:
    case afpSetDirParms:
    case afpSetFileDirParms:
        return 1;

    default:
        return 0;
    }
}

/*!
 * @brief Append a 16-bit value in network order
 *
 * @param[out] p      where the value goes
 * @param[in]  value  the value
 * @returns the byte after the value
 */
static unsigned char *put16(unsigned char *p, uint16_t value)
{
    value = htons(value);
    memcpy(p, &value, sizeof(value));
    return p + 2;
}

/*!
 * @brief Append a 32-bit value in network order
 *
 * @param[out] p      where the value goes
 * @param[in]  value  the value
 * @returns the byte after the value
 */
static unsigned char *put32(unsigned char *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
    return p + 4;
}

/*!
 * @brief Append a Pascal string, its length byte first
 *
 * @param[out] p     where the string goes
 * @param[in]  text  the string, 255 bytes at most
 * @returns the byte after the string
 */
static unsigned char *put_pascal(unsigned char *p, const char *text)
{
    size_t len = strlen(text);
    *p++ = (unsigned char) len;
    memcpy(p, text, len);
    return p + len;
}

/*!
 * @brief Build the block of an FPGetSrvrInfo reply
 *
 * Every server writes the signature's offset whatever kSrvrSig says, and
 * afpd writes the other three offsets and their fields whatever the flags.
 *
 * @param[in]  fake   the fake server
 * @param[out] block  receives the block, 512 bytes at most
 * @returns the length of the block
 */
static size_t status_block(const struct afp_fake_server *fake,
                           unsigned char *block)
{
    const char *machine = fake->machine_type ? fake->machine_type
                          : "Netatalk4.2.3";
    const char *name = fake->server_name ? fake->server_name : "fake";
    int netatalk = fake->afpd_offsets
                   || afp_identify_machine_type(machine)
                   == AFPC_SERVER_TYPE_NETATALK;
    unsigned char *p = put_pascal(block + 10, name);
    unsigned char *offsets;

    if ((p - block) & 1) {
        *p++ = 0;
    }

    offsets = p;
    p += 2;

    if (netatalk || (fake->status_flags & kSupportsTCP)) {
        p += 2;
    }

    if (netatalk || (fake->status_flags & kSupportsDirServices)) {
        p += 2;
    }

    if (netatalk || (fake->status_flags & kSupportsUTF8SrvrName)) {
        p += 2;
    }

    put16(block, fake->bad_status ? 0xffff : (uint16_t)(p - block));
    p = put_pascal(p, machine);
    put16(block + 2, (uint16_t)(p - block));
    *p++ = 1;
    p = put_pascal(p, "AFP3.4");
    put16(block + 4, (uint16_t)(p - block));
    *p++ = 2;
    p = put_pascal(p, "No User Authent");
    p = put_pascal(p, "Cleartxt Passwrd");
    put16(block + 6, 0);
    put16(block + 8, fake->status_flags);
    offsets = put16(offsets, (uint16_t)(p - block));
    memcpy(p, AFP_FAKE_SERVER_SIGNATURE, AFP_SIGNATURE_LEN);
    p += AFP_SIGNATURE_LEN;

    if (netatalk || (fake->status_flags & kSupportsTCP)) {
        offsets = put16(offsets, (uint16_t)(p - block));
        *p++ = 0;
    }

    if (netatalk || (fake->status_flags & kSupportsDirServices)) {
        offsets = put16(offsets, (uint16_t)(p - block));
        *p++ = 0;
    }

    if (netatalk || (fake->status_flags & kSupportsUTF8SrvrName)) {
        put16(offsets, (uint16_t)(p - block));
        p = put16(p, (uint16_t) strlen(name));
        memcpy(p, name, strlen(name));
        p += strlen(name);
    }

    return (size_t)(p - block);
}

/*!
 * @brief Record the command, length and first bytes of the first 32 requests
 *
 * @param[in,out] fake     the fake server
 * @param[in]     payload  the request's AFP bytes
 * @param[in]     length   the request's length
 */
static void record(struct afp_fake_server *fake, const unsigned char *payload,
                   uint32_t length)
{
    pthread_mutex_lock(&fake->lock);

    if (fake->ncommands < sizeof(fake->commands)) {
        size_t kept = length < sizeof(fake->requests[0])
                      ? length : sizeof(fake->requests[0]);
        memcpy(fake->requests[fake->ncommands], payload, kept);
        fake->lengths[fake->ncommands] = length;
        fake->commands[fake->ncommands++] = payload[0];
    }

    pthread_mutex_unlock(&fake->lock);
}

/*!
 * @brief Answer a request by its AFP command
 *
 * @param[in,out] fake     the fake server, which counts the forks it opens
 * @param[in]     fd       the connection to answer on
 * @param[in]     request  the request's DSI header
 * @param[in]     payload  the request's AFP bytes
 * @param[in]     length   the length of payload
 * @returns 0, or -1 when the reply cannot be sent
 */
static int answer(struct afp_fake_server *fake, int fd,
                  const struct dsi_header *request,
                  const unsigned char *payload, uint32_t length)
{
    struct dsi_header header;
    unsigned char data[256];
    const void *body = data;
    uint32_t body_len = 0;
    int32_t result = kFPNoErr;
    uint64_t offset;
    uint64_t count;
    uint16_t value;
    uint16_t access = 0;
    uint16_t bitmap = 0;
    int refused = 0;

    if (payload[0] == afpOpenFork && length >= 12) {
        memcpy(&value, payload + 10, sizeof(value));
        access = ntohs(value);
    }

    if ((payload[0] == afpGetFileDirParms || payload[0] == afpSetFileDirParms
            || payload[0] == afpSetDirParms) && length >= 10) {
        memcpy(&value, payload + 8, sizeof(value));
        bitmap = ntohs(value);
    }

    if ((payload[0] == afpSetFileDirParms || payload[0] == afpSetDirParms)
            && length >= 10) {
        uint32_t id;
        fake->set_bitmap = bitmap;
        fake->set_bitmaps |= bitmap;

        /* The privileges are the highest bit's, so they end the request, as
         * a directory's owner or group does. */
        if ((bitmap & kFPUnixPrivsBit) && length >= 26) {
            memcpy(&id, payload + length - 16, sizeof(id));
            fake->set_uid = ntohl(id);
            memcpy(&id, payload + length - 12, sizeof(id));
            fake->set_gid = ntohl(id);
        } else if ((bitmap & (kFPOwnerIDBit | kFPGroupIDBit)) && length >= 14) {
            memcpy(&id, payload + length - 4, sizeof(id));

            if (!(bitmap & kFPGroupIDBit)) {
                fake->set_uid = ntohl(id);
            } else {
                fake->set_gid = ntohl(id);

                if ((bitmap & kFPOwnerIDBit) && length >= 18) {
                    memcpy(&id, payload + length - 8, sizeof(id));
                    fake->set_uid = ntohl(id);
                }
            }
        }
    }

    if (names_a_fork(payload[0]) && length >= 4) {
        memcpy(&value, payload + 2, sizeof(value));
    } else {
        value = 0;
    }

    if (payload[0] == fake->refuse) {
        refused = fake->refuse_skip == 0
                  || fake->refuse_seen == fake->refuse_skip;
        fake->refuse_seen++;
    }

    if (names_a_fork(payload[0])
            && (fake->forks_open == 0 || ntohs(value) != AFP_FAKE_SERVER_FORK)) {
        /* afpd refuses a fork it never opened, or has closed. */
        result = kFPParamErr;
    } else if (refused
               || (fake->refuse_privs && payload[0] == afpSetFileDirParms
                   && (bitmap & kFPUnixPrivsBit))) {
        result = fake->refusal ? fake->refusal : kFPAccessDenied;
    } else if (fake->refuse_unlock && payload[0] == afpByteRangeLockExt
               && length >= 2 && (payload[1] & ByteRangeLock_Unlock)) {
        result = kFPRangeNotLocked;
    } else if (fake->read_only && refused_read_only(payload[0], access)) {
        result = kFPVolLocked;
    } else if (payload[0] == afpCreateFile && fake->name_taken && length >= 2
               && !(payload[1] & kFPHardCreate)) {
        /* A soft create refuses a name taken; afpd's hard create empties
         * the file the name holds. */
        result = kFPObjectExists;
    } else if (payload[0] == afpGetFileDirParms && fake->file_params
               && length >= 12) {
        /* An object the session owns, of mode 0644 or 0755 unless params_mode
         * says, with the time, a file's fork lengths and the privileges. */
        const int is_dir = fake->params_dir
                           || (fake->params_answers < 16U
                               && (fake->params_dirs
                                   >> fake->params_answers) & 1U);
        const int is_empty = fake->params_answers < 16U
                             && (fake->params_empty >> fake->params_answers)
                             & 1U;
        uint32_t mode = is_dir ? 040755 : 0100644;
        uint32_t privs[4];
        const uint32_t cnid = htonl(fake->params_cnid);
        const uint16_t cnid_bit = fake->params_cnid ? kFPNodeIDBit : 0;
        const uint32_t data_length = is_empty ? 0 : fake->data_length;
        const uint32_t resource_length = is_empty ? 0 : fake->resource_length;
        const uint32_t data_length32 = htonl(data_length);
        const uint32_t resource_length32 = htonl(resource_length);
        const uint64_t data_length64 = hton64((uint64_t) data_length);
        const uint64_t resource_length64 = hton64((uint64_t) resource_length);

        if (fake->params_mode) {
            mode = fake->params_mode;
        }

        if (fake->echo_ids
                && (fake->set_bitmaps & (kFPUnixPrivsBit | kFPOwnerIDBit))) {
            privs[0] = htonl(fake->set_uid);
        } else {
            privs[0] = htonl(1000);
        }

        if (fake->echo_ids
                && (fake->set_bitmaps & (kFPUnixPrivsBit | kFPGroupIDBit))) {
            privs[1] = htonl(fake->set_gid);
        } else {
            privs[1] = htonl(1000);
        }

        privs[2] = htonl(mode);
        privs[3] = htonl(fake->params_access ? fake->params_access
                         : kUserIsOwner);
        fake->params_answers++;

        if (is_dir) {
            memcpy(&value, payload + 10, sizeof(value));
            bitmap = ntohs(value) & (kFPModDateBit | cnid_bit
                                     | kFPUnixPrivsBit);
        } else {
            bitmap &= kFPAttributeBit | kFPModDateBit | cnid_bit
                      | kFPDataForkLenBit | kFPRsrcForkLenBit
                      | kFPExtDataForkLenBit | kFPExtRsrcForkLenBit
                      | kFPUnixPrivsBit;
        }

        value = htons(bitmap);
        memset(data, 0, 6);
        memcpy(data + (is_dir ? 2 : 0), &value, sizeof(value));
        data[4] = is_dir ? 0x80 : 0;
        body_len = 6;

        /* A file of no attributes. */
        if (bitmap & kFPAttributeBit) {
            memset(data + body_len, 0, 2);
            body_len += 2;
        }

        if (bitmap & kFPModDateBit) {
            memcpy(data + body_len, &fake->params_date, 4);
            body_len += 4;
        }

        if (bitmap & kFPNodeIDBit) {
            memcpy(data + body_len, &cnid, sizeof(cnid));
            body_len += sizeof(cnid);
        }

        if (bitmap & kFPDataForkLenBit) {
            memcpy(data + body_len, &data_length32, sizeof(data_length32));
            body_len += sizeof(data_length32);
        }

        if (bitmap & kFPRsrcForkLenBit) {
            memcpy(data + body_len, &resource_length32,
                   sizeof(resource_length32));
            body_len += sizeof(resource_length32);
        }

        if (bitmap & kFPExtDataForkLenBit) {
            memcpy(data + body_len, &data_length64, sizeof(data_length64));
            body_len += sizeof(data_length64);
        }

        if (bitmap & kFPExtRsrcForkLenBit) {
            memcpy(data + body_len, &resource_length64,
                   sizeof(resource_length64));
            body_len += sizeof(resource_length64);
        }

        if (bitmap & kFPUnixPrivsBit) {
            memcpy(data + body_len, privs, sizeof(privs));
            body_len += sizeof(privs);
        }
    } else if (payload[0] == afpAddComment && length >= 15) {
        /* The comment's length and bytes follow the UTF-8 path, from an even
         * byte. */
        size_t at;
        memcpy(&value, payload + 13, sizeof(value));
        at = 15U + ntohs(value);
        at += at & 1U;

        if (at < length) {
            size_t len = payload[at];
            size_t keep = fake->whole_comment ? sizeof(fake->comment) : 199U;

            if (len > length - at - 1U) {
                len = length - at - 1U;
            }

            if (len > keep) {
                len = keep;
            }

            memcpy(fake->comment, payload + at + 1U, len);
            fake->comment_len = (uint8_t) len;
        }
    } else if (payload[0] == afpGetComment) {
        if (fake->comment_len == 0) {
            result = kFPItemNotFound;
        } else {
            data[0] = fake->whole_comment || fake->comment_len < 128
                      ? fake->comment_len : 128;
            memcpy(data + 1, fake->comment, data[0]);
            body_len = 1U + data[0];
        }
    } else if (payload[0] == afpGetExtAttr && fake->xattr_value) {
        /* No bitmap, then the value's length and the value. */
        const uint32_t vlen = (uint32_t) strlen(fake->xattr_value);
        const uint32_t wire_len = htonl(vlen);
        memset(data, 0, 2);
        memcpy(data + 2, &wire_len, sizeof(wire_len));
        memcpy(data + 6, fake->xattr_value, vlen);
        body_len = 6U + vlen;
    } else if (payload[0] == afpCreateDir) {
        /* The new directory's ID */
        const uint32_t did = htonl(17);
        memcpy(data, &did, sizeof(did));
        body_len = sizeof(did);
    } else if (payload[0] == afpOpenFork) {
        /* The fork number, then the node ID and the data fork's length
         * where the open asks for them. */
        uint16_t asked = 0;

        if (length >= 10) {
            memcpy(&value, payload + 8, sizeof(value));
            asked = ntohs(value) & (kFPNodeIDBit | kFPExtDataForkLenBit);
        }

        value = htons(asked);
        memcpy(data, &value, sizeof(value));
        value = htons(AFP_FAKE_SERVER_FORK);
        memcpy(data + 2, &value, sizeof(value));
        body_len = 4;

        if (asked & kFPNodeIDBit) {
            const uint32_t cnid = htonl(fake->params_cnid);
            memcpy(data + body_len, &cnid, sizeof(cnid));
            body_len += sizeof(cnid);
        }

        if (asked & kFPExtDataForkLenBit) {
            const uint64_t fork_len = hton64((uint64_t) fake->data_length);
            memcpy(data + body_len, &fork_len, sizeof(fork_len));
            body_len += sizeof(fork_len);
        }

        fake->forks_open++;
    } else if (payload[0] == afpCloseFork) {
        fake->forks_open--;
    } else if ((payload[0] == afpReadExt || payload[0] == afpWriteExt)
               && length >= 20) {
        memcpy(&offset, payload + 4, sizeof(offset));
        memcpy(&count, payload + 12, sizeof(count));
        offset = ntoh64(offset);
        count = ntoh64(count);

        if (payload[0] == afpWriteExt) {
            /* The end of the write; an append's counts from the fork's end,
             * which a write past it moves. */
            if (payload[1] & AFP_WRITE_FROM_END) {
                offset += fake->data_length;
            }

            if (offset + count > fake->data_length) {
                fake->data_length = (uint32_t)(offset + count);
            }

            offset = hton64(offset + count);
            memcpy(data, &offset, sizeof(offset));
            body_len = 8;
        } else if (fake->read_file) {
            /* A file of data_length zeros, which a read gets in any count,
             * short with kFPEOFErr at its end. */
            static const char zeros[65536];

            if (offset > fake->data_length) {
                offset = fake->data_length;
            }

            if (count > sizeof(zeros)) {
                count = sizeof(zeros);
            }

            if (count > fake->data_length - offset) {
                count = fake->data_length - offset;
                result = kFPEOFErr;
            }

            body = zeros;
            body_len = (uint32_t) count;
        } else {
            /* A link's target, which afpd reads whole: a read that asks
             * for some of it but less than the rest is refused, and one
             * that asks for more returns the rest with kFPEOFErr. */
            size_t size = fake->read_data ? strlen(fake->read_data) : 0;

            if (offset > size) {
                offset = size;
            }

            if (count != 0 && count < size - offset) {
                count = 0;
                result = kFPParamErr;
            } else if (count > size - offset) {
                count = size - offset;
                result = kFPEOFErr;
            }

            if (fake->read_data) {
                body = fake->read_data + offset;
            }

            body_len = (uint32_t) count;
        }
    } else if (payload[0] == afpGetSrvrParms) {
        /* The server's time and one volume. */
        unsigned char *p = put32(data, 0);
        *p++ = 1;
        *p++ = 0;
        p = put_pascal(p, "vol");
        body_len = (uint32_t)(p - data);
    } else if (payload[0] == afpGetSrvrMsg) {
        /* The message's type and bitmap, then an empty message. */
        unsigned char *p = put16(put16(data, 0), 0);
        *p++ = 0;
        body_len = (uint32_t)(p - data);
    } else if (payload[0] == afpOpenVol && length >= 4) {
        unsigned char *p;
        uint16_t asked;
        memcpy(&asked, payload + 2, sizeof(asked));
        asked = ntohs(asked);
        p = put16(data, asked);

        if (asked & kFPVolAttributeBit) {
            p = put16(p, kSupportsUTF8Names);
        }

        if (asked & kFPVolSignatureBit) {
            p = put16(p, AFP_VOL_FIXED);
        }

        if (asked & kFPVolCreateDateBit) {
            p = put32(p, 0);
        }

        if (asked & kFPVolIDBit) {
            p = put16(p, 1);
        }

        if (asked & kFPVolNameBit) {
            p = put16(p, 0);
        }

        if (asked & kFPVolBlockSizeBit) {
            p = put32(p, 4096);
        }

        body_len = (uint32_t)(p - data);
    } else if (payload[0] == afpGetSessionToken) {
        unsigned char *p = put32(data, sizeof(AFP_FAKE_SERVER_TOKEN) - 1);
        memcpy(p, AFP_FAKE_SERVER_TOKEN, sizeof(AFP_FAKE_SERVER_TOKEN) - 1);
        body_len = 4 + sizeof(AFP_FAKE_SERVER_TOKEN) - 1;
    } else if (payload[0] == afpDisconnectOldSession) {
        /* The request holds the command, a pad byte, the type, the token's
         * length and the token. */
        uint32_t token_len = 0;

        if (length >= 8) {
            memcpy(&token_len, payload + 4, sizeof(token_len));
            token_len = ntohl(token_len);
        }

        if (token_len != sizeof(AFP_FAKE_SERVER_TOKEN) - 1
                || length < 8 + token_len
                || memcmp(payload + 8, AFP_FAKE_SERVER_TOKEN,
                          token_len) != 0) {
            result = kFPMiscErr;
        }
    }

    memset(&header, 0, sizeof(header));
    header.flags = DSI_REPLY;
    header.command = request->command;
    header.requestid = request->requestid;
    header.return_code.error_code = (int) htonl((uint32_t) result);
    header.length = htonl(body_len);

    if (send_reply(fake, fd, &header, sizeof(header)) < 0) {
        return -1;
    }

    return body_len ? send_reply(fake, fd, body, body_len) : 0;
}

/*!
 * @brief Refuse a session as afpd at its connection cap refuses it
 *
 * afpd sends DSIERR_SERVBUSY zero-extended, 0x0000fbd1, not as -1071.
 *
 * @param[in] fake       the fake server
 * @param[in] fd         the connection
 * @param[in] requestid  the request's ID as it came, or 0 for none
 */
static void send_busy(const struct afp_fake_server *fake, int fd,
                      uint16_t requestid)
{
    struct dsi_header header;
    uint32_t code = htonl(0xfbd1U);
    memset(&header, 0, sizeof(header));
    header.flags = DSI_REPLY;
    header.command = DSI_DSIOpenSession;
    header.requestid = requestid;
    memcpy(&header.return_code, &code, sizeof(code));
    send_reply(fake, fd, &header, sizeof(header));
}

/*!
 * @brief Answer DSIGetStatus and DSIOpenSession; a tickle gets no answer
 *
 * @param[in,out] fake     the fake server
 * @param[in]     fd       the connection to answer on
 * @param[in]     request  the request's DSI header
 * @returns 0, or -1 when the reply cannot be sent or the session is
 *          refused
 */
static int answer_dsi(struct afp_fake_server *fake, int fd,
                      const struct dsi_header *request)
{
    struct dsi_header header;
    unsigned char body[512];
    size_t body_len = 0;

    /* afpd over its cap answers busy a connection's first request, a
     * status's too, when it arrives before the refusal. */
    if (fake->busy == AFP_FAKE_SERVER_BUSY_ALWAYS) {
        pthread_mutex_lock(&fake->lock);
        fake->refused++;
        pthread_mutex_unlock(&fake->lock);
        send_busy(fake, fd, request->requestid);
        return -1;
    }

    if (request->command == DSI_DSIGetStatus) {
        struct timespec millisecond = { 0, 1000000 };
        unsigned int accepted = 0;

        for (int waited = 0; waited <= fake->hold_status_ms; waited++) {
            pthread_mutex_lock(&fake->lock);
            accepted = fake->accepted + fake->refused;
            pthread_mutex_unlock(&fake->lock);

            if (accepted >= 2 || waited == fake->hold_status_ms) {
                break;
            }

            nanosleep(&millisecond, NULL);
        }

        pthread_mutex_lock(&fake->lock);
        fake->status_sent_after = accepted;
        pthread_mutex_unlock(&fake->lock);
        body_len = status_block(fake, body);
    } else if (request->command == DSI_DSIOpenSession) {
        int refuse = 0;
        pthread_mutex_lock(&fake->lock);

        if (fake->busy == AFP_FAKE_SERVER_BUSY_AT_OPEN && fake->refused == 0) {
            fake->refused = 1;
            refuse = 1;
        }

        pthread_mutex_unlock(&fake->lock);

        if (refuse) {
            send_busy(fake, fd, request->requestid);
            return -1;
        }

        body[0] = 0;
        body[1] = 4;
        put32(body + 2, AFP_FAKE_SERVER_QUANTUM);
        body_len = 6;

        /* afpd announces its replay cache, of 128 requests. */
        if (fake->replay_cache) {
            body[6] = kServerReplayCacheSize;
            body[7] = 4;
            put32(body + 8, 128);
            body_len = 12;
        }
    } else {
        return 0;
    }

    memset(&header, 0, sizeof(header));
    header.flags = DSI_REPLY;
    header.command = request->command;
    header.requestid = request->requestid;
    header.length = htonl((uint32_t) body_len);

    if (send_reply(fake, fd, &header, sizeof(header)) < 0) {
        return -1;
    }

    return send_reply(fake, fd, body, body_len);
}

/*!
 * @brief Hold a request's answer until the test releases it
 *
 * @param[in,out] fake  the fake server
 */
static void hold_until_released(struct afp_fake_server *fake)
{
    pthread_mutex_lock(&fake->hold_mutex);
    fake->held = 1;
    pthread_cond_broadcast(&fake->hold_cond);

    while (!fake->released) {
        pthread_cond_wait(&fake->hold_cond, &fake->hold_mutex);
    }

    pthread_mutex_unlock(&fake->hold_mutex);
}

/*!
 * @brief Wait until the held request has arrived, for five seconds at most
 *
 * @param[in,out] fake  the fake server
 * @returns 0 once it has, -1 when it did not arrive
 */
int afp_fake_server_wait_held(struct afp_fake_server *fake)
{
    struct timespec deadline;
    int waited = 0;
    int held;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&fake->hold_mutex);

    while (!fake->held && waited == 0) {
        waited = pthread_cond_timedwait(&fake->hold_cond, &fake->hold_mutex,
                                        &deadline);
    }

    held = fake->held;
    pthread_mutex_unlock(&fake->hold_mutex);
    return held ? 0 : -1;
}

/*!
 * @brief Let the held request's answer go
 *
 * @param[in,out] fake  the fake server
 */
void afp_fake_server_release(struct afp_fake_server *fake)
{
    pthread_mutex_lock(&fake->hold_mutex);
    fake->released = 1;
    pthread_cond_broadcast(&fake->hold_cond);
    pthread_mutex_unlock(&fake->hold_mutex);
}

/*!
 * @brief Give the access mode of the last open the fake server received
 *
 * @param[in] fake  the fake server, stopped
 * @returns the access mode, or 0 when it received no open
 */
unsigned int afp_fake_server_open_access(const struct afp_fake_server *fake)
{
    unsigned int i = fake->ncommands;

    while (i > 0) {
        i--;

        if (fake->commands[i] == afpOpenFork && fake->lengths[i] >= 12) {
            return (unsigned int) fake->requests[i][10] << 8
                   | fake->requests[i][11];
        }
    }

    return 0;
}

/*!
 * @brief Answer the requests of one connection until it ends
 *
 * Each AFP request gets the answer fake's fields ask for, or kFPNoErr alone;
 * on a socketpair the receiver runs on each reply, as the loop would. As
 * afpd does, the connection ends at an AFP request before DSIOpenSession.
 *
 * @param[in,out] fake    the fake server
 * @param[in]     fd      the connection
 * @param[in]     opened  whether its session is open from the start
 */
static void serve_connection(struct afp_fake_server *fake, int fd, int opened)
{
    struct dsi_header header;
    /* A bit for each request ID the connection has carried; afpd's replay
     * cache spans the session's connections, and holds AFP requests alone. */
    unsigned char *own = fake->replay_cache ? NULL : calloc(65536 / 8, 1);
    unsigned char *seen = fake->replay_cache ? fake->session_ids : own;

    while (read_full(fd, &header, sizeof(header)) == 0) {
        uint32_t length = ntohl(header.length);
        uint16_t id = ntohs(header.requestid);
        unsigned char *payload;
        int rc;

        if (header.flags == DSI_REQUEST && seen
                && (own || header.command == DSI_DSICommand
                    || header.command == DSI_DSIWrite)) {
            pthread_mutex_lock(&fake->lock);

            if (seen[id / 8] & (1U << (id % 8))) {
                fake->repeated_ids++;
            }

            seen[id / 8] |= (unsigned char)(1U << (id % 8));
            pthread_mutex_unlock(&fake->lock);
        }

        if (length > AFP_FAKE_SERVER_MAX_REQUEST) {
            break;
        }

        payload = malloc(length ? length : 1);

        if (!payload || (length && read_full(fd, payload, length) < 0)) {
            free(payload);
            break;
        }

        if (header.command != DSI_DSICommand
                && header.command != DSI_DSIWrite) {
            free(payload);
            opened |= header.command == DSI_DSIOpenSession;

            if (header.flags == DSI_REQUEST
                    && answer_dsi(fake, fd, &header) < 0) {
                break;
            }

            continue;
        }

        if (length == 0 || !opened) {
            free(payload);
            break;
        }

        record(fake, payload, length);

        if (payload[0] == fake->inspect) {
            pthread_mutex_lock(&fake->volume.open_forks_mutex);

            if (fake->volume.open_forks) {
                memcpy(&fake->listed, fake->volume.open_forks,
                       sizeof(fake->listed));
                fake->inspected = 1;
            }

            pthread_mutex_unlock(&fake->volume.open_forks_mutex);
        }

        if (fake->hold && payload[0] == fake->hold) {
            if (fake->hold_skip) {
                fake->hold_skip--;
            } else {
                hold_until_released(fake);
            }
        }

        rc = answer(fake, fd, &header, payload, length);
        free(payload);

        if (rc < 0) {
            break;
        }
    }

    free(own);
}

/*!
 * @brief Serve the socketpair's one connection, then close the server's end
 *
 * The close fails a later request at once, where it would wait unread.
 *
 * @param[in,out] arg  the fake server
 * @returns NULL
 */
static void *serve(void *arg)
{
    struct afp_fake_server *fake = arg;
    serve_connection(fake, fake->peer, 1);
    close(fake->peer);
    fake->peer = -1;
    return NULL;
}

/*!
 * @brief A connection the listener accepted, for its thread
 */
struct accepted_connection {
    struct afp_fake_server *fake;  /*!< the fake server */
    int fd;                        /*!< the connection */
};

/*!
 * @brief Serve one accepted connection, then end it and free its record
 *
 * The shutdown fails a later request at once; the descriptor stays open
 * until the listener stops, so its number is not reused while listed.
 *
 * @param[in] arg  the connection's record
 * @returns NULL
 */
static void *serve_accepted(void *arg)
{
    struct accepted_connection *connection = arg;
    serve_connection(connection->fake, connection->fd, 0);
    shutdown(connection->fd, SHUT_RDWR);
    free(connection);
    return NULL;
}

/*!
 * @brief Give each accepted connection a thread until told to stop
 *
 * The listener does not block, and the stop ends the thread within a slice.
 *
 * @param[in,out] arg  the fake server
 * @returns NULL
 */
static void *accept_connections(void *arg)
{
    struct afp_fake_server *fake = arg;

    for (;;) {
        struct accepted_connection *connection;
        struct pollfd pfd = { fake->listener, POLLIN, 0 };
        int stopping;
        int fd;
        pthread_mutex_lock(&fake->lock);
        stopping = fake->stopping;
        pthread_mutex_unlock(&fake->lock);

        if (stopping) {
            return NULL;
        }

        if (poll(&pfd, 1, AFP_FAKE_SERVER_SLICE_MS) <= 0) {
            continue;
        }

        fd = accept(fake->listener, NULL, NULL);

        /* A connection its client ended before the accept is gone. */
        if (fd < 0 && (errno == EAGAIN || errno == EINTR
                       || errno == ECONNABORTED || errno == EPROTO)) {
            continue;
        }

        if (fd < 0) {
            return NULL;
        }

        /* The BSDs give the connection the listener's O_NONBLOCK. */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        pthread_mutex_lock(&fake->lock);

        /* The refusal counts once it is sent, so a status that waits for a
         * second connection goes after it. */
        if (fake->busy == AFP_FAKE_SERVER_BUSY_AT_ACCEPT && !fake->stopping
                && fake->accepted + fake->refused == 1) {
            pthread_mutex_unlock(&fake->lock);
            send_busy(fake, fd, 0);
            close(fd);
            pthread_mutex_lock(&fake->lock);
            fake->refused++;
            pthread_mutex_unlock(&fake->lock);
            continue;
        }

        if (fake->stopping
                || fake->accepted == AFP_FAKE_SERVER_MAX_CONNECTIONS
                || (connection = malloc(sizeof(*connection))) == NULL) {
            stopping = fake->stopping;
            pthread_mutex_unlock(&fake->lock);
            close(fd);

            if (stopping) {
                return NULL;
            }

            continue;
        }

        connection->fake = fake;
        connection->fd = fd;
        fake->connections[fake->accepted] = fd;

        if (pthread_create(&fake->connection_threads[fake->accepted], NULL,
                           serve_accepted, connection) != 0) {
            free(connection);
            close(fd);
        } else {
            fake->accepted++;
        }

        pthread_mutex_unlock(&fake->lock);
    }
}

/*!
 * @brief Close the session's ends and release its server and the volume's
 *        locks
 *
 * @param[in,out] fake  the fake server
 */
static void release(struct afp_fake_server *fake)
{
    if (fake->peer >= 0) {
        close(fake->peer);
    }

    close(fake->server->fd);
    fake->server->fd = -1;
    afp_server_release(fake->server);
    fake->server = NULL;
    fake->volume.server = NULL;
    pthread_rwlock_destroy(&fake->volume.open_lock);
    pthread_mutex_destroy(&fake->volume.open_forks_mutex);
    pthread_mutex_destroy(&fake->hold_mutex);
    pthread_cond_destroy(&fake->hold_cond);
    dir_cache_destroy(&fake->volume);
}

/*!
 * @brief Open a session on the fake server and start the server's thread
 *
 * The server gets AFP 3.4 and the volume its server, locks, UTF-8 names and a
 * fixed signature; the caller zeroes the fake first and sets its other fields.
 *
 * @param[in,out] fake  the fake server
 * @returns 0, or -1 when the session cannot be made
 */
int afp_fake_server_start(struct afp_fake_server *fake)
{
    static struct afp_versions afp34 = { "AFP3.4", 34 };
    int fds[2];
    fake->listener = -1;
    pthread_mutex_init(&fake->lock, NULL);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return -1;
    }

    fake->server = afp_server_init(NULL);

    if (!fake->server) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    fake->server->using_version = &afp34;
    fake->server->fd = fds[0];
    fake->server->connect_state = SERVER_STATE_CONNECTED;
    fake->peer = fds[1];
    dir_cache_init(&fake->volume);
    pthread_mutex_init(&fake->volume.open_forks_mutex, NULL);
    pthread_mutex_init(&fake->hold_mutex, NULL);
    pthread_cond_init(&fake->hold_cond, NULL);
    afp_open_lock_init(&fake->volume.open_lock);
    fake->volume.server = fake->server;
    fake->volume.path_encoding = kFPUTF8Name;
    fake->volume.signature = AFP_VOL_FIXED;

    if (pthread_create(&fake->thread, NULL, serve, fake) != 0) {
        release(fake);
        return -1;
    }

    return 0;
}

/*!
 * @brief End the client's requests, wait for the server's thread, and release
 *        the session
 *
 * The server's thread reads the requests' end and returns; the volume's
 * open-fork list stays as the client left it.
 *
 * @param[in,out] fake  the fake server
 */
void afp_fake_server_stop(struct afp_fake_server *fake)
{
    shutdown(fake->server->fd, SHUT_WR);
    pthread_join(fake->thread, NULL);
    release(fake);
    pthread_mutex_destroy(&fake->lock);
}

/*!
 * @brief Listen on the loopback address and serve every connection to it
 *
 * The caller zeroes the fake, then sets the fields that shape its answers.
 *
 * @param[in,out] fake  the fake server, whose listen_addr receives the port
 * @returns 0, or -1 when the listener cannot be made
 */
int afp_fake_server_listen(struct afp_fake_server *fake)
{
    socklen_t len = sizeof(fake->listen_addr);
    fake->peer = -1;
    pthread_mutex_init(&fake->lock, NULL);
    pthread_mutex_init(&fake->hold_mutex, NULL);
    pthread_cond_init(&fake->hold_cond, NULL);
    memset(&fake->listen_addr, 0, sizeof(fake->listen_addr));
    fake->listen_addr.sin_family = AF_INET;
    fake->listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fake->listener = socket(AF_INET, SOCK_STREAM, 0);

    if (fake->listener < 0
            || fcntl(fake->listener, F_SETFL, O_NONBLOCK) != 0
            || bind(fake->listener, (struct sockaddr *) &fake->listen_addr,
                    sizeof(fake->listen_addr)) != 0
            || getsockname(fake->listener,
                           (struct sockaddr *) &fake->listen_addr, &len) != 0
            || listen(fake->listener, AFP_FAKE_SERVER_MAX_CONNECTIONS) != 0
            || pthread_create(&fake->accept_thread, NULL, accept_connections,
                              fake) != 0) {
        if (fake->listener >= 0) {
            close(fake->listener);
        }

        fake->listener = -1;
        return -1;
    }

    return 0;
}

/*!
 * @brief End the last accepted connection, a session's, from the server's side
 *
 * @param[in,out] fake  the fake server
 */
void afp_fake_server_drop(struct afp_fake_server *fake)
{
    pthread_mutex_lock(&fake->lock);

    if (fake->accepted > 0) {
        shutdown(fake->connections[fake->accepted - 1], SHUT_RDWR);
    }

    pthread_mutex_unlock(&fake->lock);
}

/*!
 * @brief Stop accepting, end every accepted connection and join the threads
 *
 * @param[in,out] fake  the fake server
 */
void afp_fake_server_stop_listening(struct afp_fake_server *fake)
{
    pthread_mutex_lock(&fake->lock);
    fake->stopping = 1;
    pthread_mutex_unlock(&fake->lock);
    pthread_join(fake->accept_thread, NULL);
    close(fake->listener);

    /* send_reply() reads listener, so it stays set until the threads end. */
    for (unsigned int i = 0; i < fake->accepted; i++) {
        shutdown(fake->connections[i], SHUT_RDWR);
        pthread_join(fake->connection_threads[i], NULL);
        close(fake->connections[i]);
    }

    fake->listener = -1;
    pthread_mutex_destroy(&fake->lock);
    pthread_mutex_destroy(&fake->hold_mutex);
    pthread_cond_destroy(&fake->hold_cond);
}

/*!
 * @brief Count the requests of one command, of the first 32 received
 *
 * @param[in] fake     the fake server
 * @param[in] command  the AFP command
 * @returns the count
 */
unsigned int afp_fake_server_count(struct afp_fake_server *fake,
                                   uint8_t command)
{
    unsigned int count = 0;
    pthread_mutex_lock(&fake->lock);

    for (unsigned int i = 0; i < fake->ncommands; i++) {
        if (fake->commands[i] == command) {
            count++;
        }
    }

    pthread_mutex_unlock(&fake->lock);
    return count;
}

/*!
 * @brief Fill the stack below the caller's frame with a pattern
 *
 * A record the function the caller calls next declares and leaves
 * uninitialized then holds the pattern, not zeroes. It lives outside the
 * caller's file, so no build without link-time optimization inlines it into
 * the caller's frame.
 */
void afp_fake_server_dirty_stack(void)
{
    unsigned char area[65536];
    volatile unsigned char *p = area;

    for (size_t i = 0; i < sizeof(area); i++) {
        p[i] = 0xa5;
    }
}
