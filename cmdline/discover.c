/*
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>

#include "discovery/discovery.h"

#include "discover.h"

#define PICKER_INTERVAL_MS 100
#define PICKER_RESOLVE_TIMEOUT_MS 3000

struct picker_member {
    struct afpc_discovery_service service;
    int active;
};

struct picker_entry {
    struct afpc_discovery_service service;
    struct picker_member *members;
    size_t member_count;
    size_t member_capacity;
    unsigned int slot;
};

struct picker {
    struct picker_entry *entries;
    size_t count;
    size_t capacity;
    unsigned int next_slot;
};

static int service_group_equal(const struct afpc_discovery_service *left,
                               const struct afpc_discovery_service *right)
{
    return strcmp(left->instance, right->instance) == 0
           && strcmp(left->type, right->type) == 0
           && strcmp(left->domain, right->domain) == 0;
}

static int picker_entry_active(const struct picker_entry *entry)
{
    for (size_t i = 0; i < entry->member_count; i++) {
        if (entry->members[i].active) {
            return 1;
        }
    }

    return 0;
}

static struct picker_entry *find_group(
    struct picker *picker, const struct afpc_discovery_service *service)
{
    for (size_t i = 0; i < picker->count; i++) {
        if (service_group_equal(&picker->entries[i].service, service)) {
            return &picker->entries[i];
        }
    }

    return NULL;
}

static struct picker_entry *find_slot(struct picker *picker,
                                      unsigned long slot)
{
    for (size_t i = 0; i < picker->count; i++) {
        if (picker->entries[i].slot == slot
                && picker_entry_active(&picker->entries[i])) {
            return &picker->entries[i];
        }
    }

    return NULL;
}

static struct picker_member *find_member(
    struct picker_entry *entry,
    const struct afpc_discovery_service *service)
{
    for (size_t i = 0; i < entry->member_count; i++) {
        if (entry->members[i].service.interface_index
                == service->interface_index) {
            return &entry->members[i];
        }
    }

    return NULL;
}

static int add_member(struct picker_entry *entry,
                      const struct afpc_discovery_service *service)
{
    struct picker_member *members;
    size_t capacity;

    if (entry->member_count == entry->member_capacity) {
        capacity = entry->member_capacity == 0
                   ? 4 : entry->member_capacity * 2;
        members = realloc(entry->members, capacity * sizeof(*members));

        if (!members) {
            return -ENOMEM;
        }

        entry->members = members;
        entry->member_capacity = capacity;
    }

    entry->members[entry->member_count].service = *service;
    entry->members[entry->member_count].active = 1;
    entry->member_count++;
    return 1;
}

static int update_picker(struct picker *picker,
                         const struct afpc_discovery_event *event)
{
    struct picker_entry *entry = find_group(picker, &event->service);
    struct picker_member *member = entry
                                   ? find_member(entry, &event->service) : NULL;

    if (event->type == AFPC_DISCOVERY_EVENT_REMOVE) {
        if (member && member->active) {
            member->active = 0;
            return 1;
        }

        return 0;
    }

    if (event->type != AFPC_DISCOVERY_EVENT_ADD
            && event->type != AFPC_DISCOVERY_EVENT_UPDATE) {
        return 0;
    }

    if (member) {
        if (!member->active) {
            member->active = 1;
            member->service = event->service;
            return 1;
        }

        member->service = event->service;
        return event->type == AFPC_DISCOVERY_EVENT_UPDATE;
    }

    if (entry) {
        return add_member(entry, &event->service);
    }

    if (picker->count == picker->capacity) {
        struct picker_entry *entries;
        size_t capacity = picker->capacity == 0
                          ? 8 : picker->capacity * 2;
        entries = realloc(picker->entries, capacity * sizeof(*entries));

        if (!entries) {
            return -ENOMEM;
        }

        picker->entries = entries;
        picker->capacity = capacity;
    }

    entry = &picker->entries[picker->count++];
    memset(entry, 0, sizeof(*entry));
    entry->service = event->service;
    entry->slot = picker->next_slot++;
    return add_member(entry, &event->service);
}

static int has_duplicate_name(const struct picker *picker, size_t index)
{
    for (size_t i = 0; i < picker->count; i++) {
        if (i != index && picker_entry_active(&picker->entries[i])
                && strcmp(picker->entries[i].service.instance,
                          picker->entries[index].service.instance) == 0) {
            return 1;
        }
    }

    return 0;
}

static void render_picker(const struct picker *picker)
{
    puts("Discovered AFP servers\n");

    for (size_t i = 0; i < picker->count; i++) {
        const struct picker_entry *entry = &picker->entries[i];

        if (!picker_entry_active(entry)) {
            continue;
        }

        printf("  %u  %s", entry->slot, entry->service.instance);

        if (has_duplicate_name(picker, i)) {
            printf("  [%s]", entry->service.domain);
        }

        putchar('\n');
    }

    puts("\n  q  Quit\n");
    fputs("afpcmd: ", stdout);
    fflush(stdout);
}

static int drain_events(struct afpc_discovery *discovery,
                        struct picker *picker, int *changed)
{
    struct afpc_discovery_event event;
    int ret;

    while ((ret = afpc_discovery_next(discovery, &event, 0)) > 0) {
        if (event.type == AFPC_DISCOVERY_EVENT_ERROR) {
            if (event.message[0] != '\0') {
                fprintf(stderr, "Discovery failed: %s\n", event.message);
            }

            if (event.error == 0) {
                return -EIO;
            }

            return event.error > 0 ? -event.error : event.error;
        }

        if (strcasecmp(event.service.type,
                       AFPC_DISCOVERY_AFP_SERVICE_TYPE) != 0) {
            continue;
        }

        ret = update_picker(picker, &event);

        if (ret < 0) {
            return ret;
        }

        *changed |= ret;
    }

    return ret;
}

static int endpoint_score(const struct afpc_discovery_endpoint *endpoint)
{
    if (endpoint->address_len == 0) {
        return endpoint->target[0] == '\0' ? -1 : 1;
    }

    if (endpoint->address.ss_family == AF_INET) {
        const struct sockaddr_in *address =
            (const struct sockaddr_in *)&endpoint->address;
        uint32_t value = ntohl(address->sin_addr.s_addr);
        return (value & 0xff000000U) == 0x7f000000U ? 30 : 130;
    }

    if (endpoint->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *address =
            (const struct sockaddr_in6 *)&endpoint->address;
        return IN6_IS_ADDR_LOOPBACK(&address->sin6_addr) ? 20 : 120;
    }

    return -1;
}

static int make_service_url(struct afpc_discovery *discovery,
                            const struct picker_entry *entry,
                            char *url, size_t url_size)
{
    char best_host[AFPC_DISCOVERY_TARGET_LEN + IF_NAMESIZE + 2] = {0};
    char resolved_target[AFPC_DISCOVERY_TARGET_LEN] = {0};
    uint16_t resolved_port = 0;
    int best_score = -1;
    int last_error = -EHOSTUNREACH;

    for (size_t i = 0; i < entry->member_count; i++) {
        struct afpc_discovery_endpoint *endpoints = NULL;
        size_t endpoint_count = 0;
        int ret;

        if (!entry->members[i].active) {
            continue;
        }

        ret = afpc_discovery_resolve(discovery,
                                     &entry->members[i].service,
                                     &endpoints, &endpoint_count,
                                     PICKER_RESOLVE_TIMEOUT_MS);

        if (ret != 0) {
            last_error = ret;
            afpc_discovery_free_endpoints(&endpoints, endpoint_count);
            continue;
        }

        for (size_t j = 0; j < endpoint_count; j++) {
            char endpoint_host[AFPC_DISCOVERY_TARGET_LEN + IF_NAMESIZE + 2];
            int score;

            if (resolved_port == 0) {
                snprintf(resolved_target, sizeof(resolved_target), "%s",
                         endpoints[j].target);
                resolved_port = endpoints[j].port;
            } else if (resolved_port != endpoints[j].port
                       || strcasecmp(resolved_target,
                                     endpoints[j].target) != 0) {
                afpc_discovery_free_endpoints(&endpoints, endpoint_count);
                return -EEXIST;
            }

            score = endpoint_score(&endpoints[j]);

            if (score <= best_score
                    || afpc_discovery_endpoint_host(&endpoints[j],
                                                    endpoint_host,
                                                    sizeof(endpoint_host)) != 0) {
                continue;
            }

            memcpy(best_host, endpoint_host, strlen(endpoint_host) + 1);
            best_score = score;
        }

        afpc_discovery_free_endpoints(&endpoints, endpoint_count);
    }

    if (best_score < 0 || resolved_port == 0) {
        return last_error;
    }

    int length;

    if (strchr(best_host, ':')) {
        length = snprintf(url, url_size, "afp://[%s]:%u", best_host,
                          resolved_port);
    } else {
        length = snprintf(url, url_size, "afp://%s:%u", best_host,
                          resolved_port);
    }

    return length < 0 || (size_t)length >= url_size
           ? -ENAMETOOLONG : 0;
}

static int parse_slot(char *line, unsigned long *slot)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(line, &end, 10);

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }

    if (errno != 0 || end == line || *end != '\0' || value == 0) {
        return -EINVAL;
    }

    *slot = value;
    return 0;
}

int cmdline_discover_url(char *url, size_t url_size)
{
    struct afpc_discovery *discovery = NULL;
    struct picker picker = { .next_slot = 1 };
    char line[64];
    int changed = 0;
    int ret;

    if (!url || url_size == 0) {
        return -EINVAL;
    }

    ret = afpc_discovery_open(&discovery, NULL);

    if (ret != 0) {
        return ret;
    }

    for (;;) {
        fd_set read_fds;
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = PICKER_INTERVAL_MS * 1000,
        };
        unsigned long slot;
        struct picker_entry *entry;
        ret = drain_events(discovery, &picker, &changed);

        if (ret < 0) {
            break;
        }

        if (changed) {
            render_picker(&picker);
            changed = 0;
        }

        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        ret = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            ret = -errno;
            break;
        }

        if (ret == 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
            continue;
        }

        if (!fgets(line, sizeof(line), stdin)
                || line[0] == 'q' || line[0] == 'Q') {
            ret = 1;
            break;
        }

        if (parse_slot(line, &slot) != 0
                || !(entry = find_slot(&picker, slot))) {
            fputs("Choose a listed service number, or q to quit.\n",
                  stderr);
            render_picker(&picker);
            continue;
        }

        ret = make_service_url(discovery, entry, url, url_size);

        if (ret == 0) {
            break;
        }

        if (ret == -EEXIST) {
            fprintf(stderr,
                    "Could not resolve %s: advertisements disagree on "
                    "target or port\n",
                    entry->service.instance);
        } else {
            fprintf(stderr, "Could not resolve %s: %s\n",
                    entry->service.instance, strerror(-ret));
        }

        changed = 1;
    }

    for (size_t i = 0; i < picker.count; i++) {
        free(picker.entries[i].members);
    }

    free(picker.entries);
    afpc_discovery_close(&discovery);
    return ret;
}
