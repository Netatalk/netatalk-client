/*
 *  proto_session.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief A session token's requests, FPGetSessionToken and
 *        FPDisconnectOldSession
 */

#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"

/*!
 * @brief Ask the server for a session token, FPGetSessionToken
 *
 * @param[in]  server          the logged-in server
 * @param[in]  type            the token type; kLoginWithTimeAndID sends the
 *                             timestamp before the client's ID
 * @param[in]  timestamp       the time the client made its ID
 * @param[in]  outgoing_token  the client's ID
 * @param[out] incoming_token  receives the server's token
 * @returns afpc_dsi_send()'s result, or -1 for a type this client does not
 *          send or a request that cannot be allocated
 */
int afp_getsessiontoken(struct afp_server * server, int type,
                        unsigned int timestamp, struct afp_token *outgoing_token,
                        struct afp_token * incoming_token)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t type;
        uint32_t  idlength;
    } __attribute__((__packed__)) * request;
    int ret = 0;
    int offset = 0;
    int datalen = 0;
    char *data;
    int timelen = 0;
    datalen = outgoing_token->length;
    request = malloc(sizeof(*request) + AFP_TOKEN_MAX_LEN + sizeof(timestamp));

    if (!request) {
        return -1;
    }

    switch (type) {
    case kLoginWithTimeAndID:
    case kReconnWithTimeAndID: {
        uint32_t *p = (void *)(((unsigned long) request) +
                               sizeof(*request));
        offset = sizeof(timestamp);
        *p = timestamp;
        timelen = sizeof(timestamp);
    }
    break;

    case kRecon1Login:
        break;

    case kLoginWithoutID:
    case kRecon1RefreshToken:
    case kRecon1ReconnectLogin:
        datalen = 0;
        break;

    case kGetKerberosSessionKey:  /* We don't support it */
    case kReconnWithID:/* Deprecated. */
    case kLoginWithID: /* Deprecated. */
    default:
        ret = -1;
        free(request);
        goto error;
    }

    data = (void *)(((unsigned long) request) + sizeof(*request) + offset);
    request->idlength = htonl(datalen);
    request->pad = 0;
    request->type = htons(type);
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpGetSessionToken;
    memcpy(data, outgoing_token->data, datalen);
    ret = afpc_dsi_send(server, (char *)request,
                        sizeof(*request) + datalen + timelen,
                        DSI_DEFAULT_TIMEOUT, afpGetSessionToken,
                        (void *) incoming_token);
    free(request);
    return ret;
error:
    return ret;
}

/*!
 * @brief Parse an FPGetSessionToken reply into the token it carries
 *
 * @param[in]  server  unused
 * @param[in]  buf     the reply, DSI header first
 * @param[in]  size    bytes of buf
 * @param[out] other   the token, empty for a reply without one
 * @returns 0, or -1 for a token longer than the reply or than a token holds
 */
int afp_getsessiontoken_reply(struct afp_server *server _U_,
                              char *buf,
                              unsigned int size, void *other)
{
    struct afp_token * token = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint32_t token_len;
    } __attribute__((__packed__)) * reply = (void *)buf;
    char *token_data = buf + sizeof(*reply);
    unsigned int token_len;

    if (token) {
        token->length = 0;
    }

    if (size < sizeof(*reply)) {
        return 0;
    }

    token_len = ntohl(reply->token_len);

    if (token_len > AFP_TOKEN_MAX_LEN || size - sizeof(*reply) < token_len) {
        return -1;
    }

    if (token) {
        memcpy(token->data, token_data, token_len);
        token->length = token_len;
    }

    return 0;
}

/*!
 * @brief Ask for a token's session on this connection, FPDisconnectOldSession
 *
 * @param[in] server  the server, logged in over the new connection
 * @param[in] type    the token's type, 0
 * @param[in] token   the token of the session to continue
 * @returns the AFP result, ETIMEDOUT or a negative errno as afpc_dsi_send()
 *          gives them, or -1 for a token over AFP_TOKEN_MAX_LEN or a request
 *          that cannot be allocated
 */
int afp_disconnectoldsession(struct afp_server * server, int type,
                             struct afp_token * token)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t type;
        uint32_t  idlength;
    } __attribute__((__packed__)) * request;
    char *token_data;
    int ret;

    if ((request = malloc(sizeof(*request) + AFP_TOKEN_MAX_LEN)) == NULL) {
        return -1;
    }

    token_data  = (char *)request + sizeof(*request);
    request->type = htons(type);

    if (token->length > AFP_TOKEN_MAX_LEN) {
        free(request);
        return -1;
    }

    struct dsi_header hdr;

    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpDisconnectOldSession;
    request->pad = 0;
    request->idlength = htonl(token->length);
    memcpy(token_data, token->data, token->length);
    /* afpd takes five seconds to refuse a token no session holds. */
    ret = afpc_dsi_send(server, (char *)request,
                        sizeof(*request) + token->length,
                        DSI_LOGIN_TIMEOUT, afpDisconnectOldSession, NULL);
    free(request);
    return ret;
}

