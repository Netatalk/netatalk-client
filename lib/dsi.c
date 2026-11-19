/*
 *  dsi.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief DSI framing, the request queue and reply dispatch of a session
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "afp_replies.h"
#include "client.h"
#include "codepage.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "uam_registry.h"
#include "utils.h"

/* Define DEBUG_DSI explicitly to get reams of DSI debugging information. */

/*! @brief A frame's bytes: the head, then data from the caller's buffer */
struct dsi_frame {
    char *head;        /*!< the DSI header and the command's parameters */
    size_t head_len;   /*!< bytes of head */
    const char *data;  /*!< the data, or NULL */
    size_t data_len;   /*!< bytes of data */
};

static int afpc_dsi_write_frame(struct afp_server *server,
                                const struct dsi_frame *frame,
                                unsigned char subcommand, int tied,
                                unsigned int session);
int convert_utf8dec_to_utf8pre(const char *src, int src_len,
                               char *dest, int dest_len);
int convert_utf8pre_to_utf8dec(const char *src, int src_len,
                               char *dest, int dest_len);

struct dsi_attention_context {
    struct afp_server *server;
    unsigned int connection_generation;
    char packet[];
};

/*!
 * @brief Take the session's next request ID
 *
 * @param[in,out] server  the server, whose last request ID advances
 * @returns the ID, in network byte order
 */
static uint16_t take_request_id(struct afp_server *server)
{
    uint16_t id;
    pthread_mutex_lock(&server->requestid_mutex);

    if (server->lastrequestid == 65535) {
        /* AFP 3.3+: Don't reset to 0 if replay cache is supported.
         * Request IDs wrap around but remain persistent across reconnects. */
        if (!server->supports_replay_cache) {
            server->lastrequestid = 0;
        } else {
            server->lastrequestid = 1;  /* Wrap to 1, avoiding 0 */
        }
    } else {
        server->lastrequestid++;
    }

    server->expectedrequestid = server->lastrequestid;
    id = htons(server->lastrequestid);
    pthread_mutex_unlock(&server->requestid_mutex);
    return id;
}

/*!
 * @brief Fill a DSI header with the session's next request ID
 *
 * @param[in,out] server   the server, whose last request ID advances
 * @param[out]    header   the header
 * @param[in]     command  the DSI command
 */
void afpc_dsi_setup_header(struct afp_server * server,
                           struct dsi_header * header,
                           char command)
{
    memset(header, 0, sizeof(struct dsi_header));
    header->requestid = take_request_id(server);
    header->command = command;
}

/*!
 * @brief Send a tickle unless a frame or a full socket is in its way
 *
 * The loop thread must not block, as it reads every server's replies; a
 * frame under way keeps the session alive. No tickle goes during a reconnect.
 *
 * @param[in,out] server  server to tickle
 */
static void afpc_dsi_tickle_unless_busy(struct afp_server *server)
{
    struct dsi_header header;
    struct pollfd pfd;

    if (pthread_mutex_trylock(&server->send_mutex) != 0) {
        return;
    }

    pfd.fd = __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE);
    pfd.events = POLLOUT;

    if (pfd.fd >= 0 && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT)) {
        struct dsi_frame frame = {
            .head = (char *) &header,
            .head_len = sizeof(header),
        };
        afpc_dsi_setup_header(server, &header, DSI_DSITickle);
        (void) afpc_dsi_write_frame(server, &frame, DSI_DSITickle, 0, 0);
    }

    pthread_mutex_unlock(&server->send_mutex);
}

/*!
 * @brief Tell an idle server the session is alive
 *
 * @param[in,out] server  server to tickle
 * @returns 0
 */
int afpc_dsi_sendtickle(struct afp_server *server)
{
    afpc_dsi_tickle_unless_busy(server);
    return 0;
}

/*!
 * @brief Open a DSI session, offering the client's attention quantum
 *
 * @param[in,out] server  the server
 * @returns afpc_dsi_send()'s result
 */
int afpc_dsi_opensession(struct afp_server *server)
{
    struct {
        struct dsi_header dsi_header  __attribute__((__packed__));
        uint8_t flags;
        uint8_t length;
        uint32_t rx_quantum;
    } __attribute__((__packed__)) dsi_opensession_header;
    struct dsi_header hdr;
    afpc_dsi_setup_header(server, &hdr, DSI_DSIOpenSession);
    memcpy(&dsi_opensession_header.dsi_header, &hdr, sizeof(struct dsi_header));
    /* Advertize our rx quantum */
    dsi_opensession_header.flags = 1;
    dsi_opensession_header.length = sizeof(unsigned int);
    dsi_opensession_header.rx_quantum = htonl(server->attention_quantum);
    return afpc_dsi_send(server, (char *) &dsi_opensession_header,
                         sizeof(dsi_opensession_header), DSI_LOGIN_TIMEOUT,
                         DSI_DSIOpenSession, NULL);
}

/*!
 * @brief Take a sender's request off the queue, once no receiver holds it
 *
 * The request lives on the sender's stack; a flush may have taken it off
 * already. Every answer and failure is given under the queue's lock, so
 * once the call returns the sender reads the result without one.
 *
 * @param[in,out] server    server whose queue held the request
 * @param[in,out] toremove  the sender's request
 */
static void afpc_dsi_remove_from_request_queue(struct afp_server *server,
                                               struct dsi_request *toremove)
{
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** Removing %d, %s", toremove->requestid,
                   afp_get_command_name(toremove->subcommand));
#endif
    pthread_mutex_lock(&server->request_queue_mutex);

    /* The receiver may still be filling the request's buffer. */
    while (toremove->receiving) {
        pthread_cond_wait(&server->request_queue_cond,
                          &server->request_queue_mutex);
    }

    for (struct dsi_request **link = &server->command_requests; *link;
            link = &(*link)->next) {
        if (*link == toremove) {
            *link = toremove->next;
            server->stats.requests_pending--;
            break;
        }
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
    pthread_cond_destroy(&toremove->waiting_cond);
    pthread_mutex_destroy(&toremove->waiting_mutex);
}

/*!
 * @brief Empty a server's queue before it reconnects
 *
 * A reply the old connection still delivers then matches no request. A request
 * already answered keeps its result; each other one wakes with -EIO, and its
 * sender's call returns with it.
 *
 * @param[in,out] server  server whose queue to empty
 */
