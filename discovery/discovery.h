#ifndef NETATALK_CLIENT_DISCOVERY_H
#define NETATALK_CLIENT_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AFPC_DISCOVERY_INSTANCE_LEN 64
#define AFPC_DISCOVERY_TYPE_LEN 64
#define AFPC_DISCOVERY_DOMAIN_LEN 256
#define AFPC_DISCOVERY_TARGET_LEN 256
#define AFPC_DISCOVERY_ERROR_LEN 256

struct afpc_discovery;

struct afpc_discovery_options {
    /* NULL browses AFP and its companion device-info advertisements. */
    const char *service_type;
    const char *domain;
    unsigned int interface_index;
};

struct afpc_discovery_service {
    char instance[AFPC_DISCOVERY_INSTANCE_LEN];
    char type[AFPC_DISCOVERY_TYPE_LEN];
    char domain[AFPC_DISCOVERY_DOMAIN_LEN];
    unsigned int interface_index;
};

struct afpc_discovery_endpoint {
    char target[AFPC_DISCOVERY_TARGET_LEN];
    uint16_t port;
    struct sockaddr_storage address;
    socklen_t address_len;
    unsigned int interface_index;
    unsigned char *txt;
    size_t txt_len;
};

enum afpc_discovery_event_type {
    AFPC_DISCOVERY_EVENT_ADD,
    AFPC_DISCOVERY_EVENT_UPDATE,
    AFPC_DISCOVERY_EVENT_REMOVE,
    AFPC_DISCOVERY_EVENT_ERROR,
};

struct afpc_discovery_event {
    enum afpc_discovery_event_type type;
    struct afpc_discovery_service service;
    int error;
    char message[AFPC_DISCOVERY_ERROR_LEN];
};

/* Return values use negative errno values. afpc_discovery_next() returns one
 * when an event was produced and zero when the timeout expired. */
int afpc_discovery_open(struct afpc_discovery **discovery,
                        const struct afpc_discovery_options *options);
int afpc_discovery_next(struct afpc_discovery *discovery,
                        struct afpc_discovery_event *event,
                        int timeout_ms);
int afpc_discovery_resolve(struct afpc_discovery *discovery,
                           const struct afpc_discovery_service *service,
                           struct afpc_discovery_endpoint **endpoints,
                           size_t *endpoint_count, int timeout_ms);
int afpc_discovery_snapshot(struct afpc_discovery *discovery,
                            struct afpc_discovery_service **services,
                            size_t *service_count, int timeout_ms);
void afpc_discovery_free_services(struct afpc_discovery_service **services);
void afpc_discovery_free_endpoints(
    struct afpc_discovery_endpoint **endpoints, size_t endpoint_count);
int afpc_discovery_endpoint_host(
    const struct afpc_discovery_endpoint *endpoint,
    char *host, size_t host_size);
void afpc_discovery_close(struct afpc_discovery **discovery);
const char *afpc_discovery_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif
