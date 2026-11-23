/*
 *  daemon.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The FUSE manager daemon and a mount's daemon
 *
 * The top of afpfsd, the FUSE front end: the manager and a mount's daemon.
 * Lane: control.
 * Northbound: the command line of client.c, over a UNIX socket.
 * Southbound: libafpclient's event loop, which a mount's daemon runs.
 */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include <sys/param.h>
#ifndef __linux__
#include <sys/mount.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
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

#include "lib/afp_internal.h"
#include "lib/dsi.h"
#include "lib/utils.h"
#include "common/daemon_socket.h"

#include "commands.h"
#include "daemon.h"
#include "fuse_ipc.h"

static int debug_mode = 0;
static char commandfilename[PATH_MAX];
static int current_log_method = LOG_METHOD_SYSLOG;
static int current_log_level = LOG_NOTICE;
static int command_fd_global = -1;

/* Forward declaration */
void close_commands(int command_fd);

static void wake_signal_handler(int signum)
{
    (void)signum;
}

static int install_fuse_wake_signal_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wake_signal_handler;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGPIPE, &sa, NULL);
}

int get_debug_mode(void)
{
    return debug_mode;
}

void fuse_stop_listener(void)
{
    /* Unlink only once: a replacement daemon may already own this path
     * when the main loop completes its shutdown. The command thread and the
     * FUSE thread can stop it at once, and one takes the descriptor. */
    int fd = __atomic_exchange_n(&command_fd_global, -1, __ATOMIC_ACQ_REL);

    if (fd >= 0) {
        close_commands(fd);
        rm_fd_and_signal(fd);
    }
}

void fuse_forced_ending_hook(void)
{
    struct afp_volume * volume;
    /* Close and unlink the command listener socket before unmounting */
    fuse_stop_listener();

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
    struct fuse *fuse = (struct fuse *)volume->priv;
    char safe_mountpoint[PATH_MAX * 4];
    sanitize_text(volume->mountpoint, safe_mountpoint,
                  sizeof(safe_mountpoint));
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Stopping FUSE filesystem at %s", safe_mountpoint);
    fuse_exit(fuse);

    /* fuse_exit() only marks the session as exiting.  Wake the FUSE thread
     * with SIGPIPE, matching libfuse's documented wake path, while main()
     * ensures SIGPIPE cannot terminate the daemon if libfuse has not installed
     * its handler yet.  The FUSE thread owns fuse_main() cleanup. */
    if (!is_same_thread && volume->thread) {
        int wake_rc = pthread_kill(vol_thread, SIGPIPE);

        if (wake_rc != 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Could not wake FUSE thread for %s: %s",
                           safe_mountpoint, strerror(wake_rc));
        }

        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Waiting for FUSE thread to complete");
        pthread_join(volume->thread, NULL);
        volume->priv = NULL;
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
    printf("Netatalk Client %s - AFP client FUSE daemon\n"
           "Usage: afpfsd [OPTION]\n"
           "  -l, --logmethod    Either 'syslog' or 'stdout'\n"
           "  -v, --loglevel     LOG_DEBUG|LOG_INFO|LOG_NOTICE|LOG_WARNING|LOG_ERR\n"
           "  -f, --foreground   Do not fork\n"
           "  -d, --debug        Do not fork, debug loglevel, log to stdout\n"
           "  -m, --manager      Run as a manager daemon\n"
           "  -r, --ready-fd     Descriptor to write once listening\n"
           "  -s, --socket-id    Socket filename (for per-mount daemon support)\n",
           NETATALK_CLIENT_VERSION);
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

/* How often the manager reaps ended daemons */
#define MANAGER_SWEEP_USEC 500000

/* The waits' poll step, and the longest a spawn waits for its mount point's
 * daemon to listen or end */
#define SPAWN_WAIT_STEP_MS 20
#define SPAWN_WAIT_LIMIT_MS 2000

#ifdef __linux__
#if FUSE_USE_VERSION >= 30
#define FUSERMOUNT "fusermount3"
#else
#define FUSERMOUNT "fusermount"
#endif
#endif

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

static int send_response(int client_fd, char result, const void *data,
                         size_t data_len)
{
    if (data_len > UINT_MAX) {
        return -1;
    }

    struct afpfsd_ipc_response response = {
        .result = result,
        .len = (unsigned int)data_len,
    };

    if (afpfsd_ipc_write_all(client_fd, &response, sizeof(response)) < 0) {
        return -1;
    }

    if (data_len != 0
            && afpfsd_ipc_write_all(client_fd, data, data_len) < 0) {
        return -1;
    }

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
        unsigned char cmd = AFPFSD_IPC_COMMAND_EXIT;

        if (afpfsd_ipc_write_all(sock, &cmd, sizeof(cmd)) < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Could not send exit command to child daemon");
        }
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

        if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR, text,
                          strlen(text)) < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to send mount-not-found response to client");
        }
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
        if (client_fd >= 0 &&
                send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to send socket error response to client");
        }

        return -1;
    }

    struct sockaddr_un child_addr;

    memset(&child_addr, 0, sizeof(child_addr));
    child_addr.sun_family = AF_UNIX;

    if (strlcpy(child_addr.sun_path, socket_path,
                sizeof(child_addr.sun_path)) >= sizeof(child_addr.sun_path)) {
        if (client_fd >= 0) {
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

            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR, text,
                              strlen(text)) < 0) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send socket-path error response to client");
            }
        }

        close(child_sock);
        return -1;
    }

    if (connect(child_sock, (struct sockaddr *)&child_addr,
                sizeof(child_addr.sun_family) + strlen(child_addr.sun_path) + 1) < 0) {
        if (client_fd >= 0) {
            char text[1024];
            int len = sizeof(text);
            int pos = 0;

            if (afp_status_header(text, &len) >= 0) {
                pos = strlen(text);
            }

            snprintf(text + pos, sizeof(text) - pos,
                     "Could not connect to daemon\n");

            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR, text,
                              strlen(text)) < 0) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send connection error response to client");
            }
        }

        close(child_sock);
        return -1;
    }

    return child_sock;
}

