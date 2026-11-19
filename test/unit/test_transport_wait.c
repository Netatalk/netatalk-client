/*
 * test_transport_wait.c: a raw command waits as its timeout says
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit test of a raw command's wait against the fake AFP server
 */

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netatalk-client/transport.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "tap.h"

struct raw_in_thread {
    struct afpc_transport *transport;
    int timeout;
    int afp_result;
    int result;
};

/*!
 * @brief Drop a log line of the transport
 *
 * @param[in] context   unused
 * @param[in] loglevel  unused
 * @param[in] message   unused
 */
static void discard_log(void *context, int loglevel, const char *message)
{
    (void)context;
    (void)loglevel;
    (void)message;
}

/*!
 * @brief Send FPGetSrvrParms as a raw command, on a thread of its own
 *
 * @param[in,out] arg  the raw_in_thread, which takes the results
 * @returns NULL
 */
static void *send_raw(void *arg)
{
    struct raw_in_thread *r = arg;
    const unsigned char payload[2] = { afpGetSrvrParms, 0 };
    unsigned char reply[64];
    r->result = afpc_transport_raw_command(r->transport, payload,
                                           sizeof(payload), AFPC_DSI_COMMAND,
                                           0, r->timeout, &r->afp_result,
                                           reply, sizeof(reply), NULL);
    return NULL;
}

/*!
 * @brief Check a raw command's wait against the fake AFP server
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afpc_transport_options options;
    struct afpc_transport *transport = NULL;
    struct afp_fake_server fake;
    struct raw_in_thread r;
    pthread_t sender;
    test_tap_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    CHECK(afpc_transport_init(discard_log, NULL) == 0);
    memset(&fake, 0, sizeof(fake));
    fake.status_flags = kSrvrSig | kSupportsTCP | kSupportsUTF8SrvrName;
    CHECK(afp_fake_server_listen(&fake) == 0);
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = ntohs(fake.listen_addr.sin_port);
    options.uam_mask = AFPC_UAM_NO_USER_AUTH;
    CHECK(afpc_transport_connect(&options, &transport) == 0);
    /* Every negative timeout has no limit, -2 too, the send's sentinel for
     * the server's default: the answer comes after Netatalk's 5 s. */
    fake.hold = afpGetSrvrParms;
    r.transport = transport;
    r.timeout = -2;
    r.afp_result = 1;
    r.result = 1;
    CHECK(pthread_create(&sender, NULL, send_raw, &r) == 0);
    CHECK(afp_fake_server_wait_held(&fake) == 0);
    sleep(6);
    afp_fake_server_release(&fake);
    pthread_join(sender, NULL);
    CHECK(r.result == 0 && r.afp_result == 0);
    afpc_transport_close(&transport);
    afp_fake_server_stop_listening(&fake);
    return test_tap_finish();
}
