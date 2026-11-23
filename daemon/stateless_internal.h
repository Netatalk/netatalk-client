/*!
 * @file
 * @brief libafpsl's reading of an answer from afpsld
 *
 * Internal to libafpsl: stateless.c and its unit test include it.
 * Lane: control.
 */

#ifndef NETATALK_CLIENT_DAEMON_STATELESS_INTERNAL_H
#define NETATALK_CLIENT_DAEMON_STATELESS_INTERNAL_H

#include <stddef.h>

int afp_sl_response_content_length(const char *response, size_t len,
                                   size_t *content_len);
int afp_sl_dispatch_response_logs(const char *response, size_t len);
int afp_sl_read_framed_response(int fd, char *data, size_t capacity,
                                size_t *response_len);
int afp_sl_read_answer_start(int fd, char *buf, size_t len);

#endif
