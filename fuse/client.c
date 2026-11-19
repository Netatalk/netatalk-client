/*
 *  client.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief afpc fs and mount_afpfs: FUSE manager requests from command lines
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <limits.h>

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "lib/afp_internal.h"
#include "lib/client.h"
#include "lib/compat.h"
#include "lib/mapping.h"
#include "lib/uam_registry.h"
#include "lib/utils.h"

#include "discovery/client/discover.h"
#include "fuse_ipc.h"
#include "controller_fs.h"

#define default_uam "Cleartxt Passwrd"

#define MAX_OUTGOING_LENGTH 8192

#define AFPFSD_FILENAME "afpfsd"
#define DEFAULT_MOUNT_FLAGS (VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE|\
	VOLUME_EXTRA_FLAGS_NO_LOCKING | VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS)

static char outgoing_buffer[MAX_OUTGOING_LENGTH];
static int outgoing_len = 0;
static unsigned int uid, gid = 0;
static int changeuid = 0;
static int changegid = 0;
static char *thisbin;
static int client_log_min_rank = 2; /* Default to LOG_NOTICE */
static int list_volumes_only = 0;
static int foreground_mount = 0;
/* The foreground mount's daemon, or 0, and how many end signals afpc took */
static volatile sig_atomic_t foreground_child = 0;
static volatile sig_atomic_t foreground_stops = 0;
static char foreground_mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
static char foreground_socket[PATH_MAX];

static int wait_readable(int sock);
static int send_command(int sock, char *msg, int len);
static int read_response(int sock);

/* struct afpc_url owns path storage, but the FUSE mount request is written
 * verbatim to another process.  A mount never consumes a URL subdirectory,
 * so release it before serializing the request rather than sending a pointer
 * that is meaningful only in this process. */
static void clear_outgoing_mount_url_path(void)
{
    struct afpfsd_ipc_mount_request *request;

    if (outgoing_buffer[0] != AFPFSD_IPC_COMMAND_MOUNT
            || outgoing_len < (int)(sizeof(*request) + 1U)) {
        return;
    }

    request = (struct afpfsd_ipc_mount_request *)(outgoing_buffer + 1);
    afpc_path_clear(&request->url.path);
}

/* Log handler that filters by log level */
static void client_log_for_client(void *priv _U_,
                                  enum logtypes logtype _U_,
                                  int loglevel, const char *message)
{
    int type_rank = loglevel_to_rank(loglevel);

    if (type_rank < client_log_min_rank) {
        return; /* Filter out less-verbose messages */
    }

    printf("%s\n", message);
}

static struct libafpclient client = {
    .unmount_volume = NULL,
    .log_for_client = client_log_for_client,
    .forced_ending_hook = NULL,
    .scan_extra_fds = NULL,
    .loop_started = NULL,
};

/* Forward declaration for get_daemon_filename */
static void get_daemon_filename(char *filename, size_t size,
                                const char *mountpoint);

/* SIGCHLD handler to reap child processes and prevent zombies */
static void sigchld_handler(int sig)
{
    (void)sig;  /* Unused parameter */
    int status;

    /* Reap all available child processes */
    while (waitpid(-1, &status, WNOHANG) > 0) {
        /* Child has been reaped */
    }
}

/*!
 * @brief Exec afpfsd in this process
 *
 * @param[in] argv  afpfsd's arguments, its name first
 */
static _Noreturn void exec_afpfsd(char *argv[])
{
    char filename[PATH_MAX];
    snprintf(filename, PATH_MAX, AFPFSD_FILENAME);

    if (getenv("PATH") == NULL) {
        /* mount runs afpc without a PATH. */
        snprintf(filename, PATH_MAX,
                 "/usr/local/bin/%s", AFPFSD_FILENAME);

        if (access(filename, X_OK)) {
            snprintf(filename, sizeof(filename), "/usr/bin/%s",
                     AFPFSD_FILENAME);
            filename[sizeof(filename) - 1] = 0;

            if (access(filename, X_OK)) {
                fprintf(stderr, "Could not find server (%s)\n",
                        filename);
                _exit(1);
            }
        }
    }

    if (execvp(filename, argv)) {
        if (errno == ENOENT) {
            /* Try the directory containing afpc. */
            char newpath[PATH_MAX];
            char binary_path[PATH_MAX];
            char *directory;

            if (strlcpy(binary_path, thisbin, sizeof(binary_path))
                    >= sizeof(binary_path)) {
                perror("Starting up afpfsd");
                _exit(1);
            }

            directory = dirname(binary_path);
            snprintf(newpath, sizeof(newpath), "%s/%s", directory,
                     AFPFSD_FILENAME);

            if (execvp(newpath, argv)) {
                perror("Starting up afpfsd");
                _exit(1);
            }
        } else {
            perror("Starting up afpfsd");
            _exit(1);
        }
    }

    /* execvp never returns on success */
    _exit(1);
}

/*!
 * @brief Start the manager, which goes into the background as it starts
 *
 * @returns 0
 */
static int start_manager_daemon(void)
{
    char *argv[4];
    int argc = 0;
    struct sigaction sa, old_sa;
    argv[argc++] = AFPFSD_FILENAME;
    argv[argc++] = "--manager";
    argv[argc] = NULL;
    /* Temporarily install SIGCHLD handler to reap any child process failures */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, &old_sa);

    if (fork() == 0) {
        if (changegid) {
            if (setegid(gid)) {
                perror("Changing gid");
                _exit(1);
            }
        }

        if (changeuid) {
            if (seteuid(uid)) {
                perror("Changing uid");
                _exit(1);
            }
        }

        exec_afpfsd(argv);
    }

    /* Restore old signal handler */
    sigaction(SIGCHLD, &old_sa, NULL);
    return 0;
}

/* A daemon or manager not listening within the limit failed to start. */
#define CONNECT_RETRY_MS 20
#define CONNECT_RETRY_LIMIT_MS 2000

/*!
 * @brief Connect to the socket of a daemon
 *
 * @param[in] path  the socket
 * @returns the connected socket, or -1 with errno set
 */
static int connect_socket(const char *path)
{
    struct sockaddr_un servaddr;
    int sock;
    int error;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;

    if (strlcpy(servaddr.sun_path, path, sizeof(servaddr.sun_path))
            >= sizeof(servaddr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&servaddr,
                sizeof(servaddr.sun_family) + sizeof(servaddr.sun_path)) < 0) {
        error = errno;
        close(sock);
        errno = error;
        return -1;
    }

    return sock;
}

