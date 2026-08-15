/*
 * Stateless controller commands for afpc
 *
 * Keep this adapter on the public libafpsl API: afpsld owns its wire
 * protocol and structured log handling.
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "netatalk-client/afpsl.h"

#include "controller_sl.h"

/* libafpsl owns the framing limit. Keep enough space for its status text
 * without making this command parser depend on afpsld's private protocol. */
#define AFPC_SL_STATUS_BUFFER_SIZE (64U * 1024U)

static int sl_log_rank(int loglevel)
{
    switch (loglevel) {
    case LOG_DEBUG:
        return 0;

    case LOG_INFO:
        return 1;

    case LOG_NOTICE:
        return 2;

    case LOG_WARNING:
        return 3;

    case LOG_ERR:
        return 4;

    default:
        return 4;
    }
}

static void sl_log_callback(void *user_data, int loglevel, const char *message)
{
    (void)user_data;

    if (sl_log_rank(loglevel) < sl_log_rank(LOG_NOTICE)) {
        return;
    }

    fprintf(stderr, "%s\n", message);
}

static void sl_usage(FILE *stream)
{
    fputs("Usage:\n"
          "  afpc sl status\n"
          "  afpc sl exit\n", stream);
}

int afpc_sl_command(int argc, char **argv)
{
    afp_sl_set_log_callback(sl_log_callback, NULL);

    if (argc != 1) {
        sl_usage(stderr);
        return 2;
    }

    if (strcmp(argv[0], "exit") == 0) {
        int ret = afp_sl_exit();

        if (ret != 0) {
            fprintf(stderr, "afpc: could not exit afpsld: %s\n",
                    strerror(-ret));
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[0], "status") == 0) {
        char text[AFPC_SL_STATUS_BUFFER_SIZE] = {0};
        unsigned int remaining = sizeof(text);
        int ret = afp_sl_status(NULL, NULL, text, &remaining);

        if (ret != 0) {
            fprintf(stderr, "afpc: could not get afpsld status: %s\n",
                    strerror(-ret));
            return 1;
        }

        fputs(text, stdout);
        return 0;
    }

    fprintf(stderr, "afpc: '%s' is not a stateless controller command\n",
            argv[0]);
    sl_usage(stderr);
    return 2;
}