void afpc_dsi_flush_request_queue(struct afp_server *server)
{
    struct dsi_request *p, *next;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Flushing pending request queue before reconnection");
    pthread_mutex_lock(&server->request_queue_mutex);
    p = server->command_requests;
    server->command_requests = NULL;

    while (p) {
        next = p->next;
        p->next = NULL;
        server->stats.requests_pending--;
        pthread_mutex_lock(&p->waiting_mutex);

        /* A request already answered keeps its result. */
        if (!p->done_waiting) {
            p->return_code = -EIO;
            p->done_waiting = 1;
            pthread_cond_signal(&p->waiting_cond);
        }

        pthread_mutex_unlock(&p->waiting_mutex);
        p = next;
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
}

/*!
 * @brief Wake every request waiting on a server with an error
 *
 * A request already answered, or of a newer connection, is left as it is;
 * each stays queued for its sender to remove.
 *
 * @param[in,out] server      server whose requests fail
 * @param[in]     error       the result they receive
 * @param[in]     generation  generation of the connection that ended
 */
void afpc_dsi_fail_request_queue(struct afp_server *server, int error,
                                 unsigned int generation)
{
    pthread_mutex_lock(&server->request_queue_mutex);

    for (struct dsi_request *p = server->command_requests; p; p = p->next) {
        if ((int)(p->connection_generation - generation) > 0) {
            continue;
        }

        pthread_mutex_lock(&p->waiting_mutex);

        /* A request already answered keeps its result. */
        if (!p->done_waiting) {
            p->return_code = error;
            p->done_waiting = 1;
            pthread_cond_signal(&p->waiting_cond);
        }

        pthread_mutex_unlock(&p->waiting_mutex);
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
}


/*!
 * @brief Send a request, whose reply its command's own parser reads
 *
 * @param[in]     server      server to send to
 * @param[in,out] msg         request frame, DSI header first
 * @param[in]     size        bytes of msg
 * @param[in]     wait        seconds to wait for the reply, as
 *                            afpc_dsi_send_with_reply() takes them
 * @param[in]     subcommand  AFP command, or the DSI command of DSI's own
 *                            exchanges
 * @param[in,out] other       destination of the reply's contents
 * @returns afpc_dsi_send_with_reply()'s result
 */
int afpc_dsi_send(struct afp_server *server, char *msg, int size, int wait,
                  unsigned char subcommand, void *other)
{
    return afpc_dsi_send_with_reply(server, msg, size, wait, subcommand,
                                    other, NULL, NULL);
}

/*!
 * @brief Compute the wait for a reply whose data moves at bytes_per_second
 *
 * @param[in] server            server with the default reply timeout
 * @param[in] bytes             bytes the command or its reply carries
 * @param[in] bytes_per_second  slowest rate to wait for, above 0
 * @returns the default timeout plus a second per bytes_per_second bytes, the
 *          addition capped at DSI_MAX_TRANSFER_WAIT
 */
int afpc_dsi_transfer_wait(const struct afp_server *server, uint64_t bytes,
                           unsigned int bytes_per_second)
{
    return server->dsi_default_timeout
           + (int) min(bytes / bytes_per_second, DSI_MAX_TRANSFER_WAIT);
}

/*!
 * @brief Set a written request's deadline
 *
 * Since afpd answers a session's requests in order, a request's deadline
 * adds the waits of those written ahead and still awaited to its own. A
 * server that answers out of order only waits longer than it needs. The
 * caller holds send_mutex.
 *
 * @param[in]     server   server whose request queue to read
 * @param[in,out] request  request just written, with a wait above 0
 */
void afpc_dsi_set_deadline(struct afp_server *server,
                           struct dsi_request *request)
{
    struct timeval tv;
    time_t wait;
    gettimeofday(&tv, NULL);
    pthread_mutex_lock(&server->request_queue_mutex);
    wait = request->wait;

    /* Under send_mutex, only a request written ahead of this one has a
     * deadline. An answered one has a wait of 0. */
    for (const struct dsi_request *p = server->command_requests;
            p; p = p->next) {
        if (p->deadline.tv_sec != 0) {
            wait += p->wait;
        }
    }

    request->deadline.tv_sec = tv.tv_sec + wait;
    request->deadline.tv_nsec = tv.tv_usec * 1000;
    pthread_mutex_unlock(&server->request_queue_mutex);
}

static _Thread_local const struct afp_server *tied_server;
static _Thread_local unsigned int tied_session;
static _Thread_local int fork_unsent;

/*!
 * @brief Tie the calling thread's next request to the session of a fork
 *
 * A new session reuses fork numbers, so the request fails with EIO once a
 * reconnect has started one. The tie lasts until the thread's next send.
 *
 * @param[in] server   the fork's server
 * @param[in] session  the session generation that gave the fork
 * @param[in] forkid   the fork's number
 * @returns forkid
 */
unsigned short afpc_dsi_fork(const struct afp_server *server,
                             unsigned int session, unsigned short forkid)
{
    tied_server = server;
    tied_session = session;
    return forkid;
}

/*!
 * @brief Whether the thread's last request tied to a fork went unwritten
 *
 * The server then holds the fork as it was.
 *
 * @returns 1 when the frame was not written, 0 otherwise
 */
int afpc_dsi_fork_unsent(void)
{
    return fork_unsent;
}

/*!
 * @brief Whether the thread may write a frame to the descriptor it loaded
 *
 * The reconnect flag is set before a new descriptor is published and cleared
 * after any new session is counted: a writer on it sees the flag or the count.
 *
 * @param[in] server   the server
 * @param[in] tied     1 for a frame tied to a fork
 * @param[in] session  that fork's session generation
 * @returns 1 when the frame may be written, 0 when it fails with EIO
 */
static int may_write(const struct afp_server *server, int tied,
                     unsigned int session)
{
    if (afp_server_reconnects_here(server)) {
        return 1;
    }

    return !afp_server_reconnect_is_in_progress(server)
           && (!tied || session == afp_server_session_generation(server));
}

/*!
 * @brief Write one DSI frame to the server's socket
 *
 * The caller holds send_mutex: frames never interleave, and the descriptor
 * loaded at the start stays open until the frame ends.
 *
 * @param[in,out] server      server with a connected socket
 * @param[in]     frame       the frame's head, at any address, and data
 * @param[in]     subcommand  the AFP command the frame carries, counted for
 *                            DSICommand and DSIWrite frames
 * @param[in]     tied        1 for a frame tied to a fork
 * @param[in]     session     that fork's session generation
 * @returns 0, or -EIO when may_write() refuses the frame or the write fails
 */
static int afpc_dsi_write_frame(struct afp_server *server,
                                const struct dsi_frame *frame,
                                unsigned char subcommand, int tied,
                                unsigned int session)
{
    /* The head may be unaligned, so its command is read as a byte. */
    unsigned char command =
        (unsigned char) frame->head[offsetof(struct dsi_header, command)];
    int fd = __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE);
    /* iov_base has no const, but the send only reads the data. */
    void *data = (void *) frame->data; /* NOSONAR */
    struct iovec iov[2] = {
        { .iov_base = frame->head, .iov_len = frame->head_len },
        { .iov_base = data, .iov_len = frame->data_len },
    };
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = frame->data_len > 0 ? 2 : 1,
    };
    ssize_t ret;

    if (!may_write(server, tied, session)) {
        return -EIO;
    }

    while (msg.msg_iovlen > 0) {
        /* A part on its own goes by send(): FreeBSD's sendmsg() allocates
         * in the kernel for the iovec it copies in. */
        ret = msg.msg_iovlen == 1
              ? send(fd, msg.msg_iov->iov_base, msg.msg_iov->iov_len,
                     MSG_NOSIGNAL)
              : sendmsg(fd, &msg, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            /* A connection the server has closed ends when the loop reads
             * the close; the write only reports it. */
            if (errno != EPIPE && errno != EBADF) {
                perror("writing to server");
            }

            return -EIO;
        }

        while (msg.msg_iovlen > 0
                && (size_t) ret >= msg.msg_iov->iov_len) {
            ret -= (ssize_t) msg.msg_iov->iov_len;
            msg.msg_iov++;
            msg.msg_iovlen--;
        }

        if (msg.msg_iovlen > 0) {
            msg.msg_iov->iov_base = (char *) msg.msg_iov->iov_base + ret;
            msg.msg_iov->iov_len -= (size_t) ret;
        }
    }

    server->stats.tx_bytes += frame->head_len + frame->data_len;

    /* DSICommand frames carry AFP requests, and DSIWrite frames carry
     * writes. Status, session, and tickle exchanges are DSI's own. */
    if (command == DSI_DSICommand || command == DSI_DSIWrite) {
        server->stats.commands[subcommand]++;
    }

    return 0;
}

