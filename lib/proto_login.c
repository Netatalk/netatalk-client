/*
 *  proto_login.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "codepage.h"
#include "compat.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "proto_login.h"
#include "utils.h"

static void write_uint16_be(uint8_t **cursor, uint16_t value)
{
    uint16_t encoded = htons(value);
    memcpy(*cursor, &encoded, sizeof(encoded));
    *cursor += sizeof(encoded);
}

static void write_uint32_be(uint8_t **cursor, uint32_t value)
{
    uint32_t encoded = htonl(value);
    memcpy(*cursor, &encoded, sizeof(encoded));
    *cursor += sizeof(encoded);
}

static void write_pascal_string(uint8_t **cursor, const char *value,
                                size_t value_len)
{
    *(*cursor)++ = (uint8_t)value_len;
    memcpy(*cursor, value, value_len);
    *cursor += value_len;
}

static void write_afp_name(uint8_t **cursor, const char *value,
                           size_t value_len)
{
    write_uint32_be(cursor, AFP_TEXT_ENCODING_UTF8);
    write_uint16_be(cursor, (uint16_t)value_len);
    memcpy(*cursor, value, value_len);
    *cursor += value_len;
}

int afp_loginext_build_payload(const char *afp_version,
                               const char *uam_name,
                               const char *username,
                               const char *directory_domain,
                               const void *userauthinfo,
                               size_t userauthinfo_len,
                               uint8_t **payload_out,
                               size_t *payload_len_out)
{
    const size_t fixed_len = 4; /* command + pad + flags */
    size_t version_len;
    size_t uam_len;
    size_t username_len;
    size_t domain_len;
    size_t prefix_len;
    size_t payload_len;
    size_t path_pad;
    uint8_t *payload;
    uint8_t *p;
    int ret;

    if (payload_out == NULL || payload_len_out == NULL) {
        return -EINVAL;
    }

    *payload_out = NULL;
    *payload_len_out = 0;

    if (afp_version == NULL || uam_name == NULL || username == NULL
            || directory_domain == NULL
            || (userauthinfo_len != 0 && userauthinfo == NULL)) {
        return -EINVAL;
    }

    version_len = strnlen(afp_version, (size_t)UINT8_MAX + 1U);
    uam_len = strnlen(uam_name, (size_t)UINT8_MAX + 1U);

    if (version_len > UINT8_MAX || uam_len > UINT8_MAX) {
        return -EOVERFLOW;
    }

    ret = afp_validate_username(username, &username_len);

    if (ret != 0) {
        return ret;
    }

    ret = afp_validate_utf8_name(directory_domain,
                                 AFP_MAX_UTF8_NAME_CHARS, &domain_len);

    if (ret != 0) {
        return ret;
    }

    prefix_len = fixed_len
                 + 1U + version_len
                 + 1U + uam_len
                 + 1U + sizeof(uint32_t) + sizeof(uint16_t) + username_len
                 + 1U + sizeof(uint32_t) + sizeof(uint16_t) + domain_len;
    path_pad = prefix_len & 1U;

    if (userauthinfo_len > (size_t)INT_MAX - sizeof(struct dsi_header)
            || prefix_len + path_pad
            > (size_t)INT_MAX - sizeof(struct dsi_header) - userauthinfo_len) {
        return -EOVERFLOW;
    }

    payload_len = prefix_len + path_pad + userauthinfo_len;
    payload = calloc(1, payload_len);

    if (payload == NULL) {
        return -ENOMEM;
    }

    p = payload;
    *p++ = afpLoginExt;
    *p++ = 0;
    write_uint16_be(&p, 0);
    write_pascal_string(&p, afp_version, version_len);
    write_pascal_string(&p, uam_name, uam_len);
    *p++ = kFPUTF8Name;
    write_afp_name(&p, username, username_len);
    *p++ = kFPUTF8Name;
    write_afp_name(&p, directory_domain, domain_len);

    if (path_pad != 0) {
        *p++ = 0;
    }

    if (userauthinfo_len != 0) {
        memcpy(p, userauthinfo, userauthinfo_len);
    }

    *payload_out = payload;
    *payload_len_out = payload_len;
    return 0;
}

void afp_loginext_payload_free(uint8_t *payload, size_t payload_len)
{
    if (payload != NULL) {
        explicit_bzero(payload, payload_len);
        free(payload);
    }
}

