/*
 * test_log.c: the library's log calls and the escaping of text
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of the library's log calls and of the escaping of text
 */

#include <stdio.h>
#include <string.h>

#include "lib/compat.h"
#include "lib/client.h"
#include "tap.h"
#include "lib/utils.h"

static char captured_message[MAX_ERROR_LEN * 4];
static int last_asked_level = -1;
static int asked;
static char argument[8];

/*!
 * @brief Keep the text the client's logger receives
 *
 * @param[in] priv      unused
 * @param[in] logtype   unused
 * @param[in] loglevel  unused
 * @param[in] message   the text
 */
static void capture_log_message(
    void *priv _U_,
    enum logtypes logtype _U_,
    int loglevel _U_,
    const char *message)
{
    snprintf(captured_message, sizeof(captured_message), "%s", message);
}

/*!
 * @brief Keep a client's messages, as afpsld does, and others above debug
 *
 * The call rewrites argument[], so a message formatted after it shows
 * "early", one formatted before it "late".
 *
 * @param[in] priv      the client the message is for, or NULL
 * @param[in] loglevel  the message's level
 * @returns 1 for a client's message and for a level above debug, 0
 *          otherwise
 */
static int keep_above_debug(void *priv, int loglevel)
{
    asked++;
    last_asked_level = loglevel;
    memcpy(argument, "early", sizeof("early"));
    return priv != NULL || loglevel != LOG_DEBUG;
}

/*!
 * @brief Run the log checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    char sanitized[sizeof("line 1\\r\\nline\\t2\\x01\\x7f\\r\\n")];
    struct libafpclient test_client = {
        .unmount_volume = NULL,
        .log_for_client = capture_log_message,
        .forced_ending_hook = NULL,
        .scan_extra_fds = NULL,
        .loop_started = NULL,
    };
    const char input[] = "line 1\r\nline\t2\x01\x7f\r\n";
    const char expected_sanitized[] =
        "line 1\\r\\nline\\t2\\x01\\x7f\\r\\n";
    const char expected_log[] = "line 1\\r\\nline\\t2\\x01\\x7f";
    test_tap_init(argc, argv);
    sanitize_text(input, sanitized, sizeof(sanitized));
    CHECK(strcmp(sanitized, expected_sanitized) == 0);
    libafpclient_register(&test_client);
    log_for_client(NULL, AFPFSD, LOG_INFO, "%s", input);
    CHECK(strcmp(captured_message, expected_log) == 0);
    (log_for_client)(NULL, AFPFSD, LOG_INFO, input);
    CHECK(strcmp(captured_message, expected_log) == 0);
    (log_for_client)(NULL, AFPFSD, LOG_INFO, NULL);
    CHECK(strcmp(captured_message, "(null)") == 0);
    test_client.log_enabled = keep_above_debug;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "%s", "dropped");
    CHECK(asked == 1 && last_asked_level == LOG_DEBUG
          && strcmp(captured_message, "(null)") == 0);
    (log_for_client)(NULL, AFPFSD, LOG_DEBUG, "dropped");
    CHECK(asked == 2 && strcmp(captured_message, "(null)") == 0);
    memcpy(argument, "late", sizeof("late"));
    log_for_client(NULL, AFPFSD, LOG_NOTICE, "%s", argument);
    CHECK(asked == 3 && last_asked_level == LOG_NOTICE
          && strcmp(captured_message, "early") == 0);
    (log_for_client)(NULL, AFPFSD, LOG_NOTICE, "kept");
    CHECK(asked == 4 && strcmp(captured_message, "kept") == 0);
    log_for_client(&test_client, AFPFSD, LOG_DEBUG, "%s", "for a client");
    CHECK(asked == 5 && strcmp(captured_message, "for a client") == 0);
    libafpclient_register(NULL);
    return test_tap_finish();
}
