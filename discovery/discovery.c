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
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "backend.h"

struct discovery_entry {
    struct afpc_discovery_service service;
    unsigned int sources;
};

struct discovery_event_node {
    struct afpc_discovery_event event;
    struct discovery_event_node *next;
};

struct afpc_discovery {
    void *backend_context;
    struct discovery_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    struct discovery_event_node *events_head;
    struct discovery_event_node *events_tail;
};

static long long now_ms(void)
{
    struct timeval now;

    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + now.tv_usec / 1000;
}

static int copy_network_text(char *destination, size_t destination_size,
                             const char *source)
{
    size_t source_len;

    if (!destination || destination_size == 0 || !source) {
        return -EINVAL;
    }

    source_len = strlen(source);

    if (source_len >= destination_size) {
        return -ENAMETOOLONG;
    }

    for (size_t i = 0; i < source_len; i++) {
        unsigned char ch = (unsigned char)source[i];
        destination[i] = (ch < 0x20 || ch == 0x7f) ? '?' : (char)ch;
    }

    destination[source_len] = '\0';
    return 0;
}

static int copy_service(struct afpc_discovery_service *destination,
                        const struct afpc_discovery_service *source)
{
    int ret;
    memset(destination, 0, sizeof(*destination));
    ret = copy_network_text(destination->instance,
                            sizeof(destination->instance), source->instance);

    if (ret != 0) {
        return ret;
    }

    ret = copy_network_text(destination->type, sizeof(destination->type),
                            source->type);

    if (ret != 0) {
        return ret;
    }

    size_t type_len = strlen(destination->type);

    if (type_len > 0 && destination->type[type_len - 1] == '.') {
        destination->type[type_len - 1] = '\0';
    }

    ret = copy_network_text(destination->domain,
                            sizeof(destination->domain), source->domain);

    if (ret != 0) {
        return ret;
    }

    destination->interface_index = source->interface_index;
    return 0;
}

static int service_equal(const struct afpc_discovery_service *left,
                         const struct afpc_discovery_service *right)
{
    return left->interface_index == right->interface_index
           && strcmp(left->instance, right->instance) == 0
           && strcmp(left->type, right->type) == 0
           && strcmp(left->domain, right->domain) == 0;
}

static int find_entry(const struct afpc_discovery *discovery,
                      const struct afpc_discovery_service *service)
{
    for (size_t i = 0; i < discovery->entry_count; i++) {
        if (service_equal(&discovery->entries[i].service, service)) {
            return (int)i;
        }
    }

    return -1;
}

static int reserve_entry(struct afpc_discovery *discovery)
{
    struct discovery_entry *entries;
    size_t capacity;

    if (discovery->entry_count < discovery->entry_capacity) {
        return 0;
    }

    capacity = discovery->entry_capacity == 0
               ? 8 : discovery->entry_capacity * 2;
    entries = realloc(discovery->entries, capacity * sizeof(*entries));

    if (!entries) {
        return -ENOMEM;
    }

    discovery->entries = entries;
    discovery->entry_capacity = capacity;
    return 0;
}

static int queue_event(struct afpc_discovery *discovery,
                       enum afpc_discovery_event_type type,
                       const struct afpc_discovery_service *service,
                       int error, const char *message)
{
    struct discovery_event_node *node = calloc(1, sizeof(*node));
    int ret;

    if (!node) {
        return -ENOMEM;
    }

    node->event.type = type;
    node->event.error = error;

    if (service) {
        ret = copy_service(&node->event.service, service);

        if (ret != 0) {
            free(node);
            return ret;
        }
    }

    if (message) {
        ret = copy_network_text(node->event.message,
                                sizeof(node->event.message), message);

        if (ret != 0) {
            free(node);
            return ret;
        }
    }

    if (discovery->events_tail) {
        discovery->events_tail->next = node;
    } else {
        discovery->events_head = node;
    }

    discovery->events_tail = node;
    return 0;
}

static int pop_event(struct afpc_discovery *discovery,
                     struct afpc_discovery_event *event)
{
    struct discovery_event_node *node = discovery->events_head;

    if (!node) {
        return 0;
    }

    discovery->events_head = node->next;

    if (!discovery->events_head) {
        discovery->events_tail = NULL;
    }

    *event = node->event;
    free(node);
    return 1;
}

