/*
 *  loop.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "afp.h"
#include "compat.h"
#include "dsi.h"
#include "utils.h"

#define SIGNAL_TO_USE SIGUSR2
/* Define DEBUG_AFP_LOOP explicitly for verbose pselect/event-loop tracing. */

static unsigned char exit_program = 0;

static pthread_t ending_thread = (pthread_t)NULL;
static pthread_t main_thread = (pthread_t)NULL;

static int loop_started = 0;
static pthread_cond_t loop_started_condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t loop_started_mutex = PTHREAD_MUTEX_INITIALIZER;


void trigger_exit(void)
{
    exit_program = 1;
}

void termination_handler(int signum)
{
    switch (signum) {
    case SIGINT:
    case SIGTERM:
        exit_program = 2;
        break;

    default:
        break;
    }

    signal(SIGNAL_TO_USE, termination_handler);
}

#define max(a,b) (((a)>(b)) ? (a) : (b))

static fd_set rds;
static int max_fd = 0;

static void add_fd(int fd)
{
    FD_SET(fd, &rds);

    if ((fd + 1) > max_fd) {
        max_fd = fd + 1;
    }
}

static void rm_fd(int fd)
{
    int i;
    FD_CLR(fd, &rds);

    for (i = max_fd; i >= 0; i--)
        if (FD_ISSET(i, &rds)) {
            max_fd = i;
            break;
        }

    max_fd++;
}

static int rm_invalid_fds(void)
{
    int removed = 0;
    int old_max_fd = max_fd;

    for (int fd = 0; fd < old_max_fd; fd++) {
        if (!FD_ISSET(fd, &rds)) {
            continue;
        }

        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "afp_main_loop -- removing invalid fd %d from poll set",
                           fd);
            rm_fd(fd);
            removed++;
        }
    }

    return removed;
}

void signal_main_thread(void)
{
    /* Don't signal if we're already in the main thread - we're already awake! */
    if (main_thread && pthread_equal(pthread_self(), main_thread)) {
#ifdef DEBUG_AFP_LOOP
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "signal_main_thread: already in main thread, skipping signal");
#endif
        return;
    }

#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "signal_main_thread: sending signal to main_thread=%p", (void*)main_thread);
#endif

    if (main_thread) {
        int ret = pthread_kill(main_thread, SIGNAL_TO_USE);
#ifdef DEBUG_AFP_LOOP
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "signal_main_thread: pthread_kill returned %d", ret);
#else
        (void) ret;
#endif
    }
}

static int ending = 0;
void *just_end_it_now(void * ignore _U_)
{
    if (ending) {
        return NULL;
    }

    ending = 1;

    if (libafpclient->forced_ending_hook) {
        libafpclient->forced_ending_hook();
    }

    exit_program = 2;
    signal_main_thread();
    return NULL;
}

/*This is a hack to handle a problem where the first pthread_kill doesnt' work*/
static unsigned char firsttime = 0;
void add_fd_and_signal(int fd)
{
#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "add_fd_and_signal: adding fd=%d, max_fd before=%d", fd, max_fd);
#endif
    add_fd(fd);
#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "add_fd_and_signal: FD_ISSET(%d, &rds)=%d, max_fd after=%d",
                   fd, FD_ISSET(fd, &rds), max_fd);
#endif
    signal_main_thread();

    if (!firsttime) {
        firsttime = 1;
#ifdef DEBUG_AFP_LOOP
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "add_fd_and_signal: sending second signal (firsttime)");
#endif
        signal_main_thread();
    }

#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "add_fd_and_signal: done for fd=%d", fd);
#endif
}

void rm_fd_and_signal(int fd)
{
    rm_fd(fd);
    signal_main_thread();
}

void loop_disconnect(struct afp_server *s)
{
    if (s->connect_state != SERVER_STATE_CONNECTED &&
            s->connect_state != SERVER_STATE_CONNECTING) {
        return;
    }

    dsi_fail_request_queue(s, -EIO);
    s->data_read = 0;
    s->attention_len = 0;
    rm_fd_and_signal(s->fd);
    /* Handle disconnect */
    close(s->fd);
    s->fd = -1;
    s->connect_state = SERVER_STATE_DISCONNECTED;
    s->need_resume = 1;
}

