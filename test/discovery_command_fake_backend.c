#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "discovery/backend.h"

struct command_fake_context {
    int emitted;
};

static int command_fake_start(
    void **context_ptr, struct afpc_discovery *discovery,
    const struct afpc_discovery_options *options)
{
    (void)discovery;

    if (options->service_type != NULL) {
        return -EINVAL;
    }

    *context_ptr = calloc(1, sizeof(struct command_fake_context));
    return *context_ptr ? 0 : -ENOMEM;
}

static int command_fake_iterate(void *context_ptr,
                                struct afpc_discovery *discovery,
                                int timeout_ms)
{
    struct command_fake_context *context = context_ptr;
    struct afpc_discovery_backend_event event;
    (void)timeout_ms;

    if (context->emitted >= 4) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.type = AFPC_DISCOVERY_EVENT_ADD;
    event.source = AFPC_DISCOVERY_SOURCE_IPV4;
    memcpy(event.service.instance, "Office \"Mac\"",
           sizeof("Office \"Mac\""));

    if (context->emitted == 3) {
        memcpy(event.service.instance, "Apple AFP", sizeof("Apple AFP"));
    }

    if (context->emitted < 2) {
        snprintf(event.service.type, sizeof(event.service.type), "%s.",
                 AFPC_DISCOVERY_AFP_SERVICE_TYPE);
    } else {
        snprintf(event.service.type, sizeof(event.service.type), "%s.",
                 AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE);
    }

    memcpy(event.service.domain, "local.", sizeof("local."));
    event.service.interface_index = (unsigned int)(context->emitted % 2 + 1);
    context->emitted++;
    return afpc_discovery_backend_emit(discovery, &event) == 0 ? 1 : -EIO;
}

static int command_fake_resolve(
    void *context_ptr, struct afpc_discovery *discovery,
    const struct afpc_discovery_service *service,
    struct afpc_discovery_endpoint **endpoints, size_t *endpoint_count,
    int timeout_ms)
{
    struct sockaddr_in *address;
    static const unsigned char afp_txt[] = { 3, 'k', '=', 'v' };
    static const unsigned char device_txt[] = {
        13, 'm', 'o', 'd', 'e', 'l', '=', 'M', 'a', 'c', 'm', 'i', 'n', 'i',
    };
    static const unsigned char apple_device_txt[] = {
        20, 'm', 'o', 'd', 'e', 'l', '=', 'M', 'a', 'c', 'B', 'o', 'o', 'k',
        'P', 'r', 'o', '1', '8', ',', '3',
    };
    const unsigned char *txt;
    size_t txt_len;
    (void)context_ptr;
    (void)discovery;
    (void)timeout_ms;
    *endpoints = calloc(1, sizeof(**endpoints));

    if (!*endpoints) {
        return -ENOMEM;
    }

    memcpy((*endpoints)[0].target, "office.local.",
           sizeof("office.local."));
    (*endpoints)[0].port =
        strcasecmp(service->type, AFPC_DISCOVERY_AFP_SERVICE_TYPE) == 0
        ? 548 : 0;
    address = (struct sockaddr_in *) & (*endpoints)[0].address;
    address->sin_family = AF_INET;
    address->sin_port = htons((*endpoints)[0].port);
    inet_pton(AF_INET, "192.0.2.10", &address->sin_addr);
    (*endpoints)[0].address_len = sizeof(*address);

    if (strcasecmp(service->type,
                   AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE) != 0) {
        txt = afp_txt;
        txt_len = sizeof(afp_txt);
    } else if (strcmp(service->instance, "Apple AFP") == 0) {
        txt = apple_device_txt;
        txt_len = sizeof(apple_device_txt);
    } else {
        txt = device_txt;
        txt_len = sizeof(device_txt);
    }

    (*endpoints)[0].txt = malloc(txt_len);

    if (!(*endpoints)[0].txt) {
        free(*endpoints);
        *endpoints = NULL;
        return -ENOMEM;
    }

    memcpy((*endpoints)[0].txt, txt, txt_len);
    (*endpoints)[0].txt_len = txt_len;
    *endpoint_count = 1;
    return 0;
}

static void command_fake_stop(void *context)
{
    free(context);
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "fake-command",
    .start = command_fake_start,
    .iterate = command_fake_iterate,
    .resolve = command_fake_resolve,
    .stop = command_fake_stop,
};
