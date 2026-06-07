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

static void escape_log_message(const char *in, char *out, size_t outsize)
{
    size_t used = 0;

    if (outsize == 0) {
        return;
    }

    while (*in && used + 1 < outsize) {
        unsigned char ch = (unsigned char) * in++;

        if (ch == '\r' || ch == '\n' || ch == '\t') {
            char escaped;

            if (ch == '\r') {
                escaped = 'r';
            } else if (ch == '\n') {
                escaped = 'n';
            } else {
                escaped = 't';
            }

            if (used + 2 >= outsize) {
                break;
            }

            out[used++] = '\\';
            out[used++] = escaped;
        } else if (ch < 0x20 || ch == 0x7f) {
            if (used + 4 >= outsize) {
                break;
            }

            used += (size_t)snprintf(out + used, outsize - used,
                                     "\\x%02x", ch);
        } else {
            out[used++] = (char)ch;
        }
    }

    out[used] = '\0';
}

void log_for_client(void * priv,
                    enum logtypes logtype, int loglevel, char *format, ...)
{
    va_list ap;
    char new_message[MAX_ERROR_LEN];
    char escaped[MAX_ERROR_LEN * 4];
    va_start(ap, format);
    vsnprintf(new_message, MAX_ERROR_LEN, format, ap);
    va_end(ap);
    escape_log_message(new_message, escaped, sizeof(escaped));
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