static int process_server_fds(fd_set *set, int **onfd)
{
    struct afp_server * s;
    int ret;
    /* Hold the server list lock while searching to prevent use-after-free.
     * A connection thread could call afp_server_remove() (freeing the server)
     * between our finding the server and calling dsi_recv() on it. */
    afp_lock_server_list();
    s  = get_server_base();

    for (; s; s = s->next) {
        if (s->next == s) {
            log_for_client(NULL, AFPFSD, LOG_WARNING, "Danger, recursive loop");
        }

        /* Skip disconnected/suspended servers.
         * CONNECTING servers need DSI processing during handshake. */
        if (s->connect_state != SERVER_STATE_CONNECTED &&
                s->connect_state != SERVER_STATE_CONNECTING) {
            continue;
        }

        if (FD_ISSET(s->fd, set)) {
            /* Take a reference before releasing the lock so the server
             * cannot be freed while we call dsi_recv(). */
            afp_server_hold(s);
            afp_unlock_server_list();
            ret = dsi_recv(s);
            *onfd = &s->fd;

            if (ret == -1) {
                /* Server disconnected or error occurred */
                log_for_client(NULL, AFPFSD, LOG_INFO,
                               "Server fd=%d disconnected, cleaning up (need_resume will be set for reconnection)",
                               s->fd);
                loop_disconnect(s);
                afp_server_release(s);
                /* Return 0 (not -1) to continue the main loop instead of exiting.
                 * The server is now disconnected and can be reconnected later if needed.
                 * Returning -1 here would cause the entire daemon to exit, breaking FUSE. */
                return 0;
            }

            afp_server_release(s);
            return 1;
        }
    }

    afp_unlock_server_list();
    return 0;
}

static void deal_with_server_signals(void)
{
    if (exit_program == 1) {
        pthread_create(&ending_thread, NULL, just_end_it_now, NULL);
    }
}

void afp_wait_for_started_loop(void)
{
    if (loop_started) {
        return;
    }

    pthread_cond_wait(&loop_started_condition, &loop_started_mutex);
}

static void *afp_main_quick_startup_thread(void * other _U_)
{
    afp_main_loop(-1);
    return NULL;
}


int afp_main_quick_startup(pthread_t * thread)
{
    pthread_t loop_thread;
    pthread_create(&loop_thread, NULL, afp_main_quick_startup_thread, NULL);

    if (thread) {
        memcpy(thread, &loop_thread, sizeof(pthread_t));
    }

    return 0;
}


int afp_main_loop(int command_fd)
{
    fd_set ords, oeds;
    struct timespec tv;
    int ret;
    int fderrors = 0;
    sigset_t sigmask, orig_sigmask;
    main_thread = pthread_self();
    FD_ZERO(&rds);

    if (command_fd >= 0) {
        add_fd(command_fd);
    }

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGNAL_TO_USE);
    sigprocmask(SIG_BLOCK, &sigmask, &orig_sigmask);
    signal(SIGNAL_TO_USE, termination_handler);
    signal(SIGTERM, termination_handler);
    signal(SIGINT, termination_handler);
#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "afp_main_loop -- Starting main loop (command_fd=%d, max_fd=%d, loop_started=%d)",
                   command_fd, max_fd, loop_started);
#endif

    while (1) {
#ifdef DEBUG_AFP_LOOP
        {
            int active_fds = 0;

            for (int i = 0; i < max_fd; i++)
            {
                if (FD_ISSET(i, &rds)) {
                    active_fds++;
                }
            }

            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "afp_main_loop -- TOP OF LOOP (max_fd=%d, active_fds=%d)", max_fd, active_fds);
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "afp_main_loop -- Loop iteration (max_fd=%d, active_fds=%d, exit_state=%d, fderrors=%d)",
                           max_fd, active_fds, exit_program, fderrors);

            for (int j = 0; j < 16; j++) if (FD_ISSET(j, &rds))
                {
                    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                                   "afp_main_loop -- FD %d is set", j);
                }
        }
#endif
        ords = rds;
        oeds = rds;

        if (loop_started) {
            tv.tv_sec = 30;
            tv.tv_nsec = 0;
        } else {
            tv.tv_sec = 0;
            tv.tv_nsec = 0;
        }

#ifdef DEBUG_AFP_LOOP
        {
            int connected_servers = 0, total_servers = 0;

            for (struct afp_server *s = get_server_base(); s; s = s->next) {
                total_servers++;

                if (s->connect_state == SERVER_STATE_CONNECTED ||
                        s->connect_state == SERVER_STATE_CONNECTING) {
                    connected_servers++;
                }
            }

            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "afp_main_loop -- pselect (timeout=%lds, servers=%d/%d connected, max_fd=%d)",
                           tv.tv_sec, connected_servers, total_servers, max_fd);
        }
#endif

        /* Check exit conditions BEFORE pselect */
        if (exit_program == 2) {
            break;
        }

        if (exit_program == 1) {
            pthread_create(&ending_thread, NULL, just_end_it_now, NULL);
            continue;
        }

#ifdef DEBUG_AFP_LOOP
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "afp_main_loop -- about to call pselect (max_fd=%d, timeout=%lds)",
                       max_fd, tv.tv_sec);