/*!
 * @brief End the sends on a descriptor taken from its connection
 *
 * A sender writes its frame, under send_mutex, to the descriptor it loaded
 * at the frame's start. The shutdown fails a send waiting for room, and
 * taking the mutex waits for the frame, so the caller closes a descriptor
 * no sender writes on, and its number can be reused. Each direction is shut
 * on a call of its own: macOS refuses SHUT_RDWR once the peer's close has
 * arrived and leaves the write side open.
 *
 * @param[in,out] server  server the descriptor was taken from
 * @param[in]     fd      the descriptor
 */
void afpc_dsi_end_sends(struct afp_server *server, int fd)
{
    shutdown(fd, SHUT_WR);
    shutdown(fd, SHUT_RD);
    pthread_mutex_lock(&server->send_mutex);
    pthread_mutex_unlock(&server->send_mutex);
}

/*!
 * @brief Send a request's head and data, and wait for its reply
 *
 * A request fails while another thread reconnects, as the new connection
 * takes nothing before its session; an expired one's late reply is dropped.
 * A written request's answer or failure that lands before it leaves the
 * queue is the result, even once the wait has expired.
 *
 * @param[in]     server         server to send to
 * @param[in,out] frame          the request's head, DSI header first and at
 *                               any address, whose length field the call
 *                               sets, and its data
 * @param[in]     wait           seconds to wait for the reply,
 *                               DSI_DEFAULT_TIMEOUT for the server's
 *                               default, 0 for no wait, -1 for no limit
 * @param[in]     subcommand     AFP command, or the DSI command of DSI's own
 *                               exchanges
 * @param[in,out] other          destination of the reply's contents
 * @param[in]     reply_handler  parser of the reply, or NULL for the
 *                               command's own
 * @param[in,out] stream_buffer  buffer that receives a read's data, or NULL
 *                               for other
 * @returns the reply's result, 0 without a wait, ETIMEDOUT once it expires,
 *          -EINVAL for a server removed or never set up or a frame too long
 *          for the DSI length field, or -EIO for an unusable session,
 *          another thread's reconnect, a reconnect into a new session but
 *          for a logout, a failed write, or a request without a wait to a
 *          disconnected session
 */
static int send_frame(struct afp_server *server,
                      const struct dsi_frame *frame, int wait,
                      unsigned char subcommand, void *other,
                      afpc_dsi_reply_handler reply_handler,
                      struct afp_rx_buffer *stream_buffer)
{
    struct dsi_header header;
    struct dsi_request request;
    struct dsi_request **link;
    int tied = server != NULL && tied_server == server;
    unsigned int tie_session = tied_session;
    int failed;
    int sent;
    int rc = 0;
    tied_server = NULL;

    if (tied) {
        fork_unsent = 1;
    }

    if (frame->data_len > UINT32_MAX - (frame->head_len - sizeof(header))) {
        return -EINVAL;
    }

    /* A packed packet lies at any address, so its header is copied out,
     * and back in once changed. */
    memcpy(&header, frame->head, sizeof(header));
    header.length = htonl((uint32_t)(frame->head_len - sizeof(header)
                                     + frame->data_len));
    memcpy(frame->head, &header, sizeof(header));

    if (!server || !server_still_valid(server)) {
        return -EINVAL;
    }

    if (afp_server_reconnect_is_in_progress(server)
            && !afp_server_reconnects_here(server)) {
        return -EIO;
    }

    if (wait == DSI_DONT_WAIT) {
        if (__atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)
                == SERVER_STATE_DISCONNECTED
                || __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE) < 0) {
            return -EIO;
        }

        pthread_mutex_lock(&server->send_mutex);
        rc = afpc_dsi_write_frame(server, frame, subcommand, tied, tie_session);
        pthread_mutex_unlock(&server->send_mutex);

        if (tied) {
            fork_unsent = rc != 0;
        }

        return rc;
    }

    if (wait == DSI_DEFAULT_TIMEOUT) {
        wait = server->dsi_default_timeout;
    }

    afp_wait_for_started_loop();

    if (server->connect_state == SERVER_STATE_DISCONNECTED) {
        char mesg[MAX_ERROR_LEN];
        unsigned int l = 0;
        unsigned int session = afp_server_session_generation(server);

        if (afp_server_is_suspended(server)) {
            return -EIO;
        }

        if (afp_server_reconnect_is_in_progress(server)) {
            return -EIO;
        }

        memset(mesg, 0, sizeof(mesg));

        if (afp_server_reconnect(server, mesg, &l, MAX_ERROR_LEN) != 0) {
            if (mesg[0] != '\0') {
                log_for_client(NULL, AFPFSD, LOG_WARNING, "%s", mesg);
            } else {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "Could not reconnect to %s",
                               server->server_name_printable);
            }

            return -EIO;
        }

        /* The reconnect's closes of kept forks overwrite fork_unsent. */
        if (tied) {
            fork_unsent = 1;
        }

        /* A request may name the old session's volumes; a logout names none,
         * and ends the new session. */
        if (afp_server_session_generation(server) != session
                && subcommand != afpLogout) {
            return -EIO;
        }

        /* A connect restarts the request IDs of a server without a replay
         * cache, so the one this request took before may come again. */
        header.requestid = take_request_id(server);
        memcpy(frame->head, &header, sizeof(header));
    }

    if (server->fd < 0) {
        return -EIO;
    }

    memset(&request, 0, sizeof(request));
    request.requestid = ntohs(header.requestid);
    request.subcommand = subcommand;
    request.other = other;
    request.wait = wait;
    request.connection_generation = afp_server_connection_generation(server);
    request.reply_handler = reply_handler;
    request.stream_buffer = stream_buffer;
    /* Initialize before queueing so dsi_recv can signal this request safely. */
    pthread_cond_init(&request.waiting_cond, NULL);
    pthread_mutex_init(&request.waiting_mutex, NULL);
    pthread_mutex_lock(&server->request_queue_mutex);

    for (link = &server->command_requests; *link; link = &(*link)->next);

    *link = &request;
    server->stats.requests_pending++;
    pthread_mutex_unlock(&server->request_queue_mutex);
    pthread_mutex_lock(&server->send_mutex);
    /* Not sent: a request failed while it waited for the lock, or one queued
     * on a connection since replaced, whose reply the receiver would drop. */
    pthread_mutex_lock(&request.waiting_mutex);
    failed = request.done_waiting
             || request.connection_generation
             != afp_server_connection_generation(server);
    pthread_mutex_unlock(&request.waiting_mutex);
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** Sending %d, %s",
                   ntohs(header.requestid),
                   afp_get_command_name(request.subcommand));