/*!
 * @brief Send a command and its request to a mount's daemon in one write
 *
 * @param[in] child_sock  connected socket of the mount's daemon
 * @param[in] command     the command byte
 * @param[in] request     the command's request
 * @param[in] length      bytes of request, up to a status request's length
 * @returns 0, or -1 when the request is too long or the write fails
 */
static int send_child_command(int child_sock, unsigned char command,
                              const void *request, size_t length)
{
    char message[1 + sizeof(struct afpfsd_ipc_status_request)];

    if (length >= sizeof(message)) {
        return -1;
    }

    message[0] = (char) command;
    memcpy(message + 1, request, length);
    return afpfsd_ipc_write_all(child_sock, message, length + 1);
}

/* Forward response from child daemon to client */
static int forward_child_response(int child_sock, int client_fd)
{
    struct afpfsd_ipc_response response;
    char *buffer = NULL;

    if (afpfsd_ipc_read_all(child_sock, &response, sizeof(response)) < 0) {
        goto child_error;
    }

    if (response.len > AFPFSD_IPC_MAX_RESPONSE) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Child response exceeds maximum allowed size");
        goto child_error;
    }

    if (response.len > 0) {
        buffer = malloc(response.len);

        if (!buffer || afpfsd_ipc_read_all(child_sock, buffer,
                                           response.len) < 0) {
            free(buffer);
            goto child_error;
        }
    }

    if (send_response(client_fd, response.result, buffer, response.len) < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Failed to forward response to client");
        free(buffer);
        return -1;
    }

    free(buffer);
    return 0;
child_error:

    if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR, NULL, 0) < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Failed to send error response to client");
    }

    return -1;
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

/*!
 * @brief Detach the FUSE mount at a path
 *
 * On Linux a user unmounts through the setuid fusermount helper, lazily so
 * a busy mount detaches at once; elsewhere a mount's owner may force it.
 *
 * @param[in] path  the mount point
 * @returns 0, or -1 when the mount stays
 */
