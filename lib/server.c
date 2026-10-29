/*
 *  server.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The steps of a connection after the socket: session, version, login
 */

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "afp_internal.h"
#include "client.h"
#include "codepage.h"
#include "compat.h"
#include "dsi.h"
#include "uam_registry.h"
#include "users.h"
#include "utils.h"

/*!
 * @brief Open the DSI session, log in, and read the server's parameters
 *
 * The password is wiped as soon as the login returns, whether or not it
 * succeeded.
 *
 * @param[in]     priv               the caller's context for log messages
 * @param[in,out] server             server with a connected socket
 * @param[in]     versions           the AFP versions the server offers
 * @param[in]     uams               the UAMs the server offers
 * @param[in]     username           the user to log in as
 * @param[in]     password           the user's password
 * @param[in]     requested_version  the highest AFP version to use, 0 for
 *                                   the highest both sides support
 * @param[in]     uam_mask           the UAMs the caller allows
 * @returns server, or NULL with errno set and the server removed
 */
struct afp_server *afp_server_complete_connection(
    void *priv,
    struct afp_server * server,
#if 0
    struct addrinfo * address,
#endif
    unsigned char *versions,
    unsigned int uams, char *username, char *password,
    unsigned int requested_version, unsigned int uam_mask)
{
    char loginmsg[AFP_LOGINMESG_LEN];
    int using_uam;
    int connection_error;
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    memset(loginmsg, 0, AFP_LOGINMESG_LEN);
    server->requested_version = requested_version;
    strlcpy(server->username, username, sizeof(server->username));
    strlcpy(server->password, password, sizeof(server->password));
    add_fd_and_signal(server->fd);

    if (afpc_dsi_opensession(server) != 0) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not open DSI session");
        errno = ECONNRESET;
        goto error;
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "afp_server_complete_connection -- DSI session: quantum=%u",
                   server->quantum);

    /* Figure out what version we're using */
    if (((server->using_version =
                pick_version(versions, requested_version)) == NULL)) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Server cannot handle AFP version %d",
                       requested_version);
        errno = EPROTONOSUPPORT;
        goto error;
    }

    using_uam = pick_uam(uams, uam_mask);

    if (using_uam == -1) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not pick a matching UAM");
        errno = EPROTONOSUPPORT;
        goto error;
    }

    server->using_uam = using_uam;

    if (afp_server_login(server, mesg, &len, MAX_ERROR_LEN)) {
        explicit_bzero(server->password, sizeof(server->password));
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Login error: %s", mesg);
        errno = EACCES;
        goto error;
    }

    explicit_bzero(server->password, sizeof(server->password));

    if (afp_getsrvrparms(server)) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not get server parameters");
        errno = EPROTO;
        goto error;
    }

    /* If we haven't gotten a proper date back, so set it to the connect time. */
    if (server->connect_time == afp_date_to_unix(NULL, 0)) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        server->connect_time = tv.tv_sec;
    }

    afp_getsrvrmsg(server, AFPMESG_LOGIN,
                   ((server->using_version && server->using_version->av_number >= 30) ? 1 : 0),
                   DSI_DEFAULT_TIMEOUT, loginmsg); /* block */

    if (strlen(loginmsg) > 0)
        log_for_client(priv, AFPFSD, LOG_NOTICE,
                       "Login message: %s", loginmsg);

    memcpy(server->loginmesg, loginmsg, AFP_LOGINMESG_LEN);
    /* A removal, or a disconnect no reconnect repaired, while the login
     * ran took the descriptor or changed the state under the list lock;
     * the connection then fails. A reconnect a request of the login ran
     * published a descriptor of its own and left it connected. */
    afp_lock_server_list();

    if (__atomic_load_n(&server->fd, __ATOMIC_ACQUIRE) < 0 ||
            server->connect_state == SERVER_STATE_DISCONNECTED ||
            server->connect_state == SERVER_STATE_DESTROYED) {
        afp_unlock_server_list();
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Connection to %s ended during the login",
                       server->server_name_printable);
        errno = EIO;
        goto error;
    }

    __atomic_store_n(&server->connect_state, SERVER_STATE_CONNECTED,
                     __ATOMIC_RELEASE);
    afp_unlock_server_list();
    return server;
error:
    connection_error = errno;
    afp_server_remove(server);
    errno = connection_error;
    return NULL;
}