#endif
    rc = failed ? -EIO : afpc_dsi_write_frame(server, frame, subcommand,
                                              tied, tie_session);
    sent = rc == 0;

    if (tied) {
        fork_unsent = rc != 0;
    }

    if (rc == 0 && wait > 0) {
        afpc_dsi_set_deadline(server, &request);
    }

    pthread_mutex_unlock(&server->send_mutex);
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Waiting for response for %d %s",
                   request.requestid,
                   afp_get_command_name(request.subcommand));
#endif
    pthread_mutex_lock(&request.waiting_mutex);

    /* A frame not written has nothing to wait for. */
    while (rc == 0 && request.done_waiting == 0) {
        rc = wait < 0
             ? pthread_cond_wait(&request.waiting_cond, &request.waiting_mutex)
             : pthread_cond_timedwait(&request.waiting_cond,
                                      &request.waiting_mutex,
                                      &request.deadline);
    }

    pthread_mutex_unlock(&request.waiting_mutex);
    afpc_dsi_remove_from_request_queue(server, &request);

    /* A request not sent has no answer of its own. */
    if (sent && request.done_waiting) {
        rc = request.return_code;
    } else if (rc == ETIMEDOUT) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Timed out waiting %d second(s) for request %d, %s",
                       wait, request.requestid,
                       afp_get_command_name(request.subcommand));
    }

#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "=== Done waiting for %d %s, waiting for %ds, return %d",
                   request.requestid,
                   afp_get_command_name(request.subcommand), wait, rc);
#endif
    return rc;
}

/*!
 * @brief Send a DSI request and wait for its reply
 *
 * @param[in]     server         server to send to
 * @param[in,out] msg            request frame, DSI header first, whose
 *                               length field the call sets
 * @param[in]     size           bytes of msg
 * @param[in]     wait           seconds to wait for the reply, as
 *                               send_frame() takes it
 * @param[in]     subcommand     AFP command, or the DSI command of DSI's own
 *                               exchanges
 * @param[in,out] other          destination of the reply's contents
 * @param[in]     reply_handler  parser of the reply, or NULL for the
 *                               command's own
 * @param[in,out] stream_buffer  buffer that receives a read's data, or NULL
 *                               for other
 * @returns what send_frame() returns
 */
int afpc_dsi_send_with_reply(struct afp_server *server, char *msg, int size,
                             int wait, unsigned char subcommand, void *other,
                             afpc_dsi_reply_handler reply_handler,
                             struct afp_rx_buffer *stream_buffer)
{
    const struct dsi_frame frame = { .head = msg, .head_len = (size_t) size };
    return send_frame(server, &frame, wait, subcommand, other, reply_handler,
                      stream_buffer);
}

/*!
 * @brief Send a DSI request and its data, and wait for its reply
 *
 * @param[in]     server      server to send to
 * @param[in,out] msg         the header and parameters, whose length field
 *                            the call sets to count the data too
 * @param[in]     size        bytes of msg
 * @param[in]     data        the data, sent as they are
 * @param[in]     data_len    bytes of data
 * @param[in]     wait        seconds to wait for the reply, as send_frame()
 *                            takes it
 * @param[in]     subcommand  the AFP command
 * @param[in,out] other       destination of the reply's contents
 * @returns what send_frame() returns
 */
int afpc_dsi_send_data(struct afp_server *server, char *msg, int size,
                       const char *data, size_t data_len, int wait,
                       unsigned char subcommand, void *other)
{
    const struct dsi_frame frame = {
        .head = msg,
        .head_len = (size_t) size,
        .data = data,
        .data_len = data_len,
    };
    return send_frame(server, &frame, wait, subcommand, other, NULL, NULL);
}

/*!
 * @brief Hand a DSICommand reply to its AFP command's parser
 *
 * A read's data goes from the socket into the read's buffer.
 *
 * @param[in,out] server      the server, with the reply read so far
 * @param[in]     subcommand  the AFP command the reply answers
 * @param[in,out] other       the request's caller data
 * @returns the parser's result, or -1 for a short reply, no command, or a
 *          read that fails or finds the connection closed
 */
int afpc_dsi_command_reply(struct afp_server* server, unsigned short subcommand,
                           void *other)
{
    int ret = 0;

    if ((unsigned long) server->data_read < sizeof(struct dsi_header)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Got a short reply command, I am just ignoring it. size: %d",
                       server->data_read);
        return -1;
    }

    if (subcommand == 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Broken subcommand: %d", subcommand);
        return -1;
    }

    if ((subcommand == afpRead) || (subcommand == afpReadExt)) {
        struct afp_rx_buffer * buf = other;
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== read() for afpRead, %d bytes",
                       buf->maxsize - buf->size);
#endif

        do {
            ret = read(server->fd, buf->data + buf->size,
                       buf->maxsize - buf->size);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            return -1;
        }

        server->stats.rx_bytes += ret;

        if (ret == 0) {
            return -1;
        }

        server->data_read += ret;
    }

    ret = afp_reply(subcommand, server, other);
    return ret;
}


/* This is the quantum of a server that advertises none or one out of range. */
#define DSI_DEFAULT_QUANTUM (1024U * 1024U)
/* The cap stays far below the int that holds a request frame's size. */
#define DSI_MAX_QUANTUM (256U * 1024U * 1024U)

/*!
 * @brief Read the session options of a DSIOpenSession reply
 *
 * Takes the server's request quantum, or DSI_DEFAULT_QUANTUM when it sends
 * none, 0, or one above DSI_MAX_QUANTUM. Also takes the replay cache size
 * when the server offers one.
 *
 * @param[in,out] server  server whose receive buffer holds the reply
 */