/*!
 * @brief Connect to the socket of a daemon that is starting
 *
 * @param[in] path  the socket
 * @returns the connected socket, or -1 with errno set when the daemon does
 *          not listen within CONNECT_RETRY_LIMIT_MS
 */
static int connect_starting_socket(const char *path)
{
    const struct timespec step = { 0, CONNECT_RETRY_MS * 1000000L };
    int sock = connect_socket(path);

    for (int waited = 0; sock < 0 && waited < CONNECT_RETRY_LIMIT_MS;
            waited += CONNECT_RETRY_MS) {
        nanosleep(&step, NULL);
        sock = connect_socket(path);
    }

    return sock;
}

/*!
 * @brief Connect to the manager, starting it when none listens
 *
 * @returns the connected socket, or -1
 */
static int manager_connect(void)
{
    char manager_socket[PATH_MAX];
    int sock;
    get_daemon_filename(manager_socket, sizeof(manager_socket), NULL);

    if ((sock = connect_socket(manager_socket)) >= 0) {
        return sock;
    }

    if (start_manager_daemon() != 0) {
        return -1;
    }

    if ((sock = connect_starting_socket(manager_socket)) < 0) {
        perror("Could not connect to manager daemon");
    }

    return sock;
}

/*!
 * @brief Ask the manager to start the daemon of a mount point
 *
 * The manager starts none when one already serves the mount point.
 *
 * @param[in] mountpoint  the mount point
 * @param[in] volumename  the volume the mount names, or NULL
 * @returns 0 once the manager has answered yes, or -1
 */
static int start_afpfsd(const char *mountpoint, const char *volumename)
{
    int sock;
    char mount_socket[PATH_MAX];
    struct afpfsd_ipc_spawn_mount_request req;
    unsigned char command = AFPFSD_IPC_COMMAND_SPAWN_MOUNT;
    unsigned char result;
    /* Get mount-specific socket name */
    get_daemon_filename(mount_socket, sizeof(mount_socket), mountpoint);

    if ((sock = manager_connect()) < 0) {
        return -1;
    }

    /* Send spawn mount request */
    memset(&req, 0, sizeof(req));
    snprintf(req.mountpoint, sizeof(req.mountpoint), "%s", mountpoint);
    snprintf(req.socket_id, sizeof(req.socket_id), "%s", mount_socket);
    snprintf(req.volumename, sizeof(req.volumename), "%s",
             volumename ? volumename : "");

    if (afpfsd_ipc_write_all(sock, &command, sizeof(command)) != 0) {
        close(sock);
        return -1;
    }

    if (afpfsd_ipc_write_all(sock, &req, sizeof(req)) != 0) {
        close(sock);
        return -1;
    }

    /* Wait for response */
    if (wait_readable(sock) != 0
            || afpfsd_ipc_read_all(sock, &result, sizeof(result)) != 0) {
        close(sock);
        return -1;
    }

    close(sock);

    if (result != AFPFSD_IPC_RESULT_OK) {
        return -1;
    }

    return 0;
}


/* Each mount gets a unique daemon process for fault isolation.
 * Management commands use NULL mountpoint to get shared management socket. */
static void get_daemon_filename(char *filename, size_t size,
                                const char *mountpoint)
{
    unsigned long hash = 5381;

    if (mountpoint) {
        /* Hash the mountpoint path to create unique socket per mount */
        for (const char *p = mountpoint; *p; p++) {
            hash = ((hash << 5) + hash) ^ (unsigned char) * p;
        }

        /* One daemon per mountpoint */
        snprintf(filename, size, "%s-%d-%lx", AFPFSD_IPC_SOCKET_PATH, uid, hash);
    } else {
        /* Shared management socket for status/exit commands */
        snprintf(filename, size, "%s-%d", AFPFSD_IPC_SOCKET_PATH, uid);
    }
}

/*!
 * @brief Connect to the daemon a command goes to
 *
 * With no daemon listening at the mount point, a mount has the manager start
 * one, and another command goes to the manager, where an unmount releases a
 * dead mount.
 *
 * @param[in] mountpoint  the mount point, or NULL
 * @param[in] volumename  the volume a mount names, or NULL
 * @param[in] command     the command byte
 * @returns the connected socket, or -1
 */
static int daemon_connect(const char *mountpoint, const char *volumename,
                          unsigned char command)
{
    char filename[PATH_MAX];
    int sock;

    if (mountpoint == NULL) {
        return manager_connect();
    }

    get_daemon_filename(filename, sizeof(filename), mountpoint);

    if ((sock = connect_socket(filename)) >= 0) {
        return sock;
    }

    if (command != AFPFSD_IPC_COMMAND_MOUNT) {
        return manager_connect();
    }

    if (start_afpfsd(mountpoint, volumename) != 0) {
        perror("Error in starting up afpfsd");
        return -1;
    }

    if ((sock = connect_starting_socket(filename)) < 0) {
        perror("Trying to startup afpfsd");
    }

    return sock;
}

/*!
 * @brief Take a signal that ends a mount in the foreground
 *
 * wait_foreground_daemon() acts on the first, asking the daemon to unmount.
 *
 * @param[in] sig  the signal, SIGINT, SIGQUIT, SIGTERM or SIGHUP
 */
static void stop_foreground(int sig)
{
    /* One hangup brings SIGHUP twice, from the shell and from the kernel as
     * the session leader exits, so a SIGHUP only asks for the unmount. */
    if (sig == SIGHUP && foreground_stops > 0) {
        return;
    }

    if (++foreground_stops > 1 && foreground_child > 0) {
        kill((pid_t) foreground_child, SIGTERM);
    }
}

/*!
 * @brief Take SIGCHLD, which wakes the wait for a daemon in the foreground
 *
 * @param[in] sig  the signal
 */
static void wake_for_child(int sig _U_)
{
    /* The wait reaps the daemon itself. */
}

/*!
 * @brief Wait for a foreground daemon's byte on its ready pipe
 *
 * @param[in] fd  the pipe's read end
 * @returns 1 once the daemon listens, 0 when it ended without listening or
 *          did not listen within CONNECT_RETRY_LIMIT_MS
 */
static int daemon_listens(int fd)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    char byte;
    int ret;

    do {
        ret = poll(&pfd, 1, CONNECT_RETRY_LIMIT_MS);
    } while (ret < 0 && errno == EINTR);

    return ret > 0 && read(fd, &byte, 1) == 1;
}

