/*
 *  commands.c
 *  Portions of this file are derived from the SSH Filesystem (SSHFS) project
 *
 *  Copyright (C) 2004 Miklos Szeredi <miklos@szeredi.hu>
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief A mount daemon's commands, and the threads of its FUSE mounts
 *
 * Part of afpfsd, the FUSE front end: a mount daemon's commands.
 * Lane: control.
 * Northbound: the command line of client.c, over a UNIX socket.
 * Southbound: libafpclient's session layer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <fuse.h>

#include "lib/afp_internal.h"
#include "lib/client.h"
#include "lib/codepage.h"
#include "lib/compat.h"
#include "lib/dsi.h"
#include "lib/mapping.h"
#include "lib/uam_registry.h"
#include "lib/utils.h"

#include "daemon.h"
#include "fuse_error.h"
#include "fuse_fs.h"
#include "fuse_ipc.h"

#if defined(__APPLE__)
#define FUSE_DEVICE "/dev/macfuse0"
#else
#define FUSE_DEVICE "/dev/fuse"
#endif

#define AFPFSD_CLIENT_INCOMING_BUF (2048 + 256)

_Static_assert(AFPFSD_CLIENT_INCOMING_BUF
               >= 1 + sizeof(struct afpfsd_ipc_mount_request),
               "the buffer holds the longest command");

struct afpfsd_client {
    char incoming_string[AFPFSD_CLIENT_INCOMING_BUF];
    char client_string[AFPFSD_IPC_MAX_RESPONSE];
    int fd;
};

static int fuse_log_method = LOG_METHOD_SYSLOG;
static int fuse_log_min_rank = 2; /* Default rank: notice */

/* One mount at a time, so a second mount of the daemon's mount point finds
 * the first one's volume. */
static pthread_mutex_t mount_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Mounts waiting for mount_mutex; under it, whether the daemon is ending */
static int mounts_waiting = 0;
static int daemon_ending = 0;

/*!
 * @brief Decide, under mount_mutex, whether a daemon with no server left ends
 *
 * A mount waiting for the lock keeps the daemon; one that takes it once the
 * daemon is ending is refused.
 *
 * @returns 1 when the daemon ends, 0 when it stays or is already ending
 */
static int ends_without_servers(void)
{
    if (daemon_ending || !afp_get_auto_shutdown_on_unmount()
            || get_server_base() != NULL
            || __atomic_load_n(&mounts_waiting, __ATOMIC_ACQUIRE) > 0) {
        return 0;
    }

    daemon_ending = 1;
    fuse_stop_listener();
    return 1;
}

static int volopen(struct afpfsd_client * c, struct afp_volume * volume);
static void *process_command_thread(void *other);
static struct afp_volume *mount_volume(struct afpfsd_client * c,
                                       struct afp_server * server, char *volname, char *volpassword);

static unsigned int fuse_client_response_len(struct afpfsd_client *c)
{
    size_t len = strnlen(c->client_string, sizeof(c->client_string));

    if (len == sizeof(c->client_string)) {
        c->client_string[sizeof(c->client_string) - 1] = '\0';
        len = sizeof(c->client_string) - 1;
    }

    return (unsigned int)len;
}

static void fuse_client_append(struct afpfsd_client *c, const char *text)
{
    size_t len = fuse_client_response_len(c);
    size_t remaining;
    size_t text_len;

    if (len >= sizeof(c->client_string) - 1) {
        return;
    }

    remaining = sizeof(c->client_string) - len - 1;
    text_len = strnlen(text, remaining);
    memcpy(c->client_string + len, text, text_len);
    c->client_string[len + text_len] = '\0';
}

static void fuse_client_append_line(struct afpfsd_client *c, const char *text)
{
    size_t len;
    fuse_client_append(c, text);
    len = fuse_client_response_len(c);

    if (len >= sizeof(c->client_string) - 1) {
        return;
    }

    c->client_string[len] = '\n';
    c->client_string[len + 1] = '\0';
}

void fuse_set_log_method(int new_method)
{
    fuse_log_method = new_method;
}

void fuse_set_log_level(int loglevel)
{
    fuse_log_min_rank = loglevel_to_rank(loglevel);
}


/*!
 * @brief Accept a ready connection and start a thread to answer its command
 *
 * A client that sends its command slowly or in parts holds its own thread,
 * not this loop; one silent for COMMAND_WAIT_S seconds lets it go.
 *
 * @param[in]     command_fd  the listener
 * @param[in]     set         the descriptors pselect() found ready
 * @param[in,out] max_fd      unused: no client joins the loop's set
 * @returns 1 when a connection was taken, 0 otherwise
 */