void afpc_dsi_opensession_reply(struct afp_server * server)
{
    const char *p;
    int offset;
    uint8_t option_type, option_len;
    unsigned int quantum = 0;
    /* The loop parses a DSIOpenSession reply's repeated options: an
     * option_type of 1 byte, an option_len of 1 byte, and option_len bytes
     * of data. Option 0 is the server request quantum. */
    offset = sizeof(struct dsi_header);
    p = server->incoming_buffer + offset;

    while (offset + 2 <= server->data_read) {
        option_type = (uint8_t)p[0];
        option_len = (uint8_t)p[1];

        if (offset + 2 + option_len > server->data_read) {
            break;  /* Malformed option */
        }

        if (option_type == 0 && option_len == 4) {
            uint32_t wire;
            memcpy(&wire, p + 2, 4);
            quantum = ntohl(wire);
        } else if (option_type == kServerReplayCacheSize && option_len == 4) {
            /* Option for replay cache */
            uint32_t cache_size;
            memcpy(&cache_size, p + 2, 4);
            server->replay_cache_size = ntohl(cache_size);
            server->supports_replay_cache = 1;
            log_for_client(NULL, AFPFSD, LOG_INFO,
                           "Replay cache enabled (size: %u)",
                           server->replay_cache_size);
        }

        offset += 2 + option_len;
        p += 2 + option_len;
    }

    if (quantum == 0 || quantum > DSI_MAX_QUANTUM) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Server quantum %u unusable, using %u",
                       quantum, DSI_DEFAULT_QUANTUM);
        quantum = DSI_DEFAULT_QUANTUM;
    }

    /* During a reconnect, other threads read the old quantum until this
     * assignment. */
    server->quantum = quantum;
}

/*!
 * @brief Read the AFP versions of a status reply
 *
 * @param[in]  p       the count byte of the list
 * @param[in]  end     the end of the reply
 * @param[out] status  receives the versions this library knows
 * @returns 0, or -EPROTO when the list runs past the reply
 */
static int parse_status_versions(const char *p, const char *end,
                                 struct afp_server_status *status)
{
    unsigned char count;
    char name[33];
    int j = 0;

    if (p >= end) {
        return -EPROTO;
    }

    count = (unsigned char) p[0];
    p++;

    for (unsigned char i = 0; i < count; i++) {
        if (p >= end || (unsigned char) *p >= end - p) {
            return -EPROTO;
        }

        p += copy_from_pascal(name, p, sizeof(name)) + 1;

        for (const struct afp_versions *v = afp_versions; v->av_name; v++) {
            if (strcmp(v->av_name, name) == 0 && j < SERVER_MAX_VERSIONS) {
                status->versions[j++] = (unsigned char) v->av_number;
                break;
            }
        }
    }

    return 0;
}

/*!
 * @brief Read the UAM names of a status reply
 *
 * @param[in]  p       the count byte of the list
 * @param[in]  end     the end of the reply
 * @param[out] status  receives the UAMs as a bitmap
 * @returns 0, or -EPROTO when the list runs past the reply
 */
static int parse_status_uams(const char *p, const char *end,
                             struct afp_server_status *status)
{
    unsigned char count;
    char name[AFP_UAM_LENGTH + 1];

    if (p >= end) {
        return -EPROTO;
    }

    count = (unsigned char) p[0];
    p++;

    for (unsigned char i = 0; i < count; i++) {
        if (p >= end || (unsigned char) *p >= end - p) {
            return -EPROTO;
        }

        p += copy_from_pascal(name, p, sizeof(name)) + 1;
        status->supported_uams |= uam_string_to_bitmap(name);
    }

    return 0;
}

/*!
 * @brief Test whether a Pascal string at an offset lies within a block
 *
 * @param[in] data    the block
 * @param[in] size    bytes of the block
 * @param[in] offset  the string's offset in the block
 * @returns 1 when its length byte and its bytes lie within the block
 */
static int pascal_within(const char *data, size_t size, size_t offset)
{
    return offset < size
           && (size_t)(unsigned char) data[offset] < size - offset;
}

/*!
 * @brief Read an FPGetSrvrInfo reply into status
 *
 * Every server sends the signature's offset whatever kSrvrSig says; afpd
 * sends all four offsets whatever its flags, other servers those flagged.
 *
 * @param[in]  buf     the reply, DSI header first
 * @param[in]  len     bytes of buf
 * @param[out] status  the server's names, type, versions, UAMs, flags,
 *                     signature and icon
 * @returns 0, or -EPROTO for a reply cut short or pointing past its end
 */
int afpc_dsi_parse_status(const char *buf, size_t len,
                          struct afp_server_status *status)
{
    const struct {
        struct dsi_header dsi __attribute__((__packed__));
        uint16_t machine_offset;
        uint16_t version_offset;
        uint16_t uams_offset;
        uint16_t icon_offset;
        uint16_t flags;
    } __attribute__((__packed__)) *head = (const void *) buf;
    const char *data = buf + sizeof(struct dsi_header);
    size_t size;
    size_t at;
    uint16_t offset;
    int all;
    memset(status, 0, sizeof(*status));

    if (len < sizeof(*head)) {
        return -EPROTO;
    }

    /* Each offset is checked as a number before a pointer is made of it. */
    size = len - sizeof(struct dsi_header);
    offset = ntohs(head->machine_offset);

    if (!pascal_within(data, size, offset)) {
        return -EPROTO;
    }

    copy_from_pascal(status->machine_type, data + offset,
                     sizeof(status->machine_type));

    if (ntohs(head->version_offset) >= size
            || ntohs(head->uams_offset) >= size
            || parse_status_versions(data + ntohs(head->version_offset),
                                     data + size, status) != 0
            || parse_status_uams(data + ntohs(head->uams_offset), data + size,
                                 status) != 0) {
        return -EPROTO;
    }

    /* The icon and its mask are optional. */
    offset = ntohs(head->icon_offset);

    if (offset > 0) {
        if (size < AFP_SERVER_ICON_LEN
                || offset > size - AFP_SERVER_ICON_LEN) {
            return -EPROTO;
        }

        memcpy(status->icon, data + offset, AFP_SERVER_ICON_LEN);
    }

    status->flags = ntohs(head->flags);
    at = sizeof(*head) - sizeof(struct dsi_header);

    if (!pascal_within(data, size, at)) {
        return -EPROTO;
    }

    at += copy_from_pascal(status->server_name, data + at,
                           sizeof(status->server_name)) + 1U;
    /* Offsets start on an even byte; afpd's machine type follows all four. */
    at += at & 1U;
    all = ntohs(head->machine_offset) == at + 8;

    if (at + 2 > size) {
        return -EPROTO;
    }

    memcpy(&offset, data + at, sizeof(offset));
    offset = ntohs(offset);
    at += 2;

    if (offset != 0) {
        if (size < AFP_SIGNATURE_LEN || offset > size - AFP_SIGNATURE_LEN) {
            return -EPROTO;
        }

        memcpy(status->signature, data + offset, AFP_SIGNATURE_LEN);
    }

    if (all || (status->flags & kSupportsTCP)) {
        at += 2;
    }

    if (all || (status->flags & kSupportsDirServices)) {
        at += 2;
    }

    offset = 0;

    if (status->flags & kSupportsUTF8SrvrName) {
        if (at + 2 > size) {
            return -EPROTO;
        }

        memcpy(&offset, data + at, sizeof(offset));
        offset = ntohs(offset);
    }

