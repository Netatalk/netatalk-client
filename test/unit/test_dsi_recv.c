/*
    test_dsi_recv.c: the DSI receive path of a session

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief Unit tests of a session's DSI sends, receive path and connect
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/mapping.h"
#include "lib/utils.h"
#include "tap.h"

/*!
 * @brief Swallow a log message
 */
static void discard_log(void *priv, enum logtypes logtype, int loglevel,
                        const char *message)
{
    (void) priv;
    (void) logtype;
    (void) loglevel;
    (void) message;
}

/* The library's log lines would otherwise land in the TAP stream. */
static struct libafpclient quiet_client = {
    .log_for_client = discard_log,
};

/*!
 * @brief Wait for the loop's start on a thread of its own, then count it
 *
 * @param[in,out] started  the count of threads the start has woken
 * @returns NULL
 */
static void *wait_for_loop(void *started)
{
    afp_wait_for_started_loop();
    __atomic_add_fetch((int *) started, 1, __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Sleep for a number of milliseconds
 *
 * @param[in] ms  milliseconds to sleep
 */
static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*!
 * @brief The loop's start wakes every thread waiting for it
 *
 * Requests' threads wait for the loop; two that wait before the loop runs both
 * return once it does. A waiter that never wakes would hang a join, so the
 * count is the check and the threads are not joined, which is why the count
 * outlives the call. The loop this starts serves the tests after it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_loop_start_wakes_every_waiter(void)
{
    static int started;
    pthread_t waiters[2];

    if (pthread_create(&waiters[0], NULL, wait_for_loop, &started) != 0
            || pthread_create(&waiters[1], NULL, wait_for_loop, &started)
            != 0) {
        return 1;
    }

    sleep_ms(100);

    if (afp_main_quick_startup(NULL) != 0) {
        return 2;
    }

    for (int i = 0;
            i < 200 && __atomic_load_n(&started, __ATOMIC_ACQUIRE) < 2; i++) {
        sleep_ms(10);
    }

    return __atomic_load_n(&started, __ATOMIC_ACQUIRE) == 2 ? 0 : 3;
}

/*!
 * @brief The loop's wake-up keeps its handler through a delivery
 *
 * A handler a delivery resets, or one the signal interrupts, leaves a
 * second wake-up to its default action, which ends the process. The loop
 * the first test started installed it; one wake-up reaches it before the
 * check.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_wake_up_keeps_its_handler(void)
{
    struct sigaction wake;
    signal_main_thread();
    sleep_ms(100);

    if (sigaction(SIGUSR2, NULL, &wake) != 0) {
        return 1;
    }

    if (wake.sa_handler == SIG_DFL || wake.sa_handler == SIG_IGN) {
        return 2;
    }

    return (wake.sa_flags & (SA_RESETHAND | SA_NODEFER)) != 0 ? 3 : 0;
}

/* The byte that fills the guard after a session's receive buffer. */
#define BUFFER_GUARD 0xa5

/* A session whose socket's other end the test holds, so the test plays
 * the server: it writes frames to peer and runs the receiver on server. */
struct session {
    struct afp_server *server;
    int peer;
};

/*!
 * @brief Release a session's server
 *
 * The requests a test leaves queued are its own, on its stack, so the queue is
 * emptied first.
 *
 * @param[in,out] s  the session
 */
static void session_close(struct session *s)
{
    pthread_mutex_lock(&s->server->request_queue_mutex);
    s->server->command_requests = NULL;
    pthread_mutex_unlock(&s->server->request_queue_mutex);

    if (s->server->fd >= 0) {
        close(s->server->fd);
    }

    if (s->peer >= 0) {
        close(s->peer);
    }

    afp_server_release(s->server);
}

/*!
 * @brief Open a session on a socket pair, its server connected
 *
 * The receive buffer is followed by a guard as long, which buffer_kept()
 * reads.
 *
 * @param[out] s        receives the session
 * @param[in]  bufsize  size of the server's receive buffer
 * @returns 0, or -1
 */
static int session_open(struct session *s, int bufsize)
{
    int fds[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return -1;
    }

    s->server = afp_server_init(NULL);

    if (s->server == NULL) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    s->server->fd = fds[0];
    s->peer = fds[1];
    free(s->server->incoming_buffer);
    s->server->incoming_buffer = calloc(2, (size_t) bufsize);
    s->server->bufsize = bufsize;
    s->server->connect_state = SERVER_STATE_CONNECTED;
    s->server->quantum = 65536;

    if (s->server->incoming_buffer == NULL) {
        session_close(s);
        return -1;
    }

    memset(s->server->incoming_buffer + bufsize, BUFFER_GUARD,
           (size_t) bufsize);
    return 0;
}

/*!
 * @brief Queue a request that waits for its reply, as a sender's thread would
 *
 * @param[in,out] server      server whose queue takes the request
 * @param[out]    r           the request, on the caller's stack
 * @param[in]     id          its request ID
 * @param[in]     subcommand  its AFP command
 * @param[in]     other       what its reply handler receives, or NULL
 * @param[in]     handler     its reply handler, or NULL
 */
static void queue_request(struct afp_server *server, struct dsi_request *r,
                          unsigned short id, unsigned char subcommand,
                          void *other, afpc_dsi_reply_handler handler)
{
    memset(r, 0, sizeof(*r));
    r->requestid = id;
    r->subcommand = subcommand;
    r->other = other;
    r->reply_handler = handler;
    r->wait = server->dsi_default_timeout;
    r->connection_generation = afp_server_connection_generation(server);
    pthread_cond_init(&r->waiting_cond, NULL);
    pthread_mutex_init(&r->waiting_mutex, NULL);
    pthread_mutex_lock(&server->request_queue_mutex);
    r->next = server->command_requests;
    server->command_requests = r;
    server->stats.requests_pending++;
    pthread_mutex_unlock(&server->request_queue_mutex);
}

/*!
 * @brief A DSI header that announces length payload bytes
 *
 * @param[in] flags    the header's flags
 * @param[in] command  its DSI command
 * @param[in] id       its request ID
 * @param[in] length   payload bytes it announces
 * @returns the header
 */
static struct dsi_header dsi_header_of(uint8_t flags, uint8_t command,
                                       unsigned short id, uint32_t length)
{
    struct dsi_header header;
    memset(&header, 0, sizeof(header));
    header.flags = flags;
    header.command = command;
    header.requestid = htons(id);
    header.length = htonl(length);
    return header;
}

/*!
 * @brief Write a DSI header that announces length payload bytes
 *
 * @param[in] peer     the server's end of the session
 * @param[in] flags    the header's flags
 * @param[in] command  its DSI command
 * @param[in] id       its request ID
 * @param[in] length   payload bytes it announces
 * @returns 0, or -1
 */
static int send_header(int peer, uint8_t flags, uint8_t command,
                       unsigned short id, uint32_t length)
{
    struct dsi_header header = dsi_header_of(flags, command, id, length);
    return write(peer, &header, sizeof(header)) == (ssize_t) sizeof(header)
           ? 0 : -1;
}

/*!
 * @brief Write a DSI frame, its header and its payload
 *
 * @param[in] peer     the server's end of the session
 * @param[in] flags    the header's flags
 * @param[in] command  its DSI command
 * @param[in] id       its request ID
 * @param[in] payload  the payload, or NULL
 * @param[in] len      bytes of payload
 * @returns 0, or -1
 */
static int send_frame(int peer, uint8_t flags, uint8_t command,
                      unsigned short id, const void *payload, uint32_t len)
{
    if (send_header(peer, flags, command, id, len) != 0) {
        return -1;
    }

    if (len > 0 && write(peer, payload, len) != (ssize_t) len) {
        return -1;
    }

    return 0;
}

/*!
 * @brief Run the receiver while the socket holds data, as the loop thread
 *        would
 *
 * The test writes its frames before the call, so after the first pass an
 * empty socket ends the run.
 *
 * @param[in,out] server  server whose stream to read
 * @returns 0, or the first negative result, which ends the run
 */
static int receive_all(struct afp_server *server)
{
    struct pollfd pfd;
    int rc = 0;
    int wait_ms = 200;
    pfd.fd = server->fd;
    pfd.events = POLLIN;

    while (poll(&pfd, 1, wait_ms) == 1 && (pfd.revents & POLLIN)) {
        rc = afpc_dsi_recv(server);

        if (rc < 0) {
            return rc;
        }

        wait_ms = 0;
    }

    return rc;
}

/*!
 * @brief Tell whether the receiver kept inside a session's receive buffer
 *
 * @param[in] s  the session
 * @returns 1 when the guard after the buffer is whole, 0 otherwise
 */
static int buffer_kept(const struct session *s)
{
    const unsigned char *buffer =
        (const unsigned char *) s->server->incoming_buffer;

    for (int i = s->server->bufsize; i < 2 * s->server->bufsize; i++) {
        if (buffer[i] != BUFFER_GUARD) {
            return 0;
        }
    }

    return 1;
}

/*!
 * @brief A tickle carrying a pending request's id is not its reply
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_tickle_with_pending_id_is_a_request(void)
{
    struct session s;
    struct dsi_request pending;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &pending, 7, afpGetVolParms, NULL, NULL);

    if (send_frame(s.peer, DSI_REQUEST, DSI_DSITickle, 7, NULL, 0) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != 0) {
        ret = 3;
    } else if (pending.done_waiting != 0) {
        ret = 4;
    } else if (s.server->command_requests != &pending) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A command frame with request flags is read and dropped
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_request_flag_command_is_dropped(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (send_frame(s.peer, DSI_REQUEST, DSI_DSICommand, 99, "abcd", 4)
            != 0) {
        ret = 2;
    } else if (receive_all(s.server) != 0) {
        ret = 3;
    } else if (s.server->data_read != 0) {
        ret = 4;
    } else if (s.server->stats.runt_packets != 1) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A frame with a command DSI does not define ends the session
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_undefined_command_ends_session(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (send_frame(s.peer, DSI_REQUEST, 9, 100, NULL, 0) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Flags that are neither a request's nor a reply's end the session
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_undefined_flags_end_session(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (send_frame(s.peer, 2, DSI_DSICommand, 101, "abcd", 4) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A read reply shorter than its buffer's data ends the session
 *
 * Nothing is read into the buffer.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_short_read_reply_ends_session(void)
{
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    char data[64];
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    rx.data = data;
    rx.maxsize = sizeof(data);
    rx.size = 10;
    rx.errorcode = 0;
    queue_request(s.server, &pending, 3, afpReadExt, &rx, NULL);

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 3, "wxyz", 4) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    } else if (rx.size != 10) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A read reply longer than its buffer ends the session
 *
 * No byte is read first.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_long_read_reply_ends_session(void)
{
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    char data[64];
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    rx.data = data;
    rx.maxsize = sizeof(data);
    rx.size = 0;
    rx.errorcode = 0;
    queue_request(s.server, &pending, 5, afpReadExt, &rx, NULL);

    if (send_header(s.peer, DSI_REPLY, DSI_DSICommand, 5, 8192) != 0
            || write(s.peer, "wxyz", 4) != 4) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    } else if (rx.size != 0) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A read reply with no buffer fails
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_without_buffer_fails(void)
{
    struct session s;
    struct dsi_request pending;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &pending, 4, afpReadExt, NULL, NULL);

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 4, "wxyz", 4) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Count a reply, as a request's reply handler
 *
 * @param[in]     server   unused
 * @param[in]     buffer   unused
 * @param[in]     size     unused
 * @param[in,out] context  the count
 * @returns 0
 */
static int record_reply(struct afp_server *server, char *buffer,
                        unsigned int size, void *context)
{
    (void) server;
    (void) buffer;
    (void) size;
    *(int *) context += 1;
    return 0;
}

/*!
 * @brief An orphan reply larger than the buffer is drained
 *
 * The frame after it is served.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_orphan_oversize_reply_is_dropped(void)
{
    struct session s;
    struct dsi_request pending;
    char payload[5000];
    int replies = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    memset(payload, 'p', sizeof(payload));

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 5, payload,
                   sizeof(payload)) != 0) {
        ret = 2;
        goto out;
    }

    if (receive_all(s.server) != 0) {
        ret = 3;
        goto out;
    }

    if (s.server->data_read != 0 || s.server->stats.runt_packets != 1) {
        ret = 4;
        goto out;
    }

    queue_request(s.server, &pending, 6, afpGetVolParms, &replies,
                  record_reply);

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 6, NULL, 0) != 0) {
        ret = 5;
    } else if (receive_all(s.server) != 0) {
        ret = 6;
    } else if (replies != 1 || pending.done_waiting != 1) {
        ret = 7;
    } else if (!buffer_kept(&s)) {
        ret = 8;
    }

out:
    session_close(&s);
    return ret;
}

/*!
 * @brief The rest of an abandoned read reply is drained
 *
 * The request left the queue after part of the reply streamed into its buffer;
 * the next reply, which carries a payload, is served.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_abandoned_read_rest_is_dropped(void)
{
    struct session s;
    struct dsi_request pending;
    struct dsi_request later;
    struct afp_rx_buffer rx;
    char data[5000];
    char payload[5000];
    int replies = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    rx.data = data;
    rx.maxsize = sizeof(data);
    rx.size = 0;
    rx.errorcode = 0;
    queue_request(s.server, &pending, 11, afpReadExt, &rx, NULL);
    memset(payload, 'p', sizeof(payload));

    if (send_header(s.peer, DSI_REPLY, DSI_DSICommand, 11, sizeof(payload))
            != 0 || write(s.peer, payload, 100) != 100) {
        ret = 2;
        goto out;
    }

    if (receive_all(s.server) != 0 || rx.size != 100) {
        ret = 3;
        goto out;
    }

    /* The sender's wait expires: it takes its request off the queue. */
    pthread_mutex_lock(&s.server->request_queue_mutex);
    s.server->command_requests = NULL;
    s.server->stats.requests_pending = 0;
    pthread_mutex_unlock(&s.server->request_queue_mutex);

    if (write(s.peer, payload + 100, sizeof(payload) - 100)
            != (ssize_t)(sizeof(payload) - 100)) {
        ret = 4;
        goto out;
    }

    queue_request(s.server, &later, 12, afpGetVolParms, &replies,
                  record_reply);

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 12, "abcd", 4) != 0) {
        ret = 5;
    } else if (receive_all(s.server) != 0) {
        ret = 6;
    } else if (replies != 1) {
        ret = 7;
    } else if (rx.size != 100 || s.server->data_read != 0) {
        ret = 8;
    } else if (!buffer_kept(&s)) {
        ret = 9;
    }

out:
    session_close(&s);
    return ret;
}