static int fuse_scan_extra_fds(int command_fd, fd_set *set, int *max_fd _U_)
{
    struct timeval command_wait = { COMMAND_WAIT_S, 0 };
    struct afpfsd_client *c;
    pthread_t thread;
    int fd;

    /* Any other ready descriptor is a server's, which the loop reads, has
     * dropped, or clears at the next pselect()'s EBADF (rm_invalid_fds()). */
    if (!FD_ISSET(command_fd, set)) {
        return 0;
    }

    /* A listener whose accept() failed at the descriptor limit stays ready
     * until a descriptor frees. */
    if ((fd = accept(command_fd, NULL, NULL)) < 0) {
        return 0;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &command_wait,
                   sizeof(command_wait)) != 0
            || (c = calloc(1, sizeof(*c))) == NULL) {
        close(fd);
        return 0;
    }

    c->fd = fd;

    if (pthread_create(&thread, NULL, process_command_thread, c) != 0) {
        close(fd);
        free(c);
        return 0;
    }

    pthread_detach(thread);
    return 1;
}

/*!
 * @brief Test whether the daemon logs a message at a level
 *
 * @param[in] priv      unused
 * @param[in] loglevel  the message's level
 * @returns 1 for a level at or above the daemon's, 0 otherwise
 */
static int fuse_log_enabled(void *priv _U_, int loglevel)
{
    return loglevel_to_rank(loglevel) >= fuse_log_min_rank;
}

/*!
 * @brief Add a message to its client's reply, or write it to the daemon's log
 *
 * @param[in] priv      the client the message is for, or NULL
 * @param[in] logtype   the message's type, unused
 * @param[in] loglevel  the message's level
 * @param[in] message   the message
 */
static void fuse_log_for_client(void *priv,
                                enum logtypes logtype _U_,
                                int loglevel, const char *message)
{
    struct afpfsd_client * c = priv;

    if (c) {
        fuse_client_append_line(c, message);
    } else {
        if (fuse_log_method & LOG_METHOD_SYSLOG) {
            syslog(loglevel, "%s", message);
        }

        if (fuse_log_method & LOG_METHOD_STDOUT) {
            printf("%s\n", message);
        }
    }
}

struct start_fuse_thread_arg {
    struct afp_volume *volume;
    int fuse_result;
    int fuse_errno;
    int changeuid;
    char fuse_options[AFPFSD_IPC_FUSE_OPTIONS_LEN];
    /* The mount command and the mount's thread, which writes its result as
     * the mount ends, long after the command has returned */
    int holders;
    /* The command stopped waiting, leaving a failed start to the thread */
    int abandoned;
};

/*!
 * @brief Let go of a mount thread's argument, which the last holder frees
 *
 * @param[in,out] arg  the argument
 */
static void start_fuse_thread_release(struct start_fuse_thread_arg *arg)
{
    if (__atomic_sub_fetch(&arg->holders, 1, __ATOMIC_ACQ_REL) == 0) {
        free(arg);
    }
}

/*!
 * @brief End a mount thread's start with its result and let go of its argument
 *
 * @param[in,out] arg     the thread's argument
 * @param[in]     result  the thread's result
 * @param[in]     error   the errno that goes with it
 * @returns 1 when the command has stopped waiting for the start
 */
static int end_fuse_thread(struct start_fuse_thread_arg *arg, int result,
                           int error)
{
    struct afp_volume *volume = arg->volume;
    int abandoned;
    pthread_mutex_lock(&volume->startup_mutex);
    arg->fuse_result = result;
    arg->fuse_errno = error;
    volume->starting = 0;
    abandoned = arg->abandoned;
    pthread_cond_signal(&volume->startup_condition_cond);
    pthread_mutex_unlock(&volume->startup_mutex);
    start_fuse_thread_release(arg);
    return abandoned;
}

/*
 * Remove commas from fsname, as it confuses the fuse option parser.
 */
static void fsname_remove_commas(char *fsname)
{
    if (strchr(fsname, ',') != NULL) {
        char *s = fsname;
        char *d = s;

        for (; *s; s++) {
            if (*s != ',') {
                *d++ = *s;
            }
        }

        *d = *s;
    }
}

static char *fsname_escape_commas(char *fsnameold)
{
    char *fsname = malloc(strlen(fsnameold) * 2 + 1);
    char *d = fsname;
    char *s;

    for (s = fsnameold; *s; s++) {
        if (*s == '\\' || *s == ',') {
            *d++ = '\\';
        }

        *d++ = *s;
    }

    *d = '\0';
    free(fsnameold);
    return fsname;
}

/*!
 * @brief Run a volume's FUSE mount until it ends
 *
 * afp_init() ends the start once the mount runs. The thread holds the server,
 * which an external unmount removes, while it still uses the volume.
 *
 * @param[in,out] other  the thread's argument, which the thread lets go of
 * @returns NULL
 */
