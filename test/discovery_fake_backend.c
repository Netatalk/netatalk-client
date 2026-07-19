#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "discovery/backend.h"

struct fake_context {
    unsigned int step;
};

static void set_service(struct afpc_discovery_backend_event *event,
                        unsigned int interface_index)
{
    memcpy(event->service.instance, "Office\nMac", sizeof("Office\nMac"));
    memcpy(event->service.type, AFPC_DISCOVERY_AFP_SERVICE_TYPE,
           sizeof(AFPC_DISCOVERY_AFP_SERVICE_TYPE));
    memcpy(event->service.domain, "local.", sizeof("local."));
    event->service.interface_index = interface_index;
}

static int fake_start(void **context_ptr,
                      struct afpc_discovery *discovery,
                      const struct afpc_discovery_options *options)
{
    (void)discovery;
    (void)options;
    *context_ptr = calloc(1, sizeof(struct fake_context));
    return *context_ptr ? 0 : -ENOMEM;
}

static int fake_iterate(void *context_ptr,
                        struct afpc_discovery *discovery, int timeout_ms)
{
    struct fake_context *context = context_ptr;
    struct afpc_discovery_backend_event event;
    (void)timeout_ms;
    memset(&event, 0, sizeof(event));

    switch (context->step++) {
    case 0:
        event.type = AFPC_DISCOVERY_EVENT_ADD;
        event.source = AFPC_DISCOVERY_SOURCE_IPV4;
        set_service(&event, 1);
        break;

    case 1:
        event.type = AFPC_DISCOVERY_EVENT_ADD;
        event.source = AFPC_DISCOVERY_SOURCE_IPV6;
        set_service(&event, 1);
        break;

    case 2:
        event.type = AFPC_DISCOVERY_EVENT_ADD;
        event.source = AFPC_DISCOVERY_SOURCE_IPV4;
        set_service(&event, 2);
        break;

    case 3:
        event.type = AFPC_DISCOVERY_EVENT_REMOVE;
        event.source = AFPC_DISCOVERY_SOURCE_IPV4;
        set_service(&event, 1);
        break;

    case 4:
        event.type = AFPC_DISCOVERY_EVENT_REMOVE;
        event.source = AFPC_DISCOVERY_SOURCE_IPV6;
        set_service(&event, 1);
        break;

    case 5:
        event.type = AFPC_DISCOVERY_EVENT_ERROR;
        event.error = EIO;
        event.message = "provider\tfailure";
        break;

    default:
        return 0;
    }

    return afpc_discovery_backend_emit(discovery, &event) == 0 ? 1 : -EIO;
}

static int fake_resolve(void *context_ptr,
                        struct afpc_discovery *discovery,
                        const struct afpc_discovery_service *service,
                        struct afpc_discovery_endpoint **endpoints,
                        size_t *endpoint_count, int timeout_ms)
{
    static const unsigned char txt[] = { 3, 'k', '=', 'v' };
    (void)context_ptr;
    (void)discovery;
    (void)service;
    (void)timeout_ms;
    *endpoints = calloc(1, sizeof(**endpoints));

    if (!*endpoints) {
        return -ENOMEM;
    }

    memcpy((*endpoints)[0].target, "office.local.",
           sizeof("office.local."));
    (*endpoints)[0].port = 1548;
    (*endpoints)[0].interface_index = 1;
    (*endpoints)[0].txt = malloc(sizeof(txt));

    if (!(*endpoints)[0].txt) {
        free(*endpoints);
        *endpoints = NULL;
        return -ENOMEM;
    }

    memcpy((*endpoints)[0].txt, txt, sizeof(txt));
    (*endpoints)[0].txt_len = sizeof(txt);
    *endpoint_count = 1;
    return 0;
}

static void fake_stop(void *context)
{
    free(context);
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "fake",
    .start = fake_start,
    .iterate = fake_iterate,
    .resolve = fake_resolve,
    .stop = fake_stop,
};
