/*
 * test_connect.c: the connect of a server, its status and its session
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of a server's connect, its status and its session,
 *        against the fake AFP server
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_fake_server.h"
#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/uam_registry.h"
#include "tap.h"

/* The flags a status reply of the fake server carries in these cases. */
#define FAKE_FLAGS (kSupportsCopyfile | kSupportsSrvrMsg | kSrvrSig \
                    | kSupportsTCP | kSupportsUTF8SrvrName | kSupportsReconnect)

/*!
 * @brief Swallow a log line of the library
 *
 * @param[in] priv      unused
 * @param[in] logtype   unused
 * @param[in] loglevel  unused
 * @param[in] message   unused
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
 * @brief Connect to the fake server as a guest, as a mount does
 *
 * @param[in] fake  a listening fake server
 * @returns the connected server, or NULL with errno set
 */
static struct afp_server *connect_guest(const struct afp_fake_server *fake)
{
    struct afp_connection_request request;
    memset(&request, 0, sizeof(request));
    afp_default_url(&request.url);
    snprintf(request.url.servername, sizeof(request.url.servername),
             "127.0.0.1");
    request.url.port = ntohs(fake->listen_addr.sin_port);
    request.uam_mask = UAM_NOUSERAUTHENT;
    return afp_server_full_connect(NULL, &request);
}

/*!
 * @brief Milliseconds since a start on CLOCK_MONOTONIC
 *
 * @param[in] start  the start
 * @returns the milliseconds
 */
static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000
           + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/*!
 * @brief A thread that connects to the fake server once
 */
struct connector {
    const struct afp_fake_server *fake;  /*!< the listening fake server */
    struct afp_server *server;           /*!< the server it connected */
};

/*!
 * @brief Connect to the fake server once, for a thread
 *
 * @param[in,out] arg  the struct connector, which receives the server
 * @returns NULL
 */
static void *connect_once(void *arg)
{
    struct connector *c = arg;
    c->server = connect_guest(c->fake);
    return NULL;
}

/*!
 * @brief A request whose reply handler holds the loop until the test lets go
 */
struct held_request {
    struct afp_server *server;  /*!< the server asked */
    pthread_mutex_t lock;       /*!< guards entered and released */
    pthread_cond_t cond;        /*!< signalled as either rises */
    int entered;                /*!< whether the loop runs the handler */
    int released;               /*!< whether the test has let it go */
    int result;                 /*!< the request's result */
};

/*!
 * @brief Hold the loop thread in its receive, as a slow reply handler would
 *
 * @param[in]     server   unused
 * @param[in]     buffer   unused
 * @param[in]     size     unused
 * @param[in,out] context  the struct held_request
 * @returns 0
 */
static int hold_reply(struct afp_server *server, char *buffer,
                      unsigned int size, void *context)
{
    struct held_request *h = context;
    (void) server;
    (void) buffer;
    (void) size;
    pthread_mutex_lock(&h->lock);
    h->entered = 1;
    pthread_cond_broadcast(&h->cond);

    while (!h->released) {
        pthread_cond_wait(&h->cond, &h->lock);
    }

    pthread_mutex_unlock(&h->lock);
    return 0;
}

/*!
 * @brief Ask for the server parameters with the holding handler, for a thread
 *
 * @param[in,out] arg  the struct held_request, which receives the result
 * @returns NULL
 */
static void *ask_held(void *arg)
{
    struct held_request *h = arg;
    struct dsi_header header;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
    } __attribute__((__packed__)) request;
    afpc_dsi_setup_header(h->server, &header, DSI_DSICommand);
    memcpy(&request.header, &header, sizeof(header));
    request.command = afpGetSrvrParms;
    h->result = afpc_dsi_send_with_reply(h->server, (char *) &request,
                                         sizeof(request), DSI_DEFAULT_TIMEOUT,
                                         afpGetSrvrParms, h, hold_reply, NULL);
    return NULL;
}

/*!
 * @brief Wait up to 5 s for the loop to run the holding handler
 *
 * @param[in,out] h  the request
 * @returns 1 once the loop runs it, 0 otherwise
 */
static int wait_entered(struct held_request *h)
{
    struct timespec deadline;
    int entered;
    int rc = 0;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&h->lock);

    while (!h->entered && rc == 0) {
        rc = pthread_cond_timedwait(&h->cond, &h->lock, &deadline);
    }

    entered = h->entered;
    pthread_mutex_unlock(&h->lock);
    return entered;
}

/*!
 * @brief Let the holding handler return
 *
 * @param[in,out] h  the request
 */
