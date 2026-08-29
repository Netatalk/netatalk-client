/*
 * test_reconnect.c: a reconnect continues the session it lost, or starts
 * a new one without the old one's forks
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of a reconnect against the fake AFP server
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_fake_server.h"
#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/afp_replies.h"
#include "lib/client.h"
#include "lib/dsi_protocol.h"
#include "lib/forklist.h"
#include "lib/lowlevel.h"
#include "lib/midlevel.h"
#include "lib/uam_registry.h"
#include "tap.h"

#define FAKE_FLAGS (kSrvrSig | kSupportsTCP | kSupportsUTF8SrvrName)

struct write_in_thread {
    struct afp_volume *volume;
    struct afp_file_info *fp;
    int result;
};

struct params_in_thread {
    struct afp_server *server;
    int result;
};

struct copy_in_thread {
    struct afp_volume *volume;
    struct afp_file_info *in;
    struct afp_file_info *out;
};

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
 * @brief Wait up to a second for the fake server to receive a logout
 *
 * @param[in] fake  a listening fake server
 * @returns the logouts it received
 */
static unsigned int wait_for_logout(struct afp_fake_server *fake)
{
    for (int i = 0; i < 100 && afp_fake_server_count(fake, afpLogout) == 0;
            i++) {
        sleep_ms(10);
    }

    return afp_fake_server_count(fake, afpLogout);
}

/*!
 * @brief Wait up to a second for a request to join a server's queue
 *
 * @param[in] server  the server
 * @returns the requests queued
 */
static uint64_t wait_for_queued(struct afp_server *server)
{
    uint64_t pending = 0;

    for (int i = 0; i < 100 && pending == 0; i++) {
        pthread_mutex_lock(&server->request_queue_mutex);
        pending = server->stats.requests_pending;
        pthread_mutex_unlock(&server->request_queue_mutex);

        if (pending == 0) {
            sleep_ms(10);
        }
    }

    return pending;
}

/*!
 * @brief Write 16 bytes through the file of a write_in_thread, once
 *
 * @param[in,out] arg  the write_in_thread, which receives the result
 * @returns NULL
 */
static void *write_once(void *arg)
{
    struct write_in_thread *w = arg;
    char data[16] = "";
    size_t written = 0;
    w->result = ll_write(w->volume, data, sizeof(data), 0, 0, w->fp,
                         &written);
    return NULL;
}

/*!
 * @brief Ask for the server parameters of a params_in_thread, once
 *
 * @param[in,out] arg  the params_in_thread, which receives the result
 * @returns NULL
 */
static void *params_once(void *arg)
{
    struct params_in_thread *p = arg;
    p->result = afp_getsrvrparms(p->server);
    return NULL;
}

/*!
 * @brief Copy the first MiB of a copy_in_thread's source, once
 *
 * @param[in] arg  the copy_in_thread
 * @returns NULL
 */
static void *copy_once(void *arg)
{
    const struct copy_in_thread *c = arg;
    (void) ml_copy_range_within(c->volume, c->in, 0, c->out, 0, 1U << 20,
                                SIZE_MAX);
    return NULL;
}

/*!
 * @brief Connect to the fake server as a guest and open a file on its volume
 *
 * @param[in]  fake    a listening fake server
 * @param[out] volume  the attached volume
 * @param[out] fp      the open file
 * @returns the connected server
 */
static struct afp_server *open_session(struct afp_fake_server *fake,
                                       struct afp_volume **volume,
                                       struct afp_file_info **fp)
{
    struct afp_connection_request request;
    struct afp_server *server;
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    memset(&request, 0, sizeof(request));
    afp_default_url(&request.url);
    snprintf(request.url.servername, sizeof(request.url.servername),
             "127.0.0.1");
    request.url.port = ntohs(fake->listen_addr.sin_port);
    request.uam_mask = UAM_NOUSERAUTHENT;
    server = afp_server_full_connect(NULL, &request);
    CHECK(server != NULL);
    *volume = find_volume_by_name(server, "vol");
    CHECK(*volume != NULL);
    CHECK(afp_connect_volume(*volume, server, mesg, &len, sizeof(mesg))
          == 0);
    CHECK(ml_open(*volume, "/file", O_RDWR, fp) == 0);
    CHECK((*fp)->forkid == AFP_FAKE_SERVER_FORK);
    return server;
}