/*!
 * @brief Start a mount point's daemon as a child of afpc and connect to it
 *
 * The daemon inherits afpc's descriptors, the one a /dev/fd/N mount point
 * names among them, and logs where afpc writes.
 *
 * @param[in] mountpoint  the mount point
 * @returns the connected socket, or -1 once any daemon it started has ended
 */
static int start_foreground_daemon(const char *mountpoint)
{
    char ready_arg[12];
    char *argv[] = { AFPFSD_FILENAME, "--socket-id", foreground_socket,
                     "--foreground", "--ready-fd", ready_arg, NULL
                   };
    static const int stops[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP };
    struct sigaction sa;
    int ready[2];
    pid_t pid;
    int sock;
    get_daemon_filename(foreground_socket, sizeof(foreground_socket),
                        mountpoint);
    snprintf(foreground_mountpoint, sizeof(foreground_mountpoint), "%s",
             mountpoint);

    if ((sock = connect_socket(foreground_socket)) >= 0) {
        close(sock);
        fprintf(stderr, "A daemon already serves %s\n", mountpoint);
        return -1;
    }

    if (pipe(ready) != 0) {
        perror("Starting up afpfsd");
        return -1;
    }

    snprintf(ready_arg, sizeof(ready_arg), "%d", ready[1]);
    /* SIGCHLD inherited as ignored would reap the daemon before the wait. */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = wake_for_child;
    sigaction(SIGCHLD, &sa, NULL);

    if ((pid = fork()) < 0) {
        perror("Starting up afpfsd");
        close(ready[0]);
        close(ready[1]);
        return -1;
    }

    if (pid == 0) {
        close(ready[0]);
        /* No terminal's or job's signal reaches a session of its own. */
        setsid();
        exec_afpfsd(argv);
    }

    close(ready[1]);
    foreground_child = pid;
    sa.sa_handler = stop_foreground;
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGQUIT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sigaddset(&sa.sa_mask, SIGHUP);

    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        struct sigaction old;

        /* What nohup or a shell's background command ignores stays ignored. */
        if (sigaction(stops[i], NULL, &old) != 0
                || old.sa_handler != SIG_IGN) {
            sigaction(stops[i], &sa, NULL);
        }
    }

    /* A child that lost the mount point's socket to another daemon ends
     * without the byte, and that other daemon must not take this mount. */
    if (!daemon_listens(ready[0])) {
        close(ready[0]);
        fprintf(stderr, "The daemon of %s did not start\n", mountpoint);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }

    close(ready[0]);

    if ((sock = connect_starting_socket(foreground_socket)) < 0) {
        perror("Trying to startup afpfsd");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }

    return sock;
}

/*!
 * @brief Ask the daemon of a mount in the foreground to unmount it
 *
 * The daemon unmounts as at afpc fs unmount; with libfuse 3 outside macOS
 * it logs out and ends with its last server.
 *
 * @returns 1 once the daemon has unmounted, 0 when it has not
 */
static int unmount_foreground(void)
{
    struct afpfsd_ipc_unmount_request request;
    char command[1 + sizeof(request)];
    int unmounted = 0;
    int sock = connect_socket(foreground_socket);

    if (sock < 0) {
        return 0;
    }

    memset(&request, 0, sizeof(request));
    snprintf(request.mountpoint, sizeof(request.mountpoint), "%s",
             foreground_mountpoint);
    command[0] = AFPFSD_IPC_COMMAND_UNMOUNT;
    memcpy(command + 1, &request, sizeof(request));

    if (send_command(sock, command, (int) sizeof(command)) == 0) {
        unmounted = read_response(sock) == AFPFSD_IPC_RESULT_OK;
    }

    close(sock);
    return unmounted;
}

/*!
 * @brief Wait for the daemon of a mount in the foreground to end
 *
 * The daemon ends with its mount, which afpc's first end signal unmounts with
 * libfuse 3 outside macOS. Signals are held until the wait, so none is lost.
 *
 * @param[in] result  the mount's result
 * @returns the mount's result when it failed, else 0 for a daemon that
 *          exited with status 0, or 1
 */
static int wait_foreground_daemon(int result)
{
    pid_t pid = (pid_t) foreground_child;
    sigset_t held;
    sigset_t waiting;
    pid_t ended;
    int asked = 0;
    int status = 0;

    if (result != 0) {
        kill(pid, SIGTERM);
    }

    sigemptyset(&held);
    sigaddset(&held, SIGCHLD);
    sigaddset(&held, SIGINT);
    sigaddset(&held, SIGQUIT);
    sigaddset(&held, SIGTERM);
    sigaddset(&held, SIGHUP);
    /* The wait takes these even if afpc started with them blocked. */
    sigprocmask(SIG_UNBLOCK, &held, NULL);
    sigprocmask(SIG_BLOCK, &held, &waiting);

    while ((ended = waitpid(pid, &status, WNOHANG)) == 0) {
        if (result == 0 && foreground_stops > 0 && !asked) {
            /* A second signal during the unmount ends the daemon at once,
             * a SIGHUP excepted. */
            sigprocmask(SIG_SETMASK, &waiting, NULL);
            asked = unmount_foreground();
#if !defined(__APPLE__) && FUSE_USE_VERSION >= 30

            /* A start still pending refuses the unmount, which goes again. */
            if (!asked) {
                sleep(1);
            }

#else
            /* The daemon cannot unmount; its refusal points to umount(8). */
            asked = 1;
#endif
            sigprocmask(SIG_BLOCK, &held, NULL);
            continue;
        }

        sigsuspend(&waiting);
    }

    sigprocmask(SIG_SETMASK, &waiting, NULL);

    if (result != 0) {
        return result;
    }

    return ended == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0
           ? 0 : 1;
}


/*!
 * @brief Print the usage of afpc fs
 */
