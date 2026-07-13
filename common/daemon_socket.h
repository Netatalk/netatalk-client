#ifndef NETATALK_CLIENT_DAEMON_SOCKET_H
#define NETATALK_CLIENT_DAEMON_SOCKET_H

/* Create and listen on a Unix socket, removing a conclusively stale socket
 * pathname when necessary. Returns the listener fd, or -1 on failure. */
int daemon_socket_create(const char *socket_path, int backlog);

/* Return 0 when the path is absent or a refused socket was removed. Return -1
 * when a listener is active or staleness cannot be established safely. */
int daemon_socket_cleanup_stale(const char *socket_path);

void daemon_socket_close(int fd, const char *socket_path);

#endif