int afpc_discovery_backend_emit(
    struct afpc_discovery *discovery,
    const struct afpc_discovery_backend_event *event)
{
    struct afpc_discovery_service service;
    unsigned int source;
    int index;
    int ret;

    if (!discovery || !event) {
        return -EINVAL;
    }

    if (event->type == AFPC_DISCOVERY_EVENT_ERROR) {
        return queue_event(discovery, event->type, NULL, event->error,
                           event->message);
    }

    ret = copy_service(&service, &event->service);

    if (ret != 0) {
        return ret;
    }

    source = event->source ? event->source : AFPC_DISCOVERY_SOURCE_UNSPEC;
    index = find_entry(discovery, &service);

    if (event->type == AFPC_DISCOVERY_EVENT_ADD) {
        if (index >= 0) {
            discovery->entries[index].sources |= source;
            return 0;
        }

        ret = reserve_entry(discovery);

        if (ret != 0) {
            return ret;
        }

        discovery->entries[discovery->entry_count].service = service;
        discovery->entries[discovery->entry_count].sources = source;
        discovery->entry_count++;
        return queue_event(discovery, event->type, &service, 0, NULL);
    }

    if (event->type == AFPC_DISCOVERY_EVENT_UPDATE) {
        if (index < 0) {
            return -ENOENT;
        }

        return queue_event(discovery, event->type, &service, 0, NULL);
    }

    if (event->type != AFPC_DISCOVERY_EVENT_REMOVE || index < 0) {
        return 0;
    }

    discovery->entries[index].sources &= ~source;

    if (discovery->entries[index].sources != 0) {
        return 0;
    }

    ret = queue_event(discovery, event->type,
                      &discovery->entries[index].service, 0, NULL);

    if (ret != 0) {
        return ret;
    }

    if ((size_t)index + 1 < discovery->entry_count) {
        memmove(&discovery->entries[index], &discovery->entries[index + 1],
                (discovery->entry_count - (size_t)index - 1)
                * sizeof(*discovery->entries));
    }

    discovery->entry_count--;
    return 0;
}

int afpc_discovery_open(struct afpc_discovery **discovery,
                        const struct afpc_discovery_options *options)
{
    struct afpc_discovery_options defaults = {
        .service_type = NULL,
        .domain = NULL,
        .interface_index = 0,
    };
    struct afpc_discovery *new_discovery;
    int ret;

    if (!discovery) {
        return -EINVAL;
    }

    *discovery = NULL;
    new_discovery = calloc(1, sizeof(*new_discovery));

    if (!new_discovery) {
        return -ENOMEM;
    }

    if (!options) {
        options = &defaults;
    }

    ret = afpc_discovery_backend.start(&new_discovery->backend_context,
                                       new_discovery, options);

    if (ret != 0) {
        free(new_discovery);
        return ret;
    }

    *discovery = new_discovery;
    return 0;
}

int afpc_discovery_next(struct afpc_discovery *discovery,
                        struct afpc_discovery_event *event,
                        int timeout_ms)
{
    long long deadline;
    int remaining;
    int ret;

    if (!discovery || !event || timeout_ms < -1) {
        return -EINVAL;
    }

    ret = pop_event(discovery, event);

    if (ret != 0) {
        return ret;
    }

    deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;

    do {
        remaining = timeout_ms < 0 ? -1 : (int)(deadline - now_ms());

        if (timeout_ms >= 0 && remaining < 0) {
            remaining = 0;
        }

        ret = afpc_discovery_backend.iterate(discovery->backend_context,
                                             discovery, remaining);

        if (ret < 0) {
            return ret;
        }

        ret = pop_event(discovery, event);

        if (ret != 0) {
            return ret;
        }

        if (timeout_ms == 0) {
            return 0;
        }
    } while (timeout_ms < 0 || now_ms() < deadline);

    return 0;
}

int afpc_discovery_resolve(struct afpc_discovery *discovery,
                           const struct afpc_discovery_service *service,
                           struct afpc_discovery_endpoint **endpoints,
                           size_t *endpoint_count, int timeout_ms)
{
    if (!discovery || !service || !endpoints || !endpoint_count
            || timeout_ms < -1) {
        return -EINVAL;
    }

    *endpoints = NULL;
    *endpoint_count = 0;
    return afpc_discovery_backend.resolve(discovery->backend_context,
                                          discovery, service, endpoints,
                                          endpoint_count, timeout_ms);
}

