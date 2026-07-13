#ifndef NETATALK_CLIENT_DAEMON_DAEMON_H
#define NETATALK_CLIENT_DAEMON_DAEMON_H

#include <sys/select.h>

#include "lib/afp_internal.h"

void rm_fd_and_signal(int fd);
void signal_main_thread(void);

int get_debug_mode(void);

int fuse_unmount_volume(struct afp_volume * volume);
void fuse_forced_ending_hook(void);

#endif
