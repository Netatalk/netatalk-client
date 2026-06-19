/*
 *  daemon.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <fuse.h>
#include <glob.h>

#include "afp.h"
#include "dsi.h"
#include "fuse_ipc.h"
#include "utils.h"
#include "daemon.h"
#include "commands.h"
#include "daemon_socket.h"
#include "daemon_signals.h"


static int debug_mode = 0;
static char commandfilename[PATH_MAX];
static int current_log_method = LOG_METHOD_SYSLOG;
static int current_log_level = LOG_NOTICE;
static int command_fd_global = -1;

/* Forward declaration */
void close_commands(int command_fd);

int get_debug_mode(void)
{
    return debug_mode;
}

void fuse_forced_ending_hook(void)
{
    struct afp_volume * volume;

    /* Close and unlink the command listener socket before unmounting */
    if (command_fd_global >= 0) {
        close_commands(command_fd_global);
        command_fd_global = -1;
    }

    for (struct afp_server * s = get_server_base(); s;) {
        /* Save next pointer before unmounting */
        struct afp_server * next_server = s->next;

        if (s->connect_state == SERVER_STATE_CONNECTED) {
            for (int i = 0; i < s->num_volumes; i++) {
                volume = &s->volumes[i];

                if (volume->mounted == AFP_VOLUME_MOUNTED) {
                    char safe_mountpoint[PATH_MAX * 4];
                    sanitize_text(volume->mountpoint,
                                  safe_mountpoint,
                                  sizeof(safe_mountpoint));
                    log_for_client(NULL, AFPFSD, LOG_NOTICE,
                                   "Unmounting %s", safe_mountpoint);
                }

                afp_unmount_volume(volume);
            }
        }

        s = next_server;
    }
}

int fuse_unmount_volume(struct afp_volume * volume)
{
    if (!volume || !volume->mountpoint[0]) {
        return -1;
    }

    if (!volume->priv) {
        char safe_mountpoint[PATH_MAX * 4];
        sanitize_text(volume->mountpoint, safe_mountpoint,
                      sizeof(safe_mountpoint));
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "FUSE handle already cleared for %s - skipping unmount",
                       safe_mountpoint);
        return 0;
    }

#if !defined(__APPLE__) && FUSE_USE_VERSION >= 30
    pthread_t self = pthread_self();
    pthread_t vol_thread = volume->thread;
    int is_same_thread = pthread_equal(self, vol_thread);
    char safe_mountpoint[PATH_MAX * 4];
    sanitize_text(volume->mountpoint, safe_mountpoint,
                  sizeof(safe_mountpoint));
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Unmounting FUSE filesystem at %s", safe_mountpoint);
    fuse_unmount((struct fuse *)volume->priv);

    /* Wait for FUSE thread to complete (if called from external thread) */
    if (!is_same_thread && volume->thread) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Waiting for FUSE thread to complete");
        pthread_join(volume->thread, NULL);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "FUSE thread completed for %s", safe_mountpoint);
    }

    return 0;
#else
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Programmatic unmount not supported on this platform");
    return -1;
#endif
}


static int startup_listener(void)
{
    return daemon_socket_create(commandfilename, 5); /* Just one at a time */
}

void close_commands(int command_fd)
{
    daemon_socket_close(command_fd, commandfilename);
}

static void usage(void)
{
    printf("Netatalk Client %s - Apple Filing Protocol client FUSE daemon\n"
           "Usage: afpfsd [OPTION]\n"
           "  -l, --logmethod    Either 'syslog' or 'stdout'\n"
           "  -v, --loglevel     LOG_DEBUG|LOG_INFO|LOG_NOTICE|LOG_WARNING|LOG_ERR\n"
           "  -f, --foreground   Do not fork\n"
           "  -d, --debug        Do not fork, debug loglevel, log to stdout\n"
           "  -m, --manager      Run as a manager daemon\n"
           "  -s, --socket-id    Socket filename (for per-mount daemon support)\n",
           AFPFS_VERSION);
}