static int afp_login_build_payload(const char *afp_version,
                                   const char *uam_name,
                                   const char *username,
                                   int pad_before_authinfo,
                                   const void *userauthinfo,
                                   size_t userauthinfo_len,
                                   uint8_t **payload_out,
                                   size_t *payload_len_out)
{
    char legacy_username[UINT8_MAX + 1U];
    size_t version_len;
    size_t uam_len;
    size_t username_len = 0;
    size_t prefix_len;
    size_t username_pad;
    size_t payload_len;
    uint8_t *payload;
    uint8_t *p;
    int ret;

    if (payload_out == NULL || payload_len_out == NULL) {
        return -EINVAL;
    }

    *payload_out = NULL;
    *payload_len_out = 0;

    if (afp_version == NULL || uam_name == NULL
            || (userauthinfo_len != 0 && userauthinfo == NULL)) {
        return -EINVAL;
    }

    version_len = strnlen(afp_version, (size_t)UINT8_MAX + 1U);
    uam_len = strnlen(uam_name, (size_t)UINT8_MAX + 1U);

    if (version_len > UINT8_MAX || uam_len > UINT8_MAX) {
        return -EOVERFLOW;
    }

    if (username != NULL) {
        ret = afp_validate_utf8_name(username, AFP_LEGACY_MAX_USERNAME_BYTES,
                                     NULL);

        if (ret != 0) {
            return ret;
        }

        if (*username != '\0') {
            if (convert_utf8_to_mac_roman(username, strlen(username),
                                          legacy_username,
                                          sizeof(legacy_username)) != 0) {
                return -EILSEQ;
            }

            username_len = strlen(legacy_username);
        }
    }

    prefix_len = 1U + 1U + version_len + 1U + uam_len;

    if (username != NULL) {
        prefix_len += 1U + username_len;
    }

    username_pad = pad_before_authinfo && (prefix_len & 1U);

    if (userauthinfo_len > (size_t)INT_MAX - sizeof(struct dsi_header)
            || prefix_len + username_pad
            > (size_t)INT_MAX - sizeof(struct dsi_header) - userauthinfo_len) {
        return -EOVERFLOW;
    }

    payload_len = prefix_len + username_pad + userauthinfo_len;
    payload = calloc(1, payload_len);

    if (payload == NULL) {
        return -ENOMEM;
    }

    p = payload;
    *p++ = afpLogin;
    write_pascal_string(&p, afp_version, version_len);
    write_pascal_string(&p, uam_name, uam_len);

    if (username != NULL) {
        write_pascal_string(&p, legacy_username, username_len);
    }

    p += username_pad;

    if (userauthinfo_len != 0) {
        memcpy(p, userauthinfo, userauthinfo_len);
    }

    *payload_out = payload;
    *payload_len_out = payload_len;
    return 0;
}

int afp_login_initial_build_payload(int afp_version_number,
                                    const char *afp_version,
                                    const char *uam_name,
                                    const char *username,
                                    int pad_before_authinfo,
                                    const void *userauthinfo,
                                    size_t userauthinfo_len,
                                    uint8_t **payload_out,
                                    size_t *payload_len_out)
{
    if (afp_version_number >= 30) {
        return afp_loginext_build_payload(afp_version, uam_name,
                                          username != NULL ? username : "",
                                          "", userauthinfo,
                                          userauthinfo_len, payload_out,
                                          payload_len_out);
    }

    return afp_login_build_payload(afp_version, uam_name, username,
                                   pad_before_authinfo, userauthinfo,
                                   userauthinfo_len, payload_out,
                                   payload_len_out);
}

int afp_logout(struct afp_server *server, unsigned char wait)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
    }  __attribute__((__packed__)) request;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpLogout;
    request.pad = 0;
    return afpc_dsi_send(server, (char *) &request, sizeof(request),
                         wait, afpLogout, NULL);
}

int afp_login_reply(struct afp_server *server _U_,
                    char *buf, unsigned int size,
                    void *other)
{
    struct afp_rx_buffer * rx = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        char userauthinfo[];
    } * afp_login_reply_packet = (void *)buf;
    size -= sizeof(struct dsi_header);

    if (size > 0 && rx != NULL) {
        if (size > rx->maxsize) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "afp_login_reply: Response truncated from %u to %u bytes",
                           size, rx->maxsize);
            size = rx->maxsize;
        }

        memcpy(rx->data, afp_login_reply_packet->userauthinfo, size);
        rx->size = size;
    }

    return 0;
}

