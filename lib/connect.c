/*
 *  connect.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief Address lookup and the full connection of a server, through login
 *
 * Part of the session layer: a new server's connect and login in one call.
 * Lane: session.
 * Northbound: the front ends.
 * Southbound: afp.c and server.c of its own layer.
 */

#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>

#include "afp_internal.h"
#include "client.h"
#include "codepage.h"
#include "dsi.h"
#include "uam_registry.h"
#include "users.h"
#include "utils.h"


struct addrinfo *afp_get_address(void *priv, const char *hostname,
                                 unsigned int port)
{
    char port_string[6];
    struct addrinfo hints;
    struct addrinfo * addresses;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_string, sizeof(port_string), "%u", port);
    int res = getaddrinfo(hostname, port_string, &hints, &addresses);

    if (res != 0) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not resolve %s.", hostname);
        return NULL;
    }

    return addresses;
}


/*!
 * @brief Connect to a server and log in, as a mount or a session needs
 *
 * @param[in]     priv  the caller's context for log messages
 * @param[in,out] req   the server's URL, the login it asks for, and its UAM
 *                      mask, which can change to guest
 * @returns the connected server, or NULL with errno set
 */
struct afp_server *afp_server_full_connect(void *priv,
                                           struct afp_connection_request *req)
{
    int ret;
    struct addrinfo * address;
    struct afp_server  * s;

    if ((address = afp_get_address(priv, req->url.servername,
                                   req->url.port)) == NULL) {
        errno = EHOSTUNREACH;
        return NULL;
    }

    if ((s = afp_server_init(address)) == NULL) {
        freeaddrinfo(address);
        errno = ENOMEM;
        return NULL;
    }

    /* AFP's advertised server name is not the authority the client used to
     * reach it.  Keep that authority so reconnect can recognize a changed
     * DNS endpoint without confusing it with an unrelated saved session. */
    strlcpy(s->requested_hostname, req->url.servername,
            sizeof(s->requested_hostname));
    s->requested_port = req->url.port;

    if ((ret = afp_server_connect(s, 1)) < 0) {
        log_for_client(priv, AFPFSD, LOG_INFO,
                       ret == -ETIMEDOUT
                       ? "Could not connect, no reply to the status request, %s"
                       : "Could not connect, %s", strerror(-ret));

        /* A server that never connected is not listed. */
        if (afp_server_remove(s) != 0) {
            afp_server_release(s);
        }

        errno = -ret;
        return NULL;
    }

    /* if our user and password strings are both empty, or if the username
     * is "nobody" (AFP guest user), and the server supports guest logins,
     * fall back to "No User Authent" (guest) UAM */
    if (((*req->url.username == '\0' && *req->url.password == '\0') ||
            strcmp(req->url.username, "nobody") == 0)
            && (s->supported_uams & UAM_NOUSERAUTHENT)) {
        req->uam_mask = UAM_NOUSERAUTHENT;
    }

    /* complete_connection removes the server on failure. */
    return afp_server_complete_connection(priv, s, req->url.username,
                                          req->url.password,
                                          req->url.requested_version,
                                          req->uam_mask);
}