/*!
 * @brief A reply larger than the buffer fails its request with EIO
 *
 * The reply itself is dropped.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_oversize_reply_fails_its_request(void)
{
    struct session s;
    struct dsi_request pending;
    char payload[5000];
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &pending, 13, afpGetVolParms, NULL, NULL);
    memset(payload, 'p', sizeof(payload));

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 13, payload,
                   sizeof(payload)) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != 0) {
        ret = 3;
    } else if (pending.done_waiting != 1 || pending.return_code != -EIO) {
        ret = 4;
    } else if (s.server->data_read != 0) {
        ret = 5;
    } else if (!buffer_kept(&s)) {
        ret = 6;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A frame longer than the buffer and the quantum ends the session
 *
 * No read asks for more than the quantum, so such a length is bytes of no
 * frame.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_frame_over_the_quantum_ends_session(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (send_header(s.peer, DSI_REPLY, DSI_DSICommand, 102,
                    s.server->quantum + 1) != 0
            || write(s.peer, "wxyz", 4) != 4) {
        ret = 2;
    } else if (receive_all(s.server) != -1) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/* The waits for a connection go in slices: OpenBSD 8.0 can leave a sleeping
 * accept() or connect() unwoken by a connection that completed, which each
 * poll() looks for afresh. */
#define SOCKET_SLICE_MS 50
#define SOCKET_WAIT_MS 5000

/* A listening socket on the loopback address, at a port the system picks,
 * for a server the test connects as afp_server_connect() does. */
struct listener {
    struct sockaddr_in addr;
    struct addrinfo ai;
    int fd;
};

/*!
 * @brief Close a listener
 *
 * @param[in] l  the listener
 */
static void listener_close(struct listener *l)
{
    close(l->fd);
}

/*!
 * @brief Listen on the loopback address
 *
 * @param[out] l  receives the listener
 * @returns 0, or -1
 */
static int listener_open(struct listener *l)
{
    socklen_t len = sizeof(l->addr);
    memset(&l->addr, 0, sizeof(l->addr));
    l->addr.sin_family = AF_INET;
    l->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    l->fd = socket(AF_INET, SOCK_STREAM, 0);

    if (l->fd < 0) {
        return -1;
    }

    if (fcntl(l->fd, F_SETFL, O_NONBLOCK) != 0
            || bind(l->fd, (struct sockaddr *) &l->addr, sizeof(l->addr)) != 0
            || getsockname(l->fd, (struct sockaddr *) &l->addr, &len) != 0
            || listen(l->fd, 4) != 0) {
        listener_close(l);
        return -1;
    }

    memset(&l->ai, 0, sizeof(l->ai));
    l->ai.ai_family = AF_INET;
    l->ai.ai_socktype = SOCK_STREAM;
    l->ai.ai_protocol = IPPROTO_TCP;
    l->ai.ai_addr = (struct sockaddr *) &l->addr;
    l->ai.ai_addrlen = sizeof(l->addr);
    return 0;
}

/*!
 * @brief Accept a connection on a listener, within SOCKET_WAIT_MS
 *
 * @param[in] fd  the listener, non-blocking
 * @returns the connection's descriptor, in blocking mode, or -1
 */
static int accept_within(int fd)
{
    for (int waited = 0; waited < SOCKET_WAIT_MS; waited += SOCKET_SLICE_MS) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int peer = accept(fd, NULL, NULL);

        if (peer >= 0) {
            /* The BSDs give the connection the listener's O_NONBLOCK. */
            fcntl(peer, F_SETFL, fcntl(peer, F_GETFL, 0) & ~O_NONBLOCK);
            return peer;
        }

        /* A connection its client ended before the accept is gone. */
        if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED
                && errno != EPROTO) {
            return -1;
        }

        poll(&pfd, 1, SOCKET_SLICE_MS);
    }

    errno = ETIMEDOUT;
    return -1;
}

/*!
 * @brief Connect a socket to a listener's address, within SOCKET_WAIT_MS
 *
 * @param[in] fd    the socket, in blocking mode again on return
 * @param[in] addr  the address
 * @returns 0, or -1
 */
static int connect_within(int fd, const struct sockaddr_in *addr)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int error = 0;
    socklen_t len = sizeof(error);
    int ret = -1;

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }

    if (connect(fd, (const struct sockaddr *) addr, sizeof(*addr)) == 0) {
        ret = 0;
    } else if (errno == EINPROGRESS) {
        for (int waited = 0; waited < SOCKET_WAIT_MS;
                waited += SOCKET_SLICE_MS) {
            struct pollfd pfd = { fd, POLLOUT, 0 };

            if (poll(&pfd, 1, SOCKET_SLICE_MS) == 1) {
                ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0
                      && error == 0 ? 0 : -1;
                break;
            }
        }
    }

    fcntl(fd, F_SETFL, flags);
    return ret;
}

/*! A run of the receiver on its own thread, and its result. */
struct receive_call {
    struct afp_server *server;
    int started; /*!< 1 once the thread runs the receiver */
    int result;
};

/*!
 * @brief Run the receiver once, as the loop thread does when it wakes
 *
 * @param[in,out] arg  the call, whose result receives the receiver's
 * @returns NULL
 */