    if (offset != 0) {
        uint16_t name_len;

        if (size < 2 || offset > size - 2) {
            return -EPROTO;
        }

        memcpy(&name_len, data + offset, sizeof(name_len));
        name_len = ntohs(name_len);

        if (name_len > size - 2 - offset) {
            return -EPROTO;
        }

        if (name_len > sizeof(status->server_name_utf8) - 1) {
            name_len = sizeof(status->server_name_utf8) - 1;
        }

        memcpy(status->server_name_utf8, data + offset + 2, name_len);
        convert_utf8dec_to_utf8pre(status->server_name_utf8, name_len,
                                   status->server_name_printable,
                                   AFP_SERVER_NAME_UTF8_LEN);
    } else {
        convert_mac_roman_to_utf8(status->server_name,
                                  (int) strlen(status->server_name),
                                  status->server_name_printable,
                                  AFP_SERVER_NAME_UTF8_LEN);
    }

    return 0;
}


/*!
 * @brief Treat the server's DSICloseSession as a transport loss
 *
 * @param[in,out] server  server that closed the session
 */
void afpc_dsi_incoming_closesession(struct afp_server *server)
{
    /* afpd closes a reconnect's own connection when it refuses its login,
     * and the token's session lives on. */
    if (__atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)
            == SERVER_STATE_CONNECTED) {
        __atomic_store_n(&server->token_ended, 1, __ATOMIC_RELEASE);
    }

    /* A detach would wait on replies only this thread reads and free FUSE's
     * records; the volumes stay for a reconnect into a new session. */
    loop_disconnect(server);
}

/*!
 * @brief Answer the server's tickle with one
 *
 * @param[in,out] server  server that sent the tickle
 */
void afpc_dsi_incoming_tickle(struct afp_server * server)
{
    afpc_dsi_tickle_unless_busy(server);
}


/*!
 * @brief Act on an attention the server sent, on a thread of its own
 *
 * Reads the server's message when the attention asks for it, and ends the
 * connection for a shutdown notice, unless a connect has replaced the
 * connection that carried it.
 *
 * @param[in] other  the attention's context, which the call frees with its
 *                   reference to the server
 * @returns NULL
 */
void *afpc_dsi_incoming_attention(void *other)
{
    struct dsi_attention_context *context = other;
    struct afp_server * server = context->server;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t flags;
    } __attribute__((__packed__)) *packet = (void *) context->packet;
    unsigned short flags = 0;
    char mesg[AFP_LOGINMESG_LEN];
    unsigned char shutdown = 0;
    unsigned char mins = 0;
    unsigned char checkmessage = 0;
    memset(mesg, 0, AFP_LOGINMESG_LEN);

    /* The logic here's undocumented.  If we get an attention packet and
       there's no flag, then go check the message.  Also, go check the
       the message if there is a flag and we have the AFPATTN_MESG flag.
       Checked on netatalk 2.0.3. */

    /* It's a bit tough to find docs on this, but I found it at:

    http://web.archive.org/web/20010806173437/developer.apple.com/techpubs/macosx/Networking/AFPClient/AFPClient-15.html

    */

    if (ntohl(packet->header.length) >= 2) {
        flags = ntohs(packet->flags);

        if (flags & AFPATTN_MESG) {
            checkmessage = 1;
        }

        if (flags & (AFPATTN_CRASH | AFPATTN_SHUTDOWN)) {
            shutdown = 1;
        }

        mins = flags & 0xff;
    } else {
        checkmessage = 1;
    }

    unsigned int current_generation = afp_server_connection_generation(server);

    if (context->connection_generation != current_generation) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Ignoring stale attention packet from connection generation %u (current: %u)",
                       context->connection_generation,
                       current_generation);
        afp_server_release(server);
        free(context);
        return NULL;
    }

    if (checkmessage) {
        afp_getsrvrmsg(server, AFPMESG_SERVER,
                       ((server->using_version && server->using_version->av_number >= 30) ? 1 : 0),
                       DSI_DEFAULT_TIMEOUT, mesg);

        if (memcmp(mesg, "The server is going down for maintenance.", 41) == 0) {
            shutdown = 1;
        }
    }

    if (shutdown) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Got a shutdown notice in packet %d, going down in %d mins",
                       ntohs(packet->header.requestid), mins);
        current_generation = afp_server_connection_generation(server);

        if (context->connection_generation == current_generation) {
            loop_disconnect(server);
        } else {
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "Ignoring stale shutdown notice from connection generation %u (current: %u)",
                           context->connection_generation,
                           current_generation);
        }
    }

    afp_server_release(server);
    free(context);
    return NULL;
}


/*!
 * @brief Find the queued request a reply answers, and hold it
 *
 * Its sender's call waits until afpc_dsi_release_request() lets it go.
 *
 * @param[in,out] server      server whose queue to search
 * @param[in]     request_id  the reply's request ID
 * @returns the request, or NULL when none waits for the reply
 */
struct dsi_request *afpc_dsi_find_request(struct afp_server *server,
                                          unsigned short request_id)
{
    struct dsi_request *p;
    pthread_mutex_lock(&server->request_queue_mutex);

    for (p = server->command_requests; p; p = p->next) {
        if (!p->done_waiting && request_id == p->requestid) {
            p->receiving = 1;
            pthread_mutex_unlock(&server->request_queue_mutex);
            return p;
        }
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
    return NULL;
}

/*!
 * @brief Let a request afpc_dsi_find_request() holds go to its sender
 *
 * @param[in,out] server   server whose queue holds the request
 * @param[in,out] request  the request, which the call no longer touches
 */
static void afpc_dsi_release_request(struct afp_server *server,
                                     struct dsi_request *request)
{
    pthread_mutex_lock(&server->request_queue_mutex);
    request->receiving = 0;
    pthread_cond_broadcast(&server->request_queue_cond);
    pthread_mutex_unlock(&server->request_queue_mutex);
}

/* afpc_dsi_read()'s answer for a connection replaced during the read. */
#define DSI_READ_REPLACED (-2)

/*!
 * @brief Read from the connection a receive began on
 *
 * A read a signal cut short is retried. Bytes read after a connect has
 * replaced the connection belong to no stream this side still follows.
 *
 * @param[in]  server      server whose stream to read
 * @param[in]  fd          the connection's descriptor
 * @param[in]  generation  the connection's generation
 * @param[out] buf         receives the bytes
 * @param[in]  len         bytes to read at most
 * @returns what read() returns, or DSI_READ_REPLACED
 */
static ssize_t afpc_dsi_read(struct afp_server *server, int fd,
                             unsigned int generation, void *buf, size_t len)
{
    ssize_t ret;

    do {
        ret = read(fd, buf, len);
    } while (ret < 0 && errno == EINTR);

    if (afp_server_connection_generation(server) != generation) {
        return DSI_READ_REPLACED;
    }

    return ret;
}

/*!
 * @brief Whether a DSI command byte names a command DSI defines
 *
 * @param[in] command  the header's command byte
 * @returns 1 for a defined command, 0 for another
 */
static int afpc_dsi_command_is_defined(uint8_t command)
{
    switch (command) {
    case DSI_DSICloseSession:
    case DSI_DSICommand:
    case DSI_DSIGetStatus:
    case DSI_DSIOpenSession:
    case DSI_DSITickle:
    case DSI_DSIWrite:
    case DSI_DSIAttention:
        return 1;

    default:
        return 0;
    }
}

/*!
 * @brief Read the next piece of the stream, and serve a frame it completes
 *
 * @param[in,out] server      server whose stream to read
 * @param[in]     fd          the connection's descriptor
 * @param[in]     generation  the connection's generation
 * @param[out]    found       receives the request the call holds, or NULL
 * @returns 0, or -1 for a stream that has ended or lost its framing
 */
static int afpc_dsi_recv_frame(struct afp_server *server, int fd,
                               unsigned int generation,
                               struct dsi_request **found)
{
    struct dsi_header * header = (void *) server->incoming_buffer;
    struct dsi_request * request = NULL;
#ifdef DEBUG_DSI
    int rc;
#endif
    size_t amount_to_read = 0;
    ssize_t ret;
    unsigned char runt_packet = 0;
    int result = 0;
    size_t payload_length;
    size_t payload_read;
    size_t payload_remaining;

    /* The rest of a frame being dropped, one that did not fit the buffer
     * or a read reply whose request left the queue while it streamed, is
     * read and dropped in buffer-sized pieces, one piece per call. */
    if (server->discard_remaining > 0) {
        amount_to_read = min(server->discard_remaining,
                             (size_t)server->bufsize);
        ret = afpc_dsi_read(server, fd, generation, server->incoming_buffer,
                            amount_to_read);

        if (ret == DSI_READ_REPLACED) {
            return 0;
        }

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error dropping a frame on fd=%d: %s (errno=%d)",
                           fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed while dropping a frame (EOF on fd=%d)",
                           fd);
            return -1;
        }

        server->stats.rx_bytes += ret;
        server->discard_remaining -= ret;
        return 0;
    }

    /* Make sure we have at least one  header */
    if ((size_t)server->data_read < sizeof(struct dsi_header)) {
        amount_to_read = sizeof(struct dsi_header) - server->data_read;
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< read() for dsi, %zu bytes",
                       amount_to_read);