static int remove_other_daemon(void)
{
    int ret = daemon_socket_cleanup_stale(commandfilename);

    if (ret < 0) {
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Daemon is already running and alive\n");
    }

    return ret;
}

/* Manager daemon child tracking */
struct manager_child {
    pid_t pid;
    char socket_id[PATH_MAX];
    char mountpoint[PATH_MAX];
    char volumename[AFP_VOLUME_NAME_UTF8_LEN];
    struct manager_child *next;
};

static struct manager_child *child_list = NULL;

static int append_text(char *text, size_t size, int *pos, const char *src)
{
    size_t available;
    size_t len;

    if (*pos < 0 || (size_t) * pos >= size) {
        if (size > 0) {
            text[size - 1] = '\0';
            *pos = (int)size - 1;
        }

        return -1;
    }

    available = size - (size_t) * pos;
    len = strnlen(src, available);
    memcpy(text + *pos, src, len);

    if (len >= available) {
        *pos = (int)size - 1;
        text[*pos] = '\0';
        return -1;
    }

    *pos += (int)len;
    text[*pos] = '\0';
    return 0;
}

/* Send exit command to a socket path */
static void send_exit_to_socket(const char *socket_path)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sock < 0) {
        return;
    }

    struct sockaddr_un sa;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t len = strnlen(socket_path, sizeof(sa.sun_path));

    if (len >= sizeof(sa.sun_path)) {
        goto out;
    }

    memcpy(sa.sun_path, socket_path, len + 1);

    if (connect(sock, (struct sockaddr *)&sa,
                sizeof(sa.sun_family) + len + 1) == 0) {
        unsigned char cmd = AFP_SERVER_COMMAND_EXIT;
        (void)write(sock, &cmd, 1);
    }

out:
    shutdown(sock, SHUT_RDWR);
    close(sock);
}

/* Find child by mountpoint */
static struct manager_child *find_child_by_mountpoint(const char *mountpoint,
        int client_fd)
{
    for (struct manager_child *child = child_list; child; child = child->next) {
        if (strcmp(child->mountpoint, mountpoint) == 0) {
            return child;
        }
    }

    /* Not found - send error if client_fd provided */
    if (client_fd >= 0) {
        struct afp_server_response response;
        char text[1024];
        int len = sizeof(text);
        int pos = 0;
        text[0] = '\0';

        if (afp_status_header(text, &len) >= 0) {
            pos = strlen(text);
        }

        char safe_mountpoint[PATH_MAX * 4];
        sanitize_text(mountpoint, safe_mountpoint,
                      sizeof(safe_mountpoint));
        append_text(text, sizeof(text), &pos, "No mount found at ");
        append_text(text, sizeof(text), &pos, safe_mountpoint);
        append_text(text, sizeof(text), &pos, "\n");
        response.result = AFP_SERVER_RESULT_ERROR;
        response.len = strlen(text);
        (void)write(client_fd, &response, sizeof(response));
        (void)write(client_fd, text, response.len);
    }

    return NULL;
}

/* Connect to a child daemon's socket */
static int connect_to_child_socket(const char *socket_path,
                                   const char *mountpoint,
                                   int client_fd)
{
    int child_sock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (child_sock < 0) {
        if (client_fd >= 0) {
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
        }

        return -1;
    }

    struct sockaddr_un child_addr;

    memset(&child_addr, 0, sizeof(child_addr));
    child_addr.sun_family = AF_UNIX;

    if (strlcpy(child_addr.sun_path, socket_path,
                sizeof(child_addr.sun_path)) >= sizeof(child_addr.sun_path)) {
        if (client_fd >= 0) {
            struct afp_server_response response;
            char text[1024];
            char safe_mountpoint[PATH_MAX * 4];
            int pos = 0;
            text[0] = '\0';
            sanitize_text(mountpoint, safe_mountpoint,
                          sizeof(safe_mountpoint));
            append_text(text, sizeof(text), &pos,
                        "Socket path too long for ");
            append_text(text, sizeof(text), &pos, safe_mountpoint);
            append_text(text, sizeof(text), &pos, "\n");
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = strlen(text);
            (void)write(client_fd, &response, sizeof(response));
            (void)write(client_fd, text, response.len);
        }

        close(child_sock);
        return -1;
    }

    if (connect(child_sock, (struct sockaddr *)&child_addr,
                sizeof(child_addr.sun_family) + strlen(child_addr.sun_path) + 1) < 0) {
        if (client_fd >= 0) {
            struct afp_server_response response;
            char text[1024];
            int len = sizeof(text);
            int pos = 0;

            if (afp_status_header(text, &len) >= 0) {
                pos = strlen(text);
            }

            snprintf(text + pos, sizeof(text) - pos,
                     "Could not connect to daemon\n");
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = strlen(text);
            (void)write(client_fd, &response, sizeof(response));
            (void)write(client_fd, text, response.len);
        }

        close(child_sock);
        return -1;
    }

    return child_sock;
}

