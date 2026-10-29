/*
    test_dsi_recv.c: the DSI receive path of a session

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
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
#include <time.h>
#include <unistd.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
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
    r->wait = DSI_DEFAULT_TIMEOUT;
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
    struct dsi_header header;
    memset(&header, 0, sizeof(header));
    header.flags = flags;
    header.command = command;
    header.requestid = htons(id);
    header.length = htonl(length);
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

    if (bind(l->fd, (struct sockaddr *) &l->addr, sizeof(l->addr)) != 0
            || getsockname(l->fd, (struct sockaddr *) &l->addr, &len) != 0
            || listen(l->fd, 1) != 0) {
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
 * @brief Wait for a receiver's thread to reach its read
 *
 * The thread has started once it sets the flag, and is in its read 100 ms
 * later.
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
 * The receiver's read of the old connection returns 0 once the connect has
 * shut it down, with nothing more from the peer, and the next connection's
 * first reply, which the loop reads, is still served.
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

    if (pthread_create(&receiver, NULL, receive_once, &call) != 0) {
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

    /* A receive that began before the connect returns once the connect has
     * shut its descriptor down; one that began after it would read the next
     * connection, which the close of the accepted end ends. */
    for (int i = 0; i < 200
            && __atomic_load_n(&call.result, __ATOMIC_ACQUIRE) == 1; i++) {
        sleep_ms(10);
    }

    if (__atomic_load_n(&call.result, __ATOMIC_ACQUIRE) == 1) {
        ret = 7;
        /* A receive still in the old read ends at the peer's close. */
        shutdown(s.peer, SHUT_WR);

        if (conn.result == 0 && (next_peer = accept(l.fd, NULL, NULL)) >= 0) {
            close(next_peer);
            next_peer = -1;
        }
    }

    pthread_join(receiver, NULL);

    if (ret == 0 && call.result != 0) {
        ret = 8;
    } else if (ret == 0 && conn.result != 0) {
        ret = 9;
    } else if (ret == 0 && (next_peer = accept(l.fd, NULL, NULL)) < 0) {
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
    char byte;
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
    } else if (recv(s.peer, &byte, 1, MSG_DONTWAIT) != 0) {
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

    s->peer = accept(l->fd, NULL, NULL);

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
    } else if (ret == 0 && (next_peer = accept(l.fd, NULL, NULL)) < 0) {
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
 * @brief A disconnect closes the descriptor only after the frame in flight
 *
 * A sender holds send_mutex while it writes a frame. A disconnect that takes
 * the descriptor meanwhile waits for the frame before it closes the
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

    if (ret == 0 && fcntl(old_fd, F_GETFD) != -1) {
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
            || connect(s.server->fd, (struct sockaddr *) &l.addr,
                       sizeof(l.addr)) != 0
            || (s.peer = accept(l.fd, NULL, NULL)) < 0) {
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

/* A sender's thread: one request with a wait, and its result. */
struct sender {
    struct afp_server *server;
    int wait;
    int result;
};

/*!
 * @brief Send one request and wait for its reply, as a sender's thread does
 *
 * @param[in,out] arg  the sender, whose result receives the send's
 * @returns NULL
 */
static void *send_and_wait(void *arg)
{
    struct sender *s = arg;
    struct dsi_header header;
    int result;
    afpc_dsi_setup_header(s->server, &header, DSI_DSICommand);
    result = afpc_dsi_send(s->server, (char *) &header, sizeof(header),
                           s->wait, afpGetVolParms, NULL);
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
    int fd = accept(*(int *) listener, NULL, NULL);

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
 * The connect fails with the read's errno, negative, which errno holds too,
 * and leaves the server listed.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_cut_off_status_read_fails_the_connect(void)
{
    struct listener l;
    struct afp_server *server;
    pthread_t acceptor;

    if (cut_off_open(&l, &acceptor, &server) != 0) {
        return 1;
    }

    if (afp_server_connect(server, 1) != -EIO || errno != EIO) {
        return 2;
    }

    if (pthread_join(acceptor, NULL) != 0) {
        return 3;
    }

    /* The address is the test's, not the server's to free. */
    server->address = NULL;
    afp_server_remove(server);
    listener_close(&l);
    return 0;
}

/*!
 * @brief A connect of a removed server is refused and lists nothing
 *
 * The removal comes first; the connect finds the server destroyed under the
 * list lock, and the server stays removed.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_connect_of_a_removed_server_is_refused(void)
{
    struct listener l;
    struct afp_server *server;
    pthread_t acceptor;
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

    afp_server_hold(server);

    if (afp_server_remove(server) != 0) {
        ret = 4;
    } else if (afp_server_connect(server, 0) != -ENOENT) {
        ret = 5;
    } else if (server_still_valid(server) != 0) {
        ret = 6;
    } else if (find_server_by_pointer(server) != NULL) {
        ret = 7;
    }

    server->address = NULL;
    afp_server_release(server);
    listener_close(&l);
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
    afpc_dsi_fail_request_queue(s.server, -EIO);

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
     * sender's wait expires before it can read the answer. A removal
     * frees the request under the queue's mutex, which the answer holds
     * while it finds and answers the request; the sender needs only the
     * request's mutex until then. */
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

/*! A reader's thread: one FPReadExt with a wait, into its own buffer. */
struct reader {
    struct afp_server *server;
    char data[256];
    struct afp_rx_buffer rx;
    int result;
};

/*!
 * @brief Send a read and wait for its reply, as ll_read() does
 *
 * @param[in,out] arg  the reader, whose result receives the send's
 * @returns NULL
 */
static void *read_and_wait(void *arg)
{
    struct reader *r = arg;
    struct dsi_header header;
    r->rx.data = r->data;
    r->rx.maxsize = sizeof(r->data);
    r->rx.size = 0;
    r->rx.errorcode = 0;
    afpc_dsi_setup_header(r->server, &header, DSI_DSICommand);
    __atomic_store_n(&r->result,
                     afpc_dsi_send_with_reply(r->server, (char *) &header,
                                              sizeof(header), 5, afpReadExt,
                                              NULL, NULL, &r->rx),
                     __ATOMIC_RELEASE);
    return NULL;
}

/*!
 * @brief A request a disconnect fails while the receiver fills its buffer
 *        stays its sender's until the receiver lets it go
 *
 * The receiver blocks in the read that fills the buffer; the sender, woken
 * by the failure, returns only once that read is done.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_failed_request_waits_for_its_receiver(void)
{
    struct session s;
    struct reader reader;
    struct receive_call call;
    struct dsi_header sent;
    char payload[200];
    pthread_t sender;
    pthread_t receiver;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    memset(&reader, 0, sizeof(reader));
    reader.server = s.server;
    reader.result = 1;
    memset(payload, 'p', sizeof(payload));

    if (pthread_create(&sender, NULL, read_and_wait, &reader) != 0) {
        session_close(&s);
        return 2;
    }

    /* The first 50 bytes of the reply land; the rest are on their way. */
    if (read(s.peer, &sent, sizeof(sent)) != (ssize_t) sizeof(sent)
            || send_header(s.peer, DSI_REPLY, DSI_DSICommand,
                           ntohs(sent.requestid), sizeof(payload)) != 0
            || write(s.peer, payload, 50) != 50
            || afpc_dsi_recv(s.server) != 0
            || afpc_dsi_recv(s.server) != 0) {
        ret = 3;
        goto out;
    }

    call.server = s.server;
    call.started = 0;
    call.result = 1;

    if (pthread_create(&receiver, NULL, receive_once, &call) != 0) {
        ret = 4;
        goto out;
    }

    wait_for_receiver(&call);
    afpc_dsi_fail_request_queue(s.server, -EIO);
    sleep_ms(200);

    if (__atomic_load_n(&reader.result, __ATOMIC_ACQUIRE) != 1) {
        ret = 5;
    }

    if (write(s.peer, payload + 50, sizeof(payload) - 50)
            != (ssize_t)(sizeof(payload) - 50) && ret == 0) {
        ret = 6;
    }

    pthread_join(receiver, NULL);
out:

    if (ret == 3) {
        afpc_dsi_fail_request_queue(s.server, -EIO);
    }

    pthread_join(sender, NULL);

    if (ret == 0 && reader.result != -EIO) {
        ret = 7;
    } else if (ret == 0 && reader.rx.size != sizeof(payload)) {
        ret = 8;
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
            || connect(*client, (struct sockaddr *) &l->addr, sizeof(l->addr))
            != 0) {
        close(*client);
        return -1;
    }

    *peer = accept(l->fd, NULL, NULL);

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
 * @brief A tickle waits for no room: a connection that takes no more skips it
 *
 * The session's socket pair gives way to a TCP connection with small
 * buffers, filled until it takes no more.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_tickle_waits_for_no_room(void)
{
    struct session s;
    struct listener l;
    struct tickler t;
    pthread_t thread;
    int client;
    int peer;
    int rc;
    int ret = 0;

    if (session_open(&s, 4096) != 0) {
        return 1;
    }

    if (listener_open(&l) != 0) {
        session_close(&s);
        return 2;
    }

    if (small_tcp_pair(&l, &client, &peer) != 0) {
        listener_close(&l);
        session_close(&s);
        return 3;
    }

    close(s.server->fd);
    close(s.peer);
    s.server->fd = client;
    s.peer = peer;

    if (fill_connection(client) != 0) {
        listener_close(&l);
        session_close(&s);
        return 4;
    }

    t.server = s.server;
    rc = tickle_returns(&t, &thread);
    drain_peer(peer);

    if (rc < 0) {
        ret = 5;
    } else {
        pthread_join(thread, NULL);
        ret = rc ? 6 : 0;
    }

    listener_close(&l);
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
    TEST_int(utest_answer_as_the_wait_expires_is_the_result(), 0,
             "an answer landing as the wait expires is the result");
    TEST_int(utest_failed_request_waits_for_its_receiver(), 0,
             "a failed request stays its sender's until the receiver is done");
    TEST_int(utest_tickle_waits_for_no_writer(), 0,
             "a tickle skips a frame on its way instead of waiting");
    TEST_int(utest_tickle_waits_for_no_room(), 0,
             "a tickle skips a full socket instead of waiting");
    return test_tap_finish();
}