static void usage(void)
{
    printf(
        "afpc fs <command> [options]\n"
        "    mount [mountopts] <server>:<volume> <mountpoint>\n"
        "    mount --service <name> [mountopts] [--volume <volume>] [mountpoint]\n"
        "         mount options:\n"
        "         -s, --service <name>      : resolve an advertised AFP service\n"
        "             --volume <volume>     : mount this volume from the service\n"
        "                                      If omitted, list available volumes\n"
        "         -u, --user <username>      : log in as user <username>\n"
        "         -p, --pass <password>      : use <password>\n"
        "                                      If password is '-', you get prompted for it\n"
        "         -P, --volpass <password>   : use this volume password\n"
        "                                      If password is '-', you get prompted for it\n"
        "         -o, --port <portnum>       : connect using <portnum> instead of 548\n"
        "         -f, --afpversion <version> : set the AFP version, eg. 3.1\n"
        "         -a, --uam <uam>            : use this authentication method, one of:\n"
        "                                      guest, clrtxt, randnum, randnum2, dhx, dhx2\n"
        "         -m, --map <mapname>        : use this uid/gid mapping method, one of:\n"
        "                                      common, loginids\n"
        "         -O, --options <flags>      : FUSE mount options; see the fuse man page\n"
        "         -F, --flush-on-close       : flush a written file at its last close\n"
        "             --locking <yes|no>     : refuse a second open for\n"
        "                                      writing (default no)\n"
        "             --foreground           : keep the mount's daemon under\n"
        "                                      afpc until the mount ends; the\n"
        "                                      mount point may be /dev/fd/N\n"
        "\n"
        "    unmount <mountpoint> : unmount the specified mountpoint\n"
        "    status [mountpoint]  : get status of the AFP daemon;\n"
        "                           If mountpoint is specified, show detailed\n"
        "                           status for that specific mount\n"
        "    suspend <mountpoint> : suspends the connection to the server, but\n"
        "                           maintains the mount.  For laptop suspend/resume\n"
        "    resume  <mountpoint> : resumes the server connection\n"
        "    exit                 : unmounts all volumes and exits afpfsd\n"
    );
}


static char *get_password(const char *prompt)
{
    static char pwd[AFP_MAX_PASSWORD_LEN + 1];
    explicit_bzero(pwd, sizeof(pwd));

    if (isatty(fileno(stdin))) {
        char *pass = getpass(prompt);
        size_t len;

        if (pass == NULL) {
            return NULL;
        }

        len = strnlen(pass, sizeof(pwd));

        if (len >= sizeof(pwd)) {
            explicit_bzero(pass, len);
            return NULL;
        }

        memcpy(pwd, pass, len);
        pwd[len] = '\0';
        explicit_bzero(pass, len);
    } else {
        char *askpass = NULL;
        FILE *fp;

        if (asprintf(&askpass, "ssh-askpass %s", prompt) < 0) {
            return NULL;
        }

        if ((fp = popen(askpass, "r"))) {
            size_t len = fread(pwd, 1, sizeof(pwd) - 1, fp);
            pwd[len] = '\0';
            pclose(fp);

            /* ssh-askpass always adds a newline: chop it. */
            if (len > 0 && pwd[len - 1] == '\n') {
                pwd[len - 1] = '\0';
            }
        } else {
            perror(askpass);
            free(askpass);
            return NULL;
        }

        free(askpass);
    }

    return pwd;
}