static int release_mount(const char *path)
{
#ifdef __linux__
    int status;
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        execlp(FUSERMOUNT, FUSERMOUNT, "-u", "-z", path, (char *) NULL);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
#else
    return unmount(path, MNT_FORCE);
#endif
}

/* The longest wait for a mount point's attributes */
#define PROBE_WAIT_MS 2000

/*!
 * @brief Test whether the mount at a path has lost its FUSE daemon
 *
 * The kernel answers ENOTCONN for a dead FUSE mount once it asks the daemon;
 * Linux answers stat() from attributes cached for attr_timeout. A child asks,
 * so a mount that never answers blocks only the child, which is killed, not
 * waited for.
 *
 * @param[in] path  the mount point
 * @returns 1 for a dead mount, 0 otherwise
 */
static int mount_is_dead(const char *path)
{
    const struct timespec step = { 0, SPAWN_WAIT_STEP_MS * 1000000L };
    int status;
    pid_t pid = fork();

    if (pid < 0) {
        return 0;
    }

    if (pid == 0) {
        /* musl before 1.2.5 has the flag without statx(). */
#if defined(AT_STATX_FORCE_SYNC) && defined(STATX_TYPE)
        struct statx stx;
        _exit(statx(AT_FDCWD, path, AT_STATX_FORCE_SYNC, STATX_TYPE, &stx) != 0
              && errno == ENOTCONN ? 0 : 1);
#else
        struct stat st;
        _exit(stat(path, &st) != 0 && errno == ENOTCONN ? 0 : 1);
#endif
    }

    for (int waited = 0; waited < PROBE_WAIT_MS;
            waited += SPAWN_WAIT_STEP_MS) {
        pid_t ended = waitpid(pid, &status, WNOHANG);

        if (ended == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

        if (ended < 0 && errno != EINTR) {
            return 0;
        }

        nanosleep(&step, NULL);
    }

    kill(pid, SIGKILL);
    return 0;
}

/*!
 * @brief Release the mount at a path when its daemon has ended
 *
 * @param[in] path  the mount point
 * @returns 1 when a mount was released, 0 when none is dead there or the
 *          path does not answer, -1 when a dead mount stays
 */
int release_dead_mount(const char *path)
{
    char safe_path[PATH_MAX * 4];

    if (!mount_is_dead(path)) {
        return 0;
    }

    sanitize_text(path, safe_path, sizeof(safe_path));

    if (release_mount(path) < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Could not release the mount at %s", safe_path);
        return -1;
    }

    log_for_client(NULL, AFPFSD, LOG_NOTICE,
                   "Released the mount at %s, whose daemon had ended",
                   safe_path);
    return 1;
}

/*!
 * @brief Reap ended children; forget each daemon and release any mount it left
 *
 * Any child is reaped, so neither an unrecorded one nor an orphan that a
 * manager running as a container's init adopts stays a zombie.
 */
static void reap_children(void)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct manager_child *child = child_list;
        char safe_mountpoint[PATH_MAX * 4];

        while (child && child->pid != pid) {
            child = child->next;
        }

        if (child == NULL) {
            continue;
        }

        sanitize_text(child->mountpoint, safe_mountpoint,
                      sizeof(safe_mountpoint));

        if (WIFSIGNALED(status)) {
            log_for_client(NULL, AFPFSD, LOG_NOTICE,
                           "The daemon of %s ended with signal %d",
                           safe_mountpoint, WTERMSIG(status));
        } else {
            log_for_client(NULL, AFPFSD, LOG_INFO,
                           "The daemon of %s exited with status %d",
                           safe_mountpoint, WEXITSTATUS(status));
        }

        release_dead_mount(child->mountpoint);
        remove_child(pid);
    }
}

/* A daemon's exit unmounts and logs out, each a request of up to 5 s */
#define EXIT_WAIT_MS 10000