static void *start_fuse_thread(void *other)
{
    int fuseargc = 0;
    char *fuseargv[200];
#define mountstring_len (AFP_SERVER_NAME_UTF8_LEN+1+AFP_VOLUME_NAME_UTF8_LEN+1)
    char mountstring[mountstring_len];
#define fsoption_buf_len 1024
    char fsoption_buf[fsoption_buf_len];
#ifdef __APPLE__
#define volname_option_buf_len 256
    char volname_option_buf[volname_option_buf_len];
#endif
    struct start_fuse_thread_arg * arg = other;
    struct afp_volume * volume = arg->volume;
    struct afp_server * server = volume->server;
    char *fsname;
    int fuse_result;
    int abandoned;
    int libver = fuse_version();
    /* Initialize the entire array to NULL to prevent FUSE 3 from reading garbage */
    memset(fuseargv, 0, sizeof(fuseargv));
    /* Check to see if we have permissions to access the mountpoint */
    snprintf(mountstring, mountstring_len, "%s:%s",
             server->server_name_printable,
             volume->volume_name_printable);
    fuseargc = 0;
    /* argv[0] must be the program name for FUSE */
    fuseargv[0] = "afpfs";
    fuseargc++;

    if (get_debug_mode()) {
        fuseargv[fuseargc] = "-d";
        fuseargc++;
    } else {
        fuseargv[fuseargc] = "-f";
        fuseargc++;
    }

    if (arg->changeuid) {
        fuseargv[fuseargc] = "-o";
        fuseargc++;
        fuseargv[fuseargc] = "allow_other";
        fuseargc++;
    }

    if (asprintf(&fsname, "%s@%s:%s", server->username, server->server_name,
                 volume->volume_name) < 0) {
        end_fuse_thread(arg, 1, ENOMEM);
        afp_server_release(server);
        return NULL;
    }

    if (libver >= 27) {
        if (libver >= 28) {
            fsname = fsname_escape_commas(fsname);
        } else {
            fsname_remove_commas(fsname);
        }

        snprintf(fsoption_buf, fsoption_buf_len, "-osubtype=afpfs,fsname=%s", fsname);
    } else {
        fsname_remove_commas(fsname);
        snprintf(fsoption_buf, fsoption_buf_len, "-ofsname=afpfs#%s", fsname);
    }

    fuseargv[fuseargc] = fsoption_buf;
    fuseargc++;
#ifdef __APPLE__
    /* Add volname option for macOS to display custom name in Finder */
    snprintf(volname_option_buf, volname_option_buf_len, "-ovolname=%s",
             volume->volume_name_printable);
    fuseargv[fuseargc] = volname_option_buf;
    fuseargc++;
#endif

    if (arg->fuse_options[0]) {
        fuseargv[fuseargc] = "-o";
        fuseargc++;
        fuseargv[fuseargc] = arg->fuse_options;
        fuseargc++;
    }

#ifdef USE_SINGLE_THREAD
    fuseargv[fuseargc] = "-s";
    fuseargc++;
#else
    /* On Linux with FUSE 3, cap idle worker threads to avoid libfuse warning about invalid values */
#if !defined(__APPLE__) && FUSE_USE_VERSION >= 30
    fuseargv[fuseargc] = "-o";
    fuseargc++;
    fuseargv[fuseargc] = "max_idle_threads=10";
    fuseargc++;
#endif
#endif
    /* Append mountpoint last (after all options) */
    fuseargv[fuseargc] = volume->mountpoint;
    fuseargc++;
    /* NULL-terminate the argument array for FUSE 3 compatibility */
    fuseargv[fuseargc] = NULL;
    fuse_result = afp_fuse_main(fuseargc, (char **) fuseargv, volume);
    abandoned = end_fuse_thread(arg, fuse_result, errno);
    /* Use NULL to send to stdout/syslog since this thread outlives the client connection */
    log_for_client(NULL, AFPFSD, LOG_NOTICE,
                   "Unmounting volume %s from %s",
                   volume->volume_name_printable,
                   volume->mountpoint);

    if (fsname) {
        free(fsname);
        fsname = NULL;
    }

    /* An abandoned start that failed leaves the volume to this thread. */
    if (fuse_result != 0 && abandoned
            && volume->mounted != AFP_VOLUME_UNMOUNTED) {
        volume->mounted = AFP_VOLUME_UNMOUNTED;
        afp_detach_volume(volume);
    }

    afp_server_release(server);
    /* An external unmount, an abandoned failed start or a signal ends a mount
     * without a command, and may take the last server. */
    int ending;
    pthread_mutex_lock(&mount_mutex);
    ending = ends_without_servers();
    pthread_mutex_unlock(&mount_mutex);

    if (ending) {
        trigger_exit();
        signal_main_thread();
    }

    return NULL;
}