static int copy_secret(char *dst, size_t dstlen, const char *src,
                       size_t max_chars, const char *name)
{
    size_t len;

    if (dstlen <= max_chars) {
        fprintf(stderr, "%s destination buffer too small\n", name);
        return -1;
    }

    if (src == NULL) {
        return -1;
    }

    len = strnlen(src, max_chars + 1);

    if (len > max_chars) {
        fprintf(stderr, "%s too long, maximum is %zu characters\n",
                name, max_chars);
        return -1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int send_command(int sock, char *msg, int len)
{
    return afpfsd_ipc_write_all(sock, msg, (size_t)len);
}

static int do_exit(int argc _U_, char **argv _U_)
{
    outgoing_len = 1;
    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_EXIT;
    return 0;
}

/*!
 * @brief Resolve a mount point to the one path its daemon is known by
 *
 * Only the parent is resolved, so a dead or hung mount at the mount point is
 * not looked at; a name that is a symbolic link, . or .. is resolved whole.
 *
 * @param[in]  path      the mount point as given
 * @param[out] resolved  receives the absolute mount point
 * @param[in]  size      bytes of resolved
 * @returns 0, or -1 for a path that does not fit or a working directory
 *          that cannot be read
 */
static int resolve_mountpoint(const char *path, char *resolved, size_t size)
{
    char clean[PATH_MAX];
    char cwd[PATH_MAX];
    char link;
    char *real = NULL;
    char *name;
    const char *tail = NULL;
    size_t len;
    int ret;

    if (path[0] == '/') {
        len = (size_t) snprintf(clean, sizeof(clean), "%s", path);
    } else if (getcwd(cwd, sizeof(cwd)) != NULL) {
        len = (size_t) snprintf(clean, sizeof(clean), "%s/%s", cwd, path);
    } else {
        perror("getcwd");
        return -1;
    }

    if (len >= sizeof(clean)) {
        fprintf(stderr, "Mountpoint path too long\n");
        return -1;
    }

    while (len > 1 && clean[len - 1] == '/') {
        clean[--len] = '\0';
    }

    name = strrchr(clean, '/') + 1;

    /* A descriptor's /dev/fd/N names the descriptor, not a directory. */
    if (strncmp(clean, "/dev/fd/", 8) != 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0
                || readlink(clean, &link, 1) >= 0) {
            real = realpath(clean, NULL);
        } else if (name - clean > 1) {
            name[-1] = '\0';
            real = realpath(clean, NULL);
            name[-1] = '/';
            tail = name;
        }
    }

    if (real == NULL) {
        ret = snprintf(resolved, size, "%s", clean);
    } else if (tail == NULL) {
        ret = snprintf(resolved, size, "%s", real);
    } else {
        ret = snprintf(resolved, size, "%s/%s",
                       strcmp(real, "/") == 0 ? "" : real, tail);
    }

    free(real);

    if (ret < 0 || (size_t) ret >= size) {
        fprintf(stderr, "Mountpoint path too long\n");
        return -1;
    }

    return 0;
}

static int do_status(int argc, char **argv)
{
    int c;
    int option_index = 0;
    struct afpfsd_ipc_status_request req = {0};
    struct option long_options[] = {
        {"volume", 1, 0, 'v'},
        {"server", 1, 0, 's'},
        {0, 0, 0, 0},
    };
    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_STATUS;
    outgoing_len = sizeof(struct afpfsd_ipc_status_request) + 1;

    while (1) {
        c = getopt_long(argc, argv, "v:s:", long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'v':
            snprintf(req.volumename, AFP_VOLUME_NAME_UTF8_LEN, "%s", optarg);
            break;
        }
    }

    /* Check for optional mountpoint argument */
    if (argc > 2
            && resolve_mountpoint(argv[2], req.mountpoint, sizeof(req.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &req, sizeof(req));
    return 0;
}

static int do_resume(int argc, char **argv)
{
    struct afpfsd_ipc_resume_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_RESUME;
    outgoing_len = sizeof(struct afpfsd_ipc_resume_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static int do_suspend(int argc, char **argv)
{
    struct afpfsd_ipc_suspend_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_SUSPEND;
    outgoing_len = sizeof(struct afpfsd_ipc_suspend_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static int do_unmount(int argc, char **argv)
{
    struct afpfsd_ipc_unmount_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_UNMOUNT;
    outgoing_len = sizeof(struct afpfsd_ipc_unmount_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

/*!
 * @brief Read the value of the locking option
 *
 * @param[in]  value    yes or no
 * @param[out] enabled  1 for yes, 0 for no
 * @returns 0, or -1 after saying so for any other value
 */
static int parse_locking(const char *value, int *enabled)
{
    if (strcmp(value, "yes") == 0) {
        *enabled = 1;
    } else if (strcmp(value, "no") == 0) {
        *enabled = 0;
    } else {
        printf("Unknown locking mode %s, use yes or no\n", value);
        return -1;
    }

    return 0;
}

/*!
 * @brief Build the mount request of afpc fs mount from its command line
 *
 * Prompts for a password given as "-", or left empty with a user name.
 *
 * @param[in] argc  argument count
 * @param[in] argv  the arguments, the word mount first
 * @returns 0 with the request in outgoing_buffer, or -1 for bad arguments or
 *          a failed lookup or prompt
 */
static int do_mount(int argc, char **argv)
{
    int c;
    int option_index = 0;
    struct afpfsd_ipc_mount_request request = {0};
    int optnum = 0;
    int port_override = 0;
    int flush_on_close = 0;
    int locking = 0;
    int spec_end = 0;
    unsigned int uam_mask = default_uams_mask();
    const char *service_name = NULL;
    const char *volume_name = NULL;
    enum { OPT_VOLUME = 256, OPT_LOCKING, OPT_FOREGROUND };
    struct option long_options[] = {
        {"afpversion", 1, 0, 'f'},
        {"volpass", 1, 0, 'P'},
        {"user", 1, 0, 'u'},
        {"pass", 1, 0, 'p'},
        {"port", 1, 0, 'o'},
        {"uam", 1, 0, 'a'},
        {"map", 1, 0, 'm'},
        {"options", 1, 0, 'O'},
        {"service", 1, 0, 's'},
        {"volume", 1, 0, OPT_VOLUME},
        {"locking", 1, 0, OPT_LOCKING},
        {"foreground", 0, 0, OPT_FOREGROUND},
        {"flush-on-close", 0, 0, 'F'},
        {0, 0, 0, 0},
    };

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_MOUNT;
    outgoing_len = sizeof(struct afpfsd_ipc_mount_request) + 1;
    request.url.port = 548;
    request.map = AFP_MAPPING_UNKNOWN;
    request.fuse_options[0] = '\0';

    while (1) {
        optnum++;
        c = getopt_long(argc, argv, "a:Ff:m:O:o:P:p:s:u:", long_options,
                        &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case OPT_VOLUME:
            volume_name = optarg;
            break;

        case OPT_LOCKING:
            if (parse_locking(optarg, &locking) < 0) {
                return -1;
            }

            break;

        case OPT_FOREGROUND:
            foreground_mount = 1;
            break;

        case '?':

            /* A --locking without its value would mount without the mode. */
            if (optopt == OPT_LOCKING) {
                return -1;
            }

            break;

        case 'F':
            flush_on_close = 1;
            break;

        case 'a':
            uam_mask = uam_string_to_bitmap(optarg);
            break;

        case 'm':
            request.map = map_string_to_num(optarg);

            /* Validate: if user explicitly provided a value and we got UNKNOWN,
             * they provided an invalid mapping name */
            if (request.map == AFP_MAPPING_UNKNOWN &&
                    strcasecmp(optarg, "unknown") != 0) {
                printf("Invalid mapping method: %s\n", optarg);
                printf("Valid options: \"common\", \"loginids\", "
                       "\"Common user directory\", \"Login ids\"\n");
                return -1;
            }

            break;

        case 'O':
            if (snprintf(request.fuse_options, sizeof(request.fuse_options),
                         "%s", optarg) >= (int)sizeof(request.fuse_options)) {
                fprintf(stderr, "FUSE options are longer than %zu bytes.\n",
                        sizeof(request.fuse_options) - 1);
                return -1;
            }

            break;

        case 'o':
            request.url.port = strtol(optarg, NULL, 10);
            port_override = 1;
            break;

        case 'P':
            snprintf(request.url.volpassword, 9, "%s", optarg);
            break;

        case 'p':
            snprintf(request.url.password, AFP_MAX_PASSWORD_LEN, "%s", optarg);
            break;

        case 's':
            service_name = optarg;
            break;

        case 'u':
            snprintf(request.url.username, AFP_MAX_USERNAME_LEN, "%s", optarg);
            break;

        case 'f':

            /* Parse AFP version string like "3.1" or "22" to integer format.
             * AFP 2.2 = 22, AFP 3.1 = 31, AFP 3.2 = 32, etc. */
            if (afp_parse_version(optarg, &request.url.requested_version) != 0) {
                printf("Invalid AFP version format: %s\n", optarg);
                return -1;
            }

            break;
        }
    }

    optnum = optind;

    if (service_name) {
        uint16_t service_port;
        int ret;

        if (volume_name) {
            if (argc - optnum != 1) {
                fprintf(stderr,
                        "A mount point is required when --volume is specified.\n");
                return -1;
            }

            if (snprintf(request.url.volumename,
                         sizeof(request.url.volumename), "%s", volume_name)
                    >= (int)sizeof(request.url.volumename)) {
                fprintf(stderr, "AFP volume name is too long.\n");
                return -1;
            }
        } else {
            if (argc - optnum > 1) {
                fprintf(stderr,
                        "Use --volume <volume> <mountpoint> to mount an AFP service.\n");
                return -1;
            }

            list_volumes_only = 1;
        }

        ret = afpc_discover_resolve_service(service_name,
                                            request.url.servername,
                                            sizeof(request.url.servername),
                                            &service_port, 1500);

        if (ret == -ENOENT) {
            fprintf(stderr, "No AFP service named '%s' was found.\n",
                    service_name);
            return -1;
        }

        if (ret == -EEXIST) {
            fprintf(stderr,
                    "AFP service name '%s' is ambiguous; use afpc "
                    "discover to inspect its domains and interfaces.\n",
                    service_name);
            return -1;
        }

        if (ret != 0) {
            fprintf(stderr, "Could not resolve AFP service '%s': %s\n",
                    service_name, strerror(-ret));
            return -1;
        }

        if (!port_override) {
            request.url.port = service_port;
        }
    } else {
        if (volume_name) {
            fprintf(stderr, "--volume can only be used with --service.\n");
            return -1;
        }

        if (argc - optnum != 2) {
            fprintf(stderr, "A server:volume and mount point are required.\n");
            return -1;
        }

        /* Each width leaves its field the terminator. */
        if (sscanf(argv[optnum], "%254[^:]:%254[^:]%n",
                   request.url.servername, request.url.volumename,
                   &spec_end) != 2 || argv[optnum][spec_end] != '\0') {
            printf("Incorrect server:volume specification\n");
            return -1;
        }

        optnum++;
    }

    if (uam_mask == 0) {
        printf("Unknown UAM\n");
        return -1;
    }

    request.uam_mask = uam_mask;
    request.volume_options = DEFAULT_MOUNT_FLAGS;

    if (flush_on_close) {
        request.volume_options |= VOLUME_EXTRA_FLAGS_FLUSH_ON_CLOSE;
    }

    if (locking) {
        request.volume_options |= VOLUME_EXTRA_FLAGS_DENY_WRITE;
    }

    if (!list_volumes_only
            && resolve_mountpoint(argv[optnum], request.mountpoint, 255) < 0) {
        printf("Failed to resolve mount point\n");
        return -1;
    }

    /* Handle password prompts - also prompt when username is given
     * but password is empty (without username, guest auth is used) */
    if (strcmp(request.url.password, "-") == 0 ||
            (request.url.username[0] != '\0'
             && request.url.password[0] == '\0')) {
        char *p = get_password("Password: ");

        if (p == NULL ||
                copy_secret(request.url.password,
                            sizeof(request.url.password), p,
                            sizeof(request.url.password) - 1,
                            "Password") < 0) {
            if (p) {
                explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
            }

            return -1;
        }

        explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
    }

    if (!list_volumes_only && strcmp(request.url.volpassword, "-") == 0) {
        char *p = get_password("Volume password:");

        if (p == NULL ||
                copy_secret(request.url.volpassword,
                            sizeof(request.url.volpassword), p,
                            AFP_VOLPASS_LEN, "Volume password") < 0) {
            if (p) {
                explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
            }

            return -1;
        }

        explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

/*!
 * @brief Log in to a service's server and print the volumes it offers
 *
 * @param[in] request  the mount request, whose URL names the server and the
 *                     login
 * @returns 0, or -1 when the client cannot start or the login fails
 */
static int list_authenticated_volumes(
    const struct afpfsd_ipc_mount_request *request)
{
    struct afp_connection_request connection_request;
    struct afp_server *server;
    memset(&connection_request, 0, sizeof(connection_request));
    connection_request.url = request->url;
    connection_request.uam_mask = request->uam_mask;

    if (afp_main_quick_startup(NULL) != 0) {
        fprintf(stderr, "Could not start the AFP client loop.\n");
        return -1;
    }

    if (init_uams() < 0) {
        fprintf(stderr, "Could not initialize AFP authentication methods.\n");
        return -1;
    }

    server = afp_server_full_connect(NULL, &connection_request);

    if (server == NULL) {
        fprintf(stderr, "Could not authenticate with AFP service %s.\n",
                request->url.servername);
        return -1;
    }

    printf("Available volumes on %s:\n", request->url.servername);

    if (server->num_volumes == 0) {
        printf("  (none)\n");
    } else {
        for (int i = 0; i < server->num_volumes; i++) {
            printf("  %s\n", server->volumes[i].volume_name_printable);
        }
    }

    afp_logout(server, 1);
    afp_server_remove(server);
    return 0;
}

/*!
 * @brief Print mount_afpfs's usage
 */
static void mount_afpfs_usage(void)
{
    printf("Netatalk Client %s - mount an AFP network filesystem with FUSE\n"
           "Usage:\n"
           "\tmount_afpfs [-o option[,option...]] <afp url> <mountpoint>\n",
           NETATALK_CLIENT_VERSION);
}

/*!
 * @brief Build the mount request of mount_afpfs from its command line
 *
 * Takes an AFP URL and a mount point after -o options; attr_timeout,
 * entry_timeout and negative_timeout go to libfuse, which parses them.
 *
 * @param[in] argc  argument count
 * @param[in] argv  mount_afpfs's arguments
 * @returns 0 with the request in outgoing_buffer, -1 for bad arguments
 */
static int handle_mount_afpfs(int argc, char *argv[])
{
    struct afpfsd_ipc_mount_request * req = (struct afpfsd_ipc_mount_request *)
                                            &outgoing_buffer[1];
    unsigned int uam_mask = default_uams_mask();
    char *urlstring, *mountpoint;
    char volpass[AFP_VOLPASS_LEN + 1] = {0};
    int have_volpass = 0;
    int readonly = 0;
    int flush_on_close = 0;
    int locking = 0;
    int ret = -1;
    char *temp_url = NULL;

    if (argc > 1 && (strcmp(argv[1], "discover") == 0
                     || strcmp(argv[1], "fs") == 0
                     || strcmp(argv[1], "sl") == 0
                     || strcmp(argv[1], "help") == 0
                     || strcmp(argv[1], "version") == 0)) {
        fprintf(stderr,
                "mount_afpfs: use 'afpc %s' for afpc commands\n", argv[1]);
        return -1;
    }

    if (argc < 2) {
        mount_afpfs_usage();
        return -1;
    }

    outgoing_len = sizeof(struct afpfsd_ipc_mount_request) +1;
    memset(outgoing_buffer, 0, outgoing_len);

    if (strcmp(argv[1], "-o") == 0) {
        char *p, *q;
        char command[256];
        size_t len;
        struct passwd * passwd;
        struct group * group;

        if (argc < 5) {
            printf("Option -o requires an argument, URL, and mount point\n");
            return -1;
        }

        p = argv[2];

        do {
            q = strchr(p, ',');
            len = q ? (size_t)(q - p) : strlen(p);

            if (len >= sizeof(command)) {
                printf("An -o option is longer than %zu bytes\n",
                       sizeof(command) - 1);
                goto cleanup;
            }

            strlcpy(command, p, len + 1);

            if (strncmp(command, "volpass=", 8) == 0) {
                if (copy_secret(volpass, sizeof(volpass), command + 8,
                                AFP_VOLPASS_LEN, "Volume password") < 0) {
                    explicit_bzero(command, sizeof(command));
                    goto cleanup;
                }

                have_volpass = 1;
                explicit_bzero(command, sizeof(command));
            } else if (strncmp(command, "user=", 5) == 0) {
                p = command + 5;

                if ((passwd = getpwnam(p)) == NULL) {
                    printf("Unknown user %s\n", p);
                    goto cleanup;
                }

                uid = passwd->pw_uid;

                if (geteuid() != uid) {
                    changeuid = 1;
                }
            } else if (strncmp(command, "group=", 6) == 0) {
                p = command + 6;

                if ((group = getgrnam(p)) == NULL) {
                    printf("Unknown group %s\n", p);
                    goto cleanup;
                }

                gid = group->gr_gid;
                changegid = 1;
            } else if (strcmp(command, "rw") == 0) {
                /* Don't do anything */
            } else if (strcmp(command, "ro") == 0) {
                readonly = 1;
            } else if (strcmp(command, "flush_on_close") == 0) {
                flush_on_close = 1;
            } else if (strcmp(command, "locking") == 0) {
                printf("The locking option needs yes or no\n");
                goto cleanup;
            } else if (strncmp(command, "locking=", 8) == 0) {
                if (parse_locking(command + 8, &locking) < 0) {
                    goto cleanup;
                }
            } else if (strncmp(command, "attr_timeout=", 13) == 0
                       || strncmp(command, "entry_timeout=", 14) == 0
                       || strncmp(command, "negative_timeout=", 17) == 0) {
                size_t used = strlen(req->fuse_options);
                int n = snprintf(req->fuse_options + used,
                                 sizeof(req->fuse_options) - used, "%s%s",
                                 used ? "," : "", command);

                if (n < 0 || (size_t)n >= sizeof(req->fuse_options) - used) {
                    printf("FUSE options too long at %s\n", command);
                    goto cleanup;
                }
            } else {
                printf("Unknown option %s, skipping\n", command);
            }

            if (q) {
                p = q + 1;
            } else {
                p = NULL;
            }

            explicit_bzero(command, sizeof(command));
        } while (p);

        urlstring = argv[3];
        mountpoint = argv[4];
    } else {
        if (argc < 3) {
            printf("URL and mount point required\n");
            return -1;
        }

        urlstring = argv[1];
        mountpoint = argv[2];
    }

    afp_default_url(&req->url);
    req->changeuid = changeuid;
    req->volume_options |= DEFAULT_MOUNT_FLAGS;

    if (readonly) {
        req->volume_options |= VOLUME_EXTRA_FLAGS_READONLY;
    }

    if (flush_on_close) {
        req->volume_options |= VOLUME_EXTRA_FLAGS_FLUSH_ON_CLOSE;
    }

    if (locking) {
        req->volume_options |= VOLUME_EXTRA_FLAGS_DENY_WRITE;
    }

    req->uam_mask = uam_mask;
    outgoing_buffer[0] = AFPFSD_IPC_COMMAND_MOUNT;
    req->map = AFP_MAPPING_UNKNOWN;

    if (mountpoint == NULL) {
        printf("No mount point specified\n");
        goto cleanup;
    }

    if (resolve_mountpoint(mountpoint, req->mountpoint, 255) < 0) {
        printf("Failed to resolve mount point\n");
        goto cleanup;
    }

    if (strncmp(urlstring, "afp://", 6) != 0) {
        if (asprintf(&temp_url, "afp://%s", urlstring) < 0) {
            printf("Out of memory constructing URL\n");
            goto cleanup;
        }

        urlstring = temp_url;
    }

    if (afp_parse_url(&req->url, urlstring) != 0) {
        printf("Could not parse URL\n");
        goto cleanup;
    }

    if (strcmp(req->url.password, "-") == 0 ||
            (req->url.username[0] != '\0'
             && req->url.password[0] == '\0')) {
        char *p = get_password("Password:");

        if (p == NULL) {
            goto cleanup;
        }

        if (copy_secret(req->url.password, sizeof(req->url.password),
                        p, sizeof(req->url.password) - 1,
                        "Password") < 0) {
            explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
            goto cleanup;
        }

        explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
    }

    if (have_volpass && (strcmp(volpass, "-") == 0)) {
        char *p = get_password("Volume password:");

        if (p == NULL) {
            goto cleanup;
        }

        if (copy_secret(volpass, sizeof(volpass), p,
                        AFP_VOLPASS_LEN, "Volume password") < 0) {
            explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
            goto cleanup;
        }

        explicit_bzero(p, AFP_MAX_PASSWORD_LEN + 1);
    }

    if (have_volpass
            && copy_secret(req->url.volpassword, sizeof(req->url.volpassword),
                           volpass, AFP_VOLPASS_LEN, "Volume password") < 0) {
        goto cleanup;
    }

    ret = 0;
cleanup:

    if (ret != 0) {
        clear_outgoing_mount_url_path();
    }

    if (temp_url) {
        free(temp_url);
    }

    if (have_volpass) {
        explicit_bzero(volpass, sizeof(volpass));
    }

    return ret;
}

static int prepare_buffer(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return -1;
    }

    if (strcmp(argv[1], "mount") == 0) {
        return do_mount(argc - 1, argv + 1);
    } else if (strcmp(argv[1], "resume") == 0) {
        return do_resume(argc, argv);
    } else if (strcmp(argv[1], "suspend") == 0) {
        return do_suspend(argc, argv);
    } else if (strcmp(argv[1], "status") == 0) {
        return do_status(argc, argv);
    } else if (strcmp(argv[1], "unmount") == 0) {
        return do_unmount(argc, argv);
    } else if (strcmp(argv[1], "exit") == 0) {
        return do_exit(argc, argv);
    } else {
        usage();
        return -1;
    }
}


static int wait_readable(int sock)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        perror("Getting monotonic time");
        return -1;
    }

    deadline.tv_sec += 30;

    for (;;) {
        fd_set fds;
        struct timespec now;
        struct timeval timeout;
        int ret;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("Getting monotonic time");
            return -1;
        }

        timeout.tv_sec = deadline.tv_sec - now.tv_sec;
        timeout.tv_usec = (deadline.tv_nsec - now.tv_nsec) / 1000;

        if (timeout.tv_usec < 0) {
            timeout.tv_sec--;
            timeout.tv_usec += 1000000;
        }

        if (timeout.tv_sec < 0
                || (timeout.tv_sec == 0 && timeout.tv_usec == 0)) {
            fputs("No response from server\n", stderr);
            return -1;
        }

        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        ret = select(sock + 1, &fds, NULL, NULL, &timeout);

        if (ret < 0 && errno == EINTR) {
            continue;
        }

        if (ret == 0) {
            fputs("No response from server\n", stderr);
            return -1;
        }

        if (ret < 0) {
            perror("Waiting for afpfsd response");
            return -1;
        }

        return 0;
    }
}

static int read_response_bytes(int sock, void *buffer, size_t length)
{
    char *cursor = buffer;

    while (length != 0) {
        ssize_t received;

        if (wait_readable(sock) != 0) {
            return -1;
        }

        do {
            received = read(sock, cursor, length);
        } while (received < 0 && errno == EINTR);

        if (received <= 0) {
            return -1;
        }

        cursor += received;
        length -= (size_t)received;
    }

    return 0;
}

static int read_response(int sock)
{
    struct afpfsd_ipc_response answer;
    char response[AFPFSD_IPC_MAX_RESPONSE + 1];

    if (read_response_bytes(sock, &answer, sizeof(answer)) != 0) {
        fputs("Failed to read response header\n", stderr);
        return -1;
    }

    if (answer.len > AFPFSD_IPC_MAX_RESPONSE) {
        fputs("Invalid oversized response from afpfsd\n", stderr);
        return -1;
    }

    if (answer.len != 0 && read_response_bytes(sock, response, answer.len) != 0) {
        fputs("Failed to read response body\n", stderr);
        return -1;
    }

    response[answer.len] = '\0';
    fputs(response, stdout);
    return answer.result;
}

/*!
 * @brief Run an afpc fs command, or the mount of mount_afpfs
 *
 * A mount with --foreground runs its daemon as a child of afpc and returns
 * once the daemon has ended.
 *
 * @param[in] argc      argument count
 * @param[in] argv      the arguments
 * @param[in] url_mode  1 for mount_afpfs's URL form
 * @returns the command's result (0 for success), 1 when a foreground mount's
 *          daemon did not exit with status 0, 2 for unusable arguments, or
 *          -1 when no daemon answers
 */
static int afpc_fs_run(int argc, char *argv[], int url_mode)
{
    int sock;
    int ret;
    const char *mountpoint = NULL;
    const char *volumename = NULL;
    thisbin = argv[0];
    uid = ((unsigned int) geteuid());
    /* Register logging handler to filter debug messages */
    libafpclient_register(&client);

    if (url_mode) {
        if (handle_mount_afpfs(argc, argv) < 0) {
            return 2;
        }

        /* Extract mountpoint and volumename from mount request */
        if (outgoing_buffer[0] == AFPFSD_IPC_COMMAND_MOUNT && outgoing_len > 1) {
            const struct afpfsd_ipc_mount_request *req =
                (const struct afpfsd_ipc_mount_request *)(outgoing_buffer + 1);
            mountpoint = req->mountpoint;
            volumename = req->url.volumename;
        }
    } else if (prepare_buffer(argc, argv) < 0) {
        return 2;
    }

    if (list_volumes_only) {
        const struct afpfsd_ipc_mount_request *req =
            (const struct afpfsd_ipc_mount_request *)(outgoing_buffer + 1);
        ret = list_authenticated_volumes(req);
        clear_outgoing_mount_url_path();
        return ret;
    }

    /* A suspend, resume or unmount request holds the mount point alone. */
    if (mountpoint == NULL) {
        if (outgoing_buffer[0] == AFPFSD_IPC_COMMAND_MOUNT && outgoing_len > 1) {
            const struct afpfsd_ipc_mount_request *req =
                (const struct afpfsd_ipc_mount_request *)(outgoing_buffer + 1);
            mountpoint = req->mountpoint;
            volumename = req->url.volumename;
        } else if ((outgoing_buffer[0] == AFPFSD_IPC_COMMAND_UNMOUNT
                    || outgoing_buffer[0] == AFPFSD_IPC_COMMAND_SUSPEND
                    || outgoing_buffer[0] == AFPFSD_IPC_COMMAND_RESUME)
                   && outgoing_len > 1) {
            mountpoint = outgoing_buffer + 1;
        } else if (outgoing_buffer[0] == AFPFSD_IPC_COMMAND_STATUS
                   && outgoing_len > 1) {
            const struct afpfsd_ipc_status_request *req =
                (const struct afpfsd_ipc_status_request *)(outgoing_buffer + 1);

            if (req->mountpoint[0] != '\0') {
                mountpoint = req->mountpoint;
            }
        }
    }

    /* Only afpc's own daemon holds the descriptor a /dev/fd/N names. */
    if (!foreground_mount && outgoing_buffer[0] == AFPFSD_IPC_COMMAND_MOUNT
            && mountpoint != NULL && strncmp(mountpoint, "/dev/fd/", 8) == 0) {
        fprintf(stderr,
                "A mount point /dev/fd/N needs afpc fs mount --foreground\n");
        clear_outgoing_mount_url_path();
        return 2;
    }

    sock = foreground_mount ? start_foreground_daemon(mountpoint)
           : daemon_connect(mountpoint, volumename,
                            (unsigned char) outgoing_buffer[0]);

    if (sock < 0) {
        clear_outgoing_mount_url_path();
        return -1;
    }

    clear_outgoing_mount_url_path();

    if (send_command(sock, outgoing_buffer, outgoing_len) != 0) {
        perror("Sending command to afpfsd");
        close(sock);
        return foreground_mount ? wait_foreground_daemon(-1) : -1;
    }

    ret = read_response(sock);
    close(sock);
    return foreground_mount ? wait_foreground_daemon(ret) : ret;
}

int afpc_fs_command(char *program_path, int argc, char **argv)
{
    char *client_argv[argc + 2];
    client_argv[0] = program_path;

    for (int i = 0; i < argc; i++) {
        client_argv[i + 1] = argv[i];
    }

    client_argv[argc + 1] = NULL;
    return afpc_fs_run(argc + 1, client_argv, 0);
}

int afpc_mount_url_command(int argc, char **argv)
{
    return afpc_fs_run(argc, argv, 1);
}
