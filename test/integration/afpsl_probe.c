/*
 * afpsl_probe.c: a libafpsl client for the integration suites, which times
 * one volume list on the user's live afpsld session
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Time a volume list on the live afpsld session of a URL
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "netatalk-client/afpsl.h"

/*!
 * @brief Resume a URL's live session and time a volume list of it
 *
 * The daemon answers the list from memory, under the session's lock. A
 * resume takes no password, and the probe leaves the session to its owner.
 * The result line goes to standard output, the library's log lines to
 * standard error.
 *
 * @param[in] argc  argument count
 * @param[in] argv  the program's name and the session's URL
 * @returns 0 after printing the list's result and its time, 1 for a URL
 *          that does not parse, or 2 for a session that does not resume
 */
int main(int argc, char **argv)
{
    struct afpc_url url;
    struct afpc_volume_info vols[16];
    afpc_server_t server = NULL;
    unsigned int num = 0;
    struct timespec start;
    struct timespec end;
    int ret;
    afp_sl_url_init(&url);

    if (argc != 2 || afp_sl_url_parse(&url, argv[1]) != 0) {
        fprintf(stderr, "usage: afpsl_probe URL\n");
        afp_sl_url_clear(&url);
        return 1;
    }

    memset(url.password, 0, sizeof(url.password));
    ret = afp_sl_resume(&url, 0, &server, NULL);

    if (ret != 0) {
        printf("resume %d\n", ret);
        afp_sl_url_clear(&url);
        return 2;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    ret = afp_sl_getvols(server, &url, 0, sizeof(vols) / sizeof(vols[0]),
                         &num, vols);
    clock_gettime(CLOCK_MONOTONIC, &end);
    afp_sl_url_clear(&url);
    printf("getvols %d in %.1f s, %u volumes\n", ret,
           (double)(end.tv_sec - start.tv_sec)
           + (double)(end.tv_nsec - start.tv_nsec) / 1e9, num);
    return 0;
}