int afp_changepassword(struct afp_server *server, const char * ua_name,
                       char *userauthinfo, unsigned int userauthinfo_len,
                       struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
    }  __attribute__((__packed__)) * request;
    unsigned int ua_pascal_len = (unsigned char)strlen(ua_name) + 1;
    /* Pad to even boundary after UAM pascal string */
    unsigned int ua_pad = (ua_pascal_len & 1) ? 1 : 0;
    unsigned int len =
        sizeof(*request) /* DSI Header */
        + ua_pascal_len + ua_pad /* UAM + alignment */
        + userauthinfo_len;
    msg = malloc(len);

    if (!msg) {
        return -1;
    }

    request = (void *) msg;
    p = msg + (sizeof(*request));
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpChangePassword;
    request->pad = 0;
    p += copy_to_pascal(p, ua_name) + 1;

    if (ua_pad) {
        *p++ = 0;
    }

    memcpy(p, userauthinfo, userauthinfo_len);
    ret = afpc_dsi_send(server, msg, len, DSI_DEFAULT_TIMEOUT,
                        afpChangePassword, (void *)rx);
    free(msg);
    return ret;
}

int afp_changepassword_reply(struct afp_server *server _U_,
                             char *buf,
                             unsigned int size, void *other)
{
    struct afp_rx_buffer * rx = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        char userauthinfo[];
    } * afp_changepassword_reply_packet = (void *)buf;
    size -= sizeof(struct dsi_header);

    if (size > 0 && rx != NULL) {
        if (size > rx->maxsize) {
            size = rx->maxsize;
        }

        memcpy(rx->data, afp_changepassword_reply_packet->userauthinfo, size);
        rx->size = size;
    }

    return 0;
}

int afp_login_initial(struct afp_server *server, const char *ua_name,
                      const char *username, int pad_before_authinfo,
                      const void *userauthinfo,
                      unsigned int userauthinfo_len,
                      struct afp_rx_buffer *rx)
{
    uint8_t *payload = NULL;
    uint8_t command;
    size_t payload_len = 0;
    char *msg = NULL;
    size_t msg_len;
    int ret;
    struct dsi_header hdr;

    if (server == NULL || server->using_version == NULL
            || server->using_version->av_name == NULL) {
        return -EINVAL;
    }

    ret = afp_login_initial_build_payload(server->using_version->av_number,
                                          server->using_version->av_name,
                                          ua_name, username,
                                          pad_before_authinfo, userauthinfo,
                                          userauthinfo_len, &payload,
                                          &payload_len);

    if (ret != 0) {
        if (ret == -ENOMEM) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "afp_login_initial: Failed to allocate payload buffer");
        } else {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "afp_login_initial: Rejected request data (%d)", ret);
        }

        return ret;
    }

    msg_len = sizeof(hdr) + payload_len;
    msg = malloc(msg_len);

    if (msg == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "afp_login_initial: Failed to allocate message buffer");
        afp_loginext_payload_free(payload, payload_len);
        return -ENOMEM;
    }

    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), payload, payload_len);
    command = payload[0];
    afp_loginext_payload_free(payload, payload_len);
    ret = afpc_dsi_send(server, msg, (int)msg_len, DSI_LOGIN_TIMEOUT,
                        command, (void *)rx);
    explicit_bzero(msg, msg_len);
    free(msg);
    return ret;
}


int afp_logincont(struct afp_server *server, unsigned short id,
                  char *userauthinfo, unsigned int userauthinfo_len,
                  struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t id;
    } __attribute__((__packed__)) * request;
    unsigned int len =
        sizeof(*request) /* DSI header */
        + userauthinfo_len;
    msg = malloc(len);

    if (msg == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "afp_logincont: Failed to allocate message buffer");
        return -1;
    }

    memset(msg, 0, len);
    request = (void *)msg;
    p = msg + sizeof(*request);
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpLoginCont;
    request->id = htons(id);
    memcpy(p, userauthinfo, userauthinfo_len);
    ret = afpc_dsi_send(server, msg, len, DSI_LOGIN_TIMEOUT,
                        afpLoginCont, (void *)rx);
    free(msg);
    return ret;
}