/*!
 * @brief Wait until every recorded daemon has ended, or a time has passed
 *
 * @param[in] limit_ms  the longest wait, in milliseconds
 */
static void wait_for_children(int limit_ms)
{
    const struct timespec step = { 0, SPAWN_WAIT_STEP_MS * 1000000L };
    reap_children();

    for (int waited = 0; child_list != NULL && waited < limit_ms;
            waited += SPAWN_WAIT_STEP_MS) {
        nanosleep(&step, NULL);
        reap_children();
    }
}

/*!
 * @brief Ask every mount daemon to exit; stop the recorded ones that do not
 *
 * A recorded daemon that ends is reaped, and any mount it left released.
 */
static void cleanup_all_children(void)
{
    struct manager_child *child = child_list;
    /* Also notify any stray per-mount daemons that were not tracked. */
    {
        glob_t g = {0};
        char pattern[PATH_MAX];
        int len = snprintf(pattern, sizeof(pattern), "%s-%d-*",
                           AFPFSD_IPC_SOCKET_PATH, (int) geteuid());

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

    wait_for_children(EXIT_WAIT_MS);

    for (child = child_list; child; child = child->next) {
        kill(child->pid, SIGTERM);
    }

    wait_for_children(1000);

    for (child = child_list; child; child = child->next) {
        kill(child->pid, SIGKILL);
    }

    wait_for_children(1000);

    while (child_list) {
        remove_child(child_list->pid);
    }
}

/*!
 * @brief Start the daemon of a mount point and keep its record
 *
 * In the foreground the daemon keeps the pid that the manager waits for.
 *
 * @param[in] socket_id   the socket the daemon listens on
 * @param[in] mountpoint  the mount point it serves
 * @param[in] volumename  the volume it mounts, or NULL
 * @returns 0, or -1 when the fork fails
 */
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
        char *argv[9];
        int argc = 0;
        argv[argc++] = "afpfsd";
        argv[argc++] = "--socket-id";
        argv[argc++] = socket_id;
        argv[argc++] = "--logmethod";
        argv[argc++] = log_method_str;
        argv[argc++] = "--loglevel";
        argv[argc++] = log_level_str;
        argv[argc++] = "--foreground";
        argv[argc] = NULL;
        execvp("afpfsd", argv);
        /* If exec fails */
        _exit(1);
    }

    /* Parent process */
    add_child(pid, socket_id, mountpoint, volumename);
    return 0;
}

/*!
 * @brief Answer an unmount of a mount point no daemon serves
 *
 * Only a dead mount is released; anything else there is not this client's.
 *
 * @param[in] client_fd   connected socket of the client
 * @param[in] mountpoint  the mount point
 * @returns 0, or -1 when the answer cannot be sent
 */
static int unmount_without_daemon(int client_fd, const char *mountpoint)
{
    char text[1024];
    char safe_mountpoint[PATH_MAX * 4];
    char result = AFPFSD_IPC_RESULT_OK;
    int pos = 0;
    int released;
    text[0] = '\0';
    sanitize_text(mountpoint, safe_mountpoint, sizeof(safe_mountpoint));
    released = release_dead_mount(mountpoint);

    if (released > 0) {
        append_text(text, sizeof(text), &pos, "Volume ");
        append_text(text, sizeof(text), &pos, safe_mountpoint);
        append_text(text, sizeof(text), &pos, " unmounted\n");
    } else {
        result = AFPFSD_IPC_RESULT_ERROR;
        append_text(text, sizeof(text), &pos,
                    released < 0
                    ? "Could not release the dead mount on "
                    : "Netatalk Client doesn't have anything mounted on ");
        append_text(text, sizeof(text), &pos, safe_mountpoint);
        append_text(text, sizeof(text), &pos, "\n");
    }

    return send_response(client_fd, result, text, strlen(text));
}

