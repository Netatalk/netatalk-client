/*
    test_status_text.c: the status text of a server

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "lib/afp_internal.h"
#include "tap.h"

static struct afp_versions afp34 = { "AFP3.4", 34 };

/*!
 * @brief Set up a server known by its name and nothing else yet
 *
 * @param[out] server  receives the server
 */
static void server_named(struct afp_server *server)
{
    memset(server, 0, sizeof(*server));
    pthread_mutex_init(&server->request_queue_mutex, NULL);
    snprintf(server->server_name_printable,
             sizeof(server->server_name_printable), "Netatalk File Server");
}

/*!
 * @brief Make the loopback address at port 548 in the caller's storage
 *
 * @param[out] ai   receives the address
 * @param[out] sin  the storage it points at
 */
static void loopback_548(struct addrinfo *ai, struct sockaddr_in *sin)
{
    memset(sin, 0, sizeof(*sin));
    memset(ai, 0, sizeof(*ai));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin->sin_port = htons(548);
    ai->ai_family = AF_INET;
    ai->ai_addr = (struct sockaddr *) sin;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_protocol = IPPROTO_TCP;
}

/*!
 * @brief Set up a server connected to the loopback address with AFP 3.4
 *
 * @param[out] server  receives the server
 * @param[out] ai      receives its address
 * @param[out] sin     the storage the address points at
 */
static void server_connected(struct afp_server *server, struct addrinfo *ai,
                             struct sockaddr_in *sin)
{
    server_named(server);
    loopback_548(ai, sin);
    snprintf(server->loginmesg, sizeof(server->loginmesg), "Welcome");
    snprintf(server->machine_type, sizeof(server->machine_type),
             "Netatalk4.6.1");
    server->used_address = ai;
    server->using_version = &afp34;
    server->connect_state = SERVER_STATE_CONNECTED;
}

/*!
 * @brief The status text ends within a 96-byte buffer
 *
 * It takes at most 95 bytes and the terminator, and nothing lands past it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_text_ends_within_its_buffer(void)
{
    struct afp_server server;
    struct addrinfo ai;
    struct sockaddr_in sin;
    char text[96 + 256];
    int len = 96;
    int pos;
    server_connected(&server, &ai, &sin);
    memset(text, 'C', sizeof(text));
    pos = afp_status_server(&server, text, &len);

    if (pos < 0 || pos >= 96) {
        return 1;
    }

    if (len != 96 - pos) {
        return 2;
    }

    if (text[95] != '\0') {
        return 3;
    }

    for (size_t i = 96; i < sizeof(text); i++) {
        if (text[i] != 'C') {
            return 4;
        }
    }

    return 0;
}

/*!
 * @brief The connection line names the address's port
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_text_names_the_port(void)
{
    struct afp_server server;
    struct addrinfo ai;
    struct sockaddr_in sin;
    char text[512];
    int len = sizeof(text);
    server_connected(&server, &ai, &sin);

    if (afp_status_server(&server, text, &len) < 0) {
        return 1;
    }

    return strstr(text, "127.0.0.1:548 (active)") != NULL ? 0 : 2;
}

/*!
 * @brief A server on its first connect reads as connecting
 *
 * Until its DSI session opens, it has an address in use but no version.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_connecting_server_names_its_state(void)
{
    struct afp_server connecting;
    struct addrinfo ai;
    struct sockaddr_in sin;
    char text[256];
    int len = sizeof(text);
    server_named(&connecting);
    loopback_548(&ai, &sin);
    connecting.used_address = &ai;
    connecting.connect_state = SERVER_STATE_CONNECTING;

    if (afp_status_server(&connecting, text, &len) <= 0) {
        return 1;
    }

    return strstr(text, "(connecting)") != NULL ? 0 : 2;
}

/*!
 * @brief A server whose status read failed reads as disconnected
 *
 * It has an address and no version.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_failed_server_reads_disconnected(void)
{
    struct afp_server failed;
    struct addrinfo ai;
    struct sockaddr_in sin;
    char text[256];
    int len = sizeof(text);
    server_named(&failed);
    loopback_548(&ai, &sin);
    failed.used_address = &ai;
    failed.connect_state = SERVER_STATE_DISCONNECTED;

    if (afp_status_server(&failed, text, &len) <= 0) {
        return 1;
    }

    return strstr(text, "(disconnected)") != NULL ? 0 : 2;
}

/*!
 * @brief A connected server with no address reads as active
 *
 * A replacement cleared its address as another thread's reconnect ran, so it
 * has a version and no address.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_server_without_address_reads_active(void)
{
    struct afp_server active;
    char text[256];
    int len = sizeof(text);
    server_named(&active);
    active.using_version = &afp34;
    active.connect_state = SERVER_STATE_CONNECTED;

    if (afp_status_server(&active, text, &len) <= 0) {
        return 1;
    }

    return strstr(text, "(active)") != NULL ? 0 : 2;
}

/*!
 * @brief A 16-character machine type arrives terminated
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_machine_type_arrives_terminated(void)
{
    struct afp_server server;
    struct addrinfo ai;
    struct sockaddr_in sin;
    struct afpc_server_info info;
    server_connected(&server, &ai, &sin);
    snprintf(server.machine_type, sizeof(server.machine_type), "%s",
             "Netatalk4.7.0dev");
    afp_server_fill_basic(&server, &info);

    if (memchr(info.machine_type, '\0', sizeof(info.machine_type)) == NULL) {
        return 1;
    }

    return strncmp(info.machine_type, "Netatalk4.7.0de", 15) == 0 ? 0 : 2;
}

/*!
 * @brief Run the status text checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    TEST_int(utest_text_ends_within_its_buffer(), 0,
             "the status text ends within a 96-byte buffer");
    TEST_int(utest_text_names_the_port(), 0,
             "the connection line names the address's port");
    TEST_int(utest_connecting_server_names_its_state(), 0,
             "a server on its first connect reads as connecting");
    TEST_int(utest_failed_server_reads_disconnected(), 0,
             "a server whose status read failed reads as disconnected");
    TEST_int(utest_server_without_address_reads_active(), 0,
             "a connected server with no address reads as active");
    TEST_int(utest_machine_type_arrives_terminated(), 0,
             "a 16-character machine type arrives terminated");
    return test_tap_finish();
}
