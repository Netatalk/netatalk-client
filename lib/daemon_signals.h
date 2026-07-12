/*
 * daemon_signals.h - Shared signal handling for AFP daemons
 *
 * This header provides signal handling functions used by both
 * afpsld (stateless daemon) and afpfsd (FUSE daemon).
 */

#ifndef __DAEMON_SIGNALS_H
#define __DAEMON_SIGNALS_H

/*
 * Install SIGCHLD handler to prevent zombie processes
 *
 * This handler automatically reaps child processes when they exit,
 * preventing them from becoming zombies.
 */
void daemon_install_sigchld_handler(void);

#endif /* __DAEMON_SIGNALS_H */
