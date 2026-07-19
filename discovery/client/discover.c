/*
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "discovery/discovery.h"

#include "discovery/client/discover.h"

#define DISCOVERY_DEFAULT_TIMEOUT_MS 1500
#define DISCOVERY_RESOLVE_TIMEOUT_MS 1000
#define DISCOVERY_DEVICE_TYPE_LEN 256

struct resolved_service {
    struct afpc_discovery_service service;
    struct afpc_discovery_endpoint *endpoints;
    size_t endpoint_count;
    int resolve_error;
    char device_type[DISCOVERY_DEVICE_TYPE_LEN];
};

static void discover_usage(FILE *stream)
{
    fputs("Usage: afp_client discover [--verbose | --json] "
          "[--timeout milliseconds]\n",
          stream);
}

static int service_compare(const void *left_ptr, const void *right_ptr)
{
    const struct afpc_discovery_service *left = left_ptr;
    const struct afpc_discovery_service *right = right_ptr;
    int result = strcmp(left->instance, right->instance);

    if (result == 0) {
        result = strcmp(left->domain, right->domain);
    }

    if (result == 0) {
        if (left->interface_index < right->interface_index) {
            result = -1;
        } else if (left->interface_index > right->interface_index) {
            result = 1;
        }
    }

    return result;
}

static void print_json_string(FILE *stream, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', stream);

    while (*p) {
        switch (*p) {
        case '"':
            fputs("\\\"", stream);
            break;

        case '\\':
            fputs("\\\\", stream);
            break;

        case '\b':
            fputs("\\b", stream);
            break;

        case '\f':
            fputs("\\f", stream);
            break;

        case '\n':
            fputs("\\n", stream);
            break;

        case '\r':
            fputs("\\r", stream);
            break;

        case '\t':
            fputs("\\t", stream);
            break;

        default:
            if (*p < 0x20) {
                fprintf(stream, "\\u%04x", *p);
            } else {
                fputc(*p, stream);
            }

            break;
        }

        p++;
    }

    fputc('"', stream);
}

static void print_hex(FILE *stream, const unsigned char *data, size_t size)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < size; i++) {
        fputc(digits[data[i] >> 4], stream);
        fputc(digits[data[i] & 0x0f], stream);
    }
}

static const char *interface_text(unsigned int interface_index,
                                  char *buffer, size_t buffer_size)
{
    if (interface_index != 0
            && if_indextoname(interface_index, buffer)) {
        return buffer;
    }

    snprintf(buffer, buffer_size, "%u", interface_index);
    return buffer;
}

static int service_has_type(const struct afpc_discovery_service *service,
                            const char *type)
{
    return strcasecmp(service->type, type) == 0;
}

static int is_afp_service(const struct afpc_discovery_service *service)
{
    return service_has_type(service, AFPC_DISCOVERY_AFP_SERVICE_TYPE);
}

static int is_device_info_service(
    const struct afpc_discovery_service *service)
{
    return service_has_type(service,
                            AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE);
}

static int txt_key_equal(const unsigned char *item, size_t item_len,
                         const char *key)
{
    size_t key_len = strlen(key);

    if (item_len < key_len + 1 || item[key_len] != '=') {
        return 0;
    }

    for (size_t i = 0; i < key_len; i++) {
        if (tolower(item[i]) != tolower((unsigned char)key[i])) {
            return 0;
        }
    }

    return 1;
}

static void extract_device_type(const unsigned char *txt, size_t txt_len,
                                char *device_type, size_t device_type_size)
{
    size_t offset = 0;

    if (!txt || !device_type || device_type_size == 0) {
        return;
    }

    while (offset < txt_len) {
        size_t item_len = txt[offset++];
        const unsigned char *item;
        size_t value_len;

        if (item_len > txt_len - offset) {
            return;
        }

        item = txt + offset;
        offset += item_len;

        if (!txt_key_equal(item, item_len, "model")) {
            continue;
        }

        value_len = item_len - (sizeof("model=") - 1);

        if (value_len >= device_type_size) {
            value_len = device_type_size - 1;
        }

        for (size_t i = 0; i < value_len; i++) {
            unsigned char ch = item[sizeof("model=") - 1 + i];
            device_type[i] = (ch < 0x20 || ch == 0x7f) ? '?' : (char)ch;
        }

        device_type[value_len] = '\0';
        return;
    }
}

static int corresponding_service_records(
    const struct afpc_discovery_service *afp,
    const struct afpc_discovery_service *device_info)
{
    return is_afp_service(afp)
           && service_has_type(device_info,
                               AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE)
           && strcmp(afp->instance, device_info->instance) == 0
           && strcasecmp(afp->domain, device_info->domain) == 0
           && afp->interface_index == device_info->interface_index;
}

static int corresponding_device_info(
    const struct resolved_service *afp,
    const struct resolved_service *device_info)
{
    return corresponding_service_records(&afp->service,
                                         &device_info->service);
}

static int has_corresponding_afp(
    const struct afpc_discovery_service *services, size_t count,
    const struct afpc_discovery_service *device_info)
{
    for (size_t i = 0; i < count; i++) {
        if (corresponding_service_records(&services[i], device_info)) {
            return 1;
        }
    }

    return 0;
}

static int same_service_identity(const struct resolved_service *left,
                                 const struct resolved_service *right)
{
    return strcmp(left->service.instance, right->service.instance) == 0
           && strcasecmp(left->service.type, right->service.type) == 0
           && strcasecmp(left->service.domain, right->service.domain) == 0;
}

static int same_logical_service(const struct resolved_service *left,
                                const struct resolved_service *right)
{
    if (!same_service_identity(left, right)
            || left->resolve_error != 0 || right->resolve_error != 0) {
        return 0;
    }

    return left->endpoint_count != 0 && right->endpoint_count != 0
           && left->endpoints[0].port == right->endpoints[0].port
           && strcasecmp(left->endpoints[0].target,
                         right->endpoints[0].target) == 0;
}

static void normalize_device_types(struct resolved_service *services,
                                   size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!is_device_info_service(&services[i].service)
                || services[i].resolve_error != 0
                || services[i].endpoint_count == 0) {
            continue;
        }

        extract_device_type(services[i].endpoints[0].txt,
                            services[i].endpoints[0].txt_len,
                            services[i].device_type,
                            sizeof(services[i].device_type));
    }

    for (size_t i = 0; i < count; i++) {
        if (!is_afp_service(&services[i].service)) {
            continue;
        }

        for (size_t j = 0; j < count; j++) {
            if (!corresponding_device_info(&services[i], &services[j])
                    || services[j].device_type[0] == '\0') {
                continue;
            }

            memcpy(services[i].device_type, services[j].device_type,
                   sizeof(services[i].device_type));
            break;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (!is_afp_service(&services[i].service)
                || services[i].device_type[0] != '\0') {
            continue;
        }

        for (size_t j = 0; j < count; j++) {
            if (services[j].device_type[0] != '\0'
                    && same_logical_service(&services[i], &services[j])) {
                memcpy(services[i].device_type, services[j].device_type,
                       sizeof(services[i].device_type));
                break;
            }
        }
    }
}

static size_t afp_service_count(const struct resolved_service *services,
                                size_t count)
{
    size_t result = 0;

    for (size_t i = 0; i < count; i++) {
        result += is_afp_service(&services[i].service) ? 1 : 0;
    }

    return result;
}

static size_t verbose_service_count(const struct resolved_service *services,
                                    size_t count)
{
    size_t result = 0;

    for (size_t i = 0; i < count; i++) {
        result += is_afp_service(&services[i].service)
                  || is_device_info_service(&services[i].service) ? 1 : 0;
    }

    return result;
}

static void print_human_target(const char *target)
{
    size_t length = strlen(target);

    if (length > 0 && target[length - 1] == '.') {
        length--;
    }

    printf("%-36.*s ", (int)length, target);
}

static void print_human(const struct resolved_service *services, size_t count,
                        int verbose)
{
    size_t i;

    if ((!verbose && afp_service_count(services, count) == 0)
            || (verbose && verbose_service_count(services, count) == 0)) {
        puts("No AFP services found.");
        return;
    }

    if (!verbose) {
        printf("%-30s %-36s %5s   %-24s\n", "NAME", "TARGET", "PORT",
               "MODEL");

        for (i = 0; i < count; i++) {
            const struct afpc_discovery_endpoint *endpoint =
                    services[i].endpoint_count ? &services[i].endpoints[0] : NULL;
            int duplicate = 0;

            if (!is_afp_service(&services[i].service)) {
                continue;
            }

            if (services[i].resolve_error != 0) {
                for (size_t j = 0; j < count; j++) {
                    if (j != i && services[j].resolve_error == 0
                            && same_service_identity(&services[i],
                                                     &services[j])) {
                        duplicate = 1;
                        break;
                    }
                }

                for (size_t j = 0; j < i; j++) {
                    if (services[j].resolve_error != 0
                            && same_service_identity(&services[i],
                                                     &services[j])) {
                        duplicate = 1;
                        break;
                    }
                }
            } else {
                for (size_t j = 0; j < i; j++) {
                    if (same_logical_service(&services[i], &services[j])) {
                        duplicate = 1;
                        break;
                    }
                }
            }

            if (duplicate) {
                continue;
            }

            printf("%-30s ", services[i].service.instance);
            print_human_target(endpoint ? endpoint->target : "-");

            if (endpoint) {
                printf("%5u", endpoint->port);
            } else {
                printf("%5s", "-");
            }

            printf("   %-24s", services[i].device_type[0]
                   ? services[i].device_type : "-");
            putchar('\n');
        }

        return;
    }

    size_t printed = 0;

    for (i = 0; i < count; i++) {
        char interface_name[IF_NAMESIZE];

        if (!is_afp_service(&services[i].service)
                && !is_device_info_service(&services[i].service)) {
            continue;
        }

        if (printed != 0) {
            putchar('\n');
        }

        printed++;
        printf("Name:       %s\n", services[i].service.instance);
        printf("Type:       %s\n", services[i].service.type);
        printf("Device:     %s\n", services[i].device_type[0]
               ? services[i].device_type : "-");
        printf("Domain:     %s\n", services[i].service.domain);
        printf("Interface:  %s\n",
               interface_text(services[i].service.interface_index,
                              interface_name, sizeof(interface_name)));

        if (services[i].resolve_error != 0) {
            printf("Resolution: %s\n", strerror(-services[i].resolve_error));
            continue;
        }

        printf("Target:     %s\n", services[i].endpoints[0].target);
        printf("Port:       %u\n", services[i].endpoints[0].port);
        fputs("Addresses:  ", stdout);
        size_t addresses_printed = 0;

        for (size_t j = 0; j < services[i].endpoint_count; j++) {
            char address[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];

            if (afpc_discovery_endpoint_host(&services[i].endpoints[j],
                                             address,
                                             sizeof(address)) != 0) {
                continue;
            }

            if (addresses_printed != 0) {
                fputs(", ", stdout);
            }

            fputs(address, stdout);
            addresses_printed++;
        }

        putchar('\n');

        if (services[i].endpoints[0].txt_len != 0) {
            fputs("TXT (hex):  ", stdout);
            print_hex(stdout, services[i].endpoints[0].txt,
                      services[i].endpoints[0].txt_len);
            putchar('\n');
        }
    }
}

static void print_json(const struct resolved_service *services, size_t count)
{
    size_t services_printed = 0;
    fputs("{\"backend\":", stdout);
    print_json_string(stdout, afpc_discovery_backend_name());
    fputs(",\"services\":[", stdout);

    for (size_t i = 0; i < count; i++) {
        if (!is_afp_service(&services[i].service)) {
            continue;
        }

        if (services_printed != 0) {
            putchar(',');
        }

        services_printed++;
        fputs("{\"name\":", stdout);
        print_json_string(stdout, services[i].service.instance);
        fputs(",\"type\":", stdout);
        print_json_string(stdout, services[i].service.type);
        fputs(",\"device_type\":", stdout);

        if (services[i].device_type[0] != '\0') {
            print_json_string(stdout, services[i].device_type);
        } else {
            fputs("null", stdout);
        }

        fputs(",\"domain\":", stdout);
        print_json_string(stdout, services[i].service.domain);
        printf(",\"interface_index\":%u",
               services[i].service.interface_index);

        if (services[i].resolve_error != 0) {
            printf(",\"resolve_error\":%d", -services[i].resolve_error);
        } else {
            fputs(",\"target\":", stdout);
            print_json_string(stdout, services[i].endpoints[0].target);
            printf(",\"port\":%u,\"addresses\":[",
                   services[i].endpoints[0].port);
            size_t addresses_printed = 0;

            for (size_t j = 0; j < services[i].endpoint_count; j++) {
                char address[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];

                if (afpc_discovery_endpoint_host(&services[i].endpoints[j],
                                                 address,
                                                 sizeof(address)) != 0) {
                    continue;
                }

                if (addresses_printed != 0) {
                    putchar(',');
                }

                print_json_string(stdout, address);
                addresses_printed++;
            }

            fputs("],\"txt_hex\":\"", stdout);
            print_hex(stdout, services[i].endpoints[0].txt,
                      services[i].endpoints[0].txt_len);
            putchar('"');
        }

        putchar('}');
    }

    fputs("]}\n", stdout);
}

static void free_resolved_services(struct resolved_service *services,
                                   size_t count)
{
    if (!services) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        afpc_discovery_free_endpoints(&services[i].endpoints,
                                      services[i].endpoint_count);
    }

    free(services);
}

static int parse_timeout(const char *text, int *timeout_ms)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);

    if (errno != 0 || *text == '\0' || *end != '\0'
            || value < 0 || value > 60000) {
        return -1;
    }

    *timeout_ms = (int)value;
    return 0;
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

int afpc_discover_resolve_service(const char *name, char *host,
                                  size_t host_size, uint16_t *port,
                                  int timeout_ms)
{
    struct afpc_discovery *discovery = NULL;
    struct afpc_discovery_service *services = NULL;
    size_t service_count = 0;
    const char *domain = NULL;
    char resolved_target[AFPC_DISCOVERY_TARGET_LEN] = {0};
    uint16_t resolved_port = 0;
    int best_score = -1;
    int last_error = -EHOSTUNREACH;
    int match_count = 0;
    int ret;

    if (!name || name[0] == '\0' || !host || host_size == 0 || !port
            || timeout_ms < 0) {
        return -EINVAL;
    }

    host[0] = '\0';
    *port = 0;
    ret = afpc_discovery_open(&discovery, NULL);

    if (ret != 0) {
        return ret;
    }

    ret = afpc_discovery_snapshot(discovery, &services, &service_count,
                                  timeout_ms);

    if (ret != 0) {
        goto done;
    }

    for (size_t i = 0; i < service_count; i++) {
        if (!is_afp_service(&services[i])
                || strcmp(services[i].instance, name) != 0) {
            continue;
        }

        match_count++;

        if (!domain) {
            domain = services[i].domain;
        } else if (strcasecmp(domain, services[i].domain) != 0) {
            ret = -EEXIST;
            goto done;
        }
    }

    if (match_count == 0) {
        ret = -ENOENT;
        goto done;
    }

    for (size_t i = 0; i < service_count; i++) {
        struct afpc_discovery_endpoint *endpoints = NULL;
        size_t endpoint_count = 0;

        if (!is_afp_service(&services[i])
                || strcmp(services[i].instance, name) != 0) {
            continue;
        }

        ret = afpc_discovery_resolve(discovery, &services[i], &endpoints,
                                     &endpoint_count,
                                     DISCOVERY_RESOLVE_TIMEOUT_MS);

        if (ret != 0) {
            last_error = ret;
            afpc_discovery_free_endpoints(&endpoints, endpoint_count);
            continue;
        }

        for (size_t j = 0; j < endpoint_count; j++) {
            int score;
            char endpoint_host[AFPC_DISCOVERY_TARGET_LEN + IF_NAMESIZE + 2];

            if (resolved_port == 0) {
                snprintf(resolved_target, sizeof(resolved_target), "%s",
                         endpoints[j].target);
                resolved_port = endpoints[j].port;
            } else if (resolved_port != endpoints[j].port
                       || strcasecmp(resolved_target,
                                     endpoints[j].target) != 0) {
                afpc_discovery_free_endpoints(&endpoints, endpoint_count);
                ret = -EEXIST;
                goto done;
            }

            score = endpoint_score(&endpoints[j]);

            if (score <= best_score
                    || afpc_discovery_endpoint_host(&endpoints[j],
                                                    endpoint_host,
                                                    sizeof(endpoint_host)) != 0) {
                continue;
            }

            if (strlen(endpoint_host) >= host_size) {
                last_error = -ENAMETOOLONG;
                continue;
            }

            memcpy(host, endpoint_host, strlen(endpoint_host) + 1);
            best_score = score;
        }

        afpc_discovery_free_endpoints(&endpoints, endpoint_count);
    }

    if (best_score < 0 || resolved_port == 0) {
        ret = last_error;
        goto done;
    }

    *port = resolved_port;
    ret = 0;
done:
    afpc_discovery_free_services(&services);
    afpc_discovery_close(&discovery);
    return ret;
}

int afpc_discover_command(int argc, char **argv)
{
    struct afpc_discovery *discovery = NULL;
    struct afpc_discovery_service *services = NULL;
    struct resolved_service *resolved = NULL;
    size_t service_count = 0;
    int timeout_ms = DISCOVERY_DEFAULT_TIMEOUT_MS;
    int verbose = 0;
    int json = 0;
    int ret;

    for (int argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--verbose") == 0
                || strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[argi], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[argi], "--timeout") == 0) {
            if (++argi >= argc || parse_timeout(argv[argi], &timeout_ms) != 0) {
                fprintf(stderr, "Invalid discovery timeout\n");
                discover_usage(stderr);
                return 2;
            }
        } else if (strcmp(argv[argi], "--help") == 0
                   || strcmp(argv[argi], "-h") == 0) {
            discover_usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "Unknown discover option: %s\n", argv[argi]);
            discover_usage(stderr);
            return 2;
        }
    }

    if (verbose && json) {
        fputs("--verbose and --json cannot be combined\n", stderr);
        return 2;
    }

    ret = afpc_discovery_open(&discovery, NULL);

    if (ret != 0) {
        if (ret == -ENOTSUP) {
            fputs("Zeroconf discovery support was not built.\n", stderr);
        } else {
            fprintf(stderr, "Could not start Zeroconf discovery: %s\n",
                    strerror(-ret));
        }

        return 1;
    }

    ret = afpc_discovery_snapshot(discovery, &services, &service_count,
                                  timeout_ms);

    if (ret != 0) {
        fprintf(stderr, "Zeroconf discovery failed: %s\n", strerror(-ret));
        afpc_discovery_close(&discovery);
        return 1;
    }

    if (service_count > 1) {
        qsort(services, service_count, sizeof(*services), service_compare);
    }

    if (service_count != 0) {
        resolved = calloc(service_count, sizeof(*resolved));

        if (!resolved) {
            afpc_discovery_free_services(&services);
            afpc_discovery_close(&discovery);
            fputs("Out of memory resolving discovered services\n", stderr);
            return 1;
        }
    }

    for (size_t i = 0; i < service_count; i++) {
        resolved[i].service = services[i];

        if (!is_afp_service(&services[i])
                && !(verbose && is_device_info_service(&services[i]))
                && !has_corresponding_afp(services, service_count,
                                          &services[i])) {
            continue;
        }

        resolved[i].resolve_error = afpc_discovery_resolve(
                                        discovery, &services[i],
                                        &resolved[i].endpoints,
                                        &resolved[i].endpoint_count,
                                        DISCOVERY_RESOLVE_TIMEOUT_MS);
    }

    normalize_device_types(resolved, service_count);

    if (json) {
        print_json(resolved, service_count);
    } else {
        print_human(resolved, service_count, verbose);
    }

    free_resolved_services(resolved, service_count);
    afpc_discovery_free_services(&services);
    afpc_discovery_close(&discovery);
    return 0;
}
