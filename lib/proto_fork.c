/*
 *  proto_fork.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

/*!
 * @file
 * @brief AFP fork requests and the parsing of their replies
 */

#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "afp_replies.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

/*!
 * @brief Set an open fork's length with FPSetForkParms
 *
 * With an extended length bit in bitmap, the request carries a 64-bit
 * length. Otherwise it carries the low 32 bits.
 *
 * @param[in] volume  volume of the fork
 * @param[in] forkid  the open fork
 * @param[in] bitmap  the fork's length bit
 * @param[in] len     the new length
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
int afp_setforkparms(struct afp_volume * volume,
                     unsigned short forkid, unsigned short bitmap, uint64_t len)
{
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);

    if (bitmap & (kFPExtDataForkLenBit | kFPExtRsrcForkLenBit)) {
        /* Extended 64-bit version */
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t forkid;
            uint16_t bitmap;
            uint64_t newlen64;
        }  __attribute__((__packed__)) request64;
        _Static_assert(sizeof(request64) == sizeof(struct dsi_header) + 14,
                       "invalid 64-bit FPSetForkParms request layout");
        memcpy(&request64.dsi_header, &hdr, sizeof(struct dsi_header));
        request64.command = afpSetForkParms;
        request64.pad = 0;
        request64.forkid = htons(forkid);
        request64.bitmap = htons(bitmap);
        request64.newlen64 = hton64(len);
        return afpc_dsi_send(volume->server, (char *) &request64,
                             sizeof(request64), DSI_DEFAULT_TIMEOUT, afpSetForkParms, NULL);
    } else {
        /* Legacy 32-bit version */
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t forkid;
            uint16_t bitmap;
            uint32_t newlen;
        }  __attribute__((__packed__)) request32;
        _Static_assert(sizeof(request32) == sizeof(struct dsi_header) + 10,
                       "invalid 32-bit FPSetForkParms request layout");
        memcpy(&request32.dsi_header, &hdr, sizeof(struct dsi_header));
        request32.command = afpSetForkParms;
        request32.pad = 0;
        request32.forkid = htons(forkid);
        request32.bitmap = htons(bitmap);
        request32.newlen = htonl((uint32_t) len);
        return afpc_dsi_send(volume->server, (char *) &request32,
                             sizeof(request32), DSI_DEFAULT_TIMEOUT, afpSetForkParms, NULL);
    }
}

/*!
 * @brief Send a fork command whose only parameter is the fork
 *
 * @param[in] volume   volume of the fork
 * @param[in] command  FPCloseFork, FPFlushFork, or FPSyncFork
 * @param[in] forkid   the open fork
 * @param[in] wait     seconds to wait for the reply, as afpc_dsi_send() takes
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
static int afp_fork_command(struct afp_volume * volume, unsigned char command,
                            unsigned short forkid, int wait)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t forkid;
    }  __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = command;
    request_packet.pad = 0;
    request_packet.forkid = htons(forkid);
    return afpc_dsi_send(volume->server, (char *) &request_packet,
                         sizeof(request_packet), wait, command, NULL);
}

/*!
 * @brief Close an open fork with FPCloseFork
 *
 * @param[in] volume  volume of the fork
 * @param[in] forkid  the open fork
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
int afp_closefork(struct afp_volume * volume,
                  unsigned short forkid)
{
    return afp_fork_command(volume, afpCloseFork, forkid, DSI_DEFAULT_TIMEOUT);
}


/*!
 * @brief Flush an open fork with FPFlushFork
 *
 * afpd answers with success whatever its fsync(2) of the fork returns.
 *
 * @param[in] volume  volume of the fork
 * @param[in] forkid  the open fork
 * @param[in] wait    seconds to wait for the reply
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
int afp_flushfork(struct afp_volume * volume,
                  unsigned short forkid, int wait)
{
    return afp_fork_command(volume, afpFlushFork, forkid, wait);
}

/*!
 * @brief Flush an open fork with FPSyncFork
 *
 * afpd reports a failed fsync(2) of the fork in the result.
 *
 * @param[in] volume  volume of the fork
 * @param[in] forkid  the open fork
 * @param[in] wait    seconds to wait for the reply
 * @returns the server's AFP result, or afpc_dsi_send()'s error when the
 *          request fails
 */
int afp_syncfork(struct afp_volume * volume, unsigned short forkid, int wait)
{
    return afp_fork_command(volume, afpSyncFork, forkid, wait);
}

/*!
 * @brief Parse an FPOpenFork reply into the open fork's record
 *
 * Takes the fork number and the requested node ID and data fork length from
 * a successful reply.
 *
 * @param[in]     server  server of the session
 * @param[in]     buf     the reply, DSI header first
 * @param[in]     size    bytes of buf
 * @param[in,out] x       the fork's struct afp_file_info
 * @returns 0, or -1 for a reply too short for the fork number
 */