static int volopen(struct afpfsd_client * c, struct afp_volume * volume)
{
    char mesg[MAX_ERROR_LEN];
    unsigned int l = 0;
    memset(mesg, 0, MAX_ERROR_LEN);
    int rc = afp_connect_volume(volume, volume->server, mesg, &l, MAX_ERROR_LEN);

    if (mesg[0] != '\0') {
        (log_for_client)((void *) c, AFPFSD, LOG_ERR, mesg);
    }

    return rc;
}


/*!
 * @brief Suspend the session of a mount's server, for afpc fs suspend
 *
 * The server sleeps with FPZzzzz and is marked suspended, so no request
 * reconnects it, before its connection ends.
 *
 * @param[in] c  the client, whose command names the mount point
 * @returns AFPFSD_IPC_RESULT_OK, or AFPFSD_IPC_RESULT_ERROR for an unknown
 *          mount point or a refused sleep
 */
static unsigned char process_suspend(struct afpfsd_client * c)
{
    struct afpfsd_ipc_suspend_request req;
    struct afp_server * s;
    struct afp_volume * v = NULL;
    memcpy(&req, (void *)((uintptr_t)c->incoming_string + 1), sizeof(req));

    if ((v = afp_volume_find_by_mountpoint_hold(req.mountpoint)) == NULL) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "No volume mounted at %s", req.mountpoint);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    s = v->server;

    if (afp_zzzzz(s)) {
        afp_server_release(s);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    afp_server_set_suspended(s, 1);
    loop_disconnect(s);
    afp_server_release(s);
    log_for_client((void *) c, AFPFSD, LOG_NOTICE,
                   "Suspended connection for %s", req.mountpoint);
    return AFPFSD_IPC_RESULT_OK;
}


static int afp_server_reconnect_loud(struct afpfsd_client * c,
                                     struct afp_server * s)
{
    char mesg[MAX_ERROR_LEN];
    unsigned int l = 0;
    int rc;
    memset(mesg, 0, sizeof(mesg));
    rc = afp_server_reconnect(s, mesg, &l, sizeof(mesg));

    if (rc)
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "%s", mesg);

    return rc;
}


/*!
 * @brief Resume the session of a mount's server, for afpc fs resume
 *
 * A failed reconnect leaves the server suspended.
 *
 * @param[in] c  the client, whose command names the mount point
 * @returns AFPFSD_IPC_RESULT_OK, or AFPFSD_IPC_RESULT_ERROR for an unknown
 *          mount point or a failed reconnect
 */
static unsigned char process_resume(struct afpfsd_client * c)
{
    struct afpfsd_ipc_resume_request req;
    struct afp_server * s;
    struct afp_volume * v = NULL;
    memcpy(&req, (void *)((uintptr_t)c->incoming_string + 1), sizeof(req));

    if ((v = afp_volume_find_by_mountpoint_hold(req.mountpoint)) == NULL) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "No volume mounted at %s", req.mountpoint);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    s = v->server;
    afp_server_set_suspended(s, 0);

    if (afp_server_reconnect_loud(c, s)) {
        afp_server_set_suspended(s, 1);
        afp_server_release(s);
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Unable to reconnect for %s", req.mountpoint);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    afp_server_release(s);
    log_for_client((void *) c, AFPFSD, LOG_NOTICE,
                   "Resumed connection for %s", req.mountpoint);
    return AFPFSD_IPC_RESULT_OK;
}

/*!
 * @brief Unmount the volume at a mount point, for afpc fs unmount
 *
 * @param[in] c  the client, whose command names the mount point
 * @returns AFPFSD_IPC_RESULT_OK, or AFPFSD_IPC_RESULT_ERROR for an unknown
 *          mount point, a volume not mounted, or a failed unmount
 */