/*!
 * @brief Wait until a daemon of a mount point listens, or none is left
 *
 * A recorded daemon that does not listen is starting, or ending: one stops
 * listening after its last unmount or a mount that left it no server.
 *
 * @param[in] mountpoint  the mount point
 * @returns 0 once no daemon of the mount point is left, 1 when one listens,
 *          -1 when one neither listens nor ends in time
 */
static int wait_for_mount_daemon(const char *mountpoint)
{
    const struct timespec step = { 0, SPAWN_WAIT_STEP_MS * 1000000L };
    const struct manager_child *child;

    for (int waited = 0;
            (child = find_child_by_mountpoint(mountpoint, -1)) != NULL;
            waited += SPAWN_WAIT_STEP_MS) {
        int sock = connect_to_child_socket(child->socket_id, mountpoint, -1);

        if (sock >= 0) {
            close(sock);
            return 1;
        }

        if (waited >= SPAWN_WAIT_LIMIT_MS) {
            return -1;
        }

        nanosleep(&step, NULL);
        reap_children();
    }

    return 0;
}

/*!
 * @brief Execute one command a client sends the FUSE manager
 *
 * Starts a daemon unless one serves the mount point, lists the mounts it
 * spawned, forwards a mount's commands, and releases a dead daemon's mount.
 *
 * @param[in] client_fd  connected socket of the client
 * @returns 0, -1 when the exchange or the command fails, or -2 when the
 *          client asks the manager to exit
 */
