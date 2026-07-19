/*
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "backend.h"

struct dnssd_context {
    DNSServiceRef browsers[2];
    size_t browser_count;
    struct afpc_discovery *discovery;
    int callback_error;
};

struct dnssd_resolve_result {
    int done;
    int error;
    char target[AFPC_DISCOVERY_TARGET_LEN];
    uint16_t port;
    unsigned int interface_index;
    unsigned char *txt;
    size_t txt_len;
};

#ifdef HAVE_DNSSD_GETADDRINFO
struct dnssd_address_result {
    const struct dnssd_resolve_result *resolved;
    struct afpc_discovery_endpoint *endpoints;
    size_t count;
    size_t capacity;
    int done;
    int error;
};

static long long now_ms(void)
{
    struct timeval now;

    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + now.tv_usec / 1000;
}
#endif

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

static void browse_reply(DNSServiceRef service_ref, DNSServiceFlags flags,
                         uint32_t interface_index,
                         DNSServiceErrorType error_code,
                         const char *service_name, const char *regtype,
                         const char *reply_domain, void *context_ptr)
{
    struct dnssd_context *context = context_ptr;
    struct afpc_discovery_backend_event event;
    int ret;
    (void)service_ref;
    memset(&event, 0, sizeof(event));

    if (error_code != kDNSServiceErr_NoError) {
        event.type = AFPC_DISCOVERY_EVENT_ERROR;
        event.error = EIO;
        event.message = "DNS-SD browse operation failed";
        ret = afpc_discovery_backend_emit(context->discovery, &event);

        if (ret != 0) {
            context->callback_error = ret;
        }

        return;
    }

    event.type = (flags & kDNSServiceFlagsAdd)
                 ? AFPC_DISCOVERY_EVENT_ADD : AFPC_DISCOVERY_EVENT_REMOVE;
    event.source = AFPC_DISCOVERY_SOURCE_UNSPEC;
    event.service.interface_index = interface_index;

    if (copy_string(event.service.instance,
                    sizeof(event.service.instance), service_name) != 0
            || copy_string(event.service.type, sizeof(event.service.type),
                           regtype) != 0
            || copy_string(event.service.domain,
                           sizeof(event.service.domain), reply_domain) != 0) {
        context->callback_error = -ENAMETOOLONG;
        return;
    }

    ret = afpc_discovery_backend_emit(context->discovery, &event);

    if (ret != 0) {
        context->callback_error = ret;
    }
}

static void resolve_reply(DNSServiceRef service_ref, DNSServiceFlags flags,
                          uint32_t interface_index,
                          DNSServiceErrorType error_code,
                          const char *full_name, const char *host_target,
                          uint16_t port, uint16_t txt_len,
                          const unsigned char *txt_record,
                          void *context_ptr)
{
    struct dnssd_resolve_result *result = context_ptr;
    (void)service_ref;
    (void)flags;
    (void)full_name;
    result->done = 1;

    if (error_code != kDNSServiceErr_NoError) {
        result->error = -EIO;
        return;
    }

    result->error = copy_string(result->target, sizeof(result->target),
                                host_target);

    if (result->error != 0) {
        return;
    }

    result->interface_index = interface_index;
    result->port = ntohs(port);

    if (txt_len != 0) {
        result->txt = malloc(txt_len);

        if (!result->txt) {
            result->error = -ENOMEM;
            return;
        }

        memcpy(result->txt, txt_record, txt_len);
        result->txt_len = txt_len;
    }
}

static int dnssd_start(void **context_ptr,
                       struct afpc_discovery *discovery,
                       const struct afpc_discovery_options *options)
{
    struct dnssd_context *context;
    const char *service_types[2];
    size_t service_type_count;
    DNSServiceErrorType error;

    if (!context_ptr || !discovery || !options) {
        return -EINVAL;
    }

    context = calloc(1, sizeof(*context));

    if (!context) {
        return -ENOMEM;
    }

    context->discovery = discovery;
    service_types[0] = options->service_type
                       ? options->service_type
                       : AFPC_DISCOVERY_AFP_SERVICE_TYPE;
    service_type_count = 1;

    if (!options->service_type) {
        service_types[service_type_count++] =
            AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE;
    }

    for (size_t i = 0; i < service_type_count; i++) {
        error = DNSServiceBrowse(&context->browsers[i], 0,
                                 options->interface_index,
                                 service_types[i], options->domain,
                                 browse_reply, context);

        if (error != kDNSServiceErr_NoError) {
            for (size_t j = 0; j < i; j++) {
                DNSServiceRefDeallocate(context->browsers[j]);
            }

            free(context);
            return -EIO;
        }

        context->browser_count++;
    }

    *context_ptr = context;
    return 0;
}

static int process_ref(DNSServiceRef service_ref, int timeout_ms)
{
    struct pollfd poll_fd;
    DNSServiceErrorType error;
    int ret;
    poll_fd.fd = DNSServiceRefSockFD(service_ref);
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    if (poll_fd.fd < 0) {
        return -EIO;
    }

    do {
        ret = poll(&poll_fd, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return -errno;
    }

    if (ret == 0) {
        return 0;
    }

    if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return -EIO;
    }

    if (!(poll_fd.revents & POLLIN)) {
        return 0;
    }

    error = DNSServiceProcessResult(service_ref);
    return error == kDNSServiceErr_NoError ? 1 : -EIO;
}

static int dnssd_iterate(void *context_ptr,
                         struct afpc_discovery *discovery, int timeout_ms)
{
    struct dnssd_context *context = context_ptr;
    struct pollfd poll_fds[2];
    int processed = 0;
    int ret;
    (void)discovery;

    if (!context) {
        return -EINVAL;
    }

    context->callback_error = 0;

    for (size_t i = 0; i < context->browser_count; i++) {
        poll_fds[i].fd = DNSServiceRefSockFD(context->browsers[i]);
        poll_fds[i].events = POLLIN;
        poll_fds[i].revents = 0;

        if (poll_fds[i].fd < 0) {
            return -EIO;
        }
    }

    do {
        ret = poll(poll_fds, (nfds_t)context->browser_count, timeout_ms);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return -errno;
    }

    if (ret == 0) {
        return 0;
    }

    for (size_t i = 0; i < context->browser_count; i++) {
        DNSServiceErrorType error;

        if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -EIO;
        }

        if (!(poll_fds[i].revents & POLLIN)) {
            continue;
        }

        error = DNSServiceProcessResult(context->browsers[i]);

        if (error != kDNSServiceErr_NoError) {
            return -EIO;
        }

        processed = 1;
    }

    if (context->callback_error != 0) {
        return context->callback_error;
    }

    return processed;
}

static int duplicate_txt(struct afpc_discovery_endpoint *endpoint,
                         const struct dnssd_resolve_result *result)
{
    if (result->txt_len == 0) {
        return 0;
    }

    endpoint->txt = malloc(result->txt_len);

    if (!endpoint->txt) {
        return -ENOMEM;
    }

    memcpy(endpoint->txt, result->txt, result->txt_len);
    endpoint->txt_len = result->txt_len;
    return 0;
}

#ifdef HAVE_DNSSD_GETADDRINFO
static int same_address(const struct afpc_discovery_endpoint *endpoint,
                        const struct sockaddr *address, socklen_t address_len,
                        unsigned int interface_index)
{
    if (endpoint->interface_index != interface_index
            || endpoint->address_len != address_len
            || endpoint->address.ss_family != address->sa_family) {
        return 0;
    }

    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *left =
            (const struct sockaddr_in *)&endpoint->address;
        const struct sockaddr_in *right =
            (const struct sockaddr_in *)address;
        return memcmp(&left->sin_addr, &right->sin_addr,
                      sizeof(left->sin_addr)) == 0;
    }

    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *left =
            (const struct sockaddr_in6 *)&endpoint->address;
        const struct sockaddr_in6 *right =
            (const struct sockaddr_in6 *)address;
        return memcmp(&left->sin6_addr, &right->sin6_addr,
                      sizeof(left->sin6_addr)) == 0;
    }

    return 0;
}

static int append_address(struct dnssd_address_result *result,
                          const struct sockaddr *address,
                          unsigned int interface_index)
{
    struct afpc_discovery_endpoint *endpoints;
    struct afpc_discovery_endpoint *endpoint;
    socklen_t address_len;
    size_t capacity;
    int ret;

    if (address->sa_family == AF_INET) {
        address_len = sizeof(struct sockaddr_in);
    } else if (address->sa_family == AF_INET6) {
        address_len = sizeof(struct sockaddr_in6);
    } else {
        return 0;
    }

    for (size_t i = 0; i < result->count; i++) {
        if (same_address(&result->endpoints[i], address, address_len,
                         interface_index)) {
            return 0;
        }
    }

    if (result->count == result->capacity) {
        capacity = result->capacity == 0 ? 4 : result->capacity * 2;
        endpoints = realloc(result->endpoints,
                            capacity * sizeof(*endpoints));

        if (!endpoints) {
            return -ENOMEM;
        }

        memset(endpoints + result->capacity, 0,
               (capacity - result->capacity) * sizeof(*endpoints));
        result->endpoints = endpoints;
        result->capacity = capacity;
    }

    endpoint = &result->endpoints[result->count];
    memcpy(&endpoint->address, address, address_len);
    endpoint->address_len = address_len;
    endpoint->interface_index = interface_index;
    endpoint->port = result->resolved->port;

    if (address->sa_family == AF_INET) {
        struct sockaddr_in *address4 =
            (struct sockaddr_in *)&endpoint->address;
        address4->sin_port = htons(endpoint->port);
    } else {
        struct sockaddr_in6 *address6 =
            (struct sockaddr_in6 *)&endpoint->address;
        address6->sin6_port = htons(endpoint->port);

        if (IN6_IS_ADDR_LINKLOCAL(&address6->sin6_addr)
                && address6->sin6_scope_id == 0) {
            address6->sin6_scope_id = interface_index;
        }
    }

    ret = copy_string(endpoint->target, sizeof(endpoint->target),
                      result->resolved->target);

    if (ret != 0) {
        return ret;
    }

    ret = duplicate_txt(endpoint, result->resolved);

    if (ret != 0) {
        return ret;
    }

    result->count++;
    return 0;
}

static void address_reply(DNSServiceRef service_ref, DNSServiceFlags flags,
                          uint32_t interface_index,
                          DNSServiceErrorType error_code,
                          const char *hostname,
                          const struct sockaddr *address, uint32_t ttl,
                          void *context_ptr)
{
    struct dnssd_address_result *result = context_ptr;
    (void)service_ref;
    (void)hostname;
    (void)ttl;

    if (error_code != kDNSServiceErr_NoError) {
        result->error = -EIO;
        result->done = 1;
        return;
    }

    if (flags & kDNSServiceFlagsAdd) {
        result->error = append_address(result, address, interface_index);

        if (result->error != 0) {
            result->done = 1;
            return;
        }
    }

    if (!(flags & kDNSServiceFlagsMoreComing)) {
        result->done = 1;
    }
}

static int build_scoped_endpoints(
    const struct dnssd_resolve_result *resolved,
    struct afpc_discovery_endpoint **endpoints, size_t *endpoint_count,
    int timeout_ms)
{
    struct dnssd_address_result result;
    DNSServiceRef address_query = NULL;
    DNSServiceErrorType error;
    long long deadline;
    int remaining;
    int ret;
    memset(&result, 0, sizeof(result));
    result.resolved = resolved;
    error = DNSServiceGetAddrInfo(
                &address_query, 0, resolved->interface_index,
                kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6,
                resolved->target, address_reply, &result);

    if (error != kDNSServiceErr_NoError) {
        return -EIO;
    }

    deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;

    do {
        remaining = timeout_ms < 0 ? -1 : (int)(deadline - now_ms());

        if (timeout_ms >= 0 && remaining < 0) {
            remaining = 0;
        }

        ret = process_ref(address_query, remaining);
    } while (ret > 0 && !result.done);

    DNSServiceRefDeallocate(address_query);

    if (ret < 0) {
        afpc_discovery_free_endpoints(&result.endpoints, result.count);
        return ret;
    }

    if (!result.done || result.count == 0) {
        afpc_discovery_free_endpoints(&result.endpoints, result.count);
        return -ETIMEDOUT;
    }

    if (result.error != 0) {
        afpc_discovery_free_endpoints(&result.endpoints, result.count);
        return result.error;
    }

    *endpoints = result.endpoints;
    *endpoint_count = result.count;
    return 0;
}
#endif

static int build_endpoints(const struct dnssd_resolve_result *result,
                           struct afpc_discovery_endpoint **endpoints_ptr,
                           size_t *endpoint_count)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct afpc_discovery_endpoint *endpoints;
    char port[6];
    size_t count = 0;
    size_t index = 0;
    int ret;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%u", result->port);
    ret = getaddrinfo(result->target, port, &hints, &addresses);

    if (ret == 0) {
        for (address = addresses; address; address = address->ai_next) {
            if (address->ai_addrlen <= sizeof(struct sockaddr_storage)) {
                count++;
            }
        }
    }

    if (count == 0) {
        count = 1;
    }

    endpoints = calloc(count, sizeof(*endpoints));

    if (!endpoints) {
        if (addresses) {
            freeaddrinfo(addresses);
        }

        return -ENOMEM;
    }

    if (addresses) {
        for (address = addresses; address; address = address->ai_next) {
            struct sockaddr_in6 *address6;

            if (address->ai_addrlen > sizeof(struct sockaddr_storage)) {
                continue;
            }

            memcpy(&endpoints[index].address, address->ai_addr,
                   address->ai_addrlen);
            endpoints[index].address_len = address->ai_addrlen;
            address6 = (struct sockaddr_in6 *)&endpoints[index].address;

            if (address6->sin6_family == AF_INET6
                    && IN6_IS_ADDR_LINKLOCAL(&address6->sin6_addr)
                    && address6->sin6_scope_id == 0) {
                address6->sin6_scope_id = result->interface_index;
            }

            index++;
        }
    }

    if (index == 0) {
        index = 1;
    }

    for (size_t i = 0; i < index; i++) {
        ret = copy_string(endpoints[i].target,
                          sizeof(endpoints[i].target), result->target);

        if (ret != 0) {
            afpc_discovery_free_endpoints(&endpoints, index);

            if (addresses) {
                freeaddrinfo(addresses);
            }

            return ret;
        }

        endpoints[i].port = result->port;
        endpoints[i].interface_index = result->interface_index;
        ret = duplicate_txt(&endpoints[i], result);

        if (ret != 0) {
            afpc_discovery_free_endpoints(&endpoints, index);

            if (addresses) {
                freeaddrinfo(addresses);
            }

            return ret;
        }
    }

    if (addresses) {
        freeaddrinfo(addresses);
    }

    *endpoints_ptr = endpoints;
    *endpoint_count = index;
    return 0;
}

static int dnssd_resolve(void *context_ptr,
                         struct afpc_discovery *discovery,
                         const struct afpc_discovery_service *service,
                         struct afpc_discovery_endpoint **endpoints,
                         size_t *endpoint_count, int timeout_ms)
{
    struct dnssd_resolve_result result;
    DNSServiceRef resolver = NULL;
    DNSServiceErrorType error;
    int ret;
    (void)context_ptr;
    (void)discovery;
    memset(&result, 0, sizeof(result));
    error = DNSServiceResolve(&resolver, 0, service->interface_index,
                              service->instance, service->type,
                              service->domain, resolve_reply, &result);

    if (error != kDNSServiceErr_NoError) {
        return -EIO;
    }

    ret = process_ref(resolver, timeout_ms);
    DNSServiceRefDeallocate(resolver);

    if (ret < 0) {
        free(result.txt);
        return ret;
    }

    if (!result.done) {
        free(result.txt);
        return -ETIMEDOUT;
    }

    if (result.error != 0) {
        free(result.txt);
        return result.error;
    }

#ifdef HAVE_DNSSD_GETADDRINFO
    ret = build_scoped_endpoints(&result, endpoints, endpoint_count,
                                 timeout_ms);

    if (ret == 0) {
        free(result.txt);
        return 0;
    }

#endif
    ret = build_endpoints(&result, endpoints, endpoint_count);
    free(result.txt);
    return ret;
}

static void dnssd_stop(void *context_ptr)
{
    struct dnssd_context *context = context_ptr;

    if (!context) {
        return;
    }

    for (size_t i = 0; i < context->browser_count; i++) {
        DNSServiceRefDeallocate(context->browsers[i]);
    }

    free(context);
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "dnssd",
    .start = dnssd_start,
    .iterate = dnssd_iterate,
    .resolve = dnssd_resolve,
    .stop = dnssd_stop,
};
