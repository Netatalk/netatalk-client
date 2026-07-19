/*
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include "backend.h"

struct avahi_context {
    AvahiSimplePoll *simple_poll;
    AvahiClient *client;
    AvahiServiceBrowser *browsers[2];
    size_t browser_count;
    struct afpc_discovery *discovery;
    char *service_types[2];
    size_t service_type_count;
    char *domain;
    unsigned int interface_index;
    int callback_error;
};

struct avahi_resolve_result {
    int done;
    int error;
    struct afpc_discovery_endpoint endpoint;
};

static void free_service_types(struct avahi_context *context)
{
    for (size_t i = 0; i < context->service_type_count; i++) {
        free(context->service_types[i]);
    }
}

static long long now_ms(void)
{
    struct timeval now;

    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + now.tv_usec / 1000;
}

static int copy_string(char *destination, size_t destination_size,
                       const char *source)
{
    size_t length;

    if (!destination || destination_size == 0 || !source) {
        return -EINVAL;
    }

    length = strlen(source);

    if (length >= destination_size) {
        return -ENAMETOOLONG;
    }

    memcpy(destination, source, length + 1);
    return 0;
}

static unsigned int source_for_protocol(AvahiProtocol protocol)
{
    if (protocol == AVAHI_PROTO_INET) {
        return AFPC_DISCOVERY_SOURCE_IPV4;
    }

    if (protocol == AVAHI_PROTO_INET6) {
        return AFPC_DISCOVERY_SOURCE_IPV6;
    }

    return AFPC_DISCOVERY_SOURCE_UNSPEC;
}

static unsigned int interface_index_from_avahi(AvahiIfIndex interface)
{
    return interface < 0 ? 0U : (unsigned int)interface;
}

static void emit_error(struct avahi_context *context, int error,
                       const char *message)
{
    struct afpc_discovery_backend_event event;
    int ret;
    memset(&event, 0, sizeof(event));
    event.type = AFPC_DISCOVERY_EVENT_ERROR;
    event.error = error;
    event.message = message;
    ret = afpc_discovery_backend_emit(context->discovery, &event);

    if (ret != 0) {
        context->callback_error = ret;
    }
}

static void browser_callback(AvahiServiceBrowser *browser,
                             AvahiIfIndex interface,
                             AvahiProtocol protocol,
                             AvahiBrowserEvent browser_event,
                             const char *name, const char *type,
                             const char *domain,
                             AvahiLookupResultFlags flags, void *userdata)
{
    struct avahi_context *context = userdata;
    struct afpc_discovery_backend_event event;
    int ret;
    (void)browser;
    (void)flags;

    if (browser_event == AVAHI_BROWSER_FAILURE) {
        emit_error(context, EIO, "Avahi service browser failed");
        return;
    }

    if (browser_event != AVAHI_BROWSER_NEW
            && browser_event != AVAHI_BROWSER_REMOVE) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = browser_event == AVAHI_BROWSER_NEW
                 ? AFPC_DISCOVERY_EVENT_ADD : AFPC_DISCOVERY_EVENT_REMOVE;
    event.source = source_for_protocol(protocol);
    event.service.interface_index = interface_index_from_avahi(interface);

    if (copy_string(event.service.instance,
                    sizeof(event.service.instance), name) != 0
            || copy_string(event.service.type, sizeof(event.service.type),
                           type) != 0
            || copy_string(event.service.domain,
                           sizeof(event.service.domain), domain) != 0) {
        context->callback_error = -ENAMETOOLONG;
        return;
    }

    ret = afpc_discovery_backend_emit(context->discovery, &event);

    if (ret != 0) {
        context->callback_error = ret;
    }
}

static int create_browser(struct avahi_context *context,
                          AvahiClient *client)
{
    AvahiIfIndex interface = AVAHI_IF_UNSPEC;

    if (context->browser_count != 0) {
        return 0;
    }

    if (context->interface_index != 0) {
        interface = (AvahiIfIndex)context->interface_index;
    }

    for (size_t i = 0; i < context->service_type_count; i++) {
        context->browsers[i] = avahi_service_browser_new(
                                   client, interface, AVAHI_PROTO_UNSPEC,
                                   context->service_types[i], context->domain,
                                   0, browser_callback, context);

        if (!context->browsers[i]) {
            for (size_t j = 0; j < i; j++) {
                avahi_service_browser_free(context->browsers[j]);
                context->browsers[j] = NULL;
            }

            context->browser_count = 0;
            return -EIO;
        }

        context->browser_count++;
    }

    return 0;
}

static void client_callback(AvahiClient *client, AvahiClientState state,
                            void *userdata)
{
    struct avahi_context *context = userdata;
    context->client = client;

    if (state == AVAHI_CLIENT_S_RUNNING) {
        if (create_browser(context, client) != 0) {
            emit_error(context, EIO,
                       "Could not create Avahi service browser");
        }
    } else if (state == AVAHI_CLIENT_FAILURE) {
        emit_error(context, EIO, "Avahi client connection failed");
    }
}

static int avahi_start(void **context_ptr,
                       struct afpc_discovery *discovery,
                       const struct afpc_discovery_options *options)
{
    struct avahi_context *context;
    const char *primary_service_type;
    int error;

    if (!context_ptr || !discovery || !options) {
        return -EINVAL;
    }

    context = calloc(1, sizeof(*context));

    if (!context) {
        return -ENOMEM;
    }

    primary_service_type = options->service_type
                           ? options->service_type
                           : AFPC_DISCOVERY_AFP_SERVICE_TYPE;
    context->service_types[0] = strdup(primary_service_type);
    context->service_type_count = 1;

    if (!options->service_type) {
        context->service_types[context->service_type_count++] =
            strdup(AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE);
    }

    context->domain = options->domain ? strdup(options->domain) : NULL;
    context->interface_index = options->interface_index;
    context->discovery = discovery;

    if (!context->service_types[0]
            || (context->service_type_count > 1
                && !context->service_types[1])
            || (options->domain && !context->domain)) {
        free(context->domain);
        free_service_types(context);
        free(context);
        return -ENOMEM;
    }

    context->simple_poll = avahi_simple_poll_new();

    if (!context->simple_poll) {
        free(context->domain);
        free_service_types(context);
        free(context);
        return -ENOMEM;
    }

    context->client = avahi_client_new(
                          avahi_simple_poll_get(context->simple_poll),
                          AVAHI_CLIENT_NO_FAIL, client_callback, context,
                          &error);

    if (!context->client) {
        avahi_simple_poll_free(context->simple_poll);
        free(context->domain);
        free_service_types(context);
        free(context);
        return -EIO;
    }

    *context_ptr = context;
    return 0;
}

static int avahi_iterate(void *context_ptr,
                         struct afpc_discovery *discovery, int timeout_ms)
{
    struct avahi_context *context = context_ptr;
    int ret;
    (void)discovery;

    if (!context) {
        return -EINVAL;
    }

    context->callback_error = 0;
    ret = avahi_simple_poll_iterate(context->simple_poll, timeout_ms);

    if (context->callback_error != 0) {
        return context->callback_error;
    }

    return ret < 0 ? -EIO : ret;
}

static void resolver_callback(AvahiServiceResolver *resolver,
                              AvahiIfIndex interface,
                              AvahiProtocol protocol,
                              AvahiResolverEvent resolver_event,
                              const char *name, const char *type,
                              const char *domain, const char *host_name,
                              const AvahiAddress *address, uint16_t port,
                              AvahiStringList *txt,
                              AvahiLookupResultFlags flags, void *userdata)
{
    struct avahi_resolve_result *result = userdata;
    struct sockaddr_in *address4;
    struct sockaddr_in6 *address6;
    char numeric_address[AVAHI_ADDRESS_STR_MAX];
    unsigned int interface_index;
    size_t txt_len;
    (void)resolver;
    (void)name;
    (void)type;
    (void)domain;
    (void)flags;
    result->done = 1;

    if (resolver_event != AVAHI_RESOLVER_FOUND) {
        result->error = -EIO;
        return;
    }

    result->error = copy_string(result->endpoint.target,
                                sizeof(result->endpoint.target), host_name);

    if (result->error != 0) {
        return;
    }

    interface_index = interface_index_from_avahi(interface);
    result->endpoint.port = port;
    result->endpoint.interface_index = interface_index;
    avahi_address_snprint(numeric_address, sizeof(numeric_address), address);

    if (protocol == AVAHI_PROTO_INET) {
        address4 = (struct sockaddr_in *)&result->endpoint.address;
        address4->sin_family = AF_INET;
        address4->sin_port = htons(port);

        if (inet_pton(AF_INET, numeric_address, &address4->sin_addr) != 1) {
            result->error = -EIO;
            return;
        }

        result->endpoint.address_len = sizeof(*address4);
    } else if (protocol == AVAHI_PROTO_INET6) {
        address6 = (struct sockaddr_in6 *)&result->endpoint.address;
        address6->sin6_family = AF_INET6;
        address6->sin6_port = htons(port);

        if (inet_pton(AF_INET6, numeric_address, &address6->sin6_addr) != 1) {
            result->error = -EIO;
            return;
        }

        if (IN6_IS_ADDR_LINKLOCAL(&address6->sin6_addr)
                && interface_index != 0) {
            address6->sin6_scope_id = interface_index;
        }

        result->endpoint.address_len = sizeof(*address6);
    }

    txt_len = avahi_string_list_serialize(txt, NULL, 0);

    if (txt_len != 0) {
        result->endpoint.txt = malloc(txt_len);

        if (!result->endpoint.txt) {
            result->error = -ENOMEM;
            return;
        }

        result->endpoint.txt_len = avahi_string_list_serialize(
                                       txt, result->endpoint.txt, txt_len);
    }
}

static int avahi_resolve(void *context_ptr,
                         struct afpc_discovery *discovery,
                         const struct afpc_discovery_service *service,
                         struct afpc_discovery_endpoint **endpoints,
                         size_t *endpoint_count, int timeout_ms)
{
    struct avahi_context *context = context_ptr;
    struct avahi_resolve_result result;
    AvahiServiceResolver *resolver;
    AvahiIfIndex interface;
    long long deadline;
    int remaining;
    int ret;
    (void)discovery;
    memset(&result, 0, sizeof(result));
    interface = service->interface_index == 0
                    ? AVAHI_IF_UNSPEC : (AvahiIfIndex)service->interface_index;
    resolver = avahi_service_resolver_new(
                   context->client, interface, AVAHI_PROTO_UNSPEC,
                   service->instance, service->type, service->domain,
                   AVAHI_PROTO_UNSPEC, 0, resolver_callback, &result);

    if (!resolver) {
        return -EIO;
    }

    deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;

    while (!result.done) {
        remaining = timeout_ms < 0 ? -1 : (int)(deadline - now_ms());

        if (timeout_ms >= 0 && remaining <= 0) {
            break;
        }

        ret = avahi_simple_poll_iterate(context->simple_poll, remaining);

        if (ret < 0) {
            result.error = -EIO;
            break;
        }
    }

    avahi_service_resolver_free(resolver);

    if (!result.done && result.error == 0) {
        return -ETIMEDOUT;
    }

    if (result.error != 0) {
        free(result.endpoint.txt);
        return result.error;
    }

    *endpoints = malloc(sizeof(**endpoints));

    if (!*endpoints) {
        free(result.endpoint.txt);
        return -ENOMEM;
    }

    **endpoints = result.endpoint;
    *endpoint_count = 1;
    return 0;
}

static void avahi_stop(void *context_ptr)
{
    struct avahi_context *context = context_ptr;

    if (!context) {
        return;
    }

    for (size_t i = 0; i < context->browser_count; i++) {
        avahi_service_browser_free(context->browsers[i]);
    }

    if (context->client) {
        avahi_client_free(context->client);
    }

    if (context->simple_poll) {
        avahi_simple_poll_free(context->simple_poll);
    }

    free(context->domain);
    free_service_types(context);
    free(context);
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "avahi",
    .start = avahi_start,
    .iterate = avahi_iterate,
    .resolve = avahi_resolve,
    .stop = avahi_stop,
};