#endif
        ret = afpc_dsi_read(server, fd, generation,
                            server->incoming_buffer + server->data_read,
                            amount_to_read);

        if (ret == DSI_READ_REPLACED) {
            return 0;
        }

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error on fd=%d: %s (errno=%d)",
                           fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed by server (EOF on fd=%d, data_read=%d/%zu)",
                           fd, server->data_read, sizeof(struct dsi_header));
            return -1;
        }

        server->stats.rx_bytes += ret;
        server->data_read += ret;

        if ((server->data_read == sizeof(struct dsi_header)) &&
                (ntohl(header->length) == 0)) {
            goto gotenough;
        }

        return 0;
        /* We'll get the rest of the packet next time */
    }

gotenough:

    /* Flags that are neither a request's nor a reply's, or a command DSI
     * does not define, are bytes of no frame. */
    if ((header->flags != DSI_REQUEST && header->flags != DSI_REPLY)
            || !afpc_dsi_command_is_defined(header->command)) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "dsi_recv: no DSI frame has flags %u and command %u",
                       header->flags, header->command);
        return -1;
    }

    /* A reply names a request of this side's queue. A request from the
     * server carries the server's own id sequence, so the queue is not
     * searched for it: a tickle whose id equals a pending request's would
     * otherwise be taken for that request's reply. */
    if (header->flags == DSI_REPLY) {
        request = afpc_dsi_find_request(server, ntohs(header->requestid));
        *found = request;

        if (!request) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "I have no idea what this is a reply to, id %d",
                           ntohs(header->requestid));
            runt_packet = 1;
            server->stats.runt_packets++;
        }
    } else if (header->command != DSI_DSICloseSession
               && header->command != DSI_DSITickle
               && header->command != DSI_DSIAttention) {
        /* The server sends those three as requests and nothing else. */
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Dropping a frame with flags %u, command %u, id %d",
                       header->flags, header->command,
                       ntohs(header->requestid));
        runt_packet = 1;
        server->stats.runt_packets++;
    }

    /* Check if this is a stale reply from an old connection */
    unsigned int current_generation = afp_server_connection_generation(server);

    if (request
            && request->connection_generation != current_generation) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Discarding stale reply id %d from connection generation %u (current: %u)",
                       ntohs(header->requestid), request->connection_generation,
                       current_generation);
        runt_packet = 1;
        server->stats.runt_packets++;
        request = NULL;
    }

    if (request) {
        result = (int) ntohl(header->return_code.error_code);
    }

    /* If it is a read, read in as much as we can */
    if ((request) &&
            ((request->subcommand == afpRead) ||
             (request->subcommand == afpReadExt))) {
        struct afp_rx_buffer *buf = request->stream_buffer
                                        ? request->stream_buffer
                                        : request->other;
        unsigned int announced = ntohl(header->length);
        unsigned int newmax;

        if ((server->data_read == sizeof(struct dsi_header))
                && (announced == 0)) {
            server->data_read = 0;
            goto out;
        }

        if ((!buf) || (!buf->maxsize)) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "No buffer allocated for incoming data");
            return -1;
        }

        /* The reply streams into the buffer across calls. A reply that
         * announces fewer bytes than the buffer holds, or more than it
         * can take, has put the stream out of step. */
        if (announced < buf->size || announced > buf->maxsize) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read reply of %u bytes, %u read into a buffer of %u",
                           announced, buf->size, buf->maxsize);
            return -1;
        }

        newmax = buf->maxsize - buf->size;

        if (newmax > announced - buf->size) {
            newmax = announced - buf->size;
        }