int afpc_discovery_snapshot(struct afpc_discovery *discovery,
                            struct afpc_discovery_service **services,
                            size_t *service_count, int timeout_ms)
{
    struct afpc_discovery_event event;
    struct afpc_discovery_service *result = NULL;
    long long deadline;
    int remaining;
    int ret;

    if (!discovery || !services || !service_count || timeout_ms < 0) {
        return -EINVAL;
    }

    *services = NULL;
    *service_count = 0;
    deadline = now_ms() + timeout_ms;

    do {
        remaining = (int)(deadline - now_ms());

        if (remaining < 0) {
            remaining = 0;
        }

        ret = afpc_discovery_next(discovery, &event, remaining);

        if (ret < 0) {
            return ret;
        }

        if (ret > 0 && event.type == AFPC_DISCOVERY_EVENT_ERROR) {
            return event.error > 0 ? -event.error : event.error;
        }
    } while (ret > 0 && now_ms() < deadline);

    if (discovery->entry_count != 0) {
        result = malloc(discovery->entry_count * sizeof(*result));

        if (!result) {
            return -ENOMEM;
        }

        for (size_t i = 0; i < discovery->entry_count; i++) {
            result[i] = discovery->entries[i].service;
        }
    }

    *services = result;
    *service_count = discovery->entry_count;
    return 0;
}

void afpc_discovery_free_services(struct afpc_discovery_service **services)
{
    if (!services) {
        return;
    }

    free(*services);
    *services = NULL;
}

void afpc_discovery_free_endpoints(
    struct afpc_discovery_endpoint **endpoints, size_t endpoint_count)
{
    if (!endpoints || !*endpoints) {
        return;
    }

    for (size_t i = 0; i < endpoint_count; i++) {
        free((*endpoints)[i].txt);
    }

    free(*endpoints);
    *endpoints = NULL;
}

int afpc_discovery_endpoint_host(
    const struct afpc_discovery_endpoint *endpoint,
    char *host, size_t host_size)
{
    const void *source;
    int family;

    if (!endpoint || !host || host_size == 0) {
        return -EINVAL;
    }

    if (endpoint->address_len == 0) {
        return copy_network_text(host, host_size, endpoint->target);
    }

    family = endpoint->address.ss_family;

    if (family == AF_INET) {
        const struct sockaddr_in *address4 =
            (const struct sockaddr_in *)&endpoint->address;
        source = &address4->sin_addr;
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *address6 =
            (const struct sockaddr_in6 *)&endpoint->address;
        source = &address6->sin6_addr;
    } else {
        return -EAFNOSUPPORT;
    }

    if (!inet_ntop(family, source, host, (socklen_t)host_size)) {
        return -errno;
    }

    if (family == AF_INET6) {
        const struct sockaddr_in6 *address6 =
            (const struct sockaddr_in6 *)&endpoint->address;

        if (address6->sin6_scope_id != 0) {
            char interface_name[IF_NAMESIZE];
            char scoped[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
            const char *scope;
            char numeric_scope[16];
            scope = if_indextoname(address6->sin6_scope_id, interface_name);

            if (!scope) {
                snprintf(numeric_scope, sizeof(numeric_scope), "%u",
                         address6->sin6_scope_id);
                scope = numeric_scope;
            }

            if (snprintf(scoped, sizeof(scoped), "%s%%%s", host, scope)
                    >= (int)sizeof(scoped)
                    || strlen(scoped) >= host_size) {
                return -ENAMETOOLONG;
            }

            memcpy(host, scoped, strlen(scoped) + 1);
        }
    }

    return 0;
}

void afpc_discovery_close(struct afpc_discovery **discovery)
{
    if (!discovery || !*discovery) {
        return;
    }

    afpc_discovery_backend.stop((*discovery)->backend_context);

    while ((*discovery)->events_head) {
        struct discovery_event_node *event = (*discovery)->events_head;
        (*discovery)->events_head = event->next;
        free(event);
    }

    free((*discovery)->entries);
    free(*discovery);
    *discovery = NULL;
}

const char *afpc_discovery_backend_name(void)
{
    return afpc_discovery_backend.name;
}
