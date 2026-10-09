#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discovery/backend.h"

struct service_fake_context {
    unsigned int next_event;
};

static int service_fake_start(
    void **context_ptr, struct afpc_discovery *discovery,
    const struct afpc_discovery_options *options)
{
    (void)discovery;
    (void)options;
    *context_ptr = calloc(1, sizeof(struct service_fake_context));
    return *context_ptr ? 0 : -ENOMEM;
}

static int emit_service(struct afpc_discovery *discovery, const char *name,
                        const char *domain, unsigned int interface_index)
{
    struct afpc_discovery_backend_event event;
    memset(&event, 0, sizeof(event));
    event.type = AFPC_DISCOVERY_EVENT_ADD;
    event.source = AFPC_DISCOVERY_SOURCE_UNSPEC;
    snprintf(event.service.instance, sizeof(event.service.instance), "%s",
             name);
    snprintf(event.service.type, sizeof(event.service.type), "%s",
             AFPC_DISCOVERY_AFP_SERVICE_TYPE);
    snprintf(event.service.domain, sizeof(event.service.domain), "%s",
             domain);
    event.service.interface_index = interface_index;
    return afpc_discovery_backend_emit(discovery, &event) == 0 ? 1 : -EIO;
}

static int service_fake_iterate(void *context_ptr,
                                struct afpc_discovery *discovery,
                                int timeout_ms)
{
    struct service_fake_context *context = context_ptr;
    (void)timeout_ms;

    switch (context->next_event++) {
    case 0:
        return emit_service(discovery, "Multi", "local.", 1);

    case 1:
        return emit_service(discovery, "Multi", "local.", 2);

    case 2:
        return emit_service(discovery, "Ambiguous", "local.", 1);

    case 3:
        return emit_service(discovery, "Ambiguous", "example.", 1);

    default:
        return 0;
    }
}

static int service_fake_resolve(
    void *context_ptr, struct afpc_discovery *discovery,
    const struct afpc_discovery_service *service,
    struct afpc_discovery_endpoint **endpoints, size_t *endpoint_count,
    int timeout_ms)
{
    struct sockaddr_in *address;
    const char *numeric_address;
    (void)context_ptr;
    (void)discovery;
    (void)timeout_ms;
    *endpoints = calloc(1, sizeof(**endpoints));

    if (!*endpoints) {
        return -ENOMEM;
    }

    numeric_address = service->interface_index == 1
                      ? "127.0.0.1" : "192.0.2.20";
    snprintf((*endpoints)[0].target, sizeof((*endpoints)[0].target),
             "multi.local.");
    (*endpoints)[0].port = 1548;
    (*endpoints)[0].interface_index = service->interface_index;
    address = (struct sockaddr_in *) & (*endpoints)[0].address;
    address->sin_family = AF_INET;
    address->sin_port = htons(1548);
    inet_pton(AF_INET, numeric_address, &address->sin_addr);
    (*endpoints)[0].address_len = sizeof(*address);
    *endpoint_count = 1;
    return 0;
}

static void service_fake_stop(void *context)
{
    free(context);
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "fake-service",
    .start = service_fake_start,
    .iterate = service_fake_iterate,
    .resolve = service_fake_resolve,
    .stop = service_fake_stop,
};