/* Forward response from child daemon to client */
static int forward_child_response(int child_sock, int client_fd)
{
    struct afp_server_response response;

    if (read(child_sock, &response, sizeof(response)) != sizeof(response)) {
        response.result = AFP_SERVER_RESULT_ERROR;
        response.len = 0;

        if (write(client_fd, &response, sizeof(response)) != sizeof(response)) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to send error response to client");
        }

        return -1;
    }

    if (response.len > MAX_CLIENT_RESPONSE) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Child response exceeds maximum allowed size");
        response.result = AFP_SERVER_RESULT_ERROR;
        response.len = 0;

        if (write(client_fd, &response, sizeof(response)) != sizeof(response)) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to send error response to client");
        }

        return -1;
    }

    if (write(client_fd, &response, sizeof(response)) != sizeof(response)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Failed to forward response header to client");
        return -1;
    }

    if (response.len > 0) {
        char *buffer = malloc(response.len);

        if (buffer) {
            ssize_t bytes_read = read(child_sock, buffer, response.len);

            if (bytes_read > 0) {
                ssize_t bytes_written = write(client_fd, buffer, bytes_read);

                if (bytes_written != bytes_read) {
                    log_for_client(NULL, AFPFSD, LOG_WARNING,
                                   "Failed to forward response data to client (%zd/%zd bytes)",
                                   bytes_written < 0 ? 0 : bytes_written, bytes_read);
                    free(buffer);
                    return -1;
                }
            }

            free(buffer);
        }
    }

    return 0;
}

static void add_child(pid_t pid, const char *socket_id, const char *mountpoint,
                      const char *volumename)
{
    struct manager_child *child = malloc(sizeof(struct manager_child));

    if (!child) {
        return;
    }

    child->pid = pid;
    strlcpy(child->socket_id, socket_id, sizeof(child->socket_id));
    strlcpy(child->mountpoint, mountpoint, sizeof(child->mountpoint));
    strlcpy(child->volumename, volumename ? volumename : "",
            sizeof(child->volumename));
    child->next = child_list;
    child_list = child;
}

static void remove_child(pid_t pid)
{
    struct manager_child **curr = &child_list;

    while (*curr) {
        if ((*curr)->pid == pid) {
            struct manager_child *to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            return;
        }

        curr = &(*curr)->next;
    }
}

static void cleanup_all_children(void)
{
    struct manager_child *child = child_list;
    /* Also notify any stray per-mount daemons that were not tracked. */
    {
        glob_t g = {0};
        char pattern[PATH_MAX];
        int len = snprintf(pattern, sizeof(pattern), "%s-%u*", SERVER_FUSE_SOCKET_PATH,
                           geteuid());

        if (len > 0 && (size_t)len < sizeof(pattern)) {
            if (glob(pattern, 0, NULL, &g) == 0) {
                for (size_t i = 0; i < g.gl_pathc; i++) {
                    const char *path = g.gl_pathv[i];

                    /* Skip our own manager socket to avoid recursion. */
                    if (strcmp(path, commandfilename) == 0) {
                        continue;
                    }

                    send_exit_to_socket(path);
                }
            }

            globfree(&g);
        }
    }

    while (child) {
        send_exit_to_socket(child->socket_id);
        child = child->next;
    }

    /* Short grace period. */
    sleep(1);
    /* Force-stop any stubborn children. */
    child = child_list;

    while (child) {
        kill(child->pid, SIGTERM);
        child = child->next;
    }

    sleep(1);
    child = child_list;

    while (child) {
        int status;
        kill(child->pid, SIGKILL);
        waitpid(child->pid, &status, WNOHANG);
        child = child->next;
    }

    while (child_list) {
        remove_child(child_list->pid);
    }
}

