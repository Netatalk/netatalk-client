/*!
 * @file
 * @brief afpsld's command entry and its attach of a volume
 *
 * afpsld's internal header, shared by daemon.c, daemon_client.c and commands.c.
 * Lane: control.
 */

#ifndef NETATALK_CLIENT_DAEMON_COMMANDS_H
#define NETATALK_CLIENT_DAEMON_COMMANDS_H

#include "daemon_client.h"

int process_command(struct daemon_client * c);

struct afp_volume *command_sub_attach_volume(struct daemon_client *c,
                                             struct afp_server *server,
                                             char *volname,
                                             char *volpassword,
                                             int *response_result);

#endif
