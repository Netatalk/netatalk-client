/*
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <errno.h>

#include "backend.h"

static int stub_start(void **context, struct afpc_discovery *discovery,
                      const struct afpc_discovery_options *options)
{
    (void)context;
    (void)discovery;
    (void)options;
    return -ENOTSUP;
}

static int stub_iterate(void *context, struct afpc_discovery *discovery,
                        int timeout_ms)
{
    (void)context;
    (void)discovery;
    (void)timeout_ms;
    return -ENOTSUP;
}

static int stub_resolve(void *context, struct afpc_discovery *discovery,
                        const struct afpc_discovery_service *service,
                        struct afpc_discovery_endpoint **endpoints,
                        size_t *endpoint_count, int timeout_ms)
{
    (void)context;
    (void)discovery;
    (void)service;
    (void)endpoints;
    (void)endpoint_count;
    (void)timeout_ms;
    return -ENOTSUP;
}

static void stub_stop(void *context)
{
    (void)context;
}

const struct afpc_discovery_backend_ops afpc_discovery_backend = {
    .name = "stub",
    .start = stub_start,
    .iterate = stub_iterate,
    .resolve = stub_resolve,
    .stop = stub_stop,
};