#endif
        /* pselect atomically unblocks signals (using orig_sigmask) only while waiting.
         * Do NOT unblock signals before pselect - that creates a race window! */
        ret = pselect(max_fd, &ords, NULL, &oeds, &tv, &orig_sigmask);
#ifdef DEBUG_AFP_LOOP
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "afp_main_loop -- pselect returned ret=%d, errno=%d", ret, errno);
#endif

        /* Check exit conditions first after pselect returns */
        if (exit_program == 2) {
            break;
        }

        if (exit_program == 1) {
            pthread_create(&ending_thread, NULL, just_end_it_now, NULL);
            continue;
        }

        /* Handle select errors with proper signal mask state */
        if (ret < 0) {
            if (errno == EINTR) {
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- pselect interrupted by signal (EINTR)");
#endif
                deal_with_server_signals();
                continue;
            }

            perror("afp_main_loop select");

            switch (errno) {
            case EBADF:
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- Bad file descriptor (max_fd=%d, error_count=%d/100)",
                               max_fd, fderrors + 1);
#endif

                if (rm_invalid_fds() > 0) {
                    fderrors = 0;
                    continue;
                }

                if (fderrors > 100) {
                    log_for_client(NULL, AFPFSD, LOG_ERR,
                                   "Too many fd errors (%d), exiting", fderrors + 1);
                    break;
                }

                fderrors++;
                continue;

            default:
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- select error (errno=%d: %s, ret=%d, max_fd=%d)",
                               errno, strerror(errno), ret, max_fd);
#endif

                if (libafpclient->scan_extra_fds) {
#ifdef DEBUG_AFP_LOOP
                    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                                   "afp_main_loop -- Scanning extra fds after select error");
#endif
                    ret = libafpclient->scan_extra_fds(
                              command_fd, &ords, &max_fd);
                }

                continue;
            }

            continue;
        }

        fderrors = 0;

        if (ret == 0) {
            if (!loop_started) {
                pthread_mutex_lock(&loop_started_mutex);
                loop_started = 1;
                pthread_cond_signal(&loop_started_condition);
                pthread_mutex_unlock(&loop_started_mutex);

                if (libafpclient->loop_started) {
                    libafpclient->loop_started();
                }
            } else {
                /* Select timeout - send tickles to keep connections alive */
                afp_lock_server_list();

                for (struct afp_server *s = get_server_base(); s; s = s->next) {
                    if (s->connect_state == SERVER_STATE_CONNECTED && s->fd > 0) {
#ifdef DEBUG_AFP_LOOP
                        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                                       "afp_main_loop -- Sending tickle to server %s (fd=%d, connected_for=%lds)",
                                       s->server_name_printable, s->fd,
                                       time(NULL) - s->connect_time);
#endif
                        dsi_sendtickle(s);
                    }
                }

                afp_unlock_server_list();
            }
        } else {
            int *onfd;
            fderrors = 0;

            /* Skip processing FDs if we're shutting down to avoid race conditions */
            if (exit_program >= 1) {
                continue;
            }

#ifdef DEBUG_AFP_LOOP
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "afp_main_loop -- calling process_server_fds");
#endif
            int server_result = process_server_fds(&ords, &onfd);
#ifdef DEBUG_AFP_LOOP
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "afp_main_loop -- process_server_fds returned %d", server_result);
#endif

            switch (server_result) {
            case -1:
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- Error from process_server_fds() (fd=%d, max_fd=%d)",
                               onfd ? *onfd : -1, max_fd);
#endif
                goto error;

            case 1:
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- Processed data from fd=%d successfully",
                               onfd ? *onfd : -1);
#endif
                continue;

            default:
                /* This handles the case where server_result is 0, meaning no
                 * server FDs were ready. We break to proceed with checking
                 * other file descriptors, like the client command socket. */
                break;
            }

            if (libafpclient->scan_extra_fds) {
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- calling scan_extra_fds");
#endif
                int scan_result = libafpclient->scan_extra_fds(
                                      command_fd, &ords, &max_fd);
#ifdef DEBUG_AFP_LOOP
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "afp_main_loop -- scan_extra_fds returned %d (new_max_fd=%d)",
                               scan_result, max_fd);
#endif

                if (scan_result > 0) {
                    continue;
                }
            }
        }
    }

#ifdef DEBUG_AFP_LOOP
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "afp_main_loop -- Exiting main loop (exit_program=%d, ending_thread=%p)",
                   exit_program, (void*)ending_thread);
#endif
error:

    if (ending_thread != (pthread_t)NULL) {
        pthread_detach(ending_thread);
    }

    return -1;
}