int afp_openfork_reply(struct afp_server *server,
                       char *buf, unsigned int size, void *x)
{
    struct afp_file_info *fp = (struct afp_file_info *)x;
    struct afp_file_info params;
    unsigned short bitmap;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint16_t forkid;
    } __attribute__((__packed__)) reply;
    /* Copy the buffer into our properly structured reply */
    memcpy(&reply, buf, sizeof(reply));

    if ((reply.header.return_code.error_code == kFPNoErr) ||
            (reply.header.return_code.error_code == kFPDenyConflict)) {
        if (size < sizeof(reply)) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "openfork response is too short (%u bytes)", size);
            return -1;
        }

        fp->forkid = ntohs(reply.forkid);
        bitmap = ntohs(reply.bitmap);

        if (bitmap && parse_reply_block(server, buf + sizeof(reply),
                                        size - sizeof(reply), 0, bitmap, 0,
                                        &params) == 0) {
            if (bitmap & kFPNodeIDBit) {
                fp->fileid = params.fileid;
            }

            if (bitmap & kFPExtDataForkLenBit) {
                fp->size = params.size;
            }
        }
    }

    return 0;
}

/*!
 * @brief Open a data or resource fork with FPOpenFork
 *
 * @param[in]     volume      volume of the file
 * @param[in]     forktype    1 for the resource fork, 0 for the data fork
 * @param[in]     dirid       directory of the file
 * @param[in]     accessmode  AFP_OPENFORK_ALLOWREAD and AFP_OPENFORK_ALLOWWRITE
 *                            bits
 * @param[in]     bitmap      file parameters to read into fp, the node ID or
 *                            the data fork length
 * @param[in]     filename    AFP name of the file in dirid
 * @param[in,out] fp          receives the fork number and those parameters
 * @returns the server's AFP result, afpc_dsi_send()'s error when the request
 *          fails, or -1 when it cannot be built
 */
int afp_openfork(struct afp_volume * volume,
                 unsigned char forktype,
                 unsigned int dirid,
                 unsigned short accessmode,
                 unsigned short bitmap,
                 char *filename,
                 struct afp_file_info * fp)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t forktype;
        uint16_t volid;
        uint32_t dirid;
        uint16_t bitmap;
        uint16_t accessmode;
    }  __attribute__((__packed__)) * afp_openfork_request;
    char *msg;
    char *pathptr;
    struct afp_server * server = volume->server;
    unsigned int len = sizeof(*afp_openfork_request) +
                       sizeof_path_header(volume) + strlen(filename);
    int ret;

    if ((msg = malloc(len)) == NULL) {
        return -1;
    }

    pathptr = msg + sizeof(*afp_openfork_request);
    afp_openfork_request = (void *) msg;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&afp_openfork_request->dsi_header, &hdr, sizeof(struct dsi_header));
    afp_openfork_request->command = afpOpenFork;
    afp_openfork_request->forktype = forktype ? AFP_FORKTYPE_RESOURCE :
                                     AFP_FORKTYPE_DATA;
    afp_openfork_request->bitmap = htons(bitmap);
    afp_openfork_request->volid = htons(volume->volid);
    afp_openfork_request->dirid = htonl(dirid);
    afp_openfork_request->accessmode = htons(accessmode);
    copy_path(volume, pathptr, filename, strlen(filename));
    unixpath_to_afppath(volume, pathptr);
    ret = afpc_dsi_send(server, (char *) msg, len, DSI_DEFAULT_TIMEOUT,
                        afpOpenFork, (void *) fp);
    free(msg);
    return ret;
}

int afp_byterangelock(struct afp_volume * volume,
                      unsigned char flag,
                      unsigned short forkid,
                      uint32_t offset,
                      uint32_t len, uint32_t *generated_offset)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t flag;
        uint16_t forkid;
        uint32_t offset;
        uint32_t len;
    }  __attribute__((__packed__)) request;
    int rc;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpByteRangeLock;
    request.flag = flag;
    request.forkid = htons(forkid);
    request.offset = htonl(offset);
    request.len = htonl(len);
    rc = afpc_dsi_send(volume->server, (char *) &request,
                       sizeof(request), DSI_DEFAULT_TIMEOUT,
                       afpByteRangeLock, (void *) generated_offset);
    return rc;
}

int afp_byterangelock_reply(struct afp_server *server _U_,
                            char *buf, unsigned int size, void *x)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint64_t offset;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    uint32_t *offset = x;
    *offset = 0;

    if (size >= sizeof(*reply)) {
        *offset = ntohl(reply->offset);
    }

    return reply->header.return_code.error_code;
}

int afp_byterangelockext(struct afp_volume * volume,
                         unsigned char flag,
                         unsigned short forkid,
                         uint64_t offset,
                         uint64_t len, uint64_t *generated_offset)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t flag;
        uint16_t forkid;
        uint64_t offset;
        uint64_t len;
    }  __attribute__((__packed__)) request;
    int rc;
    struct dsi_header hdr;
    afpc_dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpByteRangeLockExt;
    request.flag = flag;
    request.forkid = htons(forkid);
    request.offset = hton64(offset);
    request.len = hton64(len);
    rc = afpc_dsi_send(volume->server, (char *) &request,
                       sizeof(request), DSI_DEFAULT_TIMEOUT,
                       afpByteRangeLockExt, (void *) generated_offset);
    return rc;
}

int afp_byterangelockext_reply(struct afp_server _U_
                               *server, char *buf, unsigned int size, void *x)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint64_t offset;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    uint64_t *offset = x;
    *offset = 0;

    if (size >= sizeof(*reply)) {
        *offset = ntoh64(reply->offset);
    }

    return reply->header.return_code.error_code;
}
