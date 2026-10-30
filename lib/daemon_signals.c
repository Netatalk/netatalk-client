/*
 * daemon_signals.c - Shared signal handling for AFP daemons
 *
 * This file provides common signal handling functions used by both
 * afpsld (stateless daemon) and afpfsd (FUSE daemon).
 *
 * Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include "daemon_signals.h"

/* SIGCHLD handler to immediately reap child processes */
static void sigchld_handler(int sig)
{
    (void)sig;  /* Unused parameter */
    int status;

    /* Reap all available child processes without blocking.
     * We don't track PIDs here because modifying client/child list from a
     * signal handler would require async-signal-safe operations. The main
     * loop's timeout will clean up the tracking list safely. */
    while (waitpid(-1, &status, WNOHANG) > 0) {
        /* Child has been reaped */
    }
}

/*
 * Install SIGCHLD handler to prevent zombie processes
 *
 * This handler automatically reaps child processes when they exit,
 * preventing them from becoming zombies.
 */
void daemon_install_sigchld_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;  /* Only handle actual exits, not stops */
    sigaction(SIGCHLD, &sa, NULL);
}
