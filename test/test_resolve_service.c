#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "discovery/client/discover.h"
#include "tap.h"

int main(int argc, char **argv)
{
    char host[256];
    uint16_t port;
    test_tap_init(argc, argv);
    CHECK(afpc_discover_resolve_service("Multi", host, sizeof(host),
                                        &port, 100) == 0);
    CHECK(strcmp(host, "192.0.2.20") == 0);
    CHECK(port == 1548);
    CHECK(afpc_discover_resolve_service("Ambiguous", host, sizeof(host),
                                        &port, 100) == -EEXIST);
    CHECK(afpc_discover_resolve_service("Missing", host, sizeof(host),
                                        &port, 100) == -ENOENT);
    return test_tap_finish();
}
