#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "discovery/discovery.h"
#include "tap.h"

int main(int argc, char **argv)
{
    struct afpc_discovery *discovery = NULL;
    struct afpc_discovery_event event;
    struct afpc_discovery_service selected;
    struct afpc_discovery_service *services = NULL;
    struct afpc_discovery_endpoint *endpoints = NULL;
    size_t service_count = 0;
    size_t endpoint_count = 0;
    int ret;
    test_tap_init(argc, argv);
    CHECK(strcmp(afpc_discovery_backend_name(), "fake") == 0);
    CHECK(afpc_discovery_open(&discovery, NULL) == 0);
    ret = afpc_discovery_next(discovery, &event, 10);
    CHECK(ret == 1 && event.type == AFPC_DISCOVERY_EVENT_ADD);
    CHECK(strcmp(event.service.instance, "Office?Mac") == 0);
    CHECK(event.service.interface_index == 1);
    selected = event.service;
    /* The duplicate IPv6 announcement is coalesced; the next visible event
     * is the same instance discovered on a second interface. */
    ret = afpc_discovery_next(discovery, &event, 10);
    CHECK(ret == 1 && event.type == AFPC_DISCOVERY_EVENT_ADD);
    CHECK(event.service.interface_index == 2);
    /* Removing IPv4 does not withdraw the IPv6-backed logical service. */
    ret = afpc_discovery_next(discovery, &event, 10);
    CHECK(ret == 1 && event.type == AFPC_DISCOVERY_EVENT_REMOVE);
    CHECK(event.service.interface_index == 1);
    ret = afpc_discovery_next(discovery, &event, 10);
    CHECK(ret == 1 && event.type == AFPC_DISCOVERY_EVENT_ERROR);
    CHECK(event.error == EIO);
    CHECK(strcmp(event.message, "provider?failure") == 0);
    CHECK(afpc_discovery_resolve(discovery, &selected, &endpoints,
                                 &endpoint_count, 10) == 0);
    CHECK(endpoint_count == 1);
    CHECK(strcmp(endpoints[0].target, "office.local.") == 0);
    CHECK(endpoints[0].port == 1548);
    CHECK(endpoints[0].txt_len == 4);
    afpc_discovery_free_endpoints(&endpoints, endpoint_count);
    CHECK(endpoints == NULL);
    CHECK(afpc_discovery_snapshot(discovery, &services, &service_count,
                                  0) == 0);
    CHECK(service_count == 1);
    CHECK(services[0].interface_index == 2);
    afpc_discovery_free_services(&services);
    CHECK(services == NULL);
    afpc_discovery_close(&discovery);
    CHECK(discovery == NULL);
    return test_tap_finish();
}