static int handle_manager_command(int client_fd)
{
    unsigned char command;

    if (afpfsd_ipc_read_all(client_fd, &command, sizeof(command)) < 0) {
        return -1;
    }

    switch (command) {
    case AFPFSD_IPC_COMMAND_SPAWN_MOUNT: {
        struct afpfsd_ipc_spawn_mount_request req;
        unsigned char result = AFPFSD_IPC_RESULT_ERROR;
        int command_result = 0;
        int request_is_valid;

        if (afpfsd_ipc_read_all(client_fd, &req, sizeof(req)) < 0) {
            return -1;
        }

        request_is_valid = memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) != NULL
                           &&
                           memchr(req.socket_id, '\0', sizeof(req.socket_id)) != NULL &&
                           memchr(req.volumename, '\0', sizeof(req.volumename)) != NULL;
        int serving = request_is_valid ? wait_for_mount_daemon(req.mountpoint)
                      : 0;

        if (serving > 0) {
            /* The mount point's daemon answers the client's mount itself. */
            result = AFPFSD_IPC_RESULT_OK;
        } else if (serving < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "A mount point's daemon neither listens nor ends");
        } else if (request_is_valid) {
            release_dead_mount(req.mountpoint);
            command_result = start_mount_daemon(req.socket_id, req.mountpoint,
                                                req.volumename);

            if (command_result == 0) {
                result = AFPFSD_IPC_RESULT_OK;
            }
        }

        if (afpfsd_ipc_write_all(client_fd, &result, sizeof(result)) < 0) {
            return -1;
        }

        if (command_result < 0) {
            return -1;
        }

        break;
    }

    case AFPFSD_IPC_COMMAND_EXIT: {
        if (send_response(client_fd, AFPFSD_IPC_RESULT_OK, NULL, 0) < 0) {
            return -1;
        }

        return -2;  /* Signal to exit manager; caller closes client_fd */
    }

    case AFPFSD_IPC_COMMAND_PING: {
        unsigned char result = AFPFSD_IPC_RESULT_OK;

        if (afpfsd_ipc_write_all(client_fd, &result, sizeof(result)) < 0) {
            return -1;
        }

        break;
    }

    case AFPFSD_IPC_COMMAND_STATUS: {
        struct afpfsd_ipc_status_request req;

        if (afpfsd_ipc_read_all(client_fd, &req, sizeof(req)) < 0) {
            /* Read error */
            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
                return -1;
            }

            break;
        }

        if (memchr(req.volumename, '\0', sizeof(req.volumename)) == NULL ||
                memchr(req.servername, '\0', sizeof(req.servername)) == NULL ||
                memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) == NULL) {
            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
                return -1;
            }

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
            if (send_child_command(child_sock, AFPFSD_IPC_COMMAND_STATUS,
                                   &req, sizeof(req)) < 0) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send status command to child daemon");

                if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                                  NULL, 0) < 0) {
                    close(child_sock);
                    return -1;
                }

                close(child_sock);
                break;
            }

            forward_child_response(child_sock, client_fd);
            close(child_sock);
        } else {
            /* Manager daemon: return overview with header and list of mounts */
            char text[4096];
            int len = sizeof(text);
            int pos = 0;
            int count = 0;
            struct manager_child *child;

            /* Include the standard header */
            if (afp_status_header(text, &len) >= 0) {
                pos = strlen(text);
                len = sizeof(text) - pos;
            }

            /* Active mounts: ended daemons are reaped before each command. */
            for (child = child_list; child; child = child->next) {
                count++;
            }

            if (count == 0) {
                append_text(text, sizeof(text), &pos,
                            "Manager daemon: no active mounts");
            } else {
                char count_text[64];
                snprintf(count_text, sizeof(count_text),
                         "Manager daemon: %d active mount%s",
                         count, count == 1 ? "" : "s");
                append_text(text, sizeof(text), &pos, count_text);
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
                            "\nRun 'afpc fs status [mountpoint]' for details");
            }

            append_text(text, sizeof(text), &pos, "\n");

            if (send_response(client_fd, AFPFSD_IPC_RESULT_OK, text,
                              strlen(text)) < 0) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Failed to send status response to client");
                return -1;
            }
        }

        break;
    }

    case AFPFSD_IPC_COMMAND_SUSPEND:
    case AFPFSD_IPC_COMMAND_RESUME:
    case AFPFSD_IPC_COMMAND_UNMOUNT: {
        /* Forwarded; afpc sends an unmount here when no daemon listens. */
        _Static_assert(sizeof(struct afpfsd_ipc_unmount_request)
                       == sizeof(struct afpfsd_ipc_suspend_request)
                       && sizeof(struct afpfsd_ipc_resume_request)
                       == sizeof(struct afpfsd_ipc_suspend_request),
                       "each request is the mount point alone");
        struct afpfsd_ipc_suspend_request req;

        if (afpfsd_ipc_read_all(client_fd, &req, sizeof(req)) < 0) {
            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
                return -1;
            }

            break;
        }

        if (memchr(req.mountpoint, '\0', sizeof(req.mountpoint)) == NULL) {
            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
                return -1;
            }

            break;
        }

        if (command == AFPFSD_IPC_COMMAND_UNMOUNT
                && find_child_by_mountpoint(req.mountpoint, -1) == NULL) {
            if (unmount_without_daemon(client_fd, req.mountpoint) < 0) {
                return -1;
            }

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
        if (send_child_command(child_sock, command, &req, sizeof(req)) < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Failed to forward a command to child daemon");

            if (send_response(client_fd, AFPFSD_IPC_RESULT_ERROR,
                              NULL, 0) < 0) {
                close(child_sock);
                return -1;
            }

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

/*!
 * @brief Run the manager, one client command at a time
 *
 * @returns 0 once asked to exit or when its wait fails, or -1 when the
 *          listener cannot be made
 */
static int run_manager_daemon(void)
{
    int listen_fd = startup_listener();

    if (listen_fd < 0) {
        return -1;
    }

    /* An inherited SIG_IGN would let no daemon be waited for. */
    signal(SIGCHLD, SIG_DFL);
    char safe_commandfilename[PATH_MAX * 4];
    sanitize_text(commandfilename, safe_commandfilename,
                  sizeof(safe_commandfilename));
    log_for_client(NULL, AFPFSD, LOG_NOTICE,
                   "Starting manager daemon on %s", safe_commandfilename);

    while (1) {
        fd_set rds;
        FD_ZERO(&rds);
        FD_SET(listen_fd, &rds);
        struct timeval tv = {0, MANAGER_SWEEP_USEC};
        reap_children();
        int ret = select(listen_fd + 1, &rds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        if (ret == 0) {
            continue;
        }

        if (FD_ISSET(listen_fd, &rds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

            if (client_fd < 0) {
                continue;
            }

            struct timeval command_wait = { COMMAND_WAIT_S, 0 };

            /* A spawned daemon inherits no client socket. */
            if (fcntl(client_fd, F_SETFD, FD_CLOEXEC) != 0
                    || setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                                  &command_wait, sizeof(command_wait)) != 0) {
                close(client_fd);
                continue;
            }

            /* The command sees no daemon that ended since the pass began. */
            reap_children();
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

/*!
 * @brief Fork into the background, in a session of its own
 *
 * A terminal's signals then miss the daemon, and unless it logs to stdout, a
 * pipe that $(mount_afpfs ...) reads to its end does not stay open.
 *
 * @returns 1 in the child, 0 in the parent or when the fork fails
 */
static int go_background(void)
{
    int null;

    if (fork() != 0) {
        return 0;
    }

    setsid();

    if ((null = open("/dev/null", O_RDWR)) >= 0) {
        dup2(null, STDIN_FILENO);

        if (current_log_method & LOG_METHOD_STDOUT) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        } else {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }

        if (null > STDERR_FILENO) {
            close(null);
        }
    }

    return 1;
}

/*!
 * @brief Start afpfsd as the manager or as the daemon of a mount
 *
 * @param[in] argc  argument count
 * @param[in] argv  the options
 * @returns 0, or -1 for a bad option, a failed start or another daemon on
 *          the socket; a background start returns 0 even if fork() fails
 */
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
        {"ready-fd", 1, 0, 'r'},
        {0, 0, 0, 0},
    };
    int new_log_method = LOG_METHOD_SYSLOG;
    int log_method_set = 0;
    int log_level = LOG_NOTICE;
    int dofork = 1;
    int manager_mode = 0;
    int ready_fd = -1;
    /* getopt_long()'s return is int; specifying the variable to contain
     * this return value as char depends on endian-specific behavior,
     * breaking utterly on big endian (i.e., PowerPC)
     */
    int c;
    int command_fd = -1;
    const char *socket_id = NULL;

    while (1) {
        c = getopt_long(argc, argv, "dfhl:mr:s:v:",
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

            log_method_set = 1;
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

        case 'r': {
            char *end;
            long fd = strtol(optarg, &end, 10);

            if (*optarg == '\0' || *end != '\0' || fd < 0 || fd > INT_MAX) {
                printf("Unknown ready descriptor %s\n", optarg);
                usage();
                return -1;
            }

            ready_fd = (int) fd;
            break;
        }

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
    if (dofork == 0 && !log_method_set) {
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

    if (install_fuse_wake_signal_handler() != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Could not install FUSE wake signal handler: %s",
                       strerror(errno));
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
                 AFPFSD_IPC_SOCKET_PATH, geteuid());
    }

    /* Mount daemons (not manager) should auto-shutdown when last volume unmounts */
    if (socket_id != NULL && !manager_mode) {
        afp_set_auto_shutdown_on_unmount(1);
    }

    if (remove_other_daemon() < 0) {
        return -1;
    }

    if ((!dofork) || go_background()) {
        if (manager_mode) {
            return run_manager_daemon();
        }

        /* Run mount daemon */
        if ((command_fd = startup_listener()) < 0) {
            goto error;
        }

        /* afpc sends its mount once its own child holds the listener. */
        if (ready_fd >= 0) {
            if (write(ready_fd, "", 1) != 1) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Could not tell afpc the daemon listens: %s",
                               strerror(errno));
            }

            close(ready_fd);
        }

        command_fd_global = command_fd;
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Starting up AFP FUSE controller daemon version %s", NETATALK_CLIENT_VERSION);
        afp_main_loop(command_fd);
        fuse_stop_listener();
    }

    return 0;
error:
    printf("Could not start afpfsd\n");
    return -1;
}