static int start_mount_daemon(char *socket_id, const char *mountpoint,
                              const char *volumename)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child process - exec mount daemon */
        char *log_method_str = (current_log_method & LOG_METHOD_STDOUT)
                               ? "stdout" : "syslog";
        /* Type cast away const for execvp() */
        char *log_level_str = (char *) log_level_to_string(current_log_level);
        char *argv[8];
        argv[0] = "afpfsd";
        argv[1] = "--socket-id";
        argv[2] = socket_id;
        argv[3] = "--logmethod";
        argv[4] = log_method_str;
        argv[5] = "--loglevel";
        argv[6] = log_level_str;
        argv[7] = NULL;
        execvp("afpfsd", argv);
        /* If exec fails */
        _exit(1);
    }

    /* Parent process */
    add_child(pid, socket_id, mountpoint, volumename);
    return 0;
}

static int handle_manager_command(int client_fd)
{
    unsigned char command;
    ssize_t n = read(client_fd, &command, 1);

    if (n <= 0) {
        return -1;
    }

    switch (command) {
    case AFP_SERVER_COMMAND_SPAWN_MOUNT: {
        struct afp_server_spawn_mount_request req;
        n = read(client_fd, &req, sizeof(req));

        if (n != sizeof(req)) {
            return -1;
        }

        if (memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) == NULL ||
                memchr(req.socket_id, '\0', sizeof(req.socket_id)) == NULL ||
                memchr(req.volumename, '\0', sizeof(req.volumename)) == NULL) {
            unsigned char result = AFP_SERVER_RESULT_ERROR;
            (void)write(client_fd, &result, 1);
            break;
        }

        if (start_mount_daemon(req.socket_id, req.mountpoint, req.volumename) < 0) {
            unsigned char result = AFP_SERVER_RESULT_ERROR;
            (void)write(client_fd, &result, 1);
            return -1;
        }

        sleep(1);
        unsigned char result = AFP_SERVER_RESULT_OKAY;
        (void)write(client_fd, &result, 1);
        break;
    }

    case AFP_SERVER_COMMAND_EXIT: {
        struct afp_server_response response;
        response.result = AFP_SERVER_RESULT_OKAY;
        response.len = 0;
        (void)write(client_fd, &response, sizeof(response));
        cleanup_all_children();
        return -2;  /* Signal to exit manager; caller closes client_fd */
    }

    case AFP_SERVER_COMMAND_PING: {
        unsigned char result = AFP_SERVER_RESULT_OKAY;
        (void)write(client_fd, &result, 1);
        break;
    }

    case AFP_SERVER_COMMAND_STATUS: {
        struct afp_server_status_request req;
        n = read(client_fd, &req, sizeof(req));

        if (n != sizeof(req)) {
            /* Read error */
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
            break;
        }

        if (memchr(req.volumename, '\0', sizeof(req.volumename)) == NULL ||
                memchr(req.servername, '\0', sizeof(req.servername)) == NULL ||
                memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) == NULL) {
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
            break;
        }

        /* Check if mountpoint was specified */
        if (req.mountpoint[0] != '\0') {
            /* Forward to specific child daemon */
            const struct manager_child *child = find_child_by_mountpoint(req.mountpoint,
                                                client_fd);

            if (!child) {
                break;
            }

            int child_sock = connect_to_child_socket(child->socket_id, req.mountpoint,
                             client_fd);

            if (child_sock < 0) {
                break;
            }

            /* Send command to child */
            unsigned char cmd = AFP_SERVER_COMMAND_STATUS;

            if (write(child_sock, &cmd, 1) != 1 ||
                    write(child_sock, &req, sizeof(req)) != sizeof(req)) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send status command to child daemon");
                struct afp_server_response response;
                response.result = AFP_SERVER_RESULT_ERROR;
                response.len = 0;
                (void)write(client_fd, &response, sizeof(response));
                close(child_sock);
                break;
            }

            forward_child_response(child_sock, client_fd);
            close(child_sock);
        } else {
            /* Manager daemon: return overview with header and list of mounts */
            struct afp_server_response response;
            char text[4096];
            int len = sizeof(text);
            int pos = 0;
            int count = 0;
            int initial_count = 0;
            int removed_count = 0;

            /* Include the standard header */
            if (afp_status_header(text, &len) >= 0) {
                pos = strlen(text);
                len = sizeof(text) - pos;
            }

            /* Count initial children before validation */
            for (struct manager_child *c = child_list; c; c = c->next) {
                initial_count++;
            }

            struct manager_child *child = child_list;

            while (child) {
                struct manager_child *next = child->next;
                int is_alive = 0;
                int access_result = -1;
                int socket_result = -1;
                int connect_result = -1;
                int connect_errno = 0;
                /* Try to connect to the child's socket to verify it's alive */
                access_result = access(child->socket_id, F_OK);

                if (access_result == 0) {
                    int test_sock = socket(AF_UNIX, SOCK_STREAM, 0);
                    socket_result = test_sock;

                    if (test_sock >= 0) {
                        struct sockaddr_un test_addr;
                        memset(&test_addr, 0, sizeof(test_addr));
                        test_addr.sun_family = AF_UNIX;
                        size_t path_len = strlcpy(test_addr.sun_path, child->socket_id,
                                                  sizeof(test_addr.sun_path));

                        if (path_len < sizeof(test_addr.sun_path)) {
                            socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + path_len + 1;
                            connect_result = connect(test_sock, (struct sockaddr *)&test_addr, addr_len);

                            if (connect_result == 0) {
                                is_alive = 1;
                            } else {
                                connect_errno = errno;
                            }
                        }

                        close(test_sock);
                    }
                }

                char safe_mountpoint[PATH_MAX * 4];
                char safe_socket_id[PATH_MAX * 4];
                sanitize_text(child->mountpoint, safe_mountpoint,
                              sizeof(safe_mountpoint));
                sanitize_text(child->socket_id, safe_socket_id,
                              sizeof(safe_socket_id));
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "Child check: pid=%d mount=%s socket=%s access=%d sock=%d conn=%d errno=%d alive=%d",
                               child->pid, safe_mountpoint, safe_socket_id,
                               access_result, socket_result, connect_result, connect_errno, is_alive);

                if (!is_alive) {
                    remove_child(child->pid);
                    removed_count++;
                }

                child = next;
            }

            /* Count active mounts */
            for (child = child_list; child; child = child->next) {
                count++;
            }

            if (count == 0) {
                append_text(text, sizeof(text), &pos,
                            "Manager daemon: no active mounts");

                if (initial_count > 0) {
                    char removed_text[64];
                    snprintf(removed_text, sizeof(removed_text),
                             " (removed %d stale)", removed_count);
                    append_text(text, sizeof(text), &pos, removed_text);
                }
            } else {
                char count_text[64];
                snprintf(count_text, sizeof(count_text),
                         "Manager daemon: %d active mount%s",
                         count, count == 1 ? "" : "s");
                append_text(text, sizeof(text), &pos, count_text);

                if (removed_count > 0) {
                    char removed_text[64];
                    snprintf(removed_text, sizeof(removed_text),
                             " (removed %d stale)", removed_count);
                    append_text(text, sizeof(text), &pos, removed_text);
                }

                append_text(text, sizeof(text), &pos, "\n");

                /* List mountpoints with volume names */
                for (child = child_list; child; child = child->next) {
                    char safe_volumename[AFP_VOLUME_NAME_UTF8_LEN * 4];
                    char safe_mountpoint[PATH_MAX * 4];
                    sanitize_text(child->volumename, safe_volumename,
                                  sizeof(safe_volumename));
                    sanitize_text(child->mountpoint, safe_mountpoint,
                                  sizeof(safe_mountpoint));

                    if (child->volumename[0] != '\0') {
                        append_text(text, sizeof(text), &pos, "  ");
                        append_text(text, sizeof(text), &pos, safe_volumename);
                        append_text(text, sizeof(text), &pos, ": ");
                        append_text(text, sizeof(text), &pos, safe_mountpoint);
                        append_text(text, sizeof(text), &pos, "\n");
                    } else {
                        append_text(text, sizeof(text), &pos, "  ");
                        append_text(text, sizeof(text), &pos, safe_mountpoint);
                        append_text(text, sizeof(text), &pos, "\n");
                    }

                    if (pos >= (int)sizeof(text) - 1) {
                        break;
                    }
                }

                append_text(text, sizeof(text), &pos,
                            "\nRun 'afp_client status [mountpoint]' for details");
            }

            append_text(text, sizeof(text), &pos, "\n");
            response.result = AFP_SERVER_RESULT_OKAY;
            response.len = strlen(text);

            if (write(client_fd, &response, sizeof(response)) != sizeof(response) ||
                    write(client_fd, text, response.len) != (ssize_t)response.len) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send status response to client");
            }
        }

        break;
    }

    case AFP_SERVER_COMMAND_SUSPEND:
    case AFP_SERVER_COMMAND_RESUME: {
        /* These commands must be forwarded to the appropriate child daemon */
        struct afp_server_suspend_request req;
        n = read(client_fd, &req, sizeof(req));

        if (n != sizeof(req)) {
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
            break;
        }

        if (memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) == NULL) {
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
            break;
        }

        const struct manager_child *child = find_child_by_mountpoint(req.mountpoint,
                                            client_fd);

        if (!child) {
            break;
        }

        int child_sock = connect_to_child_socket(child->socket_id, req.mountpoint,
                         client_fd);

        if (child_sock < 0) {
            break;
        }

        /* Send command to child */
        if (write(child_sock, &command, 1) != 1 ||
                write(child_sock, &req, sizeof(req)) != sizeof(req)) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to send suspend/resume command to child daemon");
            struct afp_server_response response;
            response.result = AFP_SERVER_RESULT_ERROR;
            response.len = 0;
            (void)write(client_fd, &response, sizeof(response));
            close(child_sock);
            break;
        }

        forward_child_response(child_sock, client_fd);
        close(child_sock);
        break;
    }

    default:
        /* Unknown command */
        break;
    }

    return 0;
}

