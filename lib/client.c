/*
 *  client.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include "afp_internal.h"
#include "client.h"

static struct libafpclient null_afpclient = {
    .unmount_volume = NULL,
    .log_for_client = stdout_log_for_client,
    .forced_ending_hook = NULL,
    .scan_extra_fds = NULL,
    .loop_started = NULL,
};

struct libafpclient *libafpclient = &null_afpclient;


void libafpclient_register(struct libafpclient * tmpclient)
{
    if (tmpclient) {
        libafpclient = tmpclient;
    } else {
        libafpclient = &null_afpclient;
    }
}