/*!
 * @brief End the connection from the server, and wait for the loop to see it
 *
 * @param[in,out] fake    the fake server
 * @param[in]     server  the connected server
 */
static void drop_session(struct afp_fake_server *fake,
                         struct afp_server *server)
{
    afp_fake_server_drop(fake);

    for (int i = 0; i < 300
            && __atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)
            != SERVER_STATE_DISCONNECTED; i++) {
        sleep_ms(10);
    }

    CHECK(server->connect_state == SERVER_STATE_DISCONNECTED);
}

/*!
 * @brief Close a file and the session, and stop the fake server
 *
 * @param[in,out] fake    the fake server
 * @param[in,out] server  the connected server, which is removed
 * @param[in,out] volume  the file's volume
 * @param[in,out] fp      the open file, which is freed
 */
static void close_session(struct afp_fake_server *fake,
                          struct afp_server *server,
                          struct afp_volume *volume, struct afp_file_info *fp)
{
    ml_close(volume, fp);
    free(fp);
    afp_logout(server, 1);
    afp_server_remove(server);
    afp_fake_server_stop_listening(fake);
}

/*!
 * @brief Check the reconnect of a session against the fake AFP server
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afp_fake_server fake;
    struct afp_server *server;
    struct afp_volume *volume;
    struct afp_file_info *fp;
    struct afp_file_info *again;
    char data[16] = "";
    size_t written;
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    CHECK(init_uams() == 0);
    CHECK(afp_main_quick_startup(NULL) == 0);
    {
        /* Netatalk continues the lost session with the first login's token:
         * the write in hand keeps its fork, and nothing is opened again. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        CHECK(server->token.length == sizeof(AFP_FAKE_SERVER_TOKEN) - 1);
        /* The desktop reference the session opened stays its own too. */
        volume->dtrefnum = 1;
        drop_session(&fake, server);
        written = 0;
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == 0);
        CHECK(written == sizeof(data));
        CHECK(fp->forkid == AFP_FAKE_SERVER_FORK);
        CHECK(volume->dtrefnum == 1);
        CHECK(server->connect_state == SERVER_STATE_CONNECTED);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 1);
        CHECK(afp_fake_server_count(&fake, afpGetSessionToken) == 1);
        CHECK(afp_fake_server_count(&fake, afpLoginExt) == 2);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 1);
        CHECK(afp_fake_server_count(&fake, afpOpenFork) == 1);
        CHECK(afp_fake_server_count(&fake, afpWriteExt) == 1);

        /* The connect restarted the request IDs below the one the write took
         * before it; the requests after pass that one. */
        for (int i = 0; i < 16; i++) {
            CHECK(afp_getsrvrparms(server) == 0);
        }

        close_session(&fake, server, volume, fp);
        CHECK(fake.repeated_ids == 0);
    }
    {
        /* With afpd's replay cache, request IDs run on across the reconnect,
         * as the cache would answer a repeated one with its old reply. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.replay_cache = 1;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        CHECK(server->supports_replay_cache);
        drop_session(&fake, server);
        written = 0;
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 1);

        for (int i = 0; i < 16; i++) {
            CHECK(afp_getsrvrparms(server) == 0);
        }

        close_session(&fake, server, volume, fp);
        CHECK(fake.repeated_ids == 0);
    }
    {
        /* A reconnect cut off before its continue is answered fails, keeping
         * the token and fork for the next; a write meanwhile fails at once. */
        struct write_in_thread w;
        pthread_t writer;
        struct timespec start;
        struct timespec end;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.hold = afpDisconnectOldSession;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        w.volume = volume;
        w.fp = fp;
        CHECK(pthread_create(&writer, NULL, write_once, &w) == 0);
        CHECK(afp_fake_server_wait_held(&fake) == 0);
        clock_gettime(CLOCK_MONOTONIC, &start);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        clock_gettime(CLOCK_MONOTONIC, &end);
        CHECK(end.tv_sec - start.tv_sec < 2);
        afp_fake_server_drop(&fake);
        afp_fake_server_release(&fake);
        pthread_join(writer, NULL);
        CHECK(w.result == -EIO);
        CHECK(fp->forkid == AFP_FAKE_SERVER_FORK);
        CHECK(server->token.length == sizeof(AFP_FAKE_SERVER_TOKEN) - 1);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 2);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 1);
        CHECK(afp_fake_server_count(&fake, afpWriteExt) == 1);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A refused token starts a new session on a connection of its own:
         * the old file's writes fail with EIO unsent, and a new open works. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.refuse = afpDisconnectOldSession;
        fake.refusal = kFPMiscErr;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        volume->dtrefnum = 1;
        drop_session(&fake, server);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(fp->forkid == 0);
        CHECK(volume->dtrefnum == 0);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(afp_fake_server_count(&fake, afpWriteExt) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 1);
        CHECK(afp_fake_server_count(&fake, afpLoginExt) == 3);
        CHECK(afp_fake_server_count(&fake, afpGetSessionToken) == 2);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 2);
        CHECK(volume->open_forks == NULL);
        CHECK(volume->writable_forks == 0);
        CHECK(ml_open(volume, "/file", O_RDWR, &again) == 0);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, again, &written) == 0);
        CHECK(afp_fake_server_count(&fake, afpWriteExt) == 1);
        CHECK(ml_close(volume, again) == 0);
        free(again);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A detach after a drop leaves the desktop reference unclosed, as
         * its close would reconnect to send it. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        ml_close(volume, fp);
        free(fp);
        volume->dtrefnum = 1;
        afp_server_hold(server);
        drop_session(&fake, server);
        CHECK(afp_detach_volume(volume) == 0);
        CHECK(afp_fake_server_count(&fake, afpCloseDT) == 0);
        CHECK(afp_fake_server_count(&fake, afpLoginExt) == 1);
        CHECK(volume->dtrefnum == 0);
        afp_server_release(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* Without kSupportsReconnect, a server other than Netatalk is asked
         * for no token, and its reconnect never asks to continue. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.machine_type = "TimeCapsule8,119";
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        CHECK(server->token.length == 0);
        drop_session(&fake, server);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(afp_fake_server_count(&fake, afpGetSessionToken) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 0);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 2);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A new session whose volume does not open logs out and takes no
         * token, so the next reconnect starts another and opens the volume. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        /* The server refuses a token it did not give. */
        server->token.data[0] ^= 1;
        fake.refuse = afpOpenVol;
        drop_session(&fake, server);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(server->token.length == 0);
        CHECK(wait_for_logout(&fake) == 1);
        fake.refuse = 0;
        CHECK(afp_getsrvrparms(server) != 0);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 3);
        CHECK(server->connect_state == SERVER_STATE_CONNECTED);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A reconnect whose login the server refuses fails with EACCES,
         * whatever the connect before it left in errno. */
        char mesg[MAX_ERROR_LEN];
        unsigned int len = 0;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        fake.refuse = afpLoginExt;
        errno = 0;
        CHECK(afp_server_reconnect(server, mesg, &len, sizeof(mesg)) == 1);
        CHECK(errno == EACCES);
        fake.refuse = 0;
        close_session(&fake, server, volume, fp);
    }
    {
        /* A request on an ended session's fork number fails with EIO unsent,
         * whatever the record holds, as a new session may reuse the number. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        afp_server_next_session_generation(server);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(afp_fake_server_count(&fake, afpWriteExt) == 0);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A fork from an ended session is not listed and its record loses it,
         * as the reconnect that overtook its open cleared the listed ones. */
        struct afp_file_info late;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        memset(&late, 0, sizeof(late));
        late.forkid = AFP_FAKE_SERVER_FORK + 1;
        late.session = afp_server_session_generation(server);
        afp_server_next_session_generation(server);
        CHECK(add_opened_fork(volume, &late) == -1);
        CHECK(late.forkid == 0);
        CHECK(volume->open_forks == fp && fp->largelist_next == NULL);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A request past the send's first test when another thread's reconnect
         * begins is not written: the test is made again with the frame. */
        struct params_in_thread p;
        pthread_t asker;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        p.server = server;
        pthread_mutex_lock(&server->send_mutex);
        CHECK(pthread_create(&asker, NULL, params_once, &p) == 0);
        /* Queued, the request is past the send's first test, and waits for
         * its frame's turn. */
        CHECK(wait_for_queued(server) == 1);
        CHECK(afp_server_reconnect_try_begin(server));
        pthread_mutex_unlock(&server->send_mutex);
        pthread_join(asker, NULL);
        afp_server_reconnect_end(server);
        CHECK(p.result == -EIO);
        CHECK(afp_fake_server_count(&fake, afpGetSrvrParms) == 1);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A close while the session is suspended never leaves; the reconnect
         * that continues the session closes the fork the server kept open. */
        char mesg[MAX_ERROR_LEN];
        unsigned int len = 0;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        afp_server_set_suspended(server, 1);
        drop_session(&fake, server);
        CHECK(ml_close(volume, fp) == -EIO);
        free(fp);
        CHECK(afp_fake_server_count(&fake, afpCloseFork) == 0);
        CHECK(server->num_unclosed_forks == 1);
        afp_server_set_suspended(server, 0);
        CHECK(afp_server_reconnect(server, mesg, &len, sizeof(mesg)) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 1);
        CHECK(afp_fake_server_count(&fake, afpCloseFork) == 1);
        CHECK(server->num_unclosed_forks == 0);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A kept close outlives the volume list a server parameters reply
         * replaces once no volume is in use; the reconnect sends it. */
        char mesg[MAX_ERROR_LEN];
        unsigned int len = 0;
        struct {
            struct dsi_header header __attribute__((__packed__));
            uint32_t time;
            uint8_t numvolumes;
            uint8_t flags;
            char name[4];
        } __attribute__((__packed__)) parms_reply;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        afp_server_set_suspended(server, 1);
        drop_session(&fake, server);
        CHECK(ml_close(volume, fp) == -EIO);
        free(fp);
        volume->attached = AFP_VOLUME_DETACHED;
        volume->mounted = AFP_VOLUME_UNMOUNTED;
        memset(&parms_reply, 0, sizeof(parms_reply));
        parms_reply.numvolumes = 1;
        memcpy(parms_reply.name, "\x03vol", 4);
        CHECK(afp_getsrvrparms_reply(server, (char *) &parms_reply,
                                     sizeof(parms_reply), NULL) == 0);
        CHECK(server->volumes != volume);
        afp_server_set_suspended(server, 0);
        CHECK(afp_server_reconnect(server, mesg, &len, sizeof(mesg)) == 0);
        CHECK(afp_fake_server_count(&fake, afpCloseFork) == 1);
        afp_logout(server, 1);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A connect whose server parameters the server refuses after the
         * login logs out, as afpd keeps a session whose connection ends. */
        struct afp_connection_request request;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.refuse = afpGetSrvrParms;
        CHECK(afp_fake_server_listen(&fake) == 0);
        memset(&request, 0, sizeof(request));
        afp_default_url(&request.url);
        snprintf(request.url.servername, sizeof(request.url.servername),
                 "127.0.0.1");
        request.url.port = ntohs(fake.listen_addr.sin_port);
        request.uam_mask = UAM_NOUSERAUTHENT;
        CHECK(afp_server_full_connect(NULL, &request) == NULL);
        CHECK(wait_for_logout(&fake) == 1);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A request to continue whose wait expires counts as refused: its
         * connection carries a logout, and a new session logs in on another. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.hold = afpDisconnectOldSession;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(fp->forkid == 0);
        CHECK(server->stats.commands[afpLogout] == 1);
        /* The held connection reads nothing until its release, so a logout
         * counted here went out on another connection. */
        CHECK(afp_fake_server_count(&fake, afpLogout) == 0);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 1);
        CHECK(afp_fake_server_count(&fake, afpLoginExt) == 3);
        CHECK(server->connect_state == SERVER_STATE_CONNECTED);
        afp_fake_server_release(&fake);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A session the server ended has no token to hand back: the
         * reconnect starts a new session without asking to continue. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        __atomic_store_n(&server->token_ended, 1, __ATOMIC_RELEASE);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 0);
        CHECK(fp->forkid == 0);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A logout whose reconnect starts a new session goes out there and
         * ends that session, as it names nothing of the old one. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        fake.refuse = afpDisconnectOldSession;
        fake.refusal = kFPMiscErr;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        CHECK(afp_logout(server, 1) == 0);
        CHECK(wait_for_logout(&fake) == 1);
        ml_close(volume, fp);
        free(fp);
        afp_server_remove(server);
        afp_fake_server_stop_listening(&fake);
    }
    {
        /* A new session that forgets a copy's target while the server copies
         * finds the target listed again, with the fork the copy reopens. */
        struct copy_in_thread c;
        pthread_t copier;
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS | kSupportsCopyfile;
        fake.file_params = 1;
        fake.params_cnid = 7;
        fake.data_length = 1U << 20;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        CHECK(ml_open(volume, "/target", O_WRONLY | O_CREAT, &again) == 0);
        fake.params_empty = (uint16_t)(1U << (fake.params_answers + 1));
        fake.hold = afpCopyFile;
        again->size = 0;
        c.volume = volume;
        c.in = fp;
        c.out = again;
        CHECK(pthread_create(&copier, NULL, copy_once, &c) == 0);
        CHECK(afp_fake_server_wait_held(&fake) == 0);
        afp_server_next_session_generation(server);
        forget_open_forks(volume);
        afp_fake_server_release(&fake);
        pthread_join(copier, NULL);
        CHECK(afp_fake_server_count(&fake, afpCopyFile) == 1);
        CHECK(volume->open_forks == again);
        CHECK(volume->writable_forks == 1);
        ml_close(volume, again);
        free(again);
        close_session(&fake, server, volume, fp);
    }
    {
        /* A token request whose own reconnect starts a new session leaves
         * the token that session took. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        drop_session(&fake, server);
        afp_server_take_token(server);
        CHECK(server->token.length == sizeof(AFP_FAKE_SERVER_TOKEN) - 1);
        CHECK(afp_fake_server_count(&fake, afpGetSessionToken) == 2);
        CHECK(afp_fake_server_count(&fake, afpDisconnectOldSession) == 0);
        close_session(&fake, server, volume, fp);
    }
    {
        /* An unmount's claim, as afp_unmount_volume() holds it while FUSE
         * unmounts, outlives a reconnect that reopens the volume in a new
         * session: FUSE's destroy then starts no second teardown. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        volume->mounted = AFP_VOLUME_UNMOUNTING;
        drop_session(&fake, server);
        __atomic_store_n(&server->token_ended, 1, __ATOMIC_RELEASE);
        CHECK(ll_write(volume, data, sizeof(data), 0, 0, fp, &written) == -EIO);
        CHECK(fp->forkid == 0);
        CHECK(volume->mounted == AFP_VOLUME_UNMOUNTING);
        CHECK(afp_unmount_volume(volume) == -1);
        close_session(&fake, server, volume, fp);
    }
    {
        /* An unmount whose fork close reconnects into a new session leaves
         * its volume closed and unmounted, so a second unmount does nothing.
         * The hold keeps the removed server's volume to look at. */
        memset(&fake, 0, sizeof(fake));
        fake.status_flags = FAKE_FLAGS;
        CHECK(afp_fake_server_listen(&fake) == 0);
        server = open_session(&fake, &volume, &fp);
        snprintf(volume->mountpoint, sizeof(volume->mountpoint), "/mnt/afp");
        afp_server_hold(server);
        drop_session(&fake, server);
        __atomic_store_n(&server->token_ended, 1, __ATOMIC_RELEASE);
        CHECK(afp_unmount_volume(volume) == 0);
        CHECK(afp_fake_server_count(&fake, afpLoginExt) == 2);
        CHECK(afp_fake_server_count(&fake, afpOpenVol) == 1);
        CHECK(volume->mounted == AFP_VOLUME_UNMOUNTED);
        CHECK(volume->attached == AFP_VOLUME_DETACHED);
        CHECK(afp_unmount_volume(volume) == -1);
        afp_server_release(server);
        afp_fake_server_stop_listening(&fake);
    }
    return test_tap_finish();
}
