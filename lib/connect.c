/*
 *  connect.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>

#include "afp.h"
#include "dsi.h"
#include "utils.h"
#include "uams_def.h"
#include "codepage.h"
#include "users.h"
#include "libafpclient.h"
#include "server.h"


struct addrinfo *afp_get_address(void * priv, const char * hostname,
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


struct afp_server *afp_server_full_connect(void * priv,
        struct afp_connection_request *req)
{
    int ret;
    struct addrinfo * address;
    struct afp_server  * s = NULL;
    struct afp_server  * tmpserver;
    char signature[AFP_SIGNATURE_LEN];
    unsigned char versions[SERVER_MAX_VERSIONS];
    unsigned int uams;
    char machine_type[AFP_MACHINETYPE_LEN];
    char server_name[AFP_SERVER_NAME_LEN];
    char server_name_utf8[AFP_SERVER_NAME_UTF8_LEN];
    char server_name_printable[AFP_SERVER_NAME_UTF8_LEN];
    unsigned int rx_quantum;
    char icon[AFP_SERVER_ICON_LEN];

    if ((address = afp_get_address(priv, req->url.servername,
                                   req->url.port)) == NULL) {
        goto error;
    }

    if ((tmpserver = afp_server_init(address)) == NULL) {
        goto error;
    }

    if ((ret = afp_server_connect(tmpserver, 1)) < 0) {
        int connect_error = -ret;

        if (ret == -ETIMEDOUT) {
            log_for_client(priv, AFPFSD, LOG_ERR,
                           "Could not connect, never got a response to getstatus, %s", strerror(-ret));
        } else {
            log_for_client(priv, AFPFSD, LOG_ERR,
                           "Could not connect, %s", strerror(-ret));
        }

        afp_server_remove(tmpserver);
        errno = connect_error;
        goto error;
    }

    loop_disconnect(tmpserver);
    memcpy(icon, &tmpserver->icon, AFP_SERVER_ICON_LEN);
    memcpy(&versions, &tmpserver->versions, SERVER_MAX_VERSIONS);
    uams = tmpserver->supported_uams;
    memcpy(signature, &tmpserver->signature, AFP_SIGNATURE_LEN);
    memcpy(machine_type, &tmpserver->machine_type, AFP_MACHINETYPE_LEN);
    memcpy(server_name, &tmpserver->server_name, AFP_SERVER_NAME_LEN);
    memcpy(server_name_utf8, &tmpserver->server_name_utf8,
           AFP_SERVER_NAME_UTF8_LEN);
    memcpy(server_name_printable, &tmpserver->server_name_printable,
           AFP_SERVER_NAME_UTF8_LEN);
    rx_quantum = tmpserver->rx_quantum;
    afp_server_remove(tmpserver);
    s = afp_server_init(address);

    if (!s) {
        if (errno == 0) {
            errno = ENOMEM;
        }

        goto error;
    }

    if ((ret = afp_server_connect(s, 0)) != 0) {
        int connect_error = -ret;
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Connection to server failed with error: %s",
                       strerror(connect_error));
        afp_server_remove(s);
        errno = connect_error;
        s = NULL;
        goto error;
    }

    s->supported_uams = uams;
    memcpy(s->signature, signature, AFP_SIGNATURE_LEN);
    memcpy(s->server_name, server_name, AFP_SERVER_NAME_LEN);
    memcpy(s->server_name_utf8, server_name_utf8,
           AFP_SERVER_NAME_UTF8_LEN);
    memcpy(s->server_name_printable, server_name_printable,
           AFP_SERVER_NAME_UTF8_LEN);
    memcpy(s->machine_type, machine_type, AFP_MACHINETYPE_LEN);
    memcpy(s->icon, icon, AFP_SERVER_ICON_LEN);
    s->rx_quantum = rx_quantum;
    afp_server_identify(s);

    /* if our user and password strings are both empty, or if the username
     * is "nobody" (AFP guest user), and the server supports guest logins,
     * fall back to "No User Authent" (guest) UAM */
    if (((*req->url.username == '\0' && *req->url.password == '\0') ||
            strcmp(req->url.username, "nobody") == 0)
            && (uams & UAM_NOUSERAUTHENT)) {
        req->uam_mask = UAM_NOUSERAUTHENT;
    }

    if ((afp_server_complete_connection(priv,
                                        s, (unsigned char *) &versions, uams,
                                        req->url.username, req->url.password,
                                        req->url.requested_version, req->uam_mask)) == NULL) {
        /* complete_connection already called afp_server_remove() on failure */
        s = NULL;
        goto error;
    }

    afp_server_identify(s);
    return s;
error:
    return NULL;
}