static int run_manager_daemon(void)
{
    int listen_fd = startup_listener();

    if (listen_fd < 0) {
        return -1;
    }

    /* Install SIGCHLD handler to immediately reap child processes */
    daemon_install_sigchld_handler();
    char safe_commandfilename[PATH_MAX * 4];
    sanitize_text(commandfilename, safe_commandfilename,
                  sizeof(safe_commandfilename));
    log_for_client(NULL, AFPFSD, LOG_NOTICE,
                   "Starting manager daemon on %s", safe_commandfilename);

    while (1) {
        fd_set rds;
        FD_ZERO(&rds);
        FD_SET(listen_fd, &rds);
        struct timeval tv = {30, 0};
        int ret = select(listen_fd + 1, &rds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        if (ret == 0) {
            /* Timeout - check for dead children */
            struct manager_child *curr_child = child_list;

            while (curr_child) {
                struct manager_child *next = curr_child->next;
                int status;
                pid_t result = waitpid(curr_child->pid, &status, WNOHANG);

                if (result > 0) {
                    remove_child(curr_child->pid);
                }

                curr_child = next;
            }

            continue;
        }

        if (FD_ISSET(listen_fd, &rds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

            if (client_fd < 0) {
                continue;
            }

            int result = handle_manager_command(client_fd);
            close(client_fd);

            if (result == -2) {
                /* Exit requested */
                break;
            }
        }
    }

    close_commands(listen_fd);
    cleanup_all_children();
    return 0;
}

int main(int argc, char *argv[])
{
    int option_index = 0;
    struct option long_options[] = {
        {"logmethod", 1, 0, 'l'},
        {"loglevel", 1, 0, 'v'},
        {"foreground", 0, 0, 'f'},
        {"debug", 0, 0, 'd'},
        {"socket-id", 1, 0, 's'},
        {"manager", 0, 0, 'm'},
        {0, 0, 0, 0},
    };
    int new_log_method = LOG_METHOD_SYSLOG;
    int log_level = LOG_NOTICE;
    int dofork = 1;
    int manager_mode = 0;
    /* getopt_long()'s return is int; specifying the variable to contain
     * this return value as char depends on endian-specific behavior,
     * breaking utterly on big endian (i.e., PowerPC)
     */
    int c;
    int command_fd = -1;
    const char *socket_id = NULL;

    while (1) {
        c = getopt_long(argc, argv, "dfhl:ms:v:",
                        long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'l':
            if (strncmp(optarg, "stdout", 6) == 0) {
                new_log_method = LOG_METHOD_STDOUT;
            } else if (strncmp(optarg, "syslog", 6) == 0) {
                new_log_method = LOG_METHOD_SYSLOG;
            } else {
                printf("Unknown log method %s\n", optarg);
                usage();
                return -1;
            }

            break;

        case 'f':
            dofork = 0;
            break;

        case 'd':
            dofork = 0;
            debug_mode = 1;
            new_log_method = LOG_METHOD_STDOUT;
            log_level = LOG_DEBUG;
            break;

        case 'm':
            manager_mode = 1;
            break;

        case 's':
            socket_id = optarg;
            break;

        case 'v': {
            int parsed_loglevel;

            if (string_to_log_level(optarg, &parsed_loglevel) != 0) {
                printf("Unknown log level %s\n", optarg);
                usage();
                return -1;
            }

            log_level = parsed_loglevel;
            break;
        }

        case 'h':
        default:
            usage();
            return -1;
        }
    }

    /* If running in foreground and log method wasn't explicitly set, use stdout */
    if (dofork == 0 && new_log_method == LOG_METHOD_SYSLOG) {
        new_log_method = LOG_METHOD_STDOUT;
    }

    /* Apply log settings early, before any daemon code or fork */
    fuse_set_log_method(new_log_method);
    fuse_set_log_level(log_level);
    current_log_method = new_log_method;
    current_log_level = log_level;

    if (new_log_method & LOG_METHOD_SYSLOG) {
        openlog("afpfsd", LOG_PID | LOG_CONS, LOG_DAEMON);
    }

    /* If logging to stdout, enable line buffering for timely output */
    if (new_log_method & LOG_METHOD_STDOUT) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    /* Now register the client callback and initialize UAMs with logging ready */
    fuse_register_afpclient();

    if (init_uams() < 0) {
        return -1;
    }

    if (socket_id != NULL) {
        snprintf(commandfilename, sizeof(commandfilename), "%s", socket_id);
    } else {
        snprintf(commandfilename, sizeof(commandfilename), "%s-%d",
                 SERVER_FUSE_SOCKET_PATH, geteuid());
    }

    /* Mount daemons (not manager) should auto-shutdown when last volume unmounts */
    if (socket_id != NULL && !manager_mode) {
        afp_set_auto_shutdown_on_unmount(1);
    }

    if (remove_other_daemon() < 0) {
        return -1;
    }

    if ((!dofork) || (fork() == 0)) {
        if (manager_mode) {
            return run_manager_daemon();
        }

        /* Run mount daemon */
        if ((command_fd = startup_listener()) < 0) {
            goto error;
        }

        command_fd_global = command_fd;
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Starting up AFPFS version %s", AFPFS_VERSION);
        afp_main_loop(command_fd);
        close_commands(command_fd);
    }

    return 0;
error:
    printf("Could not start afpfsd\n");
    return -1;
}
