#ifndef NETATALK_CLIENT_DISCOVERY_CLIENT_DISCOVER_H
#define NETATALK_CLIENT_DISCOVERY_CLIENT_DISCOVER_H

#include <stddef.h>
#include <stdint.h>

int afpc_discover_command(int argc, char **argv);
int afpc_discover_resolve_service(const char *name, char *host,
                                  size_t host_size, uint16_t *port,
                                  int timeout_ms);

#endif
