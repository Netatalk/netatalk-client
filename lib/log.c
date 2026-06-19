/*
 *  log.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "libafpclient.h"
#include "utils.h"

void log_for_client(void * priv,
                    enum logtypes logtype, int loglevel, char *format, ...)
{
    va_list ap;
    size_t message_len;
    char new_message[MAX_ERROR_LEN];
    char escaped[MAX_ERROR_LEN * 4];
    va_start(ap, format);
    vsnprintf(new_message, MAX_ERROR_LEN, format, ap);
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

void stdout_log_for_client(
    __attribute__((unused)) void * priv,
    __attribute__((unused)) enum logtypes logtype,
    __attribute__((unused)) int loglevel,
    const char *message)
{
    printf("%s\n", message);
}