static void *receive_once(void *arg)
{
    struct receive_call *call = arg;
    __atomic_store_n(&call->started, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&call->result, afpc_dsi_recv(call->server),
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Run the receiver once its socket reports readable, as the loop
 *        thread does
 *
 * The receiver never waits for bytes, so the thread waits for the socket
 * first, 10 s at most. It polls a duplicate of the descriptor: the loop
 * closes a connection's descriptor itself between its passes, but here a
 * connect closes it at once, and another socket could take its number. A
 * connection shut down before the poll ends it as one shut down during it.
 *
 * @param[in,out] arg  the call, whose result receives the receiver's
 * @returns NULL
 */
static void *receive_when_ready(void *arg)
{
    struct receive_call *call = arg;
    struct pollfd pfd;
    pfd.fd = dup(__atomic_load_n(&call->server->fd, __ATOMIC_ACQUIRE));
    pfd.events = POLLIN;
    __atomic_store_n(&call->started, 1, __ATOMIC_RELEASE);
    poll(&pfd, 1, 10000);

    if (pfd.fd >= 0) {
        close(pfd.fd);
    }

    __atomic_store_n(&call->result, afpc_dsi_recv(call->server),
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Wait for a receiver's thread to reach its wait or its read
 *
 * The thread has started once it sets the flag, and is there 100 ms later.
 *
 * @param[in] call  the receiver's call
 */
static void wait_for_receiver(struct receive_call *call)
{
    for (int i = 0; i < 200
            && !__atomic_load_n(&call->started, __ATOMIC_ACQUIRE); i++) {
        sleep_ms(10);
    }

    sleep_ms(100);
}

/*!
 * @brief Wait for a receiver's thread to return, 2 s at most
 *
 * @param[in] call     the receiver's call
 * @param[in] pending  the result the call holds until the receiver returns
 * @returns the receiver's result, or pending
 */
static int receiver_result(struct receive_call *call, int pending)
{
    for (int i = 0; i < 200
            && __atomic_load_n(&call->result, __ATOMIC_ACQUIRE) == pending;
            i++) {
        sleep_ms(10);
    }

    return __atomic_load_n(&call->result, __ATOMIC_ACQUIRE);
}

/*! A connect on its own thread, and its result. */
struct connect_call {
    struct afp_server *server;
    int result;
};

/*!
 * @brief Connect the server's socket again, as a reconnect does
 *
 * @param[in,out] arg  the call, whose result receives the connect's
 * @returns NULL
 */
static void *connect_again(void *arg)
{
    struct connect_call *call = arg;
    __atomic_store_n(&call->result, afp_server_connect(call->server, 0),
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief A receive on a connection without a descriptor reads nothing
 *
 * A connect has taken the descriptor and not yet published the next
 * connection's, or a disconnect has taken it: there is no stream to read.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_receive_without_descriptor_reads_nothing(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    close(s.server->fd);
    s.server->fd = -1;

    if (afpc_dsi_recv(s.server) != 0) {
        ret = 2;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A frame dropped as a connect replaces the connection leaves the
 *        next connection's stream alone
 *
 * The receiver's wait for the old connection ends once the connect has shut
 * it down, with nothing more from the peer; its pass reads none of the next
 * connection's bytes, whose first reply, which the loop reads, is still
 * served.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_drop_across_a_reconnect_keeps_the_next_stream(void)
{
    struct session s;
    struct listener l;
    struct receive_call call;
    struct connect_call conn;
    /* The loop thread lets its reply's request go after it wakes the
     * test, which can have returned by then. */
    static struct dsi_request pending;
    pthread_t receiver;
    pthread_t connector;
    char payload[100];
    int next_peer = -1;
    int replies = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (listener_open(&l) != 0) {
        session_close(&s);
        return 2;
    }

    s.server->address = &l.ai;
    memset(payload, 'p', sizeof(payload));

    /* A reply no request waits for, larger than the buffer, being dropped. */
    if (send_header(s.peer, DSI_REPLY, DSI_DSICommand, 30, 8192) != 0
            || write(s.peer, payload, sizeof(payload))
            != (ssize_t) sizeof(payload)
            || receive_all(s.server) != 0) {
        session_close(&s);
        listener_close(&l);
        return 3;
    }

    call.server = s.server;
    call.started = 0;
    call.result = 1;
    conn.server = s.server;
    conn.result = 1;

    if (pthread_create(&receiver, NULL, receive_when_ready, &call) != 0) {
        session_close(&s);
        listener_close(&l);
        return 4;
    }

    wait_for_receiver(&call);

    if (pthread_create(&connector, NULL, connect_again, &conn) != 0) {
        shutdown(s.peer, SHUT_WR);
        pthread_join(receiver, NULL);
        session_close(&s);
        listener_close(&l);
        return 5;
    }

    pthread_join(connector, NULL);

    /* A receiver that waited before the connect wakes once the connect has
     * shut its descriptor down, and its pass finds no bytes on either
     * connection. */
    if (receiver_result(&call, 1) == 1) {
        ret = 7;
        /* A receiver still in its wait ends at the peer's close. */
        shutdown(s.peer, SHUT_WR);

        if (conn.result == 0 && (next_peer = accept_within(l.fd)) >= 0) {
            close(next_peer);
            next_peer = -1;
        }
    }

    pthread_join(receiver, NULL);

    if (ret == 0 && call.result != 0) {
        ret = 8;
    } else if (ret == 0 && conn.result != 0) {
        ret = 9;
    } else if (ret == 0 && (next_peer = accept_within(l.fd)) < 0) {
        ret = 10;
    }

    if (ret == 0) {
        queue_request(s.server, &pending, 31, afpGetVolParms, &replies,
                      record_reply);

        if (send_frame(next_peer, DSI_REPLY, DSI_DSICommand, 31, NULL, 0)
                != 0) {
            ret = 11;
        }

        for (int i = 0; ret == 0 && i < 200
                && !__atomic_load_n(&pending.done_waiting, __ATOMIC_ACQUIRE);
                i++) {
            sleep_ms(10);
        }

        if (ret == 0 && replies != 1) {
            ret = 12;
        }
    }

    pthread_mutex_lock(&s.server->request_queue_mutex);
    s.server->command_requests = NULL;
    pthread_mutex_unlock(&s.server->request_queue_mutex);
    s.server->address = NULL;
    afp_server_remove(s.server);

    if (next_peer >= 0) {
        close(next_peer);
    }

    close(s.peer);
    listener_close(&l);
    return ret;
}

/*!
 * @brief A server is valid from its set-up until its removal
 *
 * Validity does not depend on the list; a server never set up is not valid,
 * and removing one that was never listed says so and leaves it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_server_valid_until_removed(void)
{
    struct session s;
    struct afp_server unset;
    int ret = 0;
    memset(&unset, 0, sizeof(unset));

    if (server_still_valid(&unset) != 0) {
        return 1;
    }

    if (session_open(&s, 4096) != 0) {
        return 2;
    }

    afp_server_hold(s.server);

    if (server_still_valid(s.server) != 1) {
        ret = 3;
    } else if (afp_server_remove(s.server) != -ENOENT) {
        ret = 4;
    } else if (server_still_valid(s.server) != 1) {
        ret = 5;
    }

    afp_server_release(s.server);
    session_close(&s);
    return ret;
}

/*!
 * @brief Wait for the end of a session's connection at the test's end
 *
 * A shutdown does not end an AF_UNIX connection everywhere; the close the
 * loop makes between two passes does.
 *
 * @param[in] peer  the test's end
 * @returns 1 once it reads the end, 0 otherwise
 */
static int peer_ended(int peer)
{
    struct pollfd pfd = { peer, POLLIN, 0 };
    char byte;
    return poll(&pfd, 1, 2000) == 1
           && recv(peer, &byte, 1, MSG_DONTWAIT) == 0;
}

/*!
 * @brief A disconnect ends a server already marked disconnected
 *
 * The request waiting on a server marked disconnected while its descriptor is
 * still open fails, and the descriptor closes.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_ends_a_disconnected_server(void)
{
    struct session s;
    struct dsi_request waiting;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &waiting, 10, afpGetVolParms, NULL, NULL);
    s.server->connect_state = SERVER_STATE_DISCONNECTED;
    loop_disconnect(s.server);

    if (waiting.return_code != -EIO || waiting.done_waiting != 1) {
        ret = 2;
    } else if (s.server->fd != -1) {
        ret = 3;
    } else if (!peer_ended(s.peer)) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect fails a request when the descriptor is gone
 *
 * A request that queued as the connection closed waits on nothing, so the end
 * of the connection fails it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_fails_a_request_without_descriptor(void)
{
    struct session s;
    struct dsi_request waiting;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &waiting, 13, afpGetVolParms, NULL, NULL);
    close(s.server->fd);
    s.server->fd = -1;
    s.server->connect_state = SERVER_STATE_DISCONNECTED;
    loop_disconnect(s.server);

    if (waiting.return_code != -EIO || waiting.done_waiting != 1) {
        ret = 2;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect with no descriptor leaves the server disconnected
 *
 * A connect that closed the descriptor and found no address leaves the
 * connection that way.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_without_descriptor_ends(void)
{
    struct session s;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    close(s.server->fd);
    s.server->fd = -1;
    loop_disconnect(s.server);

    if (s.server->connect_state != SERVER_STATE_DISCONNECTED) {
        ret = 2;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief End the server's connection on a thread of its own, as the loop does
 *
 * @param[in,out] server  the server
 * @returns NULL
 */
static void *disconnect_server(void *server)
{
    loop_disconnect(server);
    return NULL;
}

/*!
 * @brief Open a session whose server is listed, as a connect lists it
 *
 * The test accepts the connection and plays the server on it.
 *
 * @param[out] s  receives the session
 * @param[in]  l  the listener the server connects to
 * @returns 0, or -1
 */
static int listed_session_open(struct session *s, struct listener *l)
{
    if (listener_open(l) != 0) {
        return -1;
    }

    s->server = afp_server_init(&l->ai);

    if (s->server == NULL || afp_server_connect(s->server, 0) != 0) {
        listener_close(l);
        return -1;
    }

    s->peer = accept_within(l->fd);

    if (s->peer < 0) {
        listener_close(l);
        return -1;
    }

    return 0;
}

/*!
 * @brief Remove a listed session's server, which closes its descriptor and
 *        releases it
 *
 * The requests a test leaves queued are its own, and the address is the
 * listener's, not the server's to free.
 *
 * @param[in,out] s  the session
 * @param[in]     l  its listener
 */
static void listed_session_close(struct session *s, struct listener *l)
{
    pthread_mutex_lock(&s->server->request_queue_mutex);
    s->server->command_requests = NULL;
    pthread_mutex_unlock(&s->server->request_queue_mutex);
    s->server->address = NULL;
    afp_server_remove(s->server);
    close(s->peer);
    listener_close(l);
}

/*!
 * @brief Wait for another thread to take a server's descriptor
 *
 * Polls for up to 2 s, then gives the taker 50 ms to go on.
 *
 * @param[in] server  the server
 * @returns 1 when the descriptor is taken, 0 when it is not
 */
static int descriptor_taken(struct afp_server *server)
{
    for (int i = 0; i < 200
            && __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE) != -1; i++) {
        sleep_ms(10);
    }

    sleep_ms(50);
    return __atomic_load_n(&server->fd, __ATOMIC_ACQUIRE) == -1;
}

/*!
 * @brief Tell whether a descriptor is still a session's socket pair end
 *
 * A number a close freed and a socket took again is no AF_UNIX socket.
 *
 * @param[in] fd  the descriptor
 * @returns 1 for an AF_UNIX socket, 0 otherwise
 */
static int is_socket_pair_end(int fd)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    return getsockname(fd, (struct sockaddr *) &addr, &len) == 0
           && addr.ss_family == AF_UNIX;
}

/*!
 * @brief Wait up to 2 s for the loop to close a descriptor a disconnect took
 *
 * @param[in] fd  the descriptor
 * @returns 1 once it is closed, 0 while it stays open
 */
static int descriptor_closed(int fd)
{
    for (int i = 0; i < 200 && fcntl(fd, F_GETFD) != -1; i++) {
        sleep_ms(10);
    }

    return fcntl(fd, F_GETFD) == -1;
}

/*!
 * @brief A connect closes the old descriptor only after the frame in flight
 *
 * A sender holds send_mutex while it writes a frame. A connect that takes
 * the descriptor meanwhile waits for the frame before it closes the
 * descriptor and publishes the next connection's, so no frame's tail reaches
 * a descriptor number reused since.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_connect_waits_for_the_frame_in_flight(void)
{
    struct session s;
    struct listener l;
    struct connect_call conn;
    pthread_t connector;
    int old_fd;
    int next_peer = -1;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (listener_open(&l) != 0) {
        session_close(&s);
        return 2;
    }

    s.server->address = &l.ai;
    old_fd = s.server->fd;
    conn.server = s.server;
    conn.result = 1;
    /* The sender of a frame still being written. */
    pthread_mutex_lock(&s.server->send_mutex);

    if (pthread_create(&connector, NULL, connect_again, &conn) != 0) {
        pthread_mutex_unlock(&s.server->send_mutex);
        session_close(&s);
        listener_close(&l);
        return 3;
    }

    /* The connect takes the descriptor and waits, the old socket open. */
    if (!descriptor_taken(s.server)) {
        ret = 4;
    } else if (!is_socket_pair_end(old_fd)) {
        ret = 5;
    }

    pthread_mutex_unlock(&s.server->send_mutex);
    pthread_join(connector, NULL);

    if (ret == 0 && conn.result != 0) {
        ret = 6;
    } else if (ret == 0 && (next_peer = accept_within(l.fd)) < 0) {
        ret = 7;
    }

    s.server->address = NULL;
    afp_server_remove(s.server);

    if (next_peer >= 0) {
        close(next_peer);
    }

    close(s.peer);
    listener_close(&l);
    return ret;
}

/*!
 * @brief A disconnect lets the descriptor close only after the frame in flight
 *
 * A sender holds send_mutex while it writes a frame. A disconnect that takes
 * the descriptor meanwhile waits for the frame before the loop closes the
 * descriptor, so no frame's tail reaches a descriptor number reused since.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_waits_for_the_frame_in_flight(void)
{
    struct session s;
    pthread_t thread;
    int old_fd;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    old_fd = s.server->fd;
    /* The sender of a frame still being written. */
    pthread_mutex_lock(&s.server->send_mutex);

    if (pthread_create(&thread, NULL, disconnect_server, s.server) != 0) {
        pthread_mutex_unlock(&s.server->send_mutex);
        session_close(&s);
        return 2;
    }

    /* The disconnect takes the descriptor and waits, the old socket open. */
    if (!descriptor_taken(s.server)) {
        ret = 3;
    } else if (!is_socket_pair_end(old_fd)) {
        ret = 4;
    }

    pthread_mutex_unlock(&s.server->send_mutex);
    pthread_join(thread, NULL);

    if (ret == 0 && !descriptor_closed(old_fd)) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect shuts the write side, after the peer's close too
 *
 * The test keeps a second descriptor of the connection's socket, which the
 * close of the first leaves open. macOS refuses SHUT_RDWR once the peer's
 * close has arrived and leaves the write side open, where a sender waiting
 * for room would not wake.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_shuts_the_write_side(void)
{
    struct session s;
    struct listener l;
    struct pollfd pfd;
    int kept = -1;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (listener_open(&l) != 0) {
        session_close(&s);
        return 2;
    }

    close(s.server->fd);
    close(s.peer);
    s.peer = -1;
    s.server->fd = socket(AF_INET, SOCK_STREAM, 0);

    if (s.server->fd < 0
            || connect_within(s.server->fd, &l.addr) != 0
            || (s.peer = accept_within(l.fd)) < 0) {
        ret = 3;
    } else {
        close(s.peer);
        s.peer = -1;
        /* The peer's close arrives as the end of the stream. */
        pfd.fd = s.server->fd;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 2000) != 1 || (kept = dup(s.server->fd)) < 0) {
            ret = 4;
        } else {
            loop_disconnect(s.server);

            if (send(kept, "x", 1, 0) != -1 || errno != EPIPE) {
                ret = 5;
            }
        }
    }

    if (kept >= 0) {
        close(kept);
    }

    listener_close(&l);
    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect keeps errno
 *
 * A caller that sets errno before a removal, as the signature probe does,
 * reads it after.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_keeps_errno(void)
{
    struct session s;
    int pipe_fds[2];
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (pipe(pipe_fds) != 0) {
        session_close(&s);
        return 2;
    }

    /* A pipe is no socket, so a shutdown of it fails. */
    close(s.server->fd);
    s.server->fd = pipe_fds[0];
    errno = EPROTO;
    loop_disconnect(s.server);

    if (errno != EPROTO) {
        ret = 3;
    }

    close(pipe_fds[1]);
    session_close(&s);
    return ret;
}

/*!
 * @brief The next connection outlives an overtaken disconnect
 *
 * The disconnect of the connection before it waits for the request queue
 * meanwhile; the next connection keeps its descriptor, its state and what it
 * has read.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_next_connection_outlives_the_disconnect(void)
{
    struct session s;
    struct listener l;
    pthread_t thread;
    int next[2];
    char byte;
    int ret = 0;

    if (listed_session_open(&s, &l) != 0) {
        return 1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, next) != 0) {
        listed_session_close(&s, &l);
        return 2;
    }

    pthread_mutex_lock(&s.server->request_queue_mutex);

    if (pthread_create(&thread, NULL, disconnect_server, s.server) != 0) {
        pthread_mutex_unlock(&s.server->request_queue_mutex);
        ret = 3;
        goto out;
    }

    if (!descriptor_taken(s.server)) {
        ret = 4;
    }

    /* The next connection, as afp_server_connect() makes it, with the
     * first bytes of a reply read. */
    afp_server_next_connection_generation(s.server);
    afp_lock_server_list();
    s.server->fd = next[0];
    s.server->connect_state = SERVER_STATE_CONNECTING;
    s.server->data_read = 5;
    afp_unlock_server_list();
    pthread_mutex_unlock(&s.server->request_queue_mutex);

    if (pthread_join(thread, NULL) != 0 && ret == 0) {
        ret = 5;
    }

    if (ret != 0) {
        goto out;
    }

    if (s.server->connect_state != SERVER_STATE_CONNECTING) {
        ret = 6;
    } else if (s.server->fd != next[0]) {
        ret = 7;
    } else if (s.server->data_read != 5) {
        ret = 8;
    } else if (recv(next[1], &byte, 1, MSG_DONTWAIT) != -1) {
        ret = 9;
    }

out:
    listed_session_close(&s, &l);
    close(next[1]);
    return ret;
}

/* A sender's thread: one request with a wait, its result and its replies. */
struct sender {
    struct afp_server *server;
    int wait;
    int result;
    int replies;
};

/*!
 * @brief Send one request and wait for its reply, as a sender's thread does
 *
 * @param[in,out] arg  the sender, whose result receives the send's and
 *                     whose replies counts the replies the request took
 * @returns NULL
 */
static void *send_and_wait(void *arg)
{
    struct sender *s = arg;
    struct dsi_header header;
    int result;
    s->replies = 0;
    afpc_dsi_setup_header(s->server, &header, DSI_DSICommand);
    result = afpc_dsi_send_with_reply(s->server, (char *) &header,
                                      sizeof(header), s->wait, afpGetVolParms,
                                      &s->replies, record_reply, NULL);
    /* Stored for a test that polls it while the thread runs. */
    __atomic_store_n(&s->result, result, __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Wait until the request at the queue's head has a deadline
 *
 * A deadline marks a sender in its wait.
 *
 * @param[in] server  server whose queue to watch
 * @returns the queued request, or NULL
 */
static const struct dsi_request *wait_for_deadline(struct afp_server *server)
{
    for (int i = 0; i < 200; i++) {
        const struct dsi_request *queued = server->command_requests;

        if (queued && queued->deadline.tv_sec != 0) {
            return queued;
        }

        sleep_ms(10);
    }

    return NULL;
}

/*!
 * @brief A request queued before a reconnect replaced its connection fails
 *        unsent
 *
 * The receiver would drop its reply, which comes on the next connection, and
 * the sender would wait out its time.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_request_of_a_replaced_connection_fails_unsent(void)
{
    struct session s;
    struct sender sender;
    pthread_t thread;
    char byte;
    int queued = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = 1;
    sender.result = 1;
    /* Another sender's frame is being written. */
    pthread_mutex_lock(&s.server->send_mutex);

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        pthread_mutex_unlock(&s.server->send_mutex);
        session_close(&s);
        return 2;
    }

    for (int i = 0; i < 200 && !queued; i++) {
        pthread_mutex_lock(&s.server->request_queue_mutex);
        queued = s.server->command_requests != NULL;
        pthread_mutex_unlock(&s.server->request_queue_mutex);

        if (!queued) {
            sleep_ms(10);
        }
    }

    /* A reconnect has made the next connection meanwhile. */
    afp_server_next_connection_generation(s.server);
    pthread_mutex_unlock(&s.server->send_mutex);
    pthread_join(thread, NULL);

    if (!queued) {
        ret = 3;
    } else if (sender.result != -EIO) {
        ret = 4;
    } else if (recv(s.peer, &byte, 1, MSG_DONTWAIT) != -1) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A removal fails a waiting request with EIO and wakes it
 *
 * The request leaves the queue with its sender once the sender wakes.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_removal_fails_a_waiting_request(void)
{
    struct session s;
    struct listener l;
    struct sender sender;
    struct timespec removed;
    struct timespec woken;
    pthread_t thread;
    int ret = 0;

    if (listed_session_open(&s, &l) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = 5;
    sender.result = 1;

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        listed_session_close(&s, &l);
        return 2;
    }

    if (wait_for_deadline(s.server) == NULL) {
        ret = 3;
    }

    afp_server_hold(s.server);
    clock_gettime(CLOCK_MONOTONIC, &removed);

    if (afp_server_remove(s.server) != 0 && ret == 0) {
        ret = 4;
    }

    if (pthread_join(thread, NULL) != 0 && ret == 0) {
        ret = 5;
    }

    clock_gettime(CLOCK_MONOTONIC, &woken);

    if (ret != 0) {
        goto out;
    }

    /* The removal wakes the sender, well before its 5 s wait ends. */
    if (woken.tv_sec - removed.tv_sec >= 2) {
        ret = 6;
    } else if (sender.result != -EIO) {
        ret = 7;
    } else if (s.server->command_requests != NULL
               || s.server->stats.requests_pending != 0) {
        ret = 8;
    }

out:
    s.server->address = NULL;
    afp_server_release(s.server);
    close(s.peer);
    listener_close(&l);
    return ret;
}

/*!
 * @brief A request sent without a wait is never queued
 *
 * Its frame reaches the server while the test holds the queue, which stays
 * empty.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_no_wait_request_is_never_queued(void)
{
    struct session s;
    struct listener l;
    struct sender sender;
    struct pollfd pfd;
    pthread_t thread;
    char frame[sizeof(struct dsi_header)];
    int ret = 0;

    if (listed_session_open(&s, &l) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = DSI_DONT_WAIT;
    sender.result = 1;
    pthread_mutex_lock(&s.server->request_queue_mutex);

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        pthread_mutex_unlock(&s.server->request_queue_mutex);
        listed_session_close(&s, &l);
        return 2;
    }

    pfd.fd = s.peer;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, 2000) != 1) {
        ret = 3;
    }

    /* The send returns with the queue still held: it never took it. */
    for (int i = 0; i < 200
            && __atomic_load_n(&sender.result, __ATOMIC_ACQUIRE) == 1;
            i++) {
        sleep_ms(10);
    }

    if (__atomic_load_n(&sender.result, __ATOMIC_ACQUIRE) != 0 && ret == 0) {
        ret = 4;
    }

    pthread_mutex_unlock(&s.server->request_queue_mutex);

    if (pthread_join(thread, NULL) != 0 && ret == 0) {
        ret = 5;
    }

    if (ret != 0) {
        goto out;
    }

    if (s.server->command_requests != NULL
            || s.server->stats.requests_pending != 0) {
        ret = 6;
    } else if (read(s.peer, frame, sizeof(frame))
               != (ssize_t) sizeof(frame)) {
        ret = 7;
    }

out:
    listed_session_close(&s, &l);
    return ret;
}

/*!
 * @brief A write to a connection the server has closed fails
 *
 * It raises no SIGPIPE, in a process that keeps the signal's default too,
 * and leaves the connection to the loop, which ends it when it reads the
 * close.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_write_to_closed_connection_fails(void)
{
    struct session s;
    struct dsi_header header;
    int rc;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    close(s.peer);
    s.peer = -1;
    afpc_dsi_setup_header(s.server, &header, DSI_DSITickle);
    signal(SIGPIPE, SIG_DFL);
    rc = afpc_dsi_send(s.server, (char *) &header, sizeof(header),
                       DSI_DONT_WAIT, DSI_DSITickle, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (rc != -EIO) {
        ret = 2;
    } else if (s.server->connect_state != SERVER_STATE_CONNECTED) {
        ret = 3;
    } else if (receive_all(s.server) != -1) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A second reply to an answered request is dropped
 *
 * Its handler runs once.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_second_reply_is_dropped(void)
{
    struct session s;
    struct dsi_request pending;
    int replies = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &pending, 14, afpGetVolParms, &replies,
                  record_reply);

    if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 14, NULL, 0) != 0) {
        ret = 2;
    } else if (receive_all(s.server) != 0) {
        ret = 3;
    } else if (replies != 1 || pending.done_waiting != 1) {
        ret = 4;
    } else if (send_frame(s.peer, DSI_REPLY, DSI_DSICommand, 14, NULL, 0)
               != 0) {
        ret = 5;
    } else if (receive_all(s.server) != 0) {
        ret = 6;
    } else if (replies != 1 || s.server->stats.runt_packets != 1) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Accept one connection and close it unanswered
 *
 * @param[in] listener  the listener
 * @returns NULL
 */
static void *accept_and_close(void *listener)
{
    int fd = accept_within(*(int *) listener);

    if (fd >= 0) {
        close(fd);
    }

    return NULL;
}

/*!
 * @brief Open a listener whose server closes its one connection unanswered,
 *        and a server to connect to it
 *
 * A connect that fails before it connects leaves the acceptor waiting, so a
 * test joins it only after a connect that reached it.
 *
 * @param[out] l         receives the listener
 * @param[out] acceptor  receives the accepting thread
 * @param[out] server    receives the server
 * @returns 0, or -1
 */
static int cut_off_open(struct listener *l, pthread_t *acceptor,
                        struct afp_server **server)
{
    if (listener_open(l) != 0) {
        return -1;
    }

    *server = afp_server_init(&l->ai);

    if (*server == NULL) {
        listener_close(l);
        return -1;
    }

    if (pthread_create(acceptor, NULL, accept_and_close, &l->fd) != 0) {
        (*server)->address = NULL;
        afp_server_release(*server);
        listener_close(l);
        return -1;
    }

    return 0;
}

/*!
 * @brief A connect whose status read is cut off returns -EIO
 *
 * The connect fails with the read's errno before it lists the server.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_cut_off_status_read_fails_the_connect(void)
{
    struct listener l;
    struct afp_server *server;
    pthread_t acceptor;
    int ret = 0;

    if (cut_off_open(&l, &acceptor, &server) != 0) {
        return 1;
    }

    if (afp_server_connect(server, 1) != -EIO || errno != EIO) {
        return 2;
    }

    if (pthread_join(acceptor, NULL) != 0) {
        return 3;
    }

    if (find_server_by_pointer(server) != NULL) {
        ret = 4;
    }

    /* The address is the test's, not the server's to free. */
    server->address = NULL;
    afp_server_release(server);
    listener_close(&l);
    return ret;
}

/*!
 * @brief A connect of a removed server is refused and lists nothing
 *
 * A connect without the status lists the server, and a reference holds it
 * across its removal; the next connect leaves it removed.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_connect_of_a_removed_server_is_refused(void)
{
    struct listener l;
    struct afp_server *server;
    pthread_t acceptor;
    int peer;
    int ret = 0;

    if (cut_off_open(&l, &acceptor, &server) != 0) {
        return 1;
    }

    if (afp_server_connect(server, 1) != -EIO) {
        return 2;
    }

    if (pthread_join(acceptor, NULL) != 0) {
        return 3;
    }

    if (afp_server_connect(server, 0) != 0) {
        return 4;
    }

    peer = accept_within(l.fd);
    afp_server_hold(server);

    if (peer < 0) {
        ret = 5;
    } else if (afp_server_remove(server) != 0) {
        ret = 6;
    } else if (afp_server_connect(server, 0) != -ENOENT) {
        ret = 7;
    } else if (server_still_valid(server) != 0) {
        ret = 8;
    } else if (find_server_by_pointer(server) != NULL) {
        ret = 9;
    }

    server->address = NULL;
    afp_server_release(server);
    listener_close(&l);

    if (peer >= 0) {
        close(peer);
    }

    return ret;
}

/*!
 * @brief A request without a wait to a disconnected server fails without
 *        a reconnect, whose flush would fail the requests still waiting
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_no_wait_request_never_reconnects(void)
{
    struct session s;
    struct dsi_request waiting;
    struct dsi_header header;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &waiting, 21, afpGetVolParms, NULL, NULL);
    s.server->connect_state = SERVER_STATE_DISCONNECTED;
    afpc_dsi_setup_header(s.server, &header, DSI_DSITickle);

    if (afpc_dsi_send(s.server, (char *) &header, sizeof(header),
                      DSI_DONT_WAIT, DSI_DSITickle, NULL) != -EIO) {
        ret = 2;
    } else if (waiting.done_waiting != 0) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect keeps an answered request's result
 *
 * The requests still waiting fail.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_keeps_an_answered_result(void)
{
    struct session s;
    struct dsi_request answered;
    struct dsi_request waiting;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &answered, 8, afpGetVolParms, NULL, NULL);
    queue_request(s.server, &waiting, 9, afpGetVolParms, NULL, NULL);
    answered.done_waiting = 1;
    answered.return_code = 42;
    afpc_dsi_fail_request_queue(s.server, -EIO,
                                afp_server_connection_generation(s.server));

    if (answered.return_code != 42) {
        ret = 2;
        goto out;
    }

    if (waiting.return_code != -EIO || waiting.done_waiting != 1) {
        ret = 3;
        goto out;
    }

    waiting.done_waiting = 0;
    waiting.return_code = 0;
    afpc_dsi_flush_request_queue(s.server);

    if (answered.return_code != 42) {
        ret = 4;
    } else if (waiting.return_code != -EIO || waiting.done_waiting != 1) {
        ret = 5;
    } else if (s.server->command_requests != NULL) {
        ret = 6;
    }

out:
    session_close(&s);
    return ret;
}

/*!
 * @brief A disconnect of a replaced connection spares the next one's request
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_disconnect_leaves_the_next_connection_waiting(void)
{
    struct session s;
    struct dsi_request next;
    unsigned int ended;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    ended = afp_server_connection_generation(s.server);
    afp_server_next_connection_generation(s.server);
    queue_request(s.server, &next, 8, afpGetVolParms, NULL, NULL);
    afpc_dsi_fail_request_queue(s.server, -EIO, ended);

    if (next.done_waiting != 0) {
        ret = 2;
    }

    afpc_dsi_flush_request_queue(s.server);

    if (ret == 0 && (next.return_code != -EIO || next.done_waiting != 1)) {
        ret = 3;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A busy refusal of a session's open leaves the quantum alone
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_busy_open_keeps_the_quantum(void)
{
    struct session s;
    struct dsi_request pending;
    struct dsi_header header;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    queue_request(s.server, &pending, 9, DSI_DSIOpenSession, NULL, NULL);
    memset(&header, 0, sizeof(header));
    header.flags = DSI_REPLY;
    header.command = DSI_DSIOpenSession;
    header.requestid = htons(9);
    header.return_code.error_code = (int) htonl(DSIERR_SERVBUSY);

    if (write(s.peer, &header, sizeof(header)) != (ssize_t) sizeof(header)) {
        ret = 2;
    } else if (receive_all(s.server) != 0) {
        ret = 3;
    } else if (s.server->quantum != 65536) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief An answer landing as the wait expires is the result
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_answer_as_the_wait_expires_is_the_result(void)
{
    struct session s;
    struct sender sender;
    struct dsi_request *request;
    pthread_t thread;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = 1;
    sender.result = 1;

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        session_close(&s);
        return 2;
    }

    wait_for_deadline(s.server);
    /* The sender is in its wait; the answer takes the mutex, so the
     * sender's wait expires before it can read the answer. The removal that
     * ends the send waits for the queue's mutex, which the answer holds. */
    sleep_ms(200);
    pthread_mutex_lock(&s.server->request_queue_mutex);
    request = s.server->command_requests;

    if (request == NULL) {
        ret = 3;
    } else {
        pthread_mutex_lock(&request->waiting_mutex);
        request->return_code = 42;
        request->done_waiting = 1;
        sleep_ms(1100);
        pthread_mutex_unlock(&request->waiting_mutex);
    }

    pthread_mutex_unlock(&s.server->request_queue_mutex);

    if (pthread_join(thread, NULL) != 0 && ret == 0) {
        ret = 4;
    }

    if (ret == 0 && sender.result != 42) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*! A reply whose handler holds its request until the case lets it go. */
struct held_reply {
    int running; /*!< 1 while the handler holds the request */
    int release; /*!< set by the case to let the handler go */
};

/*!
 * @brief Handle a reply until the case lets the handler go, 10 s at most
 *
 * @param[in]     server  unused
 * @param[in]     buf     unused
 * @param[in]     size    unused
 * @param[in,out] other   the held reply
 * @returns 0
 */
static int handle_until_released(struct afp_server *server, char *buf,
                                 unsigned int size, void *other)
{
    struct held_reply *reply = other;
    (void) server;
    (void) buf;
    (void) size;
    __atomic_store_n(&reply->running, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < 1000
            && !__atomic_load_n(&reply->release, __ATOMIC_ACQUIRE); i++) {
        sleep_ms(10);
    }

    __atomic_store_n(&reply->running, 0, __ATOMIC_RELEASE);
    return 0;
}

/*! A sender whose reply's handler the case holds, and the send's result. */
struct held_sender {
    struct afp_server *server;
    struct held_reply *reply;
    int result;
};

/*!
 * @brief Send a request whose reply's handler the case holds
 *
 * @param[in,out] arg  the held sender, whose result receives the send's
 * @returns NULL
 */
static void *send_and_be_held(void *arg)
{
    struct held_sender *s = arg;
    struct dsi_header header;
    afpc_dsi_setup_header(s->server, &header, DSI_DSICommand);
    __atomic_store_n(&s->result,
                     afpc_dsi_send_with_reply(s->server, (char *) &header,
                                              sizeof(header), 5,
                                              afpGetVolParms, s->reply,
                                              handle_until_released, NULL),
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief A request a disconnect fails while the receiver handles its reply
 *        stays its sender's until the receiver lets it go
 *
 * The receiver holds the request while the reply's handler runs; the
 * sender, woken by the failure, returns only once the handler is done.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_failed_request_waits_for_its_receiver(void)
{
    struct session s;
    struct held_reply reply = { 0, 0 };
    struct held_sender held;
    struct receive_call call;
    struct dsi_header sent;
    pthread_t sender;
    pthread_t receiver;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    held.server = s.server;
    held.reply = &reply;
    held.result = 1;

    if (pthread_create(&sender, NULL, send_and_be_held, &held) != 0) {
        session_close(&s);
        return 2;
    }

    if (read(s.peer, &sent, sizeof(sent)) != (ssize_t) sizeof(sent)
            || send_header(s.peer, DSI_REPLY, DSI_DSICommand,
                           ntohs(sent.requestid), 0) != 0) {
        ret = 3;
        goto out;
    }

    call.server = s.server;
    call.started = 0;
    call.result = 1;

    if (pthread_create(&receiver, NULL, receive_when_ready, &call) != 0) {
        ret = 4;
        goto out;
    }

    /* The receiver holds the request in the reply's handler. */
    for (int i = 0; i < 200
            && !__atomic_load_n(&reply.running, __ATOMIC_ACQUIRE); i++) {
        sleep_ms(10);
    }

    if (!__atomic_load_n(&reply.running, __ATOMIC_ACQUIRE)) {
        ret = 5;
    }

    afpc_dsi_fail_request_queue(s.server, -EIO,
                                afp_server_connection_generation(s.server));
    sleep_ms(200);

    if (ret == 0 && __atomic_load_n(&held.result, __ATOMIC_ACQUIRE) != 1) {
        ret = 6;
    }

    __atomic_store_n(&reply.release, 1, __ATOMIC_RELEASE);
    pthread_join(receiver, NULL);
out:

    if (ret == 3) {
        afpc_dsi_fail_request_queue(s.server, -EIO,
                                    afp_server_connection_generation(s.server));
    }

    pthread_join(sender, NULL);

    if (ret == 0 && held.result != -EIO) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Limit a socket's reads to 10 s, so a missing frame fails the case
 *
 * @param[in] fd  the socket
 * @returns 0, or -1
 */
static int limit_reads(int fd)
{
    struct timeval limit = { 10, 0 };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
}

/*! A slow handler's state: whether it runs, and whether it has run. */
struct slow_reply {
    int running;
    int handled;
};

/*!
 * @brief Handle a reply for longer than its sender's wait of 2 s
 *
 * @param[in]     server  unused
 * @param[in]     buf     unused
 * @param[in]     size    unused
 * @param[in,out] other   the slow reply
 * @returns 0
 */
static int handle_slowly(struct afp_server *server, char *buf,
                         unsigned int size, void *other)
{
    struct slow_reply *reply = other;
    (void) server;
    (void) buf;
    (void) size;
    __atomic_store_n(&reply->running, 1, __ATOMIC_RELEASE);
    sleep_ms(3000);
    reply->handled = 1;
    __atomic_store_n(&reply->running, 0, __ATOMIC_RELEASE);
    return 0;
}

/*! A sender whose reply the receiver handles slowly, and what it saw. */
struct slow_sender {
    struct afp_server *server;
    struct slow_reply *reply;
    int result;
    int running_at_return;
};

/*!
 * @brief Send a request that waits 2 s, whose handler outlasts it
 *
 * @param[in,out] arg  the slow sender, which receives the send's result and
 *                     whether the handler still ran when it returned
 * @returns NULL
 */
static void *send_slowly_answered(void *arg)
{
    struct slow_sender *s = arg;
    struct dsi_header header;
    afpc_dsi_setup_header(s->server, &header, DSI_DSICommand);
    s->result = afpc_dsi_send_with_reply(s->server, (char *) &header,
                                         sizeof(header), 2, afpGetVolParms,
                                         s->reply, handle_slowly, NULL);
    s->running_at_return = __atomic_load_n(&s->reply->running,
                                           __ATOMIC_ACQUIRE);
    return NULL;
}

/*!
 * @brief A reply whose handler outlasts its sender's wait is the send's result
 *
 * The receiver holds the request while the handler runs, so the send
 * returns once the handler is done, with the reply's result.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_slow_handler_is_the_result(void)
{
    struct session s;
    struct slow_reply reply = { 0, 0 };
    struct slow_sender sender;
    struct receive_call call;
    struct dsi_header sent;
    pthread_t sending;
    pthread_t receiving;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.reply = &reply;
    sender.result = 1;
    sender.running_at_return = 1;

    if (limit_reads(s.peer) != 0
            || pthread_create(&sending, NULL, send_slowly_answered,
                              &sender) != 0) {
        session_close(&s);
        return 2;
    }

    call.server = s.server;
    call.started = 0;
    call.result = 1;

    if (recv(s.peer, &sent, sizeof(sent), MSG_WAITALL)
            != (ssize_t) sizeof(sent)) {
        ret = 3;
    } else if (pthread_create(&receiving, NULL, receive_when_ready,
                              &call) != 0) {
        ret = 4;
    } else {
        /* The receiver waits for the socket when the reply comes, so it
         * finds the request within the sender's wait. */
        wait_for_receiver(&call);

        if (send_header(s.peer, DSI_REPLY, DSI_DSICommand,
                        ntohs(sent.requestid), 0) != 0) {
            shutdown(s.peer, SHUT_RDWR);
            ret = 3;
        }

        pthread_join(receiving, NULL);
    }

    pthread_join(sending, NULL);

    if (ret == 0 && call.result != 0) {
        ret = 5;
    } else if (ret == 0 && sender.running_at_return != 0) {
        ret = 6;
    } else if (ret == 0 && (sender.result != 0 || reply.handled != 1)) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A request without an answer fails and leaves the queue
 *
 * It fails with ETIMEDOUT once its wait runs out, and with -EIO when its
 * frame cannot be written.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unanswered_request_fails(void)
{
    struct session s;
    struct sender sender;
    struct dsi_header sent;
    pthread_t thread;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = 1;
    sender.result = 0;

    if (limit_reads(s.peer) != 0
            || pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        session_close(&s);
        return 2;
    }

    if (recv(s.peer, &sent, sizeof(sent), MSG_WAITALL)
            != (ssize_t) sizeof(sent)) {
        ret = 3;
    }

    pthread_join(thread, NULL);

    if (ret == 0 && (sender.result != ETIMEDOUT
                     || s.server->command_requests != NULL
                     || s.server->stats.requests_pending != 0)) {
        ret = 4;
    }

    /* The peer's end goes, so the next frame cannot be written. */
    close(s.peer);
    s.peer = -1;
    sender.result = 0;

    if (ret == 0
            && pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        ret = 5;
    } else if (ret == 0) {
        pthread_join(thread, NULL);

        if (sender.result != -EIO || s.server->command_requests != NULL
                || s.server->stats.requests_pending != 0) {
            ret = 6;
        }
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A request with no wait limit takes a reply past the server's default
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unlimited_wait_is_answered(void)
{
    struct session s;
    struct sender sender;
    struct dsi_header sent;
    pthread_t thread;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    s.server->dsi_default_timeout = 1;
    sender.server = s.server;
    sender.wait = DSI_BLOCK_TIMEOUT;
    sender.result = 1;

    if (limit_reads(s.peer) != 0
            || pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        session_close(&s);
        return 2;
    }

    if (recv(s.peer, &sent, sizeof(sent), MSG_WAITALL)
            != (ssize_t) sizeof(sent)) {
        ret = 3;
    } else {
        sleep_ms(2000);

        if (send_header(s.peer, DSI_REPLY, DSI_DSICommand,
                        ntohs(sent.requestid), 0) != 0
                || receive_all(s.server) != 0) {
            ret = 4;
        }
    }

    /* The disconnect fails a request the reply did not answer, queued or still
     * to come, so the join ends; an answered request keeps its result. */
    loop_disconnect(s.server);
    pthread_join(thread, NULL);

    if (ret == 0 && (sender.result != 0 || sender.replies != 1)) {
        ret = 5;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A request answered while it waits for the send's lock fails unsent
 *
 * A reconnect to a server without a replay cache restarts the request IDs, so
 * the answer can be a later request's.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unsent_request_takes_no_answer(void)
{
    struct session s;
    struct sender sender;
    pthread_t thread;
    unsigned short id = 0;
    char byte;
    int queued = 0;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    sender.server = s.server;
    sender.wait = 1;
    sender.result = 1;
    /* Another sender's frame is being written. */
    pthread_mutex_lock(&s.server->send_mutex);

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        pthread_mutex_unlock(&s.server->send_mutex);
        session_close(&s);
        return 2;
    }

    for (int i = 0; i < 200 && !queued; i++) {
        pthread_mutex_lock(&s.server->request_queue_mutex);
        queued = s.server->command_requests != NULL;

        if (queued) {
            id = s.server->command_requests->requestid;
        }

        pthread_mutex_unlock(&s.server->request_queue_mutex);

        if (!queued) {
            sleep_ms(10);
        }
    }

    if (!queued) {
        ret = 3;
    } else if (send_header(s.peer, DSI_REPLY, DSI_DSICommand, id, 0) != 0
               || receive_all(s.server) != 0) {
        ret = 4;
    }

    pthread_mutex_unlock(&s.server->send_mutex);
    pthread_join(thread, NULL);

    if (ret == 0 && sender.result != -EIO) {
        ret = 5;
    } else if (ret == 0 && recv(s.peer, &byte, 1, MSG_DONTWAIT) != -1) {
        ret = 6;
    }

    session_close(&s);
    return ret;
}

/*! A tickle sent on its own thread, and whether it has returned. */
struct tickler {
    struct afp_server *server;
    int done;
};

/*!
 * @brief Tickle the server, as the loop thread does when it idles
 *
 * @param[in,out] arg  the tickler, marked done when the tickle returns
 * @returns NULL
 */
static void *tickle(void *arg)
{
    struct tickler *t = arg;
    afpc_dsi_sendtickle(t->server);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Start a tickle and wait a second for it to return
 *
 * @param[in,out] t       the tickler
 * @param[out]    thread  receives the tickle's thread
 * @returns 0 when the tickle returned, 1 when it still waits, -1 when the
 *          thread cannot start
 */
static int tickle_returns(struct tickler *t, pthread_t *thread)
{
    t->done = 0;

    if (pthread_create(thread, NULL, tickle, t) != 0) {
        return -1;
    }

    for (int i = 0; i < 100 && !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
            i++) {
        sleep_ms(10);
    }

    return __atomic_load_n(&t->done, __ATOMIC_ACQUIRE) ? 0 : 1;
}

/*!
 * @brief A tickle waits for no writer: a frame on its way skips it
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_tickle_waits_for_no_writer(void)
{
    struct session s;
    struct tickler t;
    pthread_t thread;
    char byte;
    int rc;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    t.server = s.server;
    pthread_mutex_lock(&s.server->send_mutex);
    rc = tickle_returns(&t, &thread);
    pthread_mutex_unlock(&s.server->send_mutex);

    if (rc < 0) {
        ret = 2;
    } else {
        pthread_join(thread, NULL);
        ret = rc ? 3 : 0;
    }

    if (ret == 0 && recv(s.peer, &byte, 1, MSG_DONTWAIT) != -1) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Connect a TCP pair over the loopback, with small buffers
 *
 * A few writes fill such a connection, and TCP reports room for a write
 * only when at least its low-water mark is free, so a full connection
 * takes nothing until the peer reads; a socket pair's rules for that differ
 * between systems.
 *
 * @param[in]  l       the listener, whose receive buffer the accepted
 *                     socket inherits
 * @param[out] client  receives the connected socket
 * @param[out] peer    receives the accepted socket
 * @returns 0, or -1
 */
static int small_tcp_pair(struct listener *l, int *client, int *peer)
{
    int size = 4096;
    *client = socket(AF_INET, SOCK_STREAM, 0);

    if (*client < 0) {
        return -1;
    }

    if (setsockopt(*client, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0
            || setsockopt(l->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size))
            != 0
            || connect_within(*client, &l->addr) != 0) {
        close(*client);
        return -1;
    }

    *peer = accept_within(l->fd);

    if (*peer < 0) {
        close(*client);
        return -1;
    }

    return 0;
}

/*!
 * @brief Fill a connection until it takes no more
 *
 * Once a write would block, the kernel still moves bytes from the socket's
 * buffer to the peer's, which frees room, so each pass writes until a write
 * of one byte would block, and the fill pauses and passes again until a pass
 * takes nothing.
 *
 * @param[in] fd  the socket, in blocking mode on return
 * @returns 0, or -1 when a write fails otherwise or the connection takes more
 *          than a megabyte
 */
static int fill_connection(int fd)
{
    char fill[1024];
    size_t sent = 0;
    size_t before;
    ssize_t n = 0;
    int full;
    int flags = fcntl(fd, F_GETFL);
    memset(fill, 0, sizeof(fill));
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    do {
        before = sent;

        for (size_t len = sizeof(fill); len > 0; len /= 2) {
            while (sent < 1024 * 1024 && (n = send(fd, fill, len, 0)) > 0) {
                sent += (size_t) n;
            }
        }

        full = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        sleep_ms(50);
    } while (full && sent != before);

    fcntl(fd, F_SETFL, flags);
    return full ? 0 : -1;
}

/*!
 * @brief Drain a connection's peer of what the fill wrote
 *
 * @param[in] fd  the peer's socket
 */
static void drain_peer(int fd)
{
    char fill[4096];
    int flags = fcntl(fd, F_GETFL);
    ssize_t n;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    do {
        n = recv(fd, fill, sizeof(fill), 0);
    } while (n > 0);

    fcntl(fd, F_SETFL, flags);
}

/*!
 * @brief Open a session on a loopback TCP connection with small buffers
 *
 * @param[out] s  receives the session
 * @returns 0, or -1
 */
static int tcp_session_open(struct session *s)
{
    struct listener l;
    int client;
    int peer;
    int rc;

    if (session_open(s, 4096) != 0) {
        return -1;
    }

    if (listener_open(&l) != 0) {
        session_close(s);
        return -1;
    }

    rc = small_tcp_pair(&l, &client, &peer);
    listener_close(&l);

    if (rc != 0) {
        session_close(s);
        return -1;
    }

    close(s->server->fd);
    close(s->peer);
    s->server->fd = client;
    s->peer = peer;
    return 0;
}

/*!
 * @brief A tickle waits for no room: a connection that takes no more skips it
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_tickle_waits_for_no_room(void)
{
    struct session s;
    struct tickler t;
    pthread_t thread;
    int rc;
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    if (fill_connection(s.server->fd) != 0) {
        session_close(&s);
        return 2;
    }

    t.server = s.server;
    rc = tickle_returns(&t, &thread);
    drain_peer(s.peer);

    if (rc < 0) {
        ret = 3;
    } else {
        pthread_join(thread, NULL);
        ret = rc ? 4 : 0;
    }

    session_close(&s);
    return ret;
}

/*! What a restart's hook found: whether it ran, whether a rival's reconnect
 *  could start, and whether the password was there for that rival. */
static int restart_hook_ran;
static int restart_rival_started;
static int restart_password_seen;

/*!
 * @brief Start a rival reconnect inside a restart, as the attention thread
 *        may
 *
 * @param[in,out] server  the server the restart reconnects
 */
static void start_rival_reconnect(struct afp_server *server)
{
    restart_hook_ran = 1;
    restart_password_seen = server->password[0] != '\0';
    restart_rival_started = afp_server_reconnect_try_begin(server);

    if (restart_rival_started) {
        afp_server_reconnect_end(server);
    }
}

/*!
 * @brief A restart keeps its reconnect from the password's write to its wipe
 *
 * The hook runs once the password is written, where a rival's reconnect
 * would log in with it. The restart's reconnect then fails, as the server
 * has no signature.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_restart_keeps_its_reconnect(void)
{
    struct addrinfo hints;
    struct addrinfo *address = NULL;
    struct session s;
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    int ret = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (getaddrinfo("127.0.0.1", "548", &hints, &address) != 0) {
        session_close(&s);
        return 2;
    }

    s.server->connect_state = SERVER_STATE_DISCONNECTED;
    restart_hook_ran = 0;
    restart_rival_started = 0;
    restart_password_seen = 0;
    memset(mesg, 0, sizeof(mesg));
    afp_server_set_restart_hook(start_rival_reconnect);

    if (afp_server_restart(s.server, address, "secret", mesg, &len,
                           sizeof(mesg)) != -1) {
        ret = 3;
    }

    afp_server_set_restart_hook(NULL);

    if (ret == 0 && !restart_hook_ran) {
        ret = 4;
    } else if (ret == 0 && restart_rival_started) {
        ret = 5;
    } else if (ret == 0 && !restart_password_seen) {
        ret = 6;
    } else if (ret == 0 && s.server->password[0] != '\0') {
        ret = 7;
    } else if (ret == 0 && !afp_server_reconnect_try_begin(s.server)) {
        ret = 8;
    } else if (ret == 0) {
        afp_server_reconnect_end(s.server);
    }

    session_close(&s);
    return ret;
}

/*! A write through a volume of a session, on its own thread. */
struct volume_write {
    struct afp_volume volume;
    char *data;
    uint64_t len;
    uint64_t written;
    int result;
    int done;  /*!< 1 once the write has returned */
    pthread_t thread;
};

/*!
 * @brief Write the data through the volume, as ll_write() does
 *
 * @param[in,out] arg  the write, whose result receives afp_writeext()'s
 * @returns NULL
 */
static void *write_through(void *arg)
{
    struct volume_write *w = arg;
    w->result = afp_writeext(&w->volume, 9, 0, 0, w->len, w->data,
                             &w->written);
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief Read len bytes of a socket, a read at a time
 *
 * Each read frees room for the sender at once; a read that waits for all its
 * bytes may not until it returns, on some systems.
 *
 * @param[in]  fd   the socket, its reads limited by limit_reads()
 * @param[out] buf  receives the bytes
 * @param[in]  len  bytes to read
 * @returns 0, or -1 when the stream ends or a read fails or times out first
 */
static int recv_whole(int fd, void *buf, size_t len)
{
    for (size_t done = 0; done < len;) {
        ssize_t n = recv(fd, (char *) buf + done, len - done, 0);

        if (n <= 0) {
            return -1;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Run the receiver until a write has returned, or 10 s have gone
 *
 * One pass may end inside a reply that comes in pieces.
 *
 * @param[in] s  the session
 * @param[in] w  the write
 * @returns 0, or -1 when the receiver fails or the write has not returned
 */
static int receive_until_done(const struct session *s,
                              const struct volume_write *w)
{
    for (int pass = 0; pass < 50
            && !__atomic_load_n(&w->done, __ATOMIC_ACQUIRE); pass++) {
        if (receive_all(s->server) != 0) {
            return -1;
        }
    }

    return __atomic_load_n(&w->done, __ATOMIC_ACQUIRE) ? 0 : -1;
}

/*!
 * @brief Start a write through a volume of a session, on its own thread
 *
 * @param[in]  s     the session
 * @param[out] w     receives the write
 * @param[in]  data  the data
 * @param[in]  len   bytes of data
 * @returns 0, or -1 when the thread does not start
 */
static int start_write(const struct session *s, struct volume_write *w,
                       char *data, uint64_t len)
{
    memset(w, 0, sizeof(*w));
    w->volume.server = s->server;
    w->data = data;
    w->len = len;
    w->result = 1;
    return pthread_create(&w->thread, NULL, write_through, w) == 0 ? 0 : -1;
}

/*!
 * @brief Wait for a write to end, cut loose first when a step failed
 *
 * The disconnect frees a writer blocked in its write or waiting for its
 * reply, and fails at once one that queues after it.
 *
 * @param[in]     s       the session
 * @param[in,out] w       the write
 * @param[in]     failed  whether a step of the test failed
 * @returns 1 when the write succeeded and wrote all its data, 0 otherwise
 */
static int end_write(const struct session *s, struct volume_write *w,
                     int failed)
{
    if (failed) {
        loop_disconnect(s->server);
    }

    pthread_join(w->thread, NULL);
    return w->result == 0 && w->written == w->len;
}

/*!
 * @brief Do nothing, so a signal only interrupts the thread's blocked call
 *
 * @param[in] signal_number  unused
 */
static void interrupt(int signal_number)
{
    (void) signal_number;
}

/*!
 * @brief A write a signal cuts short again and again goes on where it stopped
 *
 * Before each 64 KiB the peer reads, the writer has filled the connection
 * and blocks in its write, which the signal cuts short.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_interrupted_write_goes_on(void)
{
    static char data[1U << 20];
    static char got[1U << 20];
    /* FPWriteExt of fork 9 at offset 0, its count 1 MiB. */
    static const unsigned char parameters[20] = {
        afpWriteExt, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0x10, 0, 0,
    };
    unsigned char got_parameters[sizeof(parameters)];
    struct session s;
    struct volume_write w;
    struct sigaction action;
    struct sigaction before;
    struct dsi_header head;
    uint64_t written = hton64(sizeof(data));
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    /* Each four-byte word holds its own index, so no stretch of the data
     * repeats another. */
    for (size_t i = 0; i < sizeof(data); i += 4) {
        uint32_t word = htonl((uint32_t)(i / 4));
        memcpy(data + i, &word, sizeof(word));
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = interrupt;

    if (limit_reads(s.peer) != 0
            || sigaction(SIGUSR1, &action, &before) != 0) {
        session_close(&s);
        return 2;
    }

    if (start_write(&s, &w, data, sizeof(data)) != 0) {
        sigaction(SIGUSR1, &before, NULL);
        session_close(&s);
        return 3;
    }

    if (recv(s.peer, &head, sizeof(head), MSG_WAITALL)
            != (ssize_t) sizeof(head)
            || recv(s.peer, got_parameters, sizeof(got_parameters),
                    MSG_WAITALL) != (ssize_t) sizeof(got_parameters)) {
        ret = 4;
    }

    for (size_t done = 0; ret == 0 && done < sizeof(got); done += 65536) {
        sleep_ms(5);

        if (pthread_kill(w.thread, SIGUSR1) != 0
                || recv_whole(s.peer, got + done, 65536) != 0) {
            ret = 5;
        }
    }

    if (ret == 0 && (ntohl(head.length) != sizeof(parameters) + sizeof(data)
                     || memcmp(got_parameters, parameters,
                               sizeof(parameters)) != 0
                     || memcmp(got, data, sizeof(data)) != 0)) {
        ret = 6;
    } else if (ret == 0 && (send_frame(s.peer, DSI_REPLY, DSI_DSIWrite,
                                       ntohs(head.requestid), &written,
                                       sizeof(written)) != 0
                            || receive_until_done(&s, &w) != 0)) {
        ret = 7;
    }

    if (!end_write(&s, &w, ret != 0) && ret == 0) {
        ret = 8;
    }

    sigaction(SIGUSR1, &before, NULL);
    session_close(&s);
    return ret;
}

/*!
 * @brief A write's wait grows with its data, and runs out
 *
 * At a default of 1 s, a 128 KiB write waits 5 s and takes a reply at 2 s;
 * an unanswered 96 KiB write waits 4 s, times out and leaves the queue.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_write_waits_by_its_size(void)
{
    static char data[128U * 1024U];
    static unsigned char rest[20U + sizeof(data)];
    struct session s;
    struct volume_write w;
    struct dsi_header head;
    struct timespec sent;
    struct timespec ended;
    uint64_t written = hton64(sizeof(data));
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    s.server->dsi_default_timeout = 1;

    if (limit_reads(s.peer) != 0
            || start_write(&s, &w, data, sizeof(data)) != 0) {
        session_close(&s);
        return 2;
    }

    if (recv_whole(s.peer, &head, sizeof(head)) != 0
            || recv_whole(s.peer, rest, sizeof(rest)) != 0) {
        ret = 3;
    } else {
        sleep_ms(2000);

        if (send_frame(s.peer, DSI_REPLY, DSI_DSIWrite,
                       ntohs(head.requestid), &written,
                       sizeof(written)) != 0
                || receive_until_done(&s, &w) != 0) {
            ret = 4;
        }
    }

    if (!end_write(&s, &w, ret != 0) && ret == 0) {
        ret = 5;
    }

    /* Read before the write starts: its 4 s wait ends no sooner than 4 s
     * after, and a 2 s wait within 3 s unless the frame takes 1 s to write. */
    clock_gettime(CLOCK_MONOTONIC, &sent);

    if (ret == 0 && start_write(&s, &w, data, 96U * 1024U) != 0) {
        ret = 6;
    } else if (ret == 0) {
        if (recv_whole(s.peer, &head, sizeof(head)) != 0
                || recv_whole(s.peer, rest, 20U + 96U * 1024U) != 0) {
            ret = 7;
        }

        for (int ms = 0; ret == 0 && ms < 8000
                && !__atomic_load_n(&w.done, __ATOMIC_ACQUIRE); ms += 10) {
            sleep_ms(10);
        }

        clock_gettime(CLOCK_MONOTONIC, &ended);

        if (ret == 0 && !__atomic_load_n(&w.done, __ATOMIC_ACQUIRE)) {
            ret = 8;
        } else if (ret == 0
                   && (ended.tv_sec - sent.tv_sec) * 1000
                   + (ended.tv_nsec - sent.tv_nsec) / 1000000 < 3000) {
            ret = 9;
        }

        end_write(&s, &w, ret != 0);

        if (ret == 0 && (w.result != ETIMEDOUT
                         || s.server->command_requests != NULL
                         || s.server->stats.requests_pending != 0)) {
            ret = 10;
        }
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Answer the next request, of the given command, with the given reply
 *
 * The receive buffer is first filled with 'x', which no reply here holds, so
 * a read past a reply's end finds no earlier reply's bytes.
 *
 * @param[in] s        the session, its peer's reads limited by limit_reads()
 * @param[in] command  the AFP command the request must carry
 * @param[in] answer   the reply's bytes after its DSI header
 * @param[in] len      bytes of answer
 * @returns 0, or -1 when the request does not arrive, carries another
 *          command, or the reply is not read
 */
static int answer_request(const struct session *s, unsigned char command,
                          const void *answer, uint32_t len)
{
    struct dsi_header head;
    unsigned char request[64];
    uint32_t request_len;
    memset(s->server->incoming_buffer, 'x', (size_t) s->server->bufsize);

    if (recv(s->peer, &head, sizeof(head), MSG_WAITALL)
            != (ssize_t) sizeof(head)) {
        return -1;
    }

    request_len = ntohl(head.length);

    if (request_len == 0 || request_len > sizeof(request)
            || recv(s->peer, request, request_len, MSG_WAITALL)
            != (ssize_t) request_len
            || request[0] != command
            || send_frame(s->peer, DSI_REPLY, DSI_DSICommand,
                          ntohs(head.requestid), answer, len) != 0
            || receive_all(s->server) != 0) {
        return -1;
    }

    return 0;
}

/*! An FPMapID on its own thread, and the name it gave. */
struct id_mapping {
    struct afp_server *server;
    unsigned char subfunction;
    char name[AFPC_MAX_NAME_BYTES];
    int result;
};

/*!
 * @brief Ask the server for user 1000's name, as a mount's mapping does
 *
 * @param[in,out] arg  the mapping, which receives the name and the result
 * @returns NULL
 */
static void *map_id(void *arg)
{
    struct id_mapping *m = arg;
    m->result = afp_mapid(m->server, m->subfunction, 1000, m->name);
    return NULL;
}

/*!
 * @brief Run a call on its own thread and answer the request it sends
 *
 * @param[in]     s        the session
 * @param[in]     command  the AFP command the request must carry
 * @param[in]     answer   the reply's bytes after its DSI header
 * @param[in]     len      bytes of answer
 * @param[in]     call     the call, run on the thread
 * @param[in,out] arg      what the call takes and gives back
 * @returns 0, or -1 when the request does not arrive or the reply is not
 *          read
 */
static int answer_call(const struct session *s, unsigned char command,
                       const void *answer, uint32_t len,
                       void *(*call)(void *), void *arg)
{
    pthread_t thread;
    int rc;

    if (pthread_create(&thread, NULL, call, arg) != 0) {
        return -1;
    }

    rc = answer_request(s, command, answer, len);

    /* The disconnect fails the request, sent or still to come. */
    if (rc != 0) {
        loop_disconnect(s->server);
    }

    pthread_join(thread, NULL);
    return rc;
}

/*!
 * @brief Ask for user 1000's name on a thread and answer with the given reply
 *
 * @param[in]  s            the session
 * @param[in]  subfunction  the request's subfunction
 * @param[in]  answer       the reply's bytes after its DSI header
 * @param[in]  len          bytes of answer
 * @param[out] m            receives the mapping, its name and its result
 * @returns 0, or -1 when the request does not arrive or the reply is not
 *          read
 */
static int answer_mapid(const struct session *s, unsigned char subfunction,
                        const void *answer, uint32_t len,
                        struct id_mapping *m)
{
    memset(m, 0, sizeof(*m));
    m->server = s->server;
    m->subfunction = subfunction;
    m->result = 1;
    return answer_call(s, afpMapID, answer, len, map_id, m);
}

/*!
 * @brief An FPMapID reply's name is read where the reply puts it
 *
 * A UTF-8 name's length takes two bytes, a Mac Roman name's one, so a 17-byte
 * reply holds an empty Mac Roman name; a name past the reply's end fails.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_mapid_reads_the_name(void)
{
    static const unsigned char utf8[] = {
        0, 6, 't', 'e', 's', 't', 0xc3, 0xa9
    };
    static const unsigned char roman[] = { 4, 't', 'e', 's', 't' };
    static const unsigned char cut[] = { 0, 200, 'c', 'u', 't' };
    static const unsigned char empty[] = { 0 };
    static const unsigned char roman_cut[] = { 200, 'c', 'u', 't' };
    struct session s;
    struct id_mapping m;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (limit_reads(s.peer) != 0) {
        ret = 2;
    } else if (answer_mapid(&s, kUserIDToUTF8Name, utf8, sizeof(utf8), &m)
               != 0 || m.result != 0 || strcmp(m.name, "test\xc3\xa9") != 0) {
        ret = 3;
    } else if (answer_mapid(&s, kUserIDToName, roman, sizeof(roman), &m)
               != 0 || m.result != 0 || strcmp(m.name, "test") != 0) {
        ret = 4;
    } else if (answer_mapid(&s, kUserIDToUTF8Name, cut, sizeof(cut), &m)
               != 0 || m.result == 0 || m.name[0] != '\0') {
        ret = 5;
    } else if (answer_mapid(&s, kUserIDToName, empty, sizeof(empty), &m)
               != 0 || m.result != 0 || m.name[0] != '\0') {
        ret = 6;
    } else if (answer_mapid(&s, kUserIDToName, roman_cut, sizeof(roman_cut),
                            &m) != 0 || m.result == 0 || m.name[0] != '\0') {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Find a volume's mapping on a thread of its own, as a mount does
 *
 * @param[in,out] volume  the volume
 * @returns NULL
 */
static void *detect_mapping(void *volume)
{
    afp_detect_mapping(volume);
    return NULL;
}

/*!
 * @brief Run a mount's mapping against a server that gives the local user's ID
 *
 * @param[in]  local_name  the local user's name
 * @param[in]  unix_privs  1 for a volume with UNIX privileges, 0 for one
 *                         without, whose mapping asks the server nothing
 * @param[in]  answer      the FPMapID reply's bytes after its DSI header, or
 *                         NULL for a mapping that asks no name
 * @param[in]  len         bytes of answer
 * @param[out] mapping     receives the volume's mapping
 * @returns 0, or -1 when a request does not arrive, carries another
 *          command or comes unasked for, or a reply is not read
 */
static int map_against(const char *local_name, int unix_privs,
                       const void *answer, uint32_t len, int *mapping)
{
    /* FPGetUserInfo's answers as afpd packs them: the bitmap, then the one
     * ID it names, 1000. */
    static const unsigned char user_id[] = {
        0, kFPGetUserInfo_USER_ID, 0, 0, 0x03, 0xe8
    };
    static const unsigned char group_id[] = {
        0, kFPGetUserInfo_PRI_GROUPID, 0, 0, 0x03, 0xe8
    };
    struct session s;
    struct afp_volume volume;
    struct dsi_header head;
    pthread_t thread;
    int rc = 0;

    if (session_open(&s, 4096) != 0) {
        return -1;
    }

    memset(&volume, 0, sizeof(volume));
    volume.server = s.server;
    volume.attributes = kSupportsUTF8Names;
    volume.extra_flags = unix_privs ? VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX : 0;
    volume.mapping = AFP_MAPPING_UNKNOWN;
    s.server->passwd.pw_uid = 1000;
    snprintf(s.server->passwd_name, sizeof(s.server->passwd_name), "%s",
             local_name);

    if (limit_reads(s.peer) != 0
            || pthread_create(&thread, NULL, detect_mapping, &volume) != 0) {
        session_close(&s);
        return -1;
    }

    if (unix_privs
            && (answer_request(&s, afpGetUserInfo, user_id,
                               sizeof(user_id)) != 0
                || answer_request(&s, afpGetUserInfo, group_id,
                                  sizeof(group_id)) != 0
                || (answer != NULL
                    && answer_request(&s, afpMapID, answer, len) != 0))) {
        loop_disconnect(s.server);
        rc = -1;
    }

    pthread_join(thread, NULL);

    /* A mapping that asks no name leaves nothing more on the wire. */
    if (rc == 0 && answer == NULL
            && recv(s.peer, &head, sizeof(head), MSG_DONTWAIT) >= 0) {
        rc = -1;
    }

    *mapping = volume.mapping;
    session_close(&s);
    return rc;
}

/*!
 * @brief Only a server that names the local user's ID makes the IDs common
 *
 * The name must reach the mapping's buffer. No name is asked for a user the
 * passwd database lacks, and nothing on a volume without UNIX privileges.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_mapping_finds_common_ids(void)
{
    static const unsigned char name[] = { 0, 4, 't', 'e', 's', 't' };
    int mapping = AFP_MAPPING_UNKNOWN;

    if (map_against("test", 1, name, sizeof(name), &mapping) != 0) {
        return 1;
    } else if (mapping != AFP_MAPPING_COMMON) {
        return 2;
    } else if (map_against("", 1, NULL, 0, &mapping) != 0) {
        return 3;
    } else if (mapping != AFP_MAPPING_LOGINIDS) {
        return 4;
    }

    if (map_against("test", 0, NULL, 0, &mapping) != 0) {
        return 5;
    } else if (mapping != AFP_MAPPING_LOGINIDS) {
        return 6;
    }

    return 0;
}

/*! An FPGetUserInfo on its own thread, and the IDs it gave. */
struct user_info {
    struct afp_server *server;
    unsigned short bitmap;
    unsigned int uid;
    unsigned int gid;
    int result;
};

/*!
 * @brief Ask the server for the session's user's IDs the bitmap names
 *
 * @param[in,out] arg  the request, which receives the IDs and the result
 * @returns NULL
 */
static void *get_user_info(void *arg)
{
    struct user_info *u = arg;
    u->result = afp_getuserinfo(u->server, 1, 0, u->bitmap, &u->uid,
                                &u->gid);
    return NULL;
}

/*!
 * @brief Ask for the user's IDs on a thread and answer with the given reply
 *
 * Both IDs start as 7, which an ID the call does not give keeps.
 *
 * @param[in]  s       the session
 * @param[in]  bitmap  the IDs to ask for
 * @param[in]  answer  the reply's bytes after its DSI header
 * @param[in]  len     bytes of answer
 * @param[out] u       receives the request, its IDs and its result
 * @returns 0, or -1 when the request does not arrive or the reply is not
 *          read
 */
static int answer_user_info(const struct session *s, unsigned short bitmap,
                            const void *answer, uint32_t len,
                            struct user_info *u)
{
    memset(u, 0, sizeof(*u));
    u->server = s->server;
    u->bitmap = bitmap;
    u->uid = 7;
    u->gid = 7;
    u->result = 1;
    return answer_call(s, afpGetUserInfo, answer, len, get_user_info, u);
}

/*!
 * @brief Read a socket's receive low-water mark
 *
 * @param[in] fd  the socket
 * @returns the mark in bytes, or -1
 */
static int low_water_mark(int fd)
{
    int mark = 0;
    socklen_t len = sizeof(mark);
    return getsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &mark, &len) == 0
           ? mark : -1;
}

/*!
 * @brief Tell whether a socket keeps a receive low-water mark it is given
 *
 * @param[in] fd  the socket, whose mark is one byte on return
 * @returns 1 when it does, 0 otherwise
 */
static int keeps_low_water_mark(int fd)
{
    int mark = 2;
    int kept = setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &mark,
                          sizeof(mark)) == 0 && low_water_mark(fd) == 2;
    mark = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &mark, sizeof(mark));
    return kept;
}

/*!
 * @brief Say in the run's log that a case has nothing to check here
 *
 * The case still passes, as tap.h has no skip.
 *
 * @param[in] why  what the platform lacks for the case
 */
static void say_nothing_to_check(const char *why)
{
    printf("# nothing to check: %s\n", why);
}

/*!
 * @brief Write the header of a read reply and the first bytes of its
 *        payload in one piece, so they arrive together
 *
 * @param[in] peer     the server's end of the session
 * @param[in] id       the reply's request ID
 * @param[in] length   payload bytes the header announces
 * @param[in] payload  the payload
 * @param[in] first    payload bytes to write with the header
 * @returns 0, or -1
 */
static int send_reply_start(int peer, unsigned short id, uint32_t length,
                            const char *payload, size_t first)
{
    char frame[sizeof(struct dsi_header) + 1024];
    struct dsi_header header = dsi_header_of(DSI_REPLY, DSI_DSICommand, id,
                                             length);

    if (first > sizeof(frame) - sizeof(header)) {
        return -1;
    }

    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), payload, first);
    return write(peer, frame, sizeof(header) + first)
           == (ssize_t)(sizeof(header) + first) ? 0 : -1;
}

/*!
 * @brief With part of a reply's payload read, the connection waits for the
 *        rest whole
 *
 * A read reply arrives in two pieces over TCP. After the first the socket
 * holds back until the rest waits, or half its receive buffer, and once the
 * reply is whole it reports the first byte again.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_reply_remainder_is_awaited_whole(void)
{
    static char payload[16384];
    static char got[sizeof(payload)];
    const size_t first = 1000;
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    int rcvbuf = 0;
    socklen_t len = sizeof(rcvbuf);
    int awaited;
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    if (!keeps_low_water_mark(s.server->fd)) {
        say_nothing_to_check("the socket refuses a receive low-water mark");
        session_close(&s);
        return 0;
    }

    if (getsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) != 0) {
        session_close(&s);
        return 2;
    }

    awaited = (int) min(sizeof(payload) - first, (size_t) rcvbuf / 2);

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i * 11 + 3);
    }

    memset(&rx, 0, sizeof(rx));
    rx.data = got;
    rx.maxsize = sizeof(got);
    queue_request(s.server, &pending, 40, afpReadExt, &rx, NULL);

    if (awaited < 2) {
        ret = 3;
    } else if (send_reply_start(s.peer, 40, sizeof(payload), payload,
                                first) != 0
               || receive_all(s.server) != 0 || rx.size != first) {
        ret = 4;
    } else if (low_water_mark(s.server->fd) != awaited) {
        ret = 5;
    } else if (write(s.peer, payload + first, sizeof(payload) - first)
               != (ssize_t)(sizeof(payload) - first)
               || receive_all(s.server) != 0) {
        ret = 6;
    } else if (pending.done_waiting != 1 || rx.size != sizeof(payload)
               || memcmp(got, payload, sizeof(payload)) != 0) {
        ret = 7;
    } else if (low_water_mark(s.server->fd) != 1) {
        ret = 8;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A receive pass that finds no bytes returns
 *
 * The loop calls the receiver for a socket that reported readable; one
 * whose bytes are gone by then gives the pass nothing, and the pass does
 * not wait for them.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_pass_without_bytes_returns(void)
{
    struct session s;
    struct receive_call call;
    pthread_t thread;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    call.server = s.server;
    call.started = 0;
    call.result = -9;

    if (pthread_create(&thread, NULL, receive_once, &call) != 0) {
        session_close(&s);
        return 2;
    }

    if (receiver_result(&call, -9) != 0) {
        ret = 3;
    }

    /* A receiver that waits in its read ends with this frame. */
    if (send_header(s.peer, DSI_REQUEST, DSI_DSITickle, 0, 0) != 0
            && ret == 0) {
        ret = 4;
    }

    pthread_join(thread, NULL);
    session_close(&s);
    return ret;
}

/*!
 * @brief A receive pass takes the bytes that wait below the mark
 *
 * A socket reports readable below its mark once its buffer or the peer's
 * window is full. The pass takes what waits and returns, and the reply
 * ends whole once its last bytes arrive.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_pass_below_the_mark_takes_what_waits(void)
{
    char payload[3000];
    char got[sizeof(payload)];
    const size_t first = 1000;
    const size_t second = 500;
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    struct receive_call call;
    pthread_t thread;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i * 17 + 9);
    }

    memset(&rx, 0, sizeof(rx));
    rx.data = got;
    rx.maxsize = sizeof(got);
    queue_request(s.server, &pending, 41, afpReadExt, &rx, NULL);

    if (send_reply_start(s.peer, 41, sizeof(payload), payload, first) != 0
            || receive_all(s.server) != 0 || rx.size != first) {
        session_close(&s);
        return 2;
    }

    call.server = s.server;
    call.started = 0;
    call.result = -9;

    if (write(s.peer, payload + first, second) != (ssize_t) second
            || pthread_create(&thread, NULL, receive_once, &call) != 0) {
        session_close(&s);
        return 3;
    }

    if (receiver_result(&call, -9) != 0 || rx.size != first + second) {
        ret = 4;
    }

    /* The rest ends a receiver that waits in its read for the mark. */
    if (write(s.peer, payload + first + second,
              sizeof(payload) - first - second)
            != (ssize_t)(sizeof(payload) - first - second) && ret == 0) {
        ret = 5;
    }

    pthread_join(thread, NULL);

    if (ret == 0 && (receive_all(s.server) != 0 || pending.done_waiting != 1
                     || rx.size != sizeof(payload)
                     || memcmp(got, payload, sizeof(payload)) != 0)) {
        ret = 6;
    }

    session_close(&s);
    return ret;
}

/*! A read reply written on its own thread, as a peer sends it. */
struct reply_writer {
    int peer;
    unsigned short id;
    const char *payload;
    size_t len;
    size_t from; /*!< payload bytes the case wrote itself, with the header */
    int result;  /*!< 0 once the whole reply is written */
};

/*!
 * @brief Write a read reply: its header and payload, or the rest of a
 *        payload the case began
 *
 * @param[in,out] arg  the writer, whose result receives 0 or -1
 * @returns NULL
 */
static void *write_reply(void *arg)
{
    struct reply_writer *w = arg;
    size_t done = w->from;
    ssize_t n = 1;
    int rc = w->from > 0 ? 0
             : send_header(w->peer, DSI_REPLY, DSI_DSICommand, w->id,
                           (uint32_t) w->len);

    while (rc == 0 && done < w->len && n > 0) {
        n = write(w->peer, w->payload + done, w->len - done);

        if (n > 0) {
            done += (size_t) n;
        }
    }

    __atomic_store_n(&w->result, rc == 0 && done == w->len ? 0 : -1,
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief The socket holds back no more than half its receive buffer
 *
 * The peer's window admits no more than the receive buffer takes, so a
 * socket that held back for more would never report readable. With a
 * buffer of 32 KiB and most of a 256 KiB reply still to come, the mark is
 * half the buffer the socket reports.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_mark_stays_within_half_the_buffer(void)
{
    static char payload[262144];
    static char got[sizeof(payload)];
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    struct reply_writer writer;
    pthread_t thread;
    int size = 32768;
    socklen_t len = sizeof(size);
    ssize_t queued = 0;
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    if (!keeps_low_water_mark(s.server->fd)) {
        say_nothing_to_check("the socket refuses a receive low-water mark");
        session_close(&s);
        return 0;
    }

    if (setsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &size,
                   sizeof(size)) != 0
            || getsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &size,
                          &len) != 0) {
        session_close(&s);
        return 2;
    }

    /* A socket that keeps a larger buffer than it was given holds back
     * for a whole batch, which the buffer does not bound. */
    if (size / 2 >= 65536) {
        say_nothing_to_check("the receive buffer stays at 128 KiB or more");
        session_close(&s);
        return 0;
    }

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i * 3 + 2);
    }

    memset(&rx, 0, sizeof(rx));
    rx.data = got;
    rx.maxsize = sizeof(got);
    queue_request(s.server, &pending, 43, afpReadExt, &rx, NULL);
    writer.peer = s.peer;
    writer.id = 43;
    writer.payload = payload;
    writer.len = sizeof(payload);
    writer.from = 0;
    writer.result = 1;

    if (pthread_create(&thread, NULL, write_reply, &writer) != 0) {
        session_close(&s);
        return 3;
    }

    /* The header and some of the payload wait before the first pass. */
    for (int i = 0; i < 200 && queued < 1016; i++) {
        sleep_ms(10);
        queued = recv(s.server->fd, got, 1016, MSG_PEEK | MSG_DONTWAIT);
    }

    if (queued < 1016 || afpc_dsi_recv(s.server) != 0
            || afpc_dsi_recv(s.server) != 0 || rx.size == 0) {
        ret = 4;
    } else if (low_water_mark(s.server->fd) != size / 2) {
        ret = 5;
    }

    for (int i = 0; i < 500 && rx.size < sizeof(payload); i++) {
        if (receive_all(s.server) != 0) {
            ret = ret ? ret : 6;
            break;
        }
    }

    if (rx.size < sizeof(payload)) {
        shutdown(s.server->fd, SHUT_RDWR);
    }

    pthread_join(thread, NULL);

    if (ret == 0 && (writer.result != 0 || pending.done_waiting != 1
                     || rx.size != sizeof(payload)
                     || memcmp(got, payload, sizeof(payload)) != 0)) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief The socket holds back for 64 KiB of a long payload at most
 *
 * The loop takes a long payload while it arrives, a batch at a time: with
 * a receive buffer of 512 KiB and a megabyte still to come, the mark is
 * 64 KiB.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_long_payload_is_awaited_a_batch_at_a_time(void)
{
    static char payload[1024 * 1024];
    static char got[sizeof(payload)];
    const size_t first = 1000;
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    struct reply_writer writer;
    pthread_t thread;
    int size = 512 * 1024;
    socklen_t len = sizeof(size);
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    if (!keeps_low_water_mark(s.server->fd)) {
        say_nothing_to_check("the socket refuses a receive low-water mark");
        session_close(&s);
        return 0;
    }

    if (setsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &size,
                   sizeof(size)) != 0
            || getsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &size,
                          &len) != 0) {
        session_close(&s);
        return 2;
    }

    /* A socket that keeps a buffer of two batches or less is bounded by
     * its half, which another case checks. */
    if (size / 2 <= 65536) {
        say_nothing_to_check("the receive buffer stays at 128 KiB or less");
        session_close(&s);
        return 0;
    }

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i * 7 + 5);
    }

    memset(&rx, 0, sizeof(rx));
    rx.data = got;
    rx.maxsize = sizeof(got);
    queue_request(s.server, &pending, 44, afpReadExt, &rx, NULL);

    if (send_reply_start(s.peer, 44, sizeof(payload), payload, first) != 0
            || receive_all(s.server) != 0 || rx.size != first) {
        session_close(&s);
        return 3;
    }

    if (low_water_mark(s.server->fd) != 65536) {
        ret = 4;
    }

    writer.peer = s.peer;
    writer.id = 44;
    writer.payload = payload;
    writer.len = sizeof(payload);
    writer.from = first;
    writer.result = 1;

    if (pthread_create(&thread, NULL, write_reply, &writer) != 0) {
        session_close(&s);
        return 5;
    }

    for (int i = 0; i < 500 && rx.size < sizeof(payload); i++) {
        if (receive_all(s.server) != 0) {
            ret = ret ? ret : 6;
            break;
        }
    }

    if (rx.size < sizeof(payload)) {
        shutdown(s.server->fd, SHUT_RDWR);
    }

    pthread_join(thread, NULL);

    if (ret == 0 && (writer.result != 0 || pending.done_waiting != 1
                     || rx.size != sizeof(payload)
                     || memcmp(got, payload, sizeof(payload)) != 0)) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A pass that took two batches of a payload leaves the socket
 *        reporting its first byte
 *
 * A peer that sends faster than the loop reads has its bytes waiting when
 * the loop is back, and holding back for more would only delay them. Once
 * 128 KiB of a 2 MiB reply wait, one pass takes them, and the socket holds
 * nothing back though the reply is not whole. A connection whose buffers
 * cannot hold that much at once has no such pass to check.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_pass_of_two_batches_holds_nothing_back(void)
{
    static char payload[2 * 1024 * 1024];
    static char got[sizeof(payload)];
    const ssize_t two_batches = 2 * 65536;
    struct session s;
    struct dsi_request pending;
    struct afp_rx_buffer rx;
    struct reply_writer writer;
    pthread_t thread;
    int size = 512 * 1024;
    ssize_t queued = 0;
    int ret = 0;

    if (tcp_session_open(&s) != 0) {
        return 1;
    }

    if (!keeps_low_water_mark(s.server->fd)) {
        say_nothing_to_check("the socket refuses a receive low-water mark");
        session_close(&s);
        return 0;
    }

    setsockopt(s.server->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i * 5 + 1);
    }

    memset(&rx, 0, sizeof(rx));
    rx.data = got;
    rx.maxsize = sizeof(got);
    queue_request(s.server, &pending, 42, afpReadExt, &rx, NULL);
    writer.peer = s.peer;
    writer.id = 42;
    writer.payload = payload;
    writer.len = sizeof(payload);
    writer.from = 0;
    writer.result = 1;

    if (pthread_create(&thread, NULL, write_reply, &writer) != 0) {
        session_close(&s);
        return 2;
    }

    /* The header and two batches wait before the first pass. */
    for (int i = 0; i < 200 && queued < two_batches + 16; i++) {
        sleep_ms(10);
        queued = recv(s.server->fd, got, (size_t) two_batches + 16,
                      MSG_PEEK | MSG_DONTWAIT);
    }

    if (queued >= two_batches + 16) {
        /* The header's pass, then the pass that takes what waits. */
        if (afpc_dsi_recv(s.server) != 0 || afpc_dsi_recv(s.server) != 0) {
            ret = 3;
        } else if (rx.size < (unsigned int) two_batches) {
            ret = 4;
        } else if (rx.size < sizeof(payload)
                   && low_water_mark(s.server->fd) != 1) {
            ret = 5;
        }
    } else {
        say_nothing_to_check("the connection never holds two batches");
    }

    /* The rest, which also ends the writer. */
    for (int i = 0; i < 500 && rx.size < sizeof(payload); i++) {
        if (receive_all(s.server) != 0) {
            ret = ret ? ret : 6;
            break;
        }
    }

    if (rx.size < sizeof(payload)) {
        shutdown(s.server->fd, SHUT_RDWR);
    }

    pthread_join(thread, NULL);

    if (ret == 0 && (writer.result != 0 || pending.done_waiting != 1
                     || rx.size != sizeof(payload)
                     || memcmp(got, payload, sizeof(payload)) != 0)) {
        ret = 7;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief An FPGetUserInfo reply's IDs are read as its bitmap packs them
 *
 * afpd puts each ID the bitmap names after it, the user's first; a reply
 * without an ID asked for or named fails and leaves the caller's IDs.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_user_info_reads_the_ids(void)
{
    static const unsigned char user[] = {
        0, kFPGetUserInfo_USER_ID, 0, 0, 0x03, 0xe8
    };
    static const unsigned char group[] = {
        0, kFPGetUserInfo_PRI_GROUPID, 0, 0, 0x03, 0xe9
    };
    static const unsigned char both[] = {
        0, kFPGetUserInfo_USER_ID | kFPGetUserInfo_PRI_GROUPID,
        0, 0, 0x03, 0xe8, 0, 0, 0x03, 0xe9
    };
    static const unsigned char no_id[] = { 0, kFPGetUserInfo_USER_ID };
    static const unsigned char one_id[] = {
        0, kFPGetUserInfo_USER_ID | kFPGetUserInfo_PRI_GROUPID, 0, 0, 0x03, 0xe8
    };
    struct session s;
    struct user_info u;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (limit_reads(s.peer) != 0) {
        ret = 2;
    } else if (answer_user_info(&s, kFPGetUserInfo_USER_ID, user,
                                sizeof(user), &u) != 0
               || u.result != 0 || u.uid != 1000 || u.gid != 7) {
        ret = 3;
    } else if (answer_user_info(&s, kFPGetUserInfo_PRI_GROUPID, group,
                                sizeof(group), &u) != 0
               || u.result != 0 || u.uid != 7 || u.gid != 1001) {
        ret = 4;
    } else if (answer_user_info(&s, kFPGetUserInfo_USER_ID
                                | kFPGetUserInfo_PRI_GROUPID, both,
                                sizeof(both), &u) != 0
               || u.result != 0 || u.uid != 1000 || u.gid != 1001) {
        ret = 5;
    } else if (answer_user_info(&s, kFPGetUserInfo_USER_ID, no_id,
                                sizeof(no_id), &u) != 0
               || u.result == 0 || u.uid != 7 || u.gid != 7) {
        ret = 6;
    } else if (answer_user_info(&s, kFPGetUserInfo_USER_ID
                                | kFPGetUserInfo_PRI_GROUPID, one_id,
                                sizeof(one_id), &u) != 0
               || u.result == 0 || u.uid != 7 || u.gid != 7) {
        ret = 7;
    } else if (answer_user_info(&s, kFPGetUserInfo_USER_ID, group,
                                sizeof(group), &u) != 0
               || u.result == 0 || u.uid != 7 || u.gid != 7) {
        ret = 8;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief A request sent with the default's sentinel waits the server's default
 *
 * The send turns the sentinel into the server's default before it looks
 * at the wait's sign, which would otherwise take it for no limit.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_default_wait_is_the_servers(void)
{
    struct session s;
    struct sender sender;
    pthread_t thread;
    int result = 1;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    s.server->dsi_default_timeout = 1;
    sender.server = s.server;
    sender.wait = DSI_DEFAULT_TIMEOUT;
    sender.result = 1;

    if (pthread_create(&thread, NULL, send_and_wait, &sender) != 0) {
        session_close(&s);
        return 2;
    }

    /* The default of 1 s ends the wait well within 3 s. */
    for (int i = 0; i < 300 && result == 1; i++) {
        sleep_ms(10);
        result = __atomic_load_n(&sender.result, __ATOMIC_ACQUIRE);
    }

    if (result == 1) {
        ret = 3;
    }

    /* A sender that still waits would wait for ever; the disconnect fails
     * it, so the join ends. */
    loop_disconnect(s.server);
    pthread_join(thread, NULL);

    if (ret == 0 && result != ETIMEDOUT) {
        ret = 4;
    }

    session_close(&s);
    return ret;
}

/*!
 * @brief Run the DSI receive path checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    /* A write to a peer the test has closed must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    TEST_int(utest_loop_start_wakes_every_waiter(), 0,
             "the loop's start wakes every thread waiting for it");
    TEST_int(utest_wake_up_keeps_its_handler(), 0,
             "the loop's wake-up keeps its handler through a delivery");
    TEST_int(utest_tickle_with_pending_id_is_a_request(), 0,
             "a tickle carrying a pending request's id is not its reply");
    TEST_int(utest_request_flag_command_is_dropped(), 0,
             "a command frame with request flags is read and dropped");
    TEST_int(utest_undefined_command_ends_session(), 0,
             "a frame with an undefined command ends the session");
    TEST_int(utest_undefined_flags_end_session(), 0,
             "a frame with undefined flags ends the session");
    TEST_int(utest_short_read_reply_ends_session(), 0,
             "a read reply shorter than its buffer's data ends the session");
    TEST_int(utest_long_read_reply_ends_session(), 0,
             "a read reply longer than its buffer ends the session");
    TEST_int(utest_read_without_buffer_fails(), 0,
             "a read reply with no buffer fails");
    TEST_int(utest_orphan_oversize_reply_is_dropped(), 0,
             "an orphan reply larger than the buffer is drained");
    TEST_int(utest_abandoned_read_rest_is_dropped(), 0,
             "the rest of an abandoned read reply is drained");
    TEST_int(utest_oversize_reply_fails_its_request(), 0,
             "a reply larger than the buffer fails its request with EIO");
    TEST_int(utest_frame_over_the_quantum_ends_session(), 0,
             "a frame over the buffer and the quantum ends the session");
    TEST_int(utest_receive_without_descriptor_reads_nothing(), 0,
             "a receive with no descriptor reads nothing");
    TEST_int(utest_drop_across_a_reconnect_keeps_the_next_stream(), 0,
             "a drop across a reconnect leaves the next connection's stream");
    TEST_int(utest_server_valid_until_removed(), 0,
             "a server is valid from its set-up until its removal");
    TEST_int(utest_disconnect_ends_a_disconnected_server(), 0,
             "a disconnect ends a server already marked disconnected");
    TEST_int(utest_disconnect_fails_a_request_without_descriptor(), 0,
             "a disconnect fails a request when the descriptor is gone");
    TEST_int(utest_disconnect_without_descriptor_ends(), 0,
             "a disconnect with no descriptor leaves the server disconnected");
    TEST_int(utest_connect_waits_for_the_frame_in_flight(), 0,
             "a connect closes the old descriptor after the frame in flight");
    TEST_int(utest_disconnect_waits_for_the_frame_in_flight(), 0,
             "a disconnect closes the descriptor after the frame in flight");
    TEST_int(utest_disconnect_shuts_the_write_side(), 0,
             "a disconnect shuts the write side after the peer's close");
    TEST_int(utest_disconnect_keeps_errno(), 0,
             "a disconnect keeps errno");
    TEST_int(utest_next_connection_outlives_the_disconnect(), 0,
             "the next connection outlives an overtaken disconnect");
    TEST_int(utest_removal_fails_a_waiting_request(), 0,
             "a removal fails a waiting request with EIO and wakes it");
    TEST_int(utest_no_wait_request_is_never_queued(), 0,
             "a request sent without a wait is never queued");
    TEST_int(utest_write_to_closed_connection_fails(), 0,
             "a write to a closed connection fails and leaves it to the loop");
    TEST_int(utest_second_reply_is_dropped(), 0,
             "a second reply to an answered request is dropped");
    TEST_int(utest_cut_off_status_read_fails_the_connect(), 0,
             "a connect whose status read is cut off returns -EIO");
    TEST_int(utest_connect_of_a_removed_server_is_refused(), 0,
             "a connect of a removed server is refused and lists nothing");
    TEST_int(utest_no_wait_request_never_reconnects(), 0,
             "a request without a wait never reconnects a disconnected server");
    TEST_int(utest_disconnect_keeps_an_answered_result(), 0,
             "a disconnect keeps an answered request's result");
    TEST_int(utest_disconnect_leaves_the_next_connection_waiting(), 0,
             "a disconnect of a replaced connection spares the next one's");
    TEST_int(utest_busy_open_keeps_the_quantum(), 0,
             "a busy refusal of a session's open leaves the quantum");
    TEST_int(utest_answer_as_the_wait_expires_is_the_result(), 0,
             "an answer landing as the wait expires is the result");
    TEST_int(utest_failed_request_waits_for_its_receiver(), 0,
             "a failed request stays its sender's until the receiver is done");
    TEST_int(utest_slow_handler_is_the_result(), 0,
             "a reply whose handler outlasts its sender's wait is the result");
    TEST_int(utest_unanswered_request_fails(), 0,
             "a request with no answer times out, one not written fails");
    TEST_int(utest_unlimited_wait_is_answered(), 0,
             "a request whose wait has no limit takes a late reply");
    TEST_int(utest_unsent_request_takes_no_answer(), 0,
             "a request answered before its frame goes out fails unsent");
    TEST_int(utest_tickle_waits_for_no_writer(), 0,
             "a tickle skips a frame on its way instead of waiting");
    TEST_int(utest_restart_keeps_its_reconnect(), 0,
             "a restart keeps its reconnect from the password's write to its "
             "wipe");
    TEST_int(utest_tickle_waits_for_no_room(), 0,
             "a tickle skips a full socket instead of waiting");
    TEST_int(utest_request_of_a_replaced_connection_fails_unsent(), 0,
             "a request queued before a reconnect replaced its connection "
             "fails unsent");
    TEST_int(utest_interrupted_write_goes_on(), 0,
             "a write a signal cuts short goes on where it stopped");
    TEST_int(utest_write_waits_by_its_size(), 0,
             "a write waits by its size, and gives up after it");
    TEST_int(utest_default_wait_is_the_servers(), 0,
             "a request with the default wait gives up after the server's");
    TEST_int(utest_mapid_reads_the_name(), 0,
             "an FPMapID reply's name is read where the reply puts it");
    TEST_int(utest_mapping_finds_common_ids(), 0,
             "a server that names the local user's ID makes the IDs common");
    TEST_int(utest_reply_remainder_is_awaited_whole(), 0,
             "a reply read in part is awaited whole");
    TEST_int(utest_pass_without_bytes_returns(), 0,
             "a receive pass that finds no bytes returns");
    TEST_int(utest_pass_below_the_mark_takes_what_waits(), 0,
             "a receive pass takes the bytes that wait below the mark");
    TEST_int(utest_mark_stays_within_half_the_buffer(), 0,
             "the socket holds back no more than half its receive buffer");
    TEST_int(utest_long_payload_is_awaited_a_batch_at_a_time(), 0,
             "a long payload is awaited a batch at a time");
    TEST_int(utest_pass_of_two_batches_holds_nothing_back(), 0,
             "a pass that took two batches holds nothing back");
    TEST_int(utest_user_info_reads_the_ids(), 0,
             "an FPGetUserInfo reply's IDs are read as its bitmap packs them");
    return test_tap_finish();
}
