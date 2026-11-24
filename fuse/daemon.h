/*!
 * @file
 * @brief afpfsd's loop, listener and mounts, as its commands use them
 *
 * afpfsd's internal header, shared by daemon.c and commands.c.
 * Lane: control.
 */

#ifndef __DAEMON_H_
#define __DAEMON_H_

void rm_fd_and_signal(int fd);
void signal_main_thread(void);

int get_debug_mode(void);

int fuse_unmount_volume(struct afp_volume * volume);
void fuse_forced_ending_hook(void);
void fuse_stop_listener(void);
int release_dead_mount(const char *path);

#define STATUS_LEN 1024

/* The wait for a client's command, which afpc writes as it connects */
#define COMMAND_WAIT_S 2

#endif
