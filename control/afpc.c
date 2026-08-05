/*
 * The afpc program-name and namespace dispatcher
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include "discovery/client/discover.h"
#include "netatalk-client/afpsl.h"

#include "controller_sl.h"
#ifdef HAVE_AFPC_FS
#include "controller_fs.h"
#endif

static const char *program_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void usage(FILE *stream)
{
    fputs("Usage:\n"
          "  afpc discover [--verbose | --json] [--timeout milliseconds]\n"
          "  afpc sl status\n"
          "  afpc sl exit\n"
#ifdef HAVE_AFPC_FS
          "  afpc fs mount [mount-options] server:volume mountpoint\n"
          "  afpc fs mount --service name [mount-options] [--volume volume] [mountpoint]\n"
          "  afpc fs unmount mountpoint\n"
          "  afpc fs suspend mountpoint\n"
          "  afpc fs resume mountpoint\n"
          "  afpc fs status [mountpoint]\n"
          "  afpc fs exit\n"
#endif
          "  afpc help\n"
          "  afpc version\n", stream);
}

static int missing_namespace(const char *command)
{
    if (strcmp(command, "mount") == 0 || strcmp(command, "unmount") == 0
            || strcmp(command, "suspend") == 0 || strcmp(command, "resume") == 0) {
#ifdef HAVE_AFPC_FS
        fprintf(stderr, "afpc: '%s' requires the fs namespace\n", command);
        fprintf(stderr, "Try 'afpc fs %s'.\n", command);
#else
        fprintf(stderr,
                "afpc: '%s' is unavailable because FUSE support was not built\n",
                command);
#endif
        return 2;
    }

    fprintf(stderr, "afpc: '%s' requires a controller namespace\n", command);
#ifdef HAVE_AFPC_FS
    fprintf(stderr, "Try 'afpc fs %s' or 'afpc sl %s'.\n", command, command);
#else
    fprintf(stderr, "Try 'afpc sl %s'.\n", command);
#endif
    return 2;
}

int main(int argc, char **argv)
{
    const char *program = program_basename(argv[0]);
#ifdef HAVE_AFPC_FS

    if (strcmp(program, "mount_afpfs") == 0) {
        return afpc_mount_url_command(argc, argv);
    }

#endif

    if (strcmp(program, "afpc") != 0) {
        fprintf(stderr,
                "afpc: unsupported invocation name '%s' (expected afpc%s)\n",
                program,
#ifdef HAVE_AFPC_FS
                " or mount_afpfs"
#else
                ""
#endif
               );
        return 2;
    }

    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "help") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 2;
        }

        usage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "version") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 2;
        }

        puts(NETATALK_CLIENT_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "discover") == 0) {
        return afpc_discover_command(argc - 1, argv + 1);
    }

    if (strcmp(argv[1], "sl") == 0) {
        return afpc_sl_command(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "fs") == 0) {
#ifdef HAVE_AFPC_FS

        if (argc == 2) {
            usage(stderr);
            return 2;
        }

        return afpc_fs_command(argv[0], argc - 2, argv + 2);
#else
        fputs("afpc: the 'fs' namespace is unavailable because FUSE support was not built\n",
              stderr);
        return 2;
#endif
    }

    if (strcmp(argv[1], "mount") == 0 || strcmp(argv[1], "unmount") == 0
            || strcmp(argv[1], "suspend") == 0 || strcmp(argv[1], "resume") == 0
            || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "exit") == 0) {
        return missing_namespace(argv[1]);
    }

    fprintf(stderr, "afpc: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