static unsigned char process_unmount(struct afpfsd_client * c)
{
    struct afpfsd_ipc_unmount_request req;
    struct afp_server * s;
    struct afp_volume * v;
    char mountpoint[AFP_MOUNTPOINT_LEN];
    int starting;
    int ret;
    memcpy(&req, (void *)((uintptr_t)c->incoming_string + 1), sizeof(req));

    if ((v = afp_volume_find_by_mountpoint_hold(req.mountpoint)) == NULL) {
        goto notfound;
    }

    s = v->server;

    if (__atomic_load_n(&v->mounted, __ATOMIC_ACQUIRE) != AFP_VOLUME_MOUNTED) {
        log_for_client((void *) c, AFPFSD, LOG_NOTICE,
                       "%s was not mounted", v->mountpoint);
        afp_server_release(s);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    /* A start still pending has no FUSE handle to unmount yet; the mount
     * command sets the start up under mount_mutex. */
    pthread_mutex_lock(&mount_mutex);
    pthread_mutex_lock(&v->startup_mutex);
    starting = v->starting;
    pthread_mutex_unlock(&v->startup_mutex);
    pthread_mutex_unlock(&mount_mutex);

    if (starting) {
        log_for_client((void *) c, AFPFSD, LOG_NOTICE,
                       "%s is still starting", v->mountpoint);
        afp_server_release(s);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    /* A successful unmount removes the server owning v, which the release
     * then frees. */
    strlcpy(mountpoint, v->mountpoint, sizeof(mountpoint));
    ret = afp_unmount_volume(v);
    afp_server_release(s);

    if (ret != 0) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Unmount failed for %s; try using 'umount' or 'fusermount -u'", mountpoint);
        return AFPFSD_IPC_RESULT_ERROR;
    }

    log_for_client((void *) c, AFPFSD, LOG_NOTICE,
                   "Volume %s unmounted", mountpoint);
    return AFPFSD_IPC_RESULT_OK;
notfound:
    log_for_client((void *)c, AFPFSD, LOG_WARNING,
                   "Netatalk Client doesn't have anything mounted on %s", req.mountpoint);
    return AFPFSD_IPC_RESULT_ERROR;
}

static unsigned char process_ping(struct afpfsd_client * c)
{
    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   "Ping!");
    return AFPFSD_IPC_RESULT_OK;
}

static unsigned char process_exit(struct afpfsd_client * c)
{
    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   "Exiting");
    return AFPFSD_IPC_RESULT_OK;
}

/*!
 * @brief Answer a status command with the status of every server
 *
 * @param[in,out] c  the client, whose command is whole
 * @returns AFPFSD_IPC_RESULT_OK, or AFPFSD_IPC_RESULT_ERROR when the header
 *          cannot be written
 */
static unsigned char process_status(struct afpfsd_client * c)
{
    struct afp_server * s;
    struct afpfsd_ipc_status_request req;
    char text[40960];
    int len;
    /* Read the request to check if mountpoint was specified */
    memcpy(&req, (void *)((uintptr_t)c->incoming_string + 1), sizeof(req));

    /* Only show header if no specific mountpoint requested */
    if (req.mountpoint[0] == '\0') {
        len = sizeof(text);

        if (afp_status_header(text, &len) < 0) {
            return AFPFSD_IPC_RESULT_ERROR;
        }

        fuse_client_append(c, text);
    }

    /* The list and each server's volumes change under the list lock. */
    afp_lock_server_list();

    if (get_server_base() == NULL) {
        len = sizeof(text);
        afp_status_server(NULL, text, &len);
        fuse_client_append(c, text);
    }

    for (s = get_server_base(); s; s = s->next) {
        len = sizeof(text);
        afp_status_server(s, text, &len);
        fuse_client_append(c, text);
    }

    afp_unlock_server_list();
    return AFPFSD_IPC_RESULT_OK;
}

/*!
 * @brief Check that a mount point and the FUSE device can take a mount
 *
 * libfuse serves /dev/fd/N, a mounted /dev/fuse descriptor, as is: the device
 * need not exist. A dead daemon's mount there, foreground or not, is released.
 *
 * @param[in,out] c           the client, told why a check fails
 * @param[in]     mountpoint  the mount point
 * @returns 0, or -1 once the client has been told why
 */
static int check_mountpoint(struct afpfsd_client *c, const char *mountpoint)
{
    struct stat st;
    unsigned int fd;
    int len = 0;

    if (sscanf(mountpoint, "/dev/fd/%u%n", &fd, &len) == 1
            && (size_t) len == strlen(mountpoint)) {
        /* A wrong number can name another descriptor, such as a pipe or a
         * terminal, which libfuse would read for a mount that never comes. */
        if (fstat((int) fd, &st) != 0 || !S_ISCHR(st.st_mode)
                || isatty((int) fd)) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Mount point %s is not an open FUSE device",
                           mountpoint);
            return -1;
        }

        return 0;
    }

    release_dead_mount(mountpoint);

    if (stat(mountpoint, &st) != 0) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Mount point %s does not exist: %s",
                       mountpoint, strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Mount point %s is not a directory", mountpoint);
        return -1;
    }

    if (access(mountpoint, X_OK) != 0) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Insufficient permissions on mount point %s: %s",
                       mountpoint, strerror(errno));
        return -1;
    }

    if (stat(FUSE_DEVICE, &st)) {
        log_for_client((void *)c, AFPFSD, LOG_ERR, "Could not find %s",
                       FUSE_DEVICE);
        return -1;
    }

    if (access(FUSE_DEVICE, R_OK | W_OK) != 0) {
        log_for_client((void *)c, AFPFSD, LOG_NOTICE,
                       "Incorrect permissions on %s, mode of device"
                       " is %o, uid/gid is %d/%d.  But your effective "
                       "uid/gid is %d/%d",
                       FUSE_DEVICE, st.st_mode, st.st_uid, st.st_gid,
                       geteuid(), getegid());
        return -1;
    }

    return 0;
}

