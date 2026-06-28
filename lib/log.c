/*
 *  log.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define AFPCLIENT_NO_LOG_MACRO
#include "compat.h"
#include "libafpclient.h"
#include "utils.h"

void log_for_client(void * priv,
                    enum logtypes logtype, int loglevel,
                    const char *message)
{
    size_t message_len;
    char new_message[MAX_ERROR_LEN];
    char escaped[MAX_ERROR_LEN * 4];

    if (message == NULL) {
        message = "(null)";
    }

    snprintf(new_message, MAX_ERROR_LEN, "%s", message);
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
