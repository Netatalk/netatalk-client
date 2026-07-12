/*
 * daemon_socket.h - Shared socket utilities for AFP daemons
 *
 * This header provides socket management functions used by both
 * afpsld (stateless daemon) and afpfsd (FUSE daemon).
 */

#ifndef __DAEMON_SOCKET_H
#define __DAEMON_SOCKET_H

/*
 * Create a UNIX domain socket listener
 *
 * socket_path: Path to the UNIX socket file
 * backlog: Maximum number of pending connections
 *
 * Returns: Socket file descriptor on success, -1 on error
 */
int daemon_socket_create(const char *socket_path, int backlog);

/*
 * Check for stale daemon and cleanup if needed
 *
 * Attempts to ping an existing daemon at socket_path. If the daemon
 * doesn't respond, removes the stale socket file.
 *
 * socket_path: Path to the UNIX socket file
 *
 * Returns:
 *   0: Socket doesn't exist or stale socket was removed
 *  -1: Active daemon is running (cannot cleanup)
 */
int daemon_socket_cleanup_stale(const char *socket_path);

/*
 * Close socket and remove socket file
 *
 * fd: Socket file descriptor to close
 * socket_path: Path to the UNIX socket file to remove
 */
void daemon_socket_close(int fd, const char *socket_path);

#endif /* __DAEMON_SOCKET_H */