/*!
 * @brief Connect to a mount command's server and mount its volume
 *
 * @param[in,out] c  the client, whose command is whole
 * @returns AFPFSD_IPC_RESULT_OK, also for a mount still starting when its
 *          wait ends, or AFPFSD_IPC_RESULT_ERROR
 */
static int process_mount(struct afpfsd_client * c)
{
    struct afpfsd_ipc_mount_request req;
    struct afp_server * s = NULL;
    struct afp_server *held = NULL;
    struct afp_volume * volume;
    struct afp_connection_request conn_req;
    int ret;
    memcpy(&req, (void *)((uintptr_t)c->incoming_string + 1), sizeof(req));

    if ((volume = afp_volume_find_by_mountpoint_hold(req.mountpoint))
            != NULL) {
        int mounted = volume->mounted != AFP_VOLUME_UNMOUNTED;
        afp_server_release(volume->server);

        if (mounted) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "A volume is already mounted on %s",
                           req.mountpoint);
            goto error;
        }
    }

    if (check_mountpoint(c, req.mountpoint) != 0) {
        goto error;
    }

    log_for_client(NULL, AFPFSD, LOG_INFO,
                   "Mounting %s from %s on %s",
                   (char *) req.url.volumename,
                   (char *) req.url.servername,
                   req.mountpoint);
    memset(&conn_req, 0, sizeof(conn_req));
    conn_req.url = req.url;
    conn_req.uam_mask = req.uam_mask;

    if ((s = afp_server_full_connect(c, &conn_req)) == NULL) {
        signal_main_thread();
        goto error;
    }

    /* FUSE suspend/resume runs without a fresh client password prompt. The
     * core connect path scrubs this field after initial login, so the
     * per-mount daemon deliberately restashes it for later reconnects. */
    if (req.url.password[0] != '\0') {
        strlcpy(s->password, req.url.password, sizeof(s->password));
    }

    /* An unmount, or a refused volume's detach, can free the server and its
     * volumes while the command still uses them. */
    afp_server_hold(s);
    held = s;
    volume = mount_volume(c, s, req.url.volumename, req.url.volpassword);

    if (volume == NULL && !server_still_valid(s)) {
        s = NULL;
        goto error;
    }

    if (volume == NULL) {
        goto error;
    }

    volume->extra_flags |= req.volume_options;
    volume->mapping = req.map;
    afp_detect_mapping(volume);
    snprintf(volume->mountpoint, 255, "%s", req.mountpoint);
    /* Set the mount time to current time for the root directory's birth time */
    volume->mount_time = time(NULL);
    /* Create the new thread and block until we get an answer back */
    {
        struct timespec ts;
        struct timeval tv;
        struct start_fuse_thread_arg *arg = calloc(1, sizeof(*arg));
        int fuse_result;
        int fuse_errno;
        int starting;

        if (arg == NULL) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Could not start the mount: %s", strerror(ENOMEM));
            volume->mounted = AFP_VOLUME_UNMOUNTED;
            goto error;
        }

        arg->volume = volume;
        arg->changeuid = req.changeuid;
        snprintf(arg->fuse_options, sizeof(arg->fuse_options), "%s",
                 req.fuse_options);
        arg->holders = 2;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec;
        ts.tv_sec += 5;
        ts.tv_nsec = tv.tv_usec * 1000;
        pthread_mutex_init(&volume->startup_mutex, NULL);
        pthread_cond_init(&volume->startup_condition_cond, NULL);
        volume->starting = 1;
        /* The thread's own hold, which start_fuse_thread() releases */
        afp_server_hold(s);
        ret = pthread_create(&volume->thread, NULL, start_fuse_thread, arg);

        if (ret != 0) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Could not start the mount: %s", strerror(ret));
            volume->mounted = AFP_VOLUME_UNMOUNTED;
            afp_server_release(s);
            free(arg);
            goto error;
        }

        /* The thread ends the start under the lock, so a start that ends
         * before the wait begins is seen, not waited for. */
        ret = 0;
        pthread_mutex_lock(&volume->startup_mutex);

        while (volume->starting && ret == 0) {
            ret = pthread_cond_timedwait(&volume->startup_condition_cond,
                                         &volume->startup_mutex, &ts);
        }

        starting = volume->starting;
        arg->abandoned = starting && ret == ETIMEDOUT;
        fuse_result = arg->fuse_result;
        fuse_errno = arg->fuse_errno;
        pthread_mutex_unlock(&volume->startup_mutex);
        start_fuse_thread_release(arg);

        if (starting && ret != ETIMEDOUT) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Error waiting for mount thread: %s", strerror(ret));
            volume->mounted = AFP_VOLUME_UNMOUNTED;
            goto error;
        }

        report_fuse_errors(c, !starting && fuse_result == 0
                           && volume->mounted != AFP_VOLUME_UNMOUNTED);

        /* A mount still starting may succeed: its result is not known yet. */
        if (starting) {
            log_for_client((void *)c, AFPFSD, LOG_NOTICE,
                           "Still trying to mount...");
            ret = AFPFSD_IPC_RESULT_OK;
            goto done;
        }

        switch (fuse_result) {
        case 0:
            if (volume->mounted == AFP_VOLUME_UNMOUNTED) {
                /* Try and discover why */
                switch (fuse_errno) {
                case ENOENT:
                    log_for_client((void *)c, AFPFSD, LOG_ERR,
                                   "Mount failed with errno %d (%s)",
                                   fuse_errno,
                                   mount_errno_to_string(fuse_errno));
                    goto error;

                default:
                    log_for_client((void *)c, AFPFSD, LOG_ERR,
                                   "Mounting of volume %s from server %s failed with errno %d (%s)",
                                   volume->volume_name_printable,
                                   volume->server->server_name_printable,
                                   fuse_errno,
                                   mount_errno_to_string(fuse_errno));
                    goto error;
                }
            } else {
                log_for_client((void *)c, AFPFSD, LOG_INFO,
                               "Mounting of volume %s from server %s succeeded.",
                               volume->volume_name_printable,
                               volume->server->server_name_printable);
                ret = AFPFSD_IPC_RESULT_OK;
                goto done;
            }

        default:
            volume->mounted = AFP_VOLUME_UNMOUNTED;
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "%s: %s (%d, %d)",
                           fuse_result_to_string(fuse_result),
                           mount_errno_to_string(fuse_errno),
                           fuse_result, fuse_errno);
            goto error;
        }
    }