static void release_held(struct held_request *h)
{
    pthread_mutex_lock(&h->lock);
    h->released = 1;
    pthread_cond_broadcast(&h->cond);
    pthread_mutex_unlock(&h->lock);
}

/*!
 * @brief Wait up to 2 s for a descriptor to close
 *
 * @param[in] fd  the descriptor
 * @returns 1 once it is closed, 0 while it stays open
 */
static int descriptor_closed(int fd)
{
    const struct timespec step = { 0, 10 * 1000000L };

    for (int i = 0; i < 200 && fcntl(fd, F_GETFD) != -1; i++) {
        nanosleep(&step, NULL);
    }

    return fcntl(fd, F_GETFD) == -1;
}

/*!
 * @brief Check the connect of a server against the fake AFP server
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afp_fake_server fake;
    struct afp_server *server;
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    CHECK(init_uams() == 0);
    CHECK(afp_main_quick_startup(NULL) == 0);
    {
        /* A connected server holds its status and the type its machine type
         * names; the status and the session took a connection each. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.server_name = "fake";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(server->connect_state == SERVER_STATE_CONNECTED);
        CHECK(server->flags == FAKE_FLAGS);
        CHECK(memcmp(server->signature, AFP_FAKE_SERVER_SIGNATURE,
                     AFP_SIGNATURE_LEN) == 0);
        CHECK(strcmp(server->server_name_printable, "fake") == 0);
        CHECK(strcmp(server->machine_type, "Netatalk4.2.3") == 0);
        CHECK(server->server_type == AFPC_SERVER_TYPE_NETATALK);
        CHECK(server->server_version.major == 4
              && server->server_version.minor == 2
              && server->server_version.patch == 3);
        CHECK(server->supported_uams & UAM_NOUSERAUTHENT);
        CHECK(server->num_volumes == 1);
        CHECK(fake.accepted == 2);
        CHECK(afp_fake_server_count(&fake, afpLogin) == 1);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* The session's connection opens while the status is read: a status
         * reply held for a second connection goes out at once. */
        struct timespec start;
        unsigned int sent_after;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.hold_status_ms = 2000;
        CHECK(afp_fake_server_listen(&fake) == 0);
        clock_gettime(CLOCK_MONOTONIC, &start);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(elapsed_ms(&start) < 1500);
        pthread_mutex_lock(&fake.lock);
        sent_after = fake.status_sent_after;
        pthread_mutex_unlock(&fake.lock);
        CHECK(sent_after == 2);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* afpd at its cap refuses the session's connection unasked: the
         * connect closes the status's, then makes the session's again. */
        struct addrinfo address;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.hold_status_ms = 2000;
        fake.busy = AFP_FAKE_SERVER_BUSY_AT_ACCEPT;
        CHECK(afp_fake_server_listen(&fake) == 0);
        memset(&address, 0, sizeof(address));
        address.ai_family = AF_INET;
        address.ai_socktype = SOCK_STREAM;
        address.ai_protocol = IPPROTO_TCP;
        address.ai_addr = (struct sockaddr *) &fake.listen_addr;
        address.ai_addrlen = sizeof(fake.listen_addr);
        server = afp_server_init(&address);
        CHECK(server != NULL);
        CHECK(afp_server_connect(server, 1) == 0);
        CHECK(afpc_dsi_opensession(server) == 0);
        /* The address is the test's, not the server's to free. */
        server->address = NULL;
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
        CHECK(fake.refused == 1 && fake.accepted == 2);
    }
    {
        /* afpd at its cap answers the status's connection busy too: the probe
         * fails with EBUSY, not EPROTO. */
        struct afp_server_status busy_status;
        struct addrinfo address;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.busy = AFP_FAKE_SERVER_BUSY_ALWAYS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        memset(&address, 0, sizeof(address));
        address.ai_family = AF_INET;
        address.ai_socktype = SOCK_STREAM;
        address.ai_protocol = IPPROTO_TCP;
        address.ai_addr = (struct sockaddr *) &fake.listen_addr;
        address.ai_addrlen = sizeof(fake.listen_addr);
        CHECK(afp_server_probe(&address, &busy_status, NULL) == -EBUSY);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* afpd at its cap answers busy a DSIOpenSession ahead of its refusal:
         * the session opens again on a new connection and logs in once. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.busy = AFP_FAKE_SERVER_BUSY_AT_OPEN;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(afp_fake_server_count(&fake, afpLogin) == 1);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
        CHECK(fake.refused == 1 && fake.accepted == 3);
    }
    {
        /* A refused session's new connection at another address of the name
         * fails with EPROTO before any session: it may be another server. */
        struct afp_fake_server other;
        struct addrinfo first;
        struct addrinfo second;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.busy = AFP_FAKE_SERVER_BUSY_AT_OPEN;
        CHECK(afp_fake_server_listen(&fake) == 0);
        memset(&other, 0, sizeof(other));
        other.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&other) == 0);
        memset(&second, 0, sizeof(second));
        second.ai_family = AF_INET;
        second.ai_socktype = SOCK_STREAM;
        second.ai_protocol = IPPROTO_TCP;
        second.ai_addr = (struct sockaddr *) &fake.listen_addr;
        second.ai_addrlen = sizeof(fake.listen_addr);
        first = second;
        first.ai_addr = (struct sockaddr *) &other.listen_addr;
        first.ai_next = &second;
        server = afp_server_init(&second);
        CHECK(server != NULL);
        CHECK(afp_server_connect(server, 1) == 0);
        server->address = &first;
        CHECK(afp_server_open_session(server) == -EPROTO);
        CHECK(server->used_address == &first);
        /* The addresses are the test's, not the server's to free. */
        server->address = NULL;
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
        afp_fake_server_stop_listening(&other);
    }
    {
        /* Ten servers connect at once, and every session is served. */
        struct connector connectors[10];
        pthread_t threads[10];
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);

        for (int i = 0; i < 10; i++) {
            connectors[i].fake = &fake;
            connectors[i].server = NULL;
            CHECK(pthread_create(&threads[i], NULL, connect_once,
                                 &connectors[i]) == 0);
        }

        for (int i = 0; i < 10; i++) {
            CHECK(pthread_join(threads[i], NULL) == 0);
            CHECK(connectors[i].server != NULL);
        }

        CHECK(fake.accepted == 20);

        for (int i = 0; i < 10; i++) {
            CHECK(afp_getsrvrparms(connectors[i].server) == 0);
        }

        for (int i = 0; i < 10; i++) {
            afp_logout(connectors[i].server, 1);
            afp_server_remove(connectors[i].server);
        }

        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A status reply that points past its end is refused, and the
         * connect fails with EPROTO. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.bad_status = 1;
        CHECK(afp_fake_server_listen(&fake) == 0);
        errno = 0;
        server = connect_guest(&fake);
        CHECK(server == NULL);
        CHECK(errno == EPROTO);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A server without a UTF-8 name is known by its Mac Roman name,
         * converted. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS & ~kSupportsUTF8SrvrName;
        fake.machine_type = "Macintosh";
        fake.server_name = "Caf\x8e";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(strcmp(server->server_name_printable, "Caf\xc3\xa9") == 0);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* afpd writes the network addresses' offset whatever its flags: its
         * status without kSupportsTCP still gives the UTF-8 name. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = (FAKE_FLAGS | kSupportsDirServices)
                            & ~kSupportsTCP;
        fake.server_name = "fake";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(strcmp(server->server_name_printable, "fake") == 0);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* afpd cuts its machine type where its version is long, so its
         * offsets are told by where the machine type starts. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = (FAKE_FLAGS | kSupportsDirServices)
                            & ~kSupportsTCP;
        fake.machine_type = "Netatal4.5.0beta";
        fake.afpd_offsets = 1;
        fake.server_name = "fake";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(strcmp(server->server_name_printable, "fake") == 0);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* Servers write the signature's offset whatever kSrvrSig says: a Mac's
         * status without the flag still gives it and the fields after it. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS & ~kSrvrSig;
        fake.machine_type = "Macintosh";
        fake.server_name = "mac";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        CHECK(memcmp(server->signature, AFP_FAKE_SERVER_SIGNATURE,
                     AFP_SIGNATURE_LEN) == 0);
        CHECK(strcmp(server->server_name_printable, "mac") == 0);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A disconnect while the loop runs a reply's handler leaves the
         * descriptor to the loop, which closes it once that receive is done:
         * a number the receive loaded never names the next connection. */
        struct held_request held;
        pthread_t asker;
        int old_fd;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = connect_guest(&fake);
        CHECK(server != NULL);
        memset(&held, 0, sizeof(held));
        pthread_mutex_init(&held.lock, NULL);
        pthread_cond_init(&held.cond, NULL);
        held.server = server;
        CHECK(pthread_create(&asker, NULL, ask_held, &held) == 0);
        CHECK(wait_entered(&held));
        old_fd = server->fd;
        loop_disconnect(server);
        CHECK(fcntl(old_fd, F_GETFD) != -1);
        release_held(&held);
        pthread_join(asker, NULL);
        CHECK(held.result == -EIO);
        CHECK(descriptor_closed(old_fd));
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
        pthread_cond_destroy(&held.cond);
        pthread_mutex_destroy(&held.lock);
    }
    return test_tap_finish();
}
