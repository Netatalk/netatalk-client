#ifndef NETATALK_CLIENT_DAEMON_COMMANDS_H
#define NETATALK_CLIENT_DAEMON_COMMANDS_H

#include "daemon_client.h"

int fuse_register_afpclient(void);
void fuse_set_log_method(int new_method);

int process_command(struct daemon_client * c);

struct afp_volume *command_sub_attach_volume(struct daemon_client * c,
        struct afp_server * server, char *volname, char *volpassword,
        int *response_result);

#endif