#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "<<< read() in response to a request, %d bytes", newmax);
#endif
        ret = afpc_dsi_read(server, fd, generation, buf->data + buf->size,
                            newmax);

        if (ret == DSI_READ_REPLACED) {
            return 0;
        }

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error in afpRead response on fd=%d: %s (errno=%d)",
                           fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed during afpRead response (EOF on fd=%d)",
                           fd);
            return -1;
        }

        server->stats.rx_bytes += ret;
        buf->size += ret;
        server->stream_read += ret;

        /* Check to see if we've read enough */
        if (ntohl(header->length) == buf->size) {
            char *tmpbuf;
            int size_to_copy = server->data_read
                               - sizeof(struct dsi_header);
            server->stream_read = 0;

            if (size_to_copy == 0) {
                server->data_read = 0;
                goto out;
            } else if (size_to_copy < 0) {
                goto error;
            }

            /* Okay, so there is a buffer we have to shift */
            if ((tmpbuf = malloc(size_to_copy)) == NULL) {
                log_for_client(NULL, AFPFSD, LOG_ERR,
                               "Problem allocating memory for dsi_recv of size %d", size_to_copy);
                goto error;
            }

            memcpy(tmpbuf,
                   server->incoming_buffer +
                   sizeof(struct dsi_header),
                   size_to_copy);
            memcpy(server->incoming_buffer, tmpbuf,
                   size_to_copy);
            server->data_read = size_to_copy;
            free(tmpbuf);
        } else {
            return 0;
        }
    } else {
        /* Okay, so it isn't a response to an afpRead or afpReadExt */
        if (((server->data_read == sizeof(struct dsi_header)) &&
                (ntohl(header->length) == 0))) {
            /* A dropped frame without a payload has nothing left to read. */
            if (runt_packet) {
                goto after_processing;
            }

            goto process_packet;
        }

        payload_length = ntohl(header->length);
        payload_read = server->data_read - sizeof(struct dsi_header);

        /* A read reply whose request is gone streamed part of its payload
         * into that request's buffer; the rest is read and dropped. */
        if (server->stream_read > 0) {
            server->discard_remaining = payload_length - server->stream_read;
            server->stream_read = 0;
            server->data_read = 0;
            goto out;
        }

        if (payload_length > (size_t)server->bufsize - sizeof(
                    struct dsi_header)) {
            /* No read asks for more than the quantum, so no frame longer
             * than both it and the buffer is one. */
            if (payload_length > server->quantum) {
                log_for_client(NULL, AFPFSD, LOG_ERR,
                               "dsi_recv: a DSI payload of %zu bytes is over the quantum of %u, id %d",
                               payload_length, server->quantum,
                               ntohs(header->requestid));
                return -1;
            }

            /* The late reply of a read that timed out carries its data,
             * more than the buffer holds. The buffer holds less than the
             * payload, so the difference is what remains to drop. */
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: dropping a DSI payload of %zu bytes over the receive buffer's %zu, id %d",
                           payload_length,
                           (size_t)server->bufsize - sizeof(struct dsi_header),
                           ntohs(header->requestid));
            result = -EIO;
            server->discard_remaining = payload_length - payload_read;
            server->data_read = 0;
            goto out;
        }

        if (payload_read > payload_length) {
            goto error;
        }

        payload_remaining = payload_length - payload_read;

        if (payload_remaining == 0) {
            goto process_packet;
        }

        amount_to_read = min(payload_remaining,
                             (size_t)server->bufsize - server->data_read);
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< read() of rest of AFP, %zu bytes",
                       amount_to_read);
#endif
        ret = afpc_dsi_read(server, fd, generation,
                            server->incoming_buffer + server->data_read,
                            amount_to_read);

        if (ret == DSI_READ_REPLACED) {
            return 0;
        }

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error reading AFP packet on fd=%d: %s (errno=%d)",
                           fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed while reading AFP packet (EOF on fd=%d)",
                           fd);
            return -1;
        }

        server->stats.rx_bytes += ret;
        server->data_read += ret;

        if ((unsigned long) server->data_read < (ntohl(header->length) + sizeof(
                                                     *header))) {
            return 0;
        }
    }

    if (runt_packet) {
        goto after_processing;
    }

process_packet:
    /* At this point, we have a full DSI packet
       (or, for an afpRead, just the header and the data's been
        dumped in the preallocated buffer */
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "<<< Handling DSI command %d, request id %d",
                   header->command, ntohs(header->requestid));
#endif

    switch (header->command) {
    case DSI_DSICloseSession:
        afpc_dsi_incoming_closesession(server);
        break;

    case DSI_DSIOpenSession:

        /* A busy refusal carries no session options. */
        if (request && result == 0) {
            afpc_dsi_opensession_reply(server);
        }

        break;

    case DSI_DSITickle:
        afpc_dsi_incoming_tickle(server);
        break;

    case DSI_DSIWrite:
    case DSI_DSICommand:
        if (!runt_packet) {
            int reply_result;

            if (request->reply_handler) {
                reply_result = request->reply_handler(
                                   server, server->incoming_buffer,
                                   (unsigned int)server->data_read,
                                   request->other);
            } else {
                reply_result = afpc_dsi_command_reply(
                                   server, request->subcommand,
                                   request->other);
            }

            if (reply_result != 0 && result == 0) {
                result = reply_result;
            }
        }

        break;

    case DSI_DSIAttention: {
        pthread_t thread;
        struct dsi_attention_context *context;
        size_t packet_len = server->data_read;
        context = malloc(sizeof(*context) + packet_len);

        if (!context) {
            goto error;
        }

        context->server = server;
        context->connection_generation = afp_server_connection_generation(server);
        memcpy(context->packet, server->incoming_buffer, packet_len);
        afp_server_hold(server);

        if (pthread_create(&thread, NULL, afpc_dsi_incoming_attention,
                           context) == 0) {
            pthread_detach(thread);
        } else {
            afp_server_release(server);
            free(context);
        }
    }
    break;

    default:
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Unknown DSI command %i", header->command);
        goto error;
    }

after_processing:

    if ((unsigned long) server->data_read == ntohl(header->length) + sizeof(
                *header)) {
        /* The most common case */
        server->data_read = 0;
    } else {
        unsigned int packet_size = ntohl(header->length) + sizeof(*header);
        unsigned int size_to_copy;

        if ((unsigned int)server->data_read < packet_size) {
            goto error;
        }

        size_to_copy = server->data_read - packet_size;
        memmove(server->incoming_buffer,
                server->incoming_buffer + packet_size,
                size_to_copy);
        server->data_read = size_to_copy;
    }

out:
#ifdef DEBUG_DSI
    rc = ntohl(header->return_code.error_code);
#endif

    if (request) {
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< Found request %d, %s",
                       request->requestid,
                       afp_get_command_name(request->subcommand));
#endif
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "<<< Signalling %d, returning %d or %d", request->requestid,
                       result, rc);
#endif
        /* A deadline sums the waits under the queue's lock; a disconnect or
         * a flush that ended the wait first keeps its own result. */
        pthread_mutex_lock(&server->request_queue_mutex);
        pthread_mutex_lock(&request->waiting_mutex);

        if (!request->done_waiting) {
            request->return_code = result;
            request->wait = 0;
            request->done_waiting = 1;
            pthread_cond_signal(&request->waiting_cond);
        }

        pthread_mutex_unlock(&request->waiting_mutex);
        pthread_mutex_unlock(&server->request_queue_mutex);
    }

    return 0;
error:
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Returning from dsi_recv with an error");
#endif
    return -1;
}

/*!
 * @brief Read the next piece of a server's stream, as the loop thread does
 *
 * The receive state describes the stream of one connection, and a new
 * connection starts it afresh. The descriptor and generation are read
 * together, so a connect that has taken the descriptor or replaced the
 * connection meanwhile leaves the call nothing to read, as does a
 * disconnect that has taken it.
 *
 * @param[in,out] server  server whose descriptor is ready
 * @returns 0, or -1 for a stream that has ended or lost its framing
 */
int afpc_dsi_recv(struct afp_server * server)
{
    struct dsi_request *found = NULL;
    unsigned int generation = afp_server_connection_generation(server);
    int fd = __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE);
    int ret;

    if (fd < 0 || afp_server_connection_generation(server) != generation) {
        return 0;
    }

    if (server->rx_generation != generation) {
        server->data_read = 0;
        server->discard_remaining = 0;
        server->stream_read = 0;
        server->rx_generation = generation;
    }

    ret = afpc_dsi_recv_frame(server, fd, generation, &found);

    if (found) {
        afpc_dsi_release_request(server, found);
    }

    return ret;
}
