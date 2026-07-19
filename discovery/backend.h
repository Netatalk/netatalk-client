#ifndef NETATALK_CLIENT_DISCOVERY_BACKEND_H
#define NETATALK_CLIENT_DISCOVERY_BACKEND_H

#include "discovery.h"

enum afpc_discovery_source {
    AFPC_DISCOVERY_SOURCE_UNSPEC = 1U << 0,
    AFPC_DISCOVERY_SOURCE_IPV4 = 1U << 1,
    AFPC_DISCOVERY_SOURCE_IPV6 = 1U << 2,
};

struct afpc_discovery_backend_event {
    enum afpc_discovery_event_type type;
    struct afpc_discovery_service service;
    unsigned int source;
    int error;
    const char *message;
};

struct afpc_discovery_backend_ops {
    const char *name;
    int (*start)(void **context, struct afpc_discovery *discovery,
                 const struct afpc_discovery_options *options);
    int (*iterate)(void *context, struct afpc_discovery *discovery,
                   int timeout_ms);
    int (*resolve)(void *context, struct afpc_discovery *discovery,
                   const struct afpc_discovery_service *service,
                   struct afpc_discovery_endpoint **endpoints,
                   size_t *endpoint_count, int timeout_ms);
    void (*stop)(void *context);
};

extern const struct afpc_discovery_backend_ops afpc_discovery_backend;

int afpc_discovery_backend_emit(
    struct afpc_discovery *discovery,
    const struct afpc_discovery_backend_event *event);

#endif