error:

    if ((s) && (!something_is_mounted(s))) {
        afp_logout(s, DSI_DONT_WAIT);
        afp_server_remove(s);
    }

    signal_main_thread();
    ret = AFPFSD_IPC_RESULT_ERROR;
done:

    if (held) {
        afp_server_release(held);
    }

    return ret;
}


/*!
 * @brief Run a client's whole command and write its answer
 *
 * @param[in,out] c  the client
 */
static void run_command(struct afpfsd_client * c)
{
    int ret = 0;
    int ending = 0;
    char tosend[sizeof(struct afpfsd_ipc_response) + AFPFSD_IPC_MAX_RESPONSE];
    struct afpfsd_ipc_response response;
    memset(c->client_string, 0, sizeof(c->client_string));

    switch (c->incoming_string[0]) {
    case AFPFSD_IPC_COMMAND_MOUNT:
        __atomic_add_fetch(&mounts_waiting, 1, __ATOMIC_ACQ_REL);
        pthread_mutex_lock(&mount_mutex);
        __atomic_sub_fetch(&mounts_waiting, 1, __ATOMIC_ACQ_REL);

        if (daemon_ending) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "The daemon of this mount point is ending");
            ret = AFPFSD_IPC_RESULT_ERROR;
        } else {
            ret = process_mount(c);
        }

        ending = ret != AFPFSD_IPC_RESULT_OK && ends_without_servers();
        pthread_mutex_unlock(&mount_mutex);
        break;

    case AFPFSD_IPC_COMMAND_STATUS:
        ret = process_status(c);
        break;

    case AFPFSD_IPC_COMMAND_UNMOUNT:
        ret = process_unmount(c);
        break;

    case AFPFSD_IPC_COMMAND_SUSPEND:
        ret = process_suspend(c);
        break;

    case AFPFSD_IPC_COMMAND_RESUME:
        ret = process_resume(c);
        break;

    case AFPFSD_IPC_COMMAND_PING:
        ret = process_ping(c);
        break;

    case AFPFSD_IPC_COMMAND_EXIT:
        ret = process_exit(c);
        break;

    default:
        log_for_client((void *)c, AFPFSD, LOG_ERR, "Unknown command");
    }

    /* Send response */
    unsigned char command = c->incoming_string[0];
    int command_result = ret;

    if (command_result == AFPFSD_IPC_RESULT_OK
            && command == AFPFSD_IPC_COMMAND_UNMOUNT) {
        pthread_mutex_lock(&mount_mutex);
        ending = ends_without_servers();
        pthread_mutex_unlock(&mount_mutex);
    }

    int should_exit = ending || (command_result == AFPFSD_IPC_RESULT_OK
                                 && command == AFPFSD_IPC_COMMAND_EXIT);

    /* Retire the listener before acknowledging shutdown so the next mount
     * cannot connect to this daemon while its main loop is still exiting. */
    if (should_exit) {
        fuse_stop_listener();
    }

    response.result = ret;
    response.len = fuse_client_response_len(c);
    bcopy(&response, tosend, sizeof(response));
    bcopy(c->client_string, tosend + sizeof(response), response.len);

    if (afpfsd_ipc_write_all(c->fd, tosend,
                             sizeof(response) + response.len) < 0) {
        perror("Writing");
    }

    if (should_exit) {
        trigger_exit();
        signal_main_thread();
    }
}

