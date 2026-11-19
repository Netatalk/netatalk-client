/*
 *  log.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief Log calls that hand the client's logger trimmed, escaped messages
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define AFPCLIENT_NO_LOG_MACRO

#include "client.h"
#include "compat.h"
#include "utils.h"

/*!
 * @brief Pass a message the client keeps to its logger, trimmed and escaped
 *
 * @param[in] priv      the client's context for the message, or NULL
 * @param[in] logtype   the message's type
 * @param[in] loglevel  the message's level
 * @param[in] message   the message, or NULL
 */
void log_for_client(void *priv,
                    enum logtypes logtype, int loglevel,
                    const char *message)
{
    log_for_clientf(priv, logtype, loglevel, "%s",
                    message == NULL ? "(null)" : message);
}

/*!
 * @brief Format and pass on a message the client keeps, trimmed and escaped
 *
 * The client is asked first, so a message it does not keep is neither
 * formatted nor copied.
 *
 * @param[in] priv      the client's context for the message, or NULL
 * @param[in] logtype   the message's type
 * @param[in] loglevel  the message's level
 * @param[in] format    a literal printf format, or NULL
 * @param[in] ...       the format's arguments
 */
void log_for_clientf(void *priv, enum logtypes logtype, int loglevel,
                     const char *format, ...)
{
    va_list ap;
    size_t message_len;
    char new_message[MAX_ERROR_LEN];
    char escaped[MAX_ERROR_LEN * 4];

    if (libafpclient->log_enabled != NULL
            && !libafpclient->log_enabled(priv, loglevel)) {
        return;
    }

    va_start(ap, format);
    vsnprintf(new_message, sizeof(new_message),
              format == NULL ? "(null)" : format, ap);
    va_end(ap);
    message_len = strlen(new_message);

    while (message_len > 0
            && (new_message[message_len - 1] == '\r'
                || new_message[message_len - 1] == '\n')) {
        new_message[--message_len] = '\0';
    }

    if (message_len == 0) {
        return;
    }

    sanitize_text(new_message, escaped, sizeof(escaped));
    libafpclient->log_for_client(priv, logtype, loglevel, escaped);
}

/*!
 * @brief Print a message as a line on standard output
 *
 * @param[in] priv      unused
 * @param[in] logtype   unused
 * @param[in] loglevel  unused
 * @param[in] message   the message, or NULL
 */
void stdout_log_for_client(
    void *priv _U_,
    enum logtypes logtype _U_,
    int loglevel _U_,
    const char *message)
{
    if (message == NULL) {
        message = "(null)";
    }

    printf("%s\n", message);
}
