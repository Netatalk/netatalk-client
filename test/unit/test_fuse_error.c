/*
 * test_fuse_error.c: the start errors of libfuse a mount reports
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of the capture of libfuse's start errors
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/compat.h"
#include "lib/client.h"
#include "fuse/fuse_error.h"
#include "tap.h"

static char captured_message[MAX_ERROR_LEN];
static int captured_level = -1;

/*!
 * @brief Keep the text and level the client's logger receives
 *
 * @param[in] priv      unused
 * @param[in] logtype   unused
 * @param[in] loglevel  the level
 * @param[in] message   the text
 */
static void capture_log_message(void *priv _U_, enum logtypes logtype _U_,
                                int loglevel, const char *message)
{
    captured_level = loglevel;
    snprintf(captured_message, sizeof(captured_message), "%s", message);
}

/*!
 * @brief Find the two lowest free descriptors, which a capture takes
 *
 * @param[out] fds  receives their numbers, -1 for one not found
 */
static void lowest_free_descriptors(int fds[2])
{
    fds[0] = dup(fileno(stderr));
    fds[1] = dup(fileno(stderr));

    for (int i = 0; i < 2; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
}

/*!
 * @brief Run the capture checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    struct libafpclient test_client = {
        .log_for_client = capture_log_message,
    };
    struct stat capture;
    int links;
    int before[2];
    int after[2];
    test_tap_init(argc, argv);
    libafpclient_register(&test_client);
    lowest_free_descriptors(before);
    fuse_capture_stderr_start();
    /* Standard error writes to the capture's file until the report. */
    links = fstat(fileno(stderr), &capture) == 0 ? (int) capture.st_nlink : -1;
    TEST_int(links, 0, "the capture's file keeps no name");
    fputs("unknown option", stderr);
    report_fuse_errors(NULL, 0);
    CHECK(strcmp(captured_message, "FUSE error: unknown option") == 0);
    TEST_int(captured_level, LOG_ERR, "a failed start's output is an error");
    lowest_free_descriptors(after);
    TEST_int(after[0] == before[0] && after[1] == before[1], 1,
             "the report closes the capture's descriptors");
    fuse_capture_stderr_start();
    fputs("INIT: 7.39", stderr);
    report_fuse_errors(NULL, 1);
    CHECK(strcmp(captured_message, "FUSE: INIT: 7.39") == 0);
    TEST_int(captured_level, LOG_DEBUG,
             "a started mount's output is logged at debug level");
    return test_tap_finish();
}
