/*
 *  transport.c
 *  Public stateful transport facade for libafpclient consumers
 *
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netatalk-client/transport.h"

#include "afp_internal.h"
#include "afp_protocol.h"
#include "client.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "uam_registry.h"

#define AFPC_AFP_ERROR_BASE (-5000)
#define AFPC_DEFAULT_RAW_TIMEOUT 20

struct afpc_transport {
    struct afp_server *server;
};

struct raw_reply_context {
    void *data;
    size_t capacity;
    size_t size;
};

static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int transport_initialized;
static pthread_t transport_loop_thread;
static afpc_transport_log_callback transport_log_callback;
static void *transport_log_context;

static void transport_log(void *priv, enum logtypes logtype, int loglevel,
                          const char *message)
{
    afpc_transport_log_callback callback;
    void *context;
    (void)priv;
    (void)logtype;
    pthread_mutex_lock(&log_mutex);
    callback = transport_log_callback;
    context = transport_log_context;
    pthread_mutex_unlock(&log_mutex);

    if (callback) {
        callback(context, loglevel, message);
    }
}

static struct libafpclient transport_client = {
    .unmount_volume = NULL,
    .log_for_client = transport_log,
    .forced_ending_hook = NULL,
    .scan_extra_fds = NULL,
    .loop_started = NULL,
};

static int ensure_transport_initialized(void)
{
    int result = 0;
    pthread_mutex_lock(&init_mutex);

    if (!transport_initialized) {
        libafpclient_register(&transport_client);

        if (init_uams() != 0) {
            result = -EIO;
        } else if (afp_main_quick_startup(&transport_loop_thread) != 0) {
            result = -EIO;
        } else {
            transport_initialized = 1;
        }
    }

    pthread_mutex_unlock(&init_mutex);
    return result;
}

int afpc_transport_init(afpc_transport_log_callback callback, void *context)
{
    pthread_mutex_lock(&log_mutex);
    transport_log_callback = callback;
    transport_log_context = context;
    pthread_mutex_unlock(&log_mutex);
    return ensure_transport_initialized();
}

unsigned int afpc_transport_default_uams(void)
{
    if (ensure_transport_initialized() != 0) {
        return 0;
    }

    return default_uams_mask();
}

unsigned int afpc_transport_uam_mask(const char *name)
{
    if (!name || !*name || ensure_transport_initialized() != 0) {
        return 0;
    }

    return find_uam_by_name(name);
}

const char *afpc_transport_uam_name(unsigned int mask)
{
    if (ensure_transport_initialized() != 0) {
        return NULL;
    }

    return uam_bitmap_to_string(mask);
}

static int copy_option(char *destination, size_t capacity, const char *source)
{
    int written;

    if (!source) {
        source = "";
    }

    written = snprintf(destination, capacity, "%s", source);

    if (written < 0 || (size_t)written >= capacity) {
        return -ENAMETOOLONG;
    }

    return 0;
}

int afpc_transport_connect(const struct afpc_transport_options *options,
                           struct afpc_transport **transport)
{
    struct afp_connection_request request;
    struct afp_server *server;
    struct afpc_transport *result;
    int error;

    if (!options || !transport || !options->host || !*options->host) {
        return -EINVAL;
    }

    *transport = NULL;
    error = ensure_transport_initialized();

    if (error != 0) {
        return error;
    }

    memset(&request, 0, sizeof(request));
    afp_default_url(&request.url);
    error = copy_option(request.url.servername,
                        sizeof(request.url.servername), options->host);

    if (error != 0) {
        return error;
    }

    error = copy_option(request.url.username,
                        sizeof(request.url.username), options->username);

    if (error != 0) {
        return error;
    }

    error = copy_option(request.url.password,
                        sizeof(request.url.password), options->password);

    if (error != 0) {
        return error;
    }

    if (options->port > 0) {
        request.url.port = options->port;
    }

    request.url.requested_version = options->requested_version;
    request.uam_mask = options->uam_mask
                       ? options->uam_mask
                       : default_uams_mask();
    server = afp_server_full_connect(NULL, &request);

    if (!server) {
        return errno ? -errno : -EIO;
    }

    result = calloc(1, sizeof(*result));

    if (!result) {
        afp_logout(server, 1);
        afp_server_remove(server);
        return -ENOMEM;
    }

    result->server = server;
    *transport = result;
    return 0;
}

void afpc_transport_close(struct afpc_transport **transport)
{
    struct afpc_transport *current;

    if (!transport || !*transport) {
        return;
    }

    current = *transport;
    *transport = NULL;

    if (current->server) {
        afp_logout(current->server, 1);
        afp_server_remove(current->server);
    }

    free(current);
}

unsigned int afpc_transport_supported_uams(
    const struct afpc_transport *transport)
{
    return transport && transport->server
           ? transport->server->supported_uams : 0;
}

unsigned int afpc_transport_using_uam(
    const struct afpc_transport *transport)
{
    return transport && transport->server ? transport->server->using_uam : 0;
}

int afpc_transport_using_version(const struct afpc_transport *transport)
{
    return transport && transport->server && transport->server->using_version
           ? transport->server->using_version->av_number : 0;
}

int afpc_transport_socket(const struct afpc_transport *transport)
{
    return transport && transport->server ? transport->server->fd : -1;
}

unsigned int afpc_transport_tx_quantum(
    const struct afpc_transport *transport)
{
    return transport && transport->server ? transport->server->tx_quantum : 0;
}

unsigned int afpc_transport_rx_quantum(
    const struct afpc_transport *transport)
{
    return transport && transport->server ? transport->server->rx_quantum : 0;
}

unsigned int afpc_transport_attention_quantum(
    const struct afpc_transport *transport)
{
    return transport && transport->server
           ? transport->server->attention_quantum : 0;
}

int afpc_transport_get_server_parameters(struct afpc_transport *transport)
{
    int result;

    if (!transport || !transport->server) {
        return -EINVAL;
    }

    result = afp_getsrvrparms(transport->server);
    return result > 0 ? -result : result;
}

static int raw_reply(struct afp_server *server, char *buffer,
                     unsigned int size, void *context)
{
    struct raw_reply_context *reply = context;
    size_t payload_size;
    (void)server;

    if (!reply || size < sizeof(struct dsi_header)) {
        return -EPROTO;
    }

    payload_size = size - sizeof(struct dsi_header);
    reply->size = payload_size < reply->capacity
                  ? payload_size : reply->capacity;

    if (reply->size > 0) {
        memcpy(reply->data, buffer + sizeof(struct dsi_header), reply->size);
    }

    return 0;
}

static size_t raw_read_size(const void *payload, size_t payload_len,
                            unsigned char subcommand)
{
    const unsigned char *bytes = payload;
    uint32_t high;
    uint32_t low;
    uint32_t count;
    uint64_t extended_count;

    if (subcommand == afpRead) {
        if (payload_len < 12) {
            return 0;
        }

        memcpy(&count, bytes + 8, sizeof(count));
        return ntohl(count);
    }

    if (subcommand == afpReadExt) {
        if (payload_len < 20) {
            return 0;
        }

        memcpy(&high, bytes + 12, sizeof(high));
        memcpy(&low, bytes + 16, sizeof(low));
        extended_count = ((uint64_t)ntohl(high) << 32) | ntohl(low);
        return extended_count > SIZE_MAX ? SIZE_MAX : (size_t)extended_count;
    }

    return 0;
}

int afpc_transport_raw_command(struct afpc_transport *transport,
                               const void *payload, size_t payload_len,
                               unsigned char dsi_command,
                               uint32_t data_offset, int timeout,
                               int *afp_result, void *reply,
                               size_t reply_capacity, size_t *reply_len)
{
    struct raw_reply_context raw_context;
    struct afp_rx_buffer read_buffer;
    struct dsi_header *header;
    unsigned char subcommand;
    size_t requested;
    size_t read_capacity;
    char *read_storage = NULL;
    char *message;
    int result;

    if (reply_len) {
        *reply_len = 0;
    }

    if (afp_result) {
        *afp_result = 0;
    }

    if (!transport || !transport->server || !payload || payload_len == 0
            || (reply_capacity > 0 && !reply)
            || payload_len > (size_t)INT_MAX - sizeof(*header)) {
        return -EINVAL;
    }

    message = calloc(1, sizeof(*header) + payload_len);

    if (!message) {
        return -ENOMEM;
    }

    header = (struct dsi_header *)message;
    afpc_dsi_setup_header(transport->server, header, (char)dsi_command);
    header->return_code.data_offset = htonl(data_offset);
    memcpy(message + sizeof(*header), payload, payload_len);
    subcommand = ((const unsigned char *)payload)[0];
    memset(&raw_context, 0, sizeof(raw_context));
    raw_context.data = reply;
    raw_context.capacity = reply_capacity;

    if (subcommand == afpRead || subcommand == afpReadExt) {
        requested = raw_read_size(payload, payload_len, subcommand);
        read_capacity = requested;

        if (transport->server->rx_quantum
                && read_capacity > transport->server->rx_quantum) {
            read_capacity = transport->server->rx_quantum;
        }

        if (read_capacity < reply_capacity) {
            read_capacity = reply_capacity;
        }

        if (read_capacity == 0) {
            read_capacity = 1;
        }

        if (read_capacity > UINT_MAX) {
            read_capacity = UINT_MAX;
        }

        if (!reply || read_capacity > reply_capacity) {
            read_storage = malloc(read_capacity);

            if (!read_storage) {
                free(message);
                return -ENOMEM;
            }
        }

        memset(&read_buffer, 0, sizeof(read_buffer));
        read_buffer.data = read_storage ? read_storage : reply;
        read_buffer.maxsize = (unsigned int)read_capacity;
        result = afpc_dsi_send_with_reply(
                     transport->server, message,
                     (int)(sizeof(*header) + payload_len),
                     timeout ? timeout : AFPC_DEFAULT_RAW_TIMEOUT,
                     subcommand, &read_buffer, NULL, &read_buffer);
        raw_context.size = read_buffer.size < reply_capacity
                           ? read_buffer.size : reply_capacity;

        if (read_storage && raw_context.size > 0) {
            memcpy(reply, read_storage, raw_context.size);
        }
    } else {
        result = afpc_dsi_send_with_reply(
                     transport->server, message,
                     (int)(sizeof(*header) + payload_len),
                     timeout ? timeout : AFPC_DEFAULT_RAW_TIMEOUT,
                     subcommand, &raw_context, raw_reply, NULL);
    }

    free(read_storage);
    free(message);

    if (afp_result) {
        *afp_result = result;
    }

    if (reply_len) {
        *reply_len = raw_context.size;
    }

    if (result == 0 || result <= AFPC_AFP_ERROR_BASE) {
        return 0;
    }

    if (result == -1) {
        return -EIO;
    }

    return result > 0 ? -result : result;
}