/*!
 * @brief Give a command's length: its command byte and its request
 *
 * @param[in] command  the command byte
 * @returns the length, 1 for a command without a request or an unknown one
 */
static size_t command_length(unsigned char command)
{
    switch (command) {
    case AFPFSD_IPC_COMMAND_MOUNT:
        return 1 + sizeof(struct afpfsd_ipc_mount_request);

    case AFPFSD_IPC_COMMAND_STATUS:
        return 1 + sizeof(struct afpfsd_ipc_status_request);

    case AFPFSD_IPC_COMMAND_UNMOUNT:
        return 1 + sizeof(struct afpfsd_ipc_unmount_request);

    case AFPFSD_IPC_COMMAND_SUSPEND:
        return 1 + sizeof(struct afpfsd_ipc_suspend_request);

    case AFPFSD_IPC_COMMAND_RESUME:
        return 1 + sizeof(struct afpfsd_ipc_resume_request);

    default:
        return 1;
    }
}

/*!
 * @brief Read a client's command until it is whole
 *
 * @param[in,out] c  the client
 * @returns 0 once the command is whole, -1 when the connection ends or
 *          fails first, or for a command afpfsd_ipc_command_valid()
 *          refuses
 */
static int read_command(struct afpfsd_client * c)
{
    if (afpfsd_ipc_read_all(c->fd, c->incoming_string, 1) < 0
            || afpfsd_ipc_read_all(c->fd, c->incoming_string + 1,
                                   command_length((unsigned char)
                                                  c->incoming_string[0]) - 1)
            < 0) {
        return -1;
    }

    return afpfsd_ipc_command_valid(c->incoming_string) ? 0 : -1;
}

/*!
 * @brief Read a client's command, run it, and end the connection
 *
 * @param[in,out] other  the struct afpfsd_client, which the thread frees
 * @returns NULL
 */
static void *process_command_thread(void *other)
{
    struct afpfsd_client * c = other;

    if (read_command(c) == 0) {
        run_command(c);
    }

    close(c->fd);
    free(c);
    return NULL;
}


static struct afp_volume *mount_volume(struct afpfsd_client *c,
                                       struct afp_server *server,
                                       char *volname,
                                       char *volpassword)
{
    struct afp_volume * using_volume;
    using_volume = find_volume_by_name(server, volname);

    if (!using_volume) {
        if (volname && volname[0]) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Volume %s does not exist on server %s", volname,
                           server->server_name_printable);
        }

        if (server->num_volumes) {
            char names[VOLNAME_LEN];
            afp_list_volnames(server, names, VOLNAME_LEN);
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Choose a volume from: %s", names);
        } else {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "No volumes available on server %s.",
                           server->server_name_printable);
        }

        goto error;
    }

    if (using_volume->mounted == AFP_VOLUME_MOUNTED) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Volume %s is already mounted on %s", volname,
                       using_volume->mountpoint);
        goto error;
    }

    if (using_volume->flags & HasPassword) {
        bcopy(volpassword, using_volume->volpassword, AFP_VOLPASS_LEN);

        if (strlen(volpassword) < 1) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Volume password needed");
            goto error;
        }
    }  else {
        memset(using_volume->volpassword, 0, AFP_VOLPASS_LEN);
    }

    if (volopen(c, using_volume)) {
        log_for_client((void *) c, AFPFSD, LOG_ERR, "Could not mount volume %s",
                       volname);
        goto error;
    }

    using_volume->server = server;
    return using_volume;
error:
    return NULL;
}


static struct libafpclient client = {
    .unmount_volume = fuse_unmount_volume,
    .log_for_client = fuse_log_for_client,
    .forced_ending_hook = fuse_forced_ending_hook,
    .scan_extra_fds = fuse_scan_extra_fds,
    .log_enabled = fuse_log_enabled
};

int fuse_register_afpclient(void)
{
    libafpclient_register(&client);
    return 0;
}
