/*
 *  afp.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief Servers and volumes: connecting, attaching, detaching, server list
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "afp_replies.h"
#include "client.h"
#include "codepage.h"
#include "compat.h"
#include "did.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "forklist.h"
#include "lowlevel.h"
#include "server.h"
#include "utils.h"

struct afp_versions      afp_versions[] = {
    { "AFPVersion 1.1", 11 },
    { "AFPVersion 2.0", 20 },
    { "AFPVersion 2.1", 21 },
    { "AFP2.2", 22 },
    { "AFPX03", 30 },
    { "AFP3.1", 31 },
    { "AFP3.2", 32 },
    { "AFP3.3", 33 },
    { "AFP3.4", 34 },
    { NULL, 0 }
};

static int afp_blank_reply(struct afp_server *server, char *buf,
                           unsigned int size, void *ignored);

/*!
 * @brief Write a server signature as hexadecimal text
 *
 * @param[in]  signature  the signature
 * @param[out] text       receives two digits a byte and a NUL
 */
static void format_server_signature(const char signature[AFP_SIGNATURE_LEN],
                                    char text[AFP_SIGNATURE_LEN * 2 + 1])
{
    for (size_t i = 0; i < AFP_SIGNATURE_LEN; i++) {
        snprintf(text + (i * 2), 3, "%02x", (unsigned char)signature[i]);
    }
}

int (*afp_replies[])(struct afp_server * server, char *buf, unsigned int len,
                     void *other) = {
    NULL, afp_byterangelock_reply, afp_blank_reply, NULL,
    afp_blank_reply, afp_blank_reply, afp_createdir_reply, afp_blank_reply, /* 0 - 7 */
    afp_blank_reply, afp_enumerate_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, NULL, NULL,                       /* 8 - 15 */
    afp_getsrvrparms_reply, afp_getvolparms_reply, afp_login_reply, afp_login_reply,
    afp_blank_reply, afp_mapid_reply, afp_mapname_reply, afp_blank_reply,  /*16 - 23 */
    afp_volopen_reply, NULL, afp_openfork_reply, afp_read_reply,
    afp_blank_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,    /*24 - 31 */
    NULL, afp_write_reply, afp_getfiledirparms_reply, afp_blank_reply,
    afp_changepassword_reply, afp_getuserinfo_reply, afp_getsrvrmsg_reply, NULL,      /*32 - 39 */

    NULL, NULL, afp_blank_reply, NULL,
    NULL, NULL, NULL, NULL,                       /*40 - 47 */
    afp_opendt_reply, afp_blank_reply, NULL, afp_geticon_reply,
    afp_geticoninfo_reply, NULL, NULL, afp_getappl_reply, /*48 - 55 */
    afp_blank_reply, NULL, afp_getcomment_reply, afp_byterangelockext_reply,
    afp_readext_reply, afp_writeext_reply,
    NULL, afp_login_reply,            /*56 - 63 */
    /* FPEnumerateExt's replies share FPEnumerateExt2's layout, a 16-bit length
     * before each entry; the requests differ in index and size widths alone. */
    afp_getsessiontoken_reply, afp_blank_reply, afp_enumerateext2_reply, NULL,
    afp_enumerateext2_reply, afp_getextattr_reply, afp_blank_reply, afp_blank_reply, /*64 - 71 */
    afp_listextattrs_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, afp_blank_reply, afp_blank_reply,                       /*72 - 79 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 80 - 99 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 100 - 119 */

    NULL, NULL, afp_blank_reply, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /* 120 - 139 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

};

/*!
 * @brief Take the result of a reply that carries nothing more
 *
 * @param[in] server   unused
 * @param[in] buf      the reply
 * @param[in] size     unused
 * @param[in] ignored  unused
 * @returns the reply's error code
 */
static int afp_blank_reply(
    struct afp_server *server _U_,
    char *buf,
    unsigned int size _U_,
    void *ignored _U_
)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
    } * reply = (void *) buf;
    return reply->header.return_code.error_code;
}

/* The seconds a server has to answer the status request. */
#define AFP_STATUS_WAIT 60
/* The longest status reply read; names, UAMs and an icon fit in far less. */
#define AFP_STATUS_MAX_LEN 16384U

/*!
 * @brief Start a TCP connection to an address
 *
 * The connect goes on in the background; await_connection() waits for it.
 *
 * @param[in] address  the address
 * @returns the socket, or -1 with errno set
 */
static int open_connection(const struct addrinfo *address)
{
    int fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
    int flags;
    int error;

    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
            || (connect(fd, address->ai_addr, address->ai_addrlen) < 0
                && errno != EINPROGRESS)) {
        error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    return fd;
}

/*!
 * @brief Wait for a connection open_connection() started
 *
 * Leaves the socket blocking, with Nagle's algorithm off, as the session's
 * reads and writes expect.
 *
 * @param[in] fd       the socket
 * @param[in] timeout  seconds to wait
 * @returns 0, or -1 with errno set, ETIMEDOUT when the wait ends first
 */
static int await_connection(int fd, int timeout)
{
    struct pollfd pfd = { fd, POLLOUT, 0 };
    int nodelay = 1;
    int error = 0;
    socklen_t error_len = sizeof(error);
    int flags;
    int ret;

    do {
        ret = poll(&pfd, 1, timeout * 1000);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    if (ret < 0
            || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        return -1;
    }

    if (error != 0) {
        errno = error;
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return -1;
    }

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   sizeof(nodelay)) < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Could not set TCP_NODELAY on fd=%d: %s",
                       fd, strerror(errno));
    }

    return 0;
}

/*!
 * @brief Test whether the server has answered, or closed, an unused connection
 *
 * @param[in] fd  the connection
 * @returns 1 when it has
 */
static int answered_unasked(int fd)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    return poll(&pfd, 1, 0) > 0;
}

/*!
 * @brief Read exactly len bytes before a deadline
 *
 * @param[in]  fd        the socket
 * @param[out] buf       receives the bytes
 * @param[in]  len       bytes to read
 * @param[in]  deadline  when the bytes are due, on CLOCK_MONOTONIC
 * @returns 0, or a negative errno: -ETIMEDOUT when the deadline passes
 *          first, -EIO when the connection ends or fails
 */
static int read_within(int fd, char *buf, size_t len,
                       const struct timespec *deadline)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    size_t done = 0;

    while (done < len) {
        struct timespec now;
        ssize_t n;
        int left;
        int ret;
        clock_gettime(CLOCK_MONOTONIC, &now);
        left = (int)((deadline->tv_sec - now.tv_sec) * 1000
                     + (deadline->tv_nsec - now.tv_nsec) / 1000000);

        if (left <= 0) {
            return -ETIMEDOUT;
        }

        ret = poll(&pfd, 1, left);

        if (ret < 0 && errno == EINTR) {
            continue;
        }

        if (ret <= 0) {
            return ret == 0 ? -ETIMEDOUT : -EIO;
        }

        n = read(fd, buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -EIO;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Ask a connected server for its status and read the reply
 *
 * @param[in]  fd      the connected socket
 * @param[out] status  what the server reports
 * @returns 0, or a negative errno: -EIO for a connection that ends,
 *          -ETIMEDOUT for a server that does not answer in time, -EBUSY
 *          for a server at its connection cap, -EPROTO for a reply that is
 *          not a status, -ENOMEM for a reply that cannot be held
 */
static int exchange_status(int fd, struct afp_server_status *status)
{
    struct dsi_header header;
    struct timespec deadline;
    uint32_t length;
    char *reply;
    size_t written = 0;
    int rc;
    memset(&header, 0, sizeof(header));
    header.command = DSI_DSIGetStatus;
    header.requestid = htons(1);

    while (written < sizeof(header)) {
        ssize_t n = send(fd, (char *) &header + written,
                         sizeof(header) - written, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -EIO;
        }

        written += (size_t) n;
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += AFP_STATUS_WAIT;
    rc = read_within(fd, (char *) &header, sizeof(header), &deadline);

    if (rc < 0) {
        return rc;
    }

    length = ntohl(header.length);

    /* afpd at its cap answers any connection with a DSIOpenSession refusal. */
    if (header.flags == DSI_REPLY && header.command == DSI_DSIOpenSession
            && ntohl((uint32_t) header.return_code.error_code)
            == DSIERR_SERVBUSY) {
        return -EBUSY;
    }

    if (header.flags != DSI_REPLY || header.command != DSI_DSIGetStatus
            || length > AFP_STATUS_MAX_LEN) {
        return -EPROTO;
    }

    if ((reply = malloc(sizeof(header) + length)) == NULL) {
        return -ENOMEM;
    }

    memcpy(reply, &header, sizeof(header));

    if (length > 0) {
        rc = read_within(fd, reply + sizeof(header), length, &deadline);

        if (rc < 0) {
            free(reply);
            return rc;
        }
    }

    rc = afpc_dsi_parse_status(reply, sizeof(header) + length, status);
    free(reply);
    return rc;
}

/*!
 * @brief Write an address as text for the log
 *
 * @param[in]  address  the address
 * @param[out] text     receives the text
 */
static void address_text(const struct addrinfo *address,
                         char text[INET6_ADDRSTRLEN])
{
    switch (address->ai_family) {
    case AF_INET6:
        inet_ntop(AF_INET6,
                  &((struct sockaddr_in6 *) address->ai_addr)->sin6_addr,
                  text, INET6_ADDRSTRLEN);
        break;

    case AF_INET:
        inet_ntop(AF_INET,
                  &((struct sockaddr_in *) address->ai_addr)->sin_addr,
                  text, INET6_ADDRSTRLEN);
        break;

    default:
        snprintf(text, INET6_ADDRSTRLEN, "unknown address family");
        break;
    }
}

/*!
 * @brief Hand a reply to its command's parser
 *
 * @param[in]     subcommand  the AFP command the reply answers
 * @param[in,out] server      the server it came from
 * @param[in,out] other       the request's caller data
 * @returns the parser's result, 0 without a parser, or -1 for a reply
 *          shorter than a DSI header
 */
int afp_reply(unsigned short subcommand, struct afp_server * server,
              void *other)
{
    int ret = 0;

    /* No AFP packet is valid if it is smaller than a DSI header. */

    if ((unsigned long) server->data_read < sizeof(struct dsi_header)) {
        return -1;
    }

    if (afp_replies[subcommand]) {
        ret = (*afp_replies[subcommand])(server,
                                         server->incoming_buffer,
                                         server->data_read, other);
    } else {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "AFP subcommand %d not supported", subcommand);
    }

    return ret;
}


static struct afp_server *server_base = NULL;
static pthread_mutex_t server_list_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Taken around getpwuid(), whose answer two connects would share */
static pthread_mutex_t passwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int auto_shutdown_on_unmount = 0;
static int auto_disconnect_on_unmount = 1;

/*!
 * @brief Set whether the front end shuts down at the last unmount
 *
 * @param[in] enabled  1 to shut down
 */
void afp_set_auto_shutdown_on_unmount(int enabled)
{
    auto_shutdown_on_unmount = enabled;
}

/*!
 * @brief Whether the front end shuts down at the last unmount
 *
 * @returns 1 when it does
 */
int afp_get_auto_shutdown_on_unmount(void)
{
    return auto_shutdown_on_unmount;
}

/*!
 * @brief Set whether a server disconnects once no volume is attached
 *
 * @param[in] enabled  1 to disconnect
 */
void afp_set_auto_disconnect_on_unmount(int enabled)
{
    auto_disconnect_on_unmount = enabled;
}

/*!
 * @brief Whether requests may still be sent to a server
 *
 * A server is valid from afp_server_init() until afp_server_remove()
 * takes it off the list, through every state the connect path assigns.
 * The server must be alive: the loop holds it across a receive, afpsld's
 * command handlers and afpfsd's commands hold it through their lookups,
 * and the FUSE front end joins its workers before it removes the last
 * volume's server. The state's writers hold the list lock and store it
 * atomically, so this check, on every request's path, takes no lock.
 *
 * @param[in] server  the server, or NULL
 * @returns 1 from afp_server_init() until afp_server_remove(), listed or
 *          not; 0 for NULL, a removed server, or one never set up
 */
int server_still_valid(struct afp_server * server)
{
    if (server == NULL) {
        return 0;
    }

    switch (__atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)) {
    case SERVER_STATE_CONNECTED:
    case SERVER_STATE_DISCONNECTED:
    case SERVER_STATE_CONNECTING:
        return 1;

    default:
        return 0;
    }
}

/*!
 * @brief The first server of the list
 *
 * @returns the server, or NULL
 */
struct afp_server *get_server_base(void)
{
    return server_base;
}

/*!
 * @brief Find a listed server by its signature
 *
 * @param[in] signature  AFP_SIGNATURE_LEN bytes
 * @returns the server, or NULL
 */
struct afp_server *find_server_by_signature(char *signature)
{
    struct afp_server * s;

    for (s = get_server_base(); s; s = s->next) {
        if (memcmp(s->signature, signature, AFP_SIGNATURE_LEN) == 0) {
            return s;
        }
    }

    return NULL;
}

/*!
 * @brief Find a listed server by any of its names
 *
 * @param[in] name  the name
 * @returns the server, or NULL
 */
struct afp_server *find_server_by_name(char *name)
{
    for (struct afp_server * s = get_server_base(); s; s = s->next) {
        if (strcmp(s->server_name_utf8, name) == 0) {
            return s;
        }

        if (strcmp(s->server_name, name) == 0) {
            return s;
        }

        if (strcmp(s->server_name_printable, name) == 0) {
            return s;
        }
    }

    return NULL;
}

/*!
 * @brief Hold a server while the list still holds it
 *
 * @param[in] target  the server
 * @returns the server, held, or NULL
 */
struct afp_server *find_server_by_pointer(struct afp_server *target)
{
    pthread_mutex_lock(&server_list_mutex);

    for (struct afp_server *s = server_base; s; s = s->next) {
        if (s == target) {
            __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&server_list_mutex);
            return s;
        }
    }

    pthread_mutex_unlock(&server_list_mutex);
    return NULL;
}

/*!
 * @brief Find a listed server by any address of an address list
 *
 * @param[in] address  the address list
 * @returns the server, or NULL
 */
struct afp_server *find_server_by_address(struct addrinfo *address)
{
    struct afp_server *s;

    for (s = server_base; s; s = s->next) {
        for (struct addrinfo *p = address; p; p = p->ai_next) {
            if (s->used_address != NULL && s->used_address->ai_addr != NULL &&
                    p != NULL && p->ai_addr != NULL &&
                    s->used_address->ai_addrlen == p->ai_addrlen &&
                    memcmp(s->used_address->ai_addr, p->ai_addr,
                           p->ai_addrlen) == 0) {
                return s;
            }
        }
    }

    return NULL;
}

/*!
 * @brief Test whether a signature names a server: 16 bytes, not all zero
 *
 * @param[in] signature  the signature of a status reply
 * @returns 1 when it does, 0 otherwise
 */
int afp_signature_is_valid(const char signature[AFP_SIGNATURE_LEN])
{
    static const char zero_signature[AFP_SIGNATURE_LEN] = {0};
    return memcmp(signature, zero_signature, sizeof(zero_signature)) != 0;
}

/*!
 * @brief Test whether a server's status carried a signature that names it
 *
 * @param[in] server  the server, or NULL
 * @returns 1 when it did, 0 otherwise
 */
int afp_server_has_valid_signature(const struct afp_server *server)
{
    return server != NULL && afp_signature_is_valid(server->signature);
}

/*!
 * @brief Read a server's status over a connection of its own
 *
 * Runs wholly on the calling thread, outside the loop.
 *
 * @param[in]  address   the addresses to try, in order
 * @param[out] status    what the server reports
 * @param[out] answered  receives the address that answered, or NULL
 * @returns 0, or a negative errno: the connect's when no address connects,
 *          the exchange's when the connected server does not answer
 */
int afp_server_probe(struct addrinfo *address,
                     struct afp_server_status *status,
                     const struct addrinfo **answered)
{
    int error = EINVAL;

    for (; address; address = address->ai_next) {
        char text[INET6_ADDRSTRLEN];
        int fd;
        int rc;
        address_text(address, text);
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Attempting connection to %s ...", text);
        fd = open_connection(address);

        if (fd < 0) {
            error = errno;
            continue;
        }

        if (await_connection(fd, DSI_DEFAULT_TIMEOUT) < 0) {
            error = errno;
            close(fd);
            continue;
        }

        if (answered) {
            *answered = address;
        }

        rc = exchange_status(fd, status);
        close(fd);
        return rc;
    }

    return error ? -error : -EIO;
}

/*!
 * @brief Take a status reply's fields into the server and identify it
 *
 * @param[in,out] server  the server
 * @param[in]     status  what the server reported
 */
void afp_server_adopt_status(struct afp_server *server,
                             const struct afp_server_status *status)
{
    memcpy(server->server_name, status->server_name,
           sizeof(server->server_name));
    memcpy(server->server_name_utf8, status->server_name_utf8,
           sizeof(server->server_name_utf8));
    memcpy(server->server_name_printable, status->server_name_printable,
           sizeof(server->server_name_printable));
    memcpy(server->machine_type, status->machine_type,
           sizeof(server->machine_type));
    memcpy(server->signature, status->signature, sizeof(server->signature));
    memcpy(server->icon, status->icon, sizeof(server->icon));
    memcpy(server->versions, status->versions, sizeof(server->versions));
    server->supported_uams = status->supported_uams;
    server->flags = status->flags;
    afp_server_identify(server);
}

/*!
 * @brief Give a server the address of its next connection
 *
 * The swap runs under the server list's lock, under which the status text
 * reads the address in use, and the old address is freed after it.
 *
 * @param[in,out] server   the server
 * @param[in]     address  the address, which the server then owns
 */
void afp_server_replace_address(struct afp_server *server,
                                struct addrinfo *address)
{
    struct addrinfo *old = NULL;

    if (!server || !address) {
        return;
    }

    pthread_mutex_lock(&server_list_mutex);

    if (server->address != address) {
        old = server->address;
        server->address = address;
    }

    server->used_address = NULL;
    pthread_mutex_unlock(&server_list_mutex);

    if (old) {
        freeaddrinfo(old);
    }
}

/*!
 * @brief Take a reference on a server
 *
 * @param[in,out] s  the server
 */
void afp_server_hold(struct afp_server *s)
{
    __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
}

/*!
 * @brief Drop a reference on a server, freeing it with the last
 *
 * @param[in,out] s  the server
 */
void afp_server_release(struct afp_server *s)
{
    if (__atomic_sub_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        afp_free_server(&s);
    }
}

/*!
 * @brief Find a server by name, and hold it
 *
 * @param[in] name  the name
 * @returns the server, held, or NULL
 */
struct afp_server *afp_server_find_by_name_hold(char *name)
{
    struct afp_server * s;
    pthread_mutex_lock(&server_list_mutex);
    s = find_server_by_name(name);

    if (s) {
        __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_unlock(&server_list_mutex);
    return s;
}

/*!
 * @brief Find a server by address, and hold it
 *
 * @param[in] address  the address list
 * @returns the server, held, or NULL
 */
struct afp_server *afp_server_find_by_address_hold(struct addrinfo * address)
{
    struct afp_server * s;
    pthread_mutex_lock(&server_list_mutex);
    s = find_server_by_address(address);

    if (s) {
        __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_unlock(&server_list_mutex);
    return s;
}

/*!
 * @brief Find a volume by its address, and hold its server
 *
 * @param[in] id  the volume's address
 * @returns the volume, or NULL
 */
struct afp_volume *afp_volume_find_by_pointer_hold(void *id)
{
    struct afp_volume * v;
    pthread_mutex_lock(&server_list_mutex);

    for (struct afp_server * s = server_base; s; s = s->next) {
        for (int j = 0; j < s->num_volumes; j++) {
            v = &s->volumes[j];

            if ((void *) v == id) {
                __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&server_list_mutex);
                return v;
            }
        }
    }

    pthread_mutex_unlock(&server_list_mutex);
    return NULL;
}

/*!
 * @brief Find the volume whose mount point is the one given, and hold its
 *        server
 *
 * The volume's mount point is matched whatever its mounted state.
 * afpfsd runs each command on a thread of its own, so another client's
 * unmount can remove the server while a command uses it; the reference
 * keeps the server until the command releases it.
 *
 * @param[in] mountpoint  the mount point
 * @returns the volume, its server held until afp_server_release(), or
 *          NULL when no volume has that mount point
 */
struct afp_volume *afp_volume_find_by_mountpoint_hold(const char *mountpoint)
{
    struct afp_volume * v;
    pthread_mutex_lock(&server_list_mutex);

    for (struct afp_server * s = server_base; s; s = s->next) {
        for (int j = 0; j < s->num_volumes; j++) {
            v = &s->volumes[j];

            if (strcmp(v->mountpoint, mountpoint) == 0) {
                __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&server_list_mutex);
                return v;
            }
        }
    }

    pthread_mutex_unlock(&server_list_mutex);
    return NULL;
}

/*!
 * @brief Lock the server list
 */
void afp_lock_server_list(void)
{
    pthread_mutex_lock(&server_list_mutex);
}

/*!
 * @brief Unlock the server list
 */
void afp_unlock_server_list(void)
{
    pthread_mutex_unlock(&server_list_mutex);
}

/*!
 * @brief Whether any of a server's volumes is mounted or unmounting
 *
 * @param[in] server  the server
 * @returns 1 when one is
 */
int something_is_mounted(struct afp_server * server)
{
    int i;

    for (i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].mounted != AFP_VOLUME_UNMOUNTED) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Whether any of a server's volumes is attached
 *
 * @param[in] server  the server
 * @returns 1 when one is
 */
int something_is_attached(struct afp_server * server)
{
    for (int i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].attached != AFP_VOLUME_DETACHED) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Detach a volume at unmount
 *
 * Closes the forks still open. Once no volume of the server stays attached,
 * the server logs out and is removed, unless afpsld turned that off.
 *
 * @param[in,out] volume  the attached volume
 * @returns 0, or -1 for NULL
 */
int afp_detach_volume(struct afp_volume * volume)
{
    struct afp_server * server;

    if (volume == NULL) {
        return -1;
    }

    server = volume->server;
    volume->attached = AFP_VOLUME_DETACHING;
    /* The forks' flushes tell the cache, which empties after them. */
    remove_fork_list(volume);
    dir_cache_purge(volume);

    /* A close on a dropped connection would reconnect to send it. */
    if (volume->dtrefnum
            && __atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)
            == SERVER_STATE_CONNECTED) {
        afp_closedt(server, volume->dtrefnum);
    }

    volume->dtrefnum = 0;
    volume->attached = AFP_VOLUME_DETACHED;

    if (something_is_attached(server)) {
        return 0;
    }

    if (auto_disconnect_on_unmount) {
        afp_logout(server, DSI_DONT_WAIT /* don't wait */);
        afp_server_remove(server);
    }

    return 0;
}

/*!
 * @brief Detach every attached volume of a server
 *
 * @param[in,out] server  the server
 * @returns 0, or 1 when a detach fails
 */
int afp_unmount_all_volumes(struct afp_server * server)
{
    int i;

    for (i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].attached == AFP_VOLUME_ATTACHED
                && afp_detach_volume(&server->volumes[i])) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Unmount a mounted volume and detach it
 *
 * @param[in,out] volume  the volume, or NULL
 * @returns 0, or -1 for a volume not mounted or one the front end keeps
 */
int afp_unmount_volume(struct afp_volume * volume)
{
    unsigned char mounted = AFP_VOLUME_MOUNTED;

    if (volume == NULL) {
        return -1;
    }

    /* Commands run on threads of their own: one unmount alone goes on. */
    if (!__atomic_compare_exchange_n(&volume->mounted, &mounted,
                                     AFP_VOLUME_UNMOUNTING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return -1;
    }

    if (libafpclient->unmount_volume && libafpclient->unmount_volume(volume) != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "FUSE unmount not supported - volume remains mounted on %s",
                       volume->mountpoint);
        __atomic_store_n(&volume->mounted, AFP_VOLUME_MOUNTED,
                         __ATOMIC_RELEASE);
        return -1;
    }

    __atomic_store_n(&volume->mounted, AFP_VOLUME_UNMOUNTED, __ATOMIC_RELEASE);
    /* Detaching the last volume can free its server and volume storage. */
    afp_detach_volume(volume);
    return 0;
}


/*!
 * @brief Free a volume list, each volume's cache and locks first
 *
 * @param[in,out] volumes  the list, or NULL
 * @param[in]     count    its length
 */
void afp_free_volumes(struct afp_volume *volumes, unsigned int count)
{
    for (unsigned int i = 0; volumes && i < count; i++) {
        dir_cache_destroy(&volumes[i]);
        pthread_rwlock_destroy(&volumes[i].open_lock);
        pthread_mutex_destroy(&volumes[i].open_forks_mutex);
    }

    free(volumes);
}

/*!
 * @brief Free a server, the requests still queued on it, and its buffers
 *
 * The saved password is cleared before its memory is freed.
 *
 * @param[in,out] sp  the server, set to NULL
 */
void afp_free_server(struct afp_server **sp)
{
    struct dsi_request * p, *next;
    struct afp_server * server;

    if (sp == NULL) {
        return;
    }

    server = *sp;

    if (!server) {
        return;
    }

    for (p = server->command_requests; p;) {
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "FSLeft in queue: %p, id: %d command: %d",
                       (void *)p, p->requestid, p->subcommand);
        next = p->next;
        free(p);
        p = next;
    }

    if (server->incoming_buffer) {
        free(server->incoming_buffer);
    }

    afp_free_volumes(server->volumes, server->num_volumes);
    free(server->unclosed_forks);
    explicit_bzero(server->password, sizeof(server->password));
    pthread_mutex_destroy(&server->unclosed_forks_mutex);
    pthread_mutex_destroy(&server->requestid_mutex);
    pthread_mutex_destroy(&server->request_queue_mutex);
    pthread_mutex_destroy(&server->send_mutex);
    pthread_mutex_destroy(&server->op_mutex);
    pthread_cond_destroy(&server->request_queue_cond);
    free(server);
    *sp = NULL;
}

/*!
 * @brief Take a server off the list and end its connection
 *
 * The unlink marks the server destroyed under the list lock, which is
 * the state validity reads. The connection then ends: its descriptor
 * leaves the loop's set and every request waiting on it fails with EIO.
 * The creation's reference is released last.
 *
 * @param[in,out] s  the server, or NULL
 * @returns 0, or -ENOENT for a server that was not listed
 */
int afp_server_remove(struct afp_server *s)
{
    struct afp_server *s2;
    int found = 0;

    if (s == NULL) {
        return 0;
    }

    pthread_mutex_lock(&server_list_mutex);

    if (s == server_base) {
        server_base = s->next;
        found = 1;
    } else {
        for (s2 = server_base; s2; s2 = s2->next) {
            if (s == s2->next) {
                s2->next = s->next;
                found = 1;
                break;
            }
        }
    }

    if (found) {
        __atomic_store_n(&s->connect_state, SERVER_STATE_DESTROYED,
                         __ATOMIC_RELEASE);
    }

    pthread_mutex_unlock(&server_list_mutex);

    if (!found) {
        return -ENOENT;
    }

    loop_disconnect(s);
    /* Release the initial reference from creation.
     * The server will be freed when all holders release. */
    afp_server_release(s);
    return 0;
}

/*!
 * @brief Allocate a server for an address, neither listed nor connected
 *
 * @param[in] address  the server's address
 * @returns the server, holding one reference, or NULL when memory runs out
 */
struct afp_server *afp_server_init(struct addrinfo * address)
{
    struct afp_server * s;
    struct passwd *pw;

    if ((s = malloc(sizeof(*s))) == NULL) {
        return NULL;
    }

    memset((void *) s, 0, sizeof(*s));
    s->exit_flag = 0;
    s->dsi_default_timeout = DSI_DEFAULT_TIMEOUT;
    s->next = NULL;
    s->bufsize = 128 * 1024;
    s->incoming_buffer = malloc(s->bufsize);

    if (s->incoming_buffer == NULL) {
        free(s);
        return NULL;
    }

    s->attention_quantum = AFP_DEFAULT_ATTENTION_QUANTUM;
    s->fd = -1;
    s->connect_state = SERVER_STATE_DISCONNECTED;
    afp_server_reconnect_end(s);
    afp_server_set_suspended(s, 0);
    s->address = address;
    /* Initialize mutexes */
    pthread_mutex_init(&s->unclosed_forks_mutex, NULL);
    pthread_mutex_init(&s->requestid_mutex, NULL);
    pthread_mutex_init(&s->request_queue_mutex, NULL);
    pthread_cond_init(&s->request_queue_cond, NULL);
    pthread_mutex_init(&s->send_mutex, NULL);
    pthread_mutex_init(&s->op_mutex, NULL);
    afp_server_connection_generation_init(s);
    s->refcount = 1;
    /* AFP 3.3+ replay cache - initialized to disabled */
    s->replay_cache_size = 0;
    s->supports_replay_cache = 0;
    /* A user the passwd database lacks keeps the process's IDs, and a name
     * no server user has. getpwuid() answers in one buffer, which each call
     * rewrites as it reads the database. */
    pthread_mutex_lock(&passwd_mutex);
    pw = getpwuid(geteuid());

    if (pw) {
        s->passwd.pw_uid = pw->pw_uid;
        s->passwd.pw_gid = pw->pw_gid;
        snprintf(s->passwd_name, sizeof(s->passwd_name), "%s", pw->pw_name);
    } else {
        s->passwd.pw_uid = geteuid();
        s->passwd.pw_gid = getegid();
    }

    pthread_mutex_unlock(&passwd_mutex);
    s->passwd.pw_name = s->passwd_name;
    return s;
}

/*!
 * @brief Copy a server's public details into the structure afpc reads
 *
 * @param[in]  server  the server
 * @param[out] basic   receives the details
 */
void afp_server_fill_basic(const struct afp_server *server,
                           struct afpc_server_info *basic)
{
    memset(basic, 0, sizeof(*basic));
    memcpy(basic->server_name_printable,
           server->server_name_printable,
           sizeof(basic->server_name_printable));
    /* The public field is a byte shorter than the server's: a type of
     * sixteen characters loses its last to keep its terminator. */
    memcpy(basic->machine_type,
           server->machine_type,
           sizeof(basic->machine_type) - 1);
    memcpy(basic->icon,
           server->icon,
           sizeof(basic->icon));
    memcpy(basic->signature,
           server->signature,
           sizeof(basic->signature));
    memcpy(basic->versions,
           server->versions,
           sizeof(basic->versions));
    basic->supported_uams = server->supported_uams;
    basic->flags = server->flags;
    basic->server_type = server->server_type;
}

#define AFP_CLIENT_ID_LEN 16

static pthread_once_t client_identity_once = PTHREAD_ONCE_INIT;
static struct afp_token client_id;
static uint32_t client_id_time;

/*!
 * @brief Make the client's ID for session tokens, once per process
 *
 * afpd ends a session of the same ID and another time as one a restarted
 * client left, so neither changes while the process runs.
 */
static void make_client_identity(void)
{
    pid_t pid = getpid();
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    client_id_time = (uint32_t) time(NULL);
    client_id.length = AFP_CLIENT_ID_LEN;
    memcpy(client_id.data, &pid, sizeof(pid));
    memcpy(client_id.data + sizeof(pid), &client_id_time,
           sizeof(client_id_time));

    if (fd >= 0) {
        char random[AFP_CLIENT_ID_LEN];

        if (read(fd, random, sizeof(random)) == (ssize_t) sizeof(random)) {
            memcpy(client_id.data, random, sizeof(random));
        }

        close(fd);
    }
}

/*!
 * @brief Ask the server for the token a reconnect continues the session with
 *
 * A server that refuses the request, or does not continue sessions, holds no
 * token, so a reconnect starts a new session.
 *
 * @param[in,out] server  server with a session logged in
 */
void afp_server_take_token(struct afp_server *server)
{
    server->token.length = 0;

    if (!afp_server_continues_sessions(server)) {
        return;
    }

    pthread_once(&client_identity_once, make_client_identity);
    /* The reply fills the token: a request whose own reconnect started a
     * new session leaves the token that session took. */
    afp_getsessiontoken(server, kLoginWithTimeAndID, client_id_time,
                        &client_id, &server->token);
}

/*!
 * @brief Log in with the server's UAM, user name and password
 *
 * @param[in,out] server  server with a DSI session open
 * @param[out]    mesg    receives why a login fails
 * @param[in,out] l       bytes of mesg in use
 * @param[in]     max     capacity of mesg
 * @returns 0, also for kFPMiscErr and kFPCallNotSupported, which it takes
 *          for a session already logged in, or 1 when the login fails
 */
int afp_server_login(struct afp_server *server,
                     char *mesg, unsigned int *l, unsigned int max)
{
    int rc;
    rc = afp_dologin(server, server->using_uam,
                     server->username, server->password);

    switch (rc) {
    case -1:
        *l += snprintf(mesg, max - *l,
                       "Could not find a valid UAM\n");
        goto error;

    case kFPAuthContinue:
        *l += snprintf(mesg, max - *l,
                       "Unsupported authentication method\n");
        goto error;

    case kFPBadUAM:
        *l += snprintf(mesg, max - *l,
                       "Specified UAM is unknown\n");
        goto error;

    case kFPBadVersNum:
        *l += snprintf(mesg, max - *l,
                       "Server does not support this AFP version\n");
        goto error;

    case kFPCallNotSupported:
    case kFPMiscErr:
        *l += snprintf(mesg, max - *l,
                       "Already logged in\n");
        break;

    case kFPNoServer:
        *l += snprintf(mesg, max - *l,
                       "Authentication server not responding\n");
        goto error;

    case kFPPwdExpiredErr:
    case kFPPwdNeedsChangeErr:
        *l += snprintf(mesg, max - *l,
                       "Warning: password needs changing\n");
        goto error;

    case kFPServerGoingDown:
        *l += snprintf(mesg, max - *l,
                       "Server going down, so I can't log you in\n");
        goto error;

    case kFPUserNotAuth:
        *l += snprintf(mesg, max - *l,
                       "Authentication failed\n");
        goto error;

    case 0:
        break;

    default:
        *l += snprintf(mesg, max - *l,
                       "Unknown error, maybe username/passwd needed?\n");
        goto error;
    }

    return 0;
error:
    return 1;
}


/*!
 * @brief Find a volume by the name a URL gives
 *
 * @param[in] server   the server
 * @param[in] volname  UTF-8 volume name
 * @returns the volume, or NULL when none matches or the name does not
 *          decode whole
 */
struct afp_volume *find_volume_by_name(struct afp_server * server,
                                       char *volname)
{
    int i;
    struct afp_volume * using_volume = NULL;
    char converted_volname[AFP_VOLUME_NAME_UTF8_LEN];

    /* A name that does not decode whole names another volume. */
    if (convert_utf8dec_to_utf8pre(volname, (int)strlen(volname),
                                   converted_volname,
                                   AFP_VOLUME_NAME_UTF8_LEN) != 0) {
        return NULL;
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "find_volume_by_name: looking for '%s' (converted: '%s')",
                   volname, converted_volname);

    for (i = 0; i < server->num_volumes; i++)  {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "find_volume_by_name: comparing with volume[%d] '%s'",
                       i, server->volumes[i].volume_name_printable);

        if (strcmp(converted_volname,
                   server->volumes[i].volume_name_printable) == 0) {
            using_volume = &server->volumes[i];
            goto out;
        }
    }

out:
    return using_volume;
}

/*!
 * @brief Open a volume and take its name encoding and attributes
 *
 * A volume without fixed directory IDs is detached again. A failure writes its
 * reason to mesg.
 *
 * @param[in,out] volume  the volume
 * @param[in]     server  the volume's server
 * @param[out]    mesg    receives the reason for a failure
 * @param[in,out] l       bytes of mesg in use
 * @param[in]     max     capacity of mesg
 * @returns 0 once open, or 1
 */
static int open_volume(struct afp_volume * volume, struct afp_server * server,
                       char *mesg, unsigned int *l, unsigned int max)
{
    unsigned short bitmap =
        kFPVolAttributeBit | kFPVolSignatureBit |
        kFPVolCreateDateBit | kFPVolIDBit |
        kFPVolNameBit;
    char new_encoding;
    int ret;

    if (server->using_version->av_number >= 30) {
        bitmap |= kFPVolNameBit | kFPVolBlockSizeBit;
    }

    /* A new session's directory IDs may differ from the last one's. */
    dir_cache_purge(volume);
    ret = afp_volopen(volume, bitmap,
                      (strlen(volume->volpassword) > 0) ? volume->volpassword : NULL);

    switch (ret) {
    case kFPAccessDenied:
        *l += snprintf(mesg, max - *l,
                       "Incorrect volume password\n");
        goto error;

    case kFPNoErr:
        break;

    case ETIMEDOUT:
        *l += snprintf(mesg, max - *l,
                       "Timed out waiting to open volume\n");
        goto error;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPObjectNotFound:
    case kFPParamErr:
    default:
        /* A request not sent or not answered opens no volume either. */
        *l += snprintf(mesg, max - *l,
                       "Could not open volume\n");
        goto error;
    }

    /* It is said that if a volume's encoding will be the same
     * the server's. */
    if (volume->attributes & kSupportsUTF8Names) {
        new_encoding = kFPUTF8Name;
    } else {
        new_encoding = kFPLongName;
    }

    volume->path_encoding = new_encoding;

    if (volume->signature != AFP_VOL_FIXED) {
        *l += snprintf(mesg, max - *l,
                       "Volume %s does not support fixed directories\n",
                       volume->volume_name_printable);
        afp_detach_volume(volume);
        goto error;
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Volume attributes: 0x%04x (UnixPrivs=%d ExtAttrs=%d UTF8Names=%d NoNetworkUIDs=%d)",
                   volume->attributes,
                   !!(volume->attributes & kSupportsUnixPrivs),
                   !!(volume->attributes & kSupportsExtAttrs),
                   !!(volume->attributes & kSupportsUTF8Names),
                   !!(volume->attributes & kNoNetworkUserIDs));

    if (server->using_version->av_number >= 30) {
        if (volume->attributes & kSupportsUnixPrivs) {
            volume->extra_flags |= VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
        } else {
            volume->extra_flags &= ~VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
        }
    } else {
        /* This is very odd, but AFP 2.x doesn't give timestamps for directories */
    }

    return 0;
error:
    return 1;
}

/*!
 * @brief Open a volume, and mark it mounted and attached
 *
 * @param[in,out] volume  the volume
 * @param[in]     server  the volume's server
 * @param[out]    mesg    receives the reason for a failure
 * @param[in,out] l       bytes of mesg in use
 * @param[in]     max     capacity of mesg
 * @returns 0 once mounted and attached, or 1
 */
int afp_connect_volume(struct afp_volume * volume, struct afp_server * server,
                       char *mesg, unsigned int *l, unsigned int max)
{
    if (open_volume(volume, server, mesg, l, max)) {
        return 1;
    }

    volume->mounted = AFP_VOLUME_MOUNTED;
    volume->attached = AFP_VOLUME_ATTACHED;
    return 0;
}

static _Thread_local const struct afp_server *reconnecting;

/*!
 * @brief Test whether this thread runs the server's reconnect
 *
 * @param[in] server  the server
 * @returns 1 when it does
 */
int afp_server_reconnects_here(const struct afp_server *server)
{
    return reconnecting == server;
}

/*!
 * @brief Connect a disconnected server again, log in and reopen its volumes,
 *        for the thread that owns its reconnect
 *
 * The server must answer with the signature it had, so a changed endpoint
 * never receives the saved credentials, and a removal or a disconnect while
 * the reconnect runs fails it. The reconnect sends alone, and forks survive
 * if the session continues. The caller releases the reconnect.
 *
 * @param[in,out] s     the server
 * @param[out]    mesg  receives the reason for a failure
 * @param[in,out] l     bytes of mesg in use
 * @param[in]     max   capacity of mesg
 * @returns 0 once connected, or 1 with errno set when the reconnect fails
 */
static int reconnect_held(struct afp_server *s, char *mesg, unsigned int *l,
                          unsigned int max)
{
    int i;
    int ret = 0;
    int rc;
    struct afp_volume * v;
    struct afp_server_status status;
    const struct addrinfo *answered = NULL;
    char expected_signature[AFP_SIGNATURE_LEN];
    char expected_signature_text[AFP_SIGNATURE_LEN * 2 + 1];
    char peer_signature_text[AFP_SIGNATURE_LEN * 2 + 1];
    reconnecting = s;
    /* Flush pending requests before reconnecting to avoid late reply confusion */
    afpc_dsi_flush_request_queue(s);

    /* ServerSignature is optional in AFP, but a nonzero value is mandatory
     * for automatic session reuse.  FPGetSrvrInfo is performed before login
     * so a changed endpoint cannot receive the saved credentials. */
    if (!afp_server_has_valid_signature(s)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Refusing to reconnect to %s: saved AFP ServerSignature is zero or unavailable",
                       s->server_name_printable);
        *l += snprintf(mesg + *l, max - *l,
                       "Refusing to resume %s without a valid server signature\n",
                       s->server_name_printable);
        errno = EPROTO;
        goto error;
    }

    memcpy(expected_signature, s->signature, sizeof(expected_signature));
    format_server_signature(expected_signature, expected_signature_text);
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Verifying AFP ServerSignature before reconnecting to %s (expected %s)",
                   s->server_name_printable, expected_signature_text);
    rc = afp_server_probe(s->address, &status, &answered);

    if (rc == 0 && !afp_signature_is_valid(status.signature)) {
        rc = -EPROTO;
    }

    if (rc != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Could not read AFP ServerSignature while reconnecting to %s: %s",
                       s->server_name_printable, strerror(-rc));
        *l += snprintf(mesg + *l, max - *l,
                       "Could not verify server signature for %s\n",
                       s->server_name_printable);
        errno = -rc;
        goto error;
    }

    format_server_signature(status.signature, peer_signature_text);
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "AFP ServerSignature for %s is %s",
                   s->server_name_printable, peer_signature_text);

    if (memcmp(status.signature, expected_signature,
               sizeof(expected_signature)) != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Refusing to reconnect to %s: AFP ServerSignature mismatch (expected %s, got %s)",
                       s->server_name_printable, expected_signature_text,
                       peer_signature_text);
        *l += snprintf(mesg + *l, max - *l,
                       "Refusing to resume %s: server signature changed or is invalid\n",
                       s->server_name_printable);
        errno = EPROTO;
        goto error;
    }

    if (afp_server_connect(s, 0))  {
        *l += snprintf(mesg, max - *l, "Error resuming connection to %s\n",
                       s->server_name_printable);

        if (errno == 0) {
            errno = ECONNRESET;
        }

        goto error;
    }

    /* The password goes only to the address whose status gave the signature:
     * another address of the name may be another server. */
    if (s->used_address != answered) {
        *l += snprintf(mesg + *l, max - *l,
                       "Refusing to resume %s at another address\n",
                       s->server_name_printable);
        errno = EPROTO;
        goto error;
    }

    if (afp_server_open_session(s) != 0) {
        *l += snprintf(mesg, max - *l, "Error opening DSI session to %s\n",
                       s->server_name_printable);
        errno = ECONNRESET;
        goto error;
    }

    /* The connect leaves EINPROGRESS, which would pass for the login's
     * error. */
    errno = 0;

    if (afp_server_login(s, mesg, l, max)) {
        if (errno == 0) {
            errno = EACCES;
        }

        goto error;
    }

    int answer = kFPMiscErr;

    if (__atomic_exchange_n(&s->token_ended, 0, __ATOMIC_ACQ_REL)) {
        s->token.length = 0;
    }

    if (s->token.length > 0) {
        answer = afp_disconnectoldsession(s, 0, &s->token);

        /* A lost reply leaves the hand-over open, so the token is kept. As
         * afpd 4.5+ queues hand-overs, an expired wait counts as refused. */
        if (ll_unanswered(answer) && answer != ETIMEDOUT) {
            *l += snprintf(mesg, max - *l,
                           "Connection to %s ended while resuming\n",
                           s->server_name_printable);
            errno = ECONNRESET;
            goto error;
        }
    }

    if (answer == kFPNoErr) {
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Continued the session on %s",
                       s->server_name_printable);
    } else {
        /* The process that refused the token ends with its connection, so the
         * new session logs in on another, whose token a reconnect can use. */
        if (s->token.length > 0) {
            /* afpd's master hands the connection of an expired wait to the
             * old session, which reads this logout once it takes it over. */
            if (answer == ETIMEDOUT) {
                afp_logout(s, DSI_DONT_WAIT);
            }

            s->token.length = 0;

            if (afp_server_connect(s, 0) || s->used_address != answered
                    || afp_server_open_session(s) != 0) {
                *l += snprintf(mesg, max - *l,
                               "Error resuming connection to %s\n",
                               s->server_name_printable);
                errno = ECONNRESET;
                goto error;
            }

            if (afp_server_login(s, mesg, l, max)) {
                /* errno may still hold this connect's EINPROGRESS. */
                errno = EACCES;
                goto error;
            }
        }

        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "New session on %s; open files fail until reopened",
                       s->server_name_printable);
        afp_server_next_session_generation(s);
        forget_unclosed_forks(s);

        for (i = 0; i < s->num_volumes; i++) {
            v = &s->volumes[i];
            forget_open_forks(v);
            /* Desktop references belong to the old AFP session. */
            v->dtrefnum = 0;

            /* A volume an unmount is detaching stays closed, and the
             * reopen leaves the mount state an unmount may hold. */
            if (v->attached == AFP_VOLUME_ATTACHED
                    && open_volume(v, v->server, mesg, l, max)) {
                *l += snprintf(mesg, max - *l,
                               "Could not mount %s\n",
                               v->volume_name_printable);
                ret = 1;
            }
        }

        /* Unless every volume opened, no token is taken and the next reconnect
         * starts anew; afpd keeps a dropped session, so the logout ends it. */
        if (ret == 0) {
            afp_server_take_token(s);
        } else {
            afp_logout(s, DSI_DONT_WAIT);
            errno = EIO;
        }
    }

    if (ret != 0) {
        goto error;
    }

    /* A removal, or a disconnect, while the reconnect ran took the
     * descriptor or changed the state under the list lock; the reconnect
     * then fails. */
    pthread_mutex_lock(&server_list_mutex);

    if (__atomic_load_n(&s->fd, __ATOMIC_ACQUIRE) < 0
            || s->connect_state != SERVER_STATE_CONNECTING) {
        pthread_mutex_unlock(&server_list_mutex);
        *l += snprintf(mesg, max - *l,
                       "Connection to %s ended while resuming\n",
                       s->server_name_printable);
        errno = ECONNRESET;
        goto error;
    }

    __atomic_store_n(&s->connect_state, SERVER_STATE_CONNECTED,
                     __ATOMIC_RELEASE);
    pthread_mutex_unlock(&server_list_mutex);
    reconnecting = NULL;
    return 0;
error:
    loop_disconnect(s);
    reconnecting = NULL;
    return 1;
}

/*!
 * @brief Connect a disconnected server again, log in and reopen its volumes
 *
 * One thread reconnects a server at a time, as reconnect_held() says. The
 * closes kept from before go out once it ends, as no other request is sent
 * while it runs.
 *
 * @param[in,out] s     the server
 * @param[out]    mesg  receives the reason for a failure
 * @param[in,out] l     bytes of mesg in use
 * @param[in]     max   capacity of mesg
 * @returns 0 once connected, or 1 when another thread reconnects it, and 1
 *          with errno set when the reconnect fails
 */
int afp_server_reconnect(struct afp_server * s, char *mesg,
                         unsigned int *l, unsigned int max)
{
    int ret;

    if (!afp_server_reconnect_try_begin(s)) {
        return 1;
    }

    ret = reconnect_held(s, mesg, l, max);
    afp_server_reconnect_end(s);

    if (ret == 0) {
        close_unclosed_forks(s);
    }

    return ret;
}

/* Run by afp_server_restart() once the password is written, or NULL. */
static void (*restart_hook)(struct afp_server *server);

/*!
 * @brief Set the function afp_server_restart() runs once the password is
 *        written, before the reconnect, for the unit tests
 *
 * @param[in] hook  the function, or NULL for none
 */
void afp_server_set_restart_hook(void (*hook)(struct afp_server *server))
{
    restart_hook = hook;
}

/*!
 * @brief Restart a disconnected server's session at an address, with a
 *        password
 *
 * The reconnect stays this thread's from the address's swap to the
 * password's wipe, so no other thread's reconnect reads either while they
 * change, or logs in with a password the wipe then clears.
 *
 * @param[in,out] s         the server
 * @param[in]     address   the address list, which the server owns from then
 *                          on, or which is freed
 * @param[in]     password  the password the login sends
 * @param[out]    mesg      receives the reason for a failure
 * @param[in,out] l         bytes of mesg in use
 * @param[in]     max       capacity of mesg
 * @returns 0 once connected, or -1 with errno: EINVAL for no server,
 *          ECONNRESET while another thread reconnects the server, or the
 *          reconnect's error
 */
int afp_server_restart(struct afp_server *s, struct addrinfo *address,
                       const char *password, char *mesg, unsigned int *l,
                       unsigned int max)
{
    int ret;

    if (!s) {
        if (address) {
            freeaddrinfo(address);
        }

        errno = EINVAL;
        return -1;
    }

    if (!afp_server_reconnect_try_begin(s)) {
        freeaddrinfo(address);
        errno = ECONNRESET;
        return -1;
    }

    afp_server_replace_address(s, address);
    strlcpy(s->password, password, sizeof(s->password));

    if (restart_hook) {
        restart_hook(s);
    }

    errno = 0;
    ret = reconnect_held(s, mesg, l, max) == 0 ? 0 : -1;
    explicit_bzero(s->password, sizeof(s->password));
    afp_server_reconnect_end(s);

    if (ret == 0) {
        close_unclosed_forks(s);
    }

    return ret;
}


/*!
 * @brief Connect a server's session socket to the first address that answers
 *
 * Replies of older connections are dropped, and the old descriptor is closed
 * once the frame being written on it ends.
 *
 * @param[in,out] server  server to connect
 * @param[in]     full    1 to read the server's status first, 0 not
 * @returns 0, or a negative errno, which errno holds too: the connect's when
 *          no address connects, -ENOENT for a server removed meanwhile, or
 *          the status read's, -ETIMEDOUT when it timed out
 */
int afp_server_connect(struct afp_server *server, int full)
{
    struct addrinfo *address;
    struct afp_server_status status;
    struct timeval t1, t2;
    int error = EINVAL;
    int fd = __atomic_exchange_n(&server->fd, -1, __ATOMIC_ACQ_REL);
    /* The next generation starts before the old descriptor is shut down,
     * so a receiver the shutdown wakes finds its connection replaced. */
    unsigned int connection_generation =
        afp_server_next_connection_generation(server);

    if (fd >= 0) {
        loop_close_descriptor(server, fd);
        log_for_client(NULL, AFPFSD, LOG_INFO,
                       "Closed old socket fd=%d before reconnection", fd);
        fd = -1;
    }

    log_for_client(NULL, AFPFSD, LOG_INFO,
                   "Connection generation now %u", connection_generation);

    for (address = server->address; address; address = address->ai_next) {
        char text[INET6_ADDRSTRLEN];
        int session;
        int rc;
        address_text(address, text);
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "Attempting connection to %s ...", text);
        fd = open_connection(address);

        if (fd < 0) {
            error = errno;
            continue;
        }

        if (await_connection(fd, DSI_DEFAULT_TIMEOUT) < 0) {
            error = errno;
            close(fd);
            fd = -1;
            continue;
        }

        if (!full) {
            break;
        }

        /* The session's connection is made while the status is read. */
        session = open_connection(address);
        gettimeofday(&t1, NULL);
        rc = exchange_status(fd, &status);
        gettimeofday(&t2, NULL);
        close(fd);

        if (rc == 0 && (session < 0
                        || await_connection(session,
                                            DSI_DEFAULT_TIMEOUT) < 0)) {
            rc = -errno;
        }

        /* afpd counts a status's process against its cap until the status
         * closes, and answers unasked a connection the cap refuses. */
        if (rc == 0 && answered_unasked(session)) {
            close(session);
            session = open_connection(address);

            if (session < 0
                    || await_connection(session, DSI_DEFAULT_TIMEOUT) < 0) {
                rc = -errno;
            }
        }

        if (rc != 0) {
            if (session >= 0) {
                close(session);
            }

            errno = -rc;
            return rc;
        }

        fd = session;
        afp_server_adopt_status(server, &status);

        if ((t2.tv_sec - t1.tv_sec) > 0) {
            server->tx_delay = (t2.tv_sec - t1.tv_sec) * 1000;
        } else {
            server->tx_delay = (t2.tv_usec - t1.tv_usec) / 1000;
        }

        break;
    }

    if (fd < 0) {
        errno = error;
        return -error;
    }

    server->exit_flag = 0;

    /* AFP 3.3+: Only reset request ID if replay cache is not supported.
     * With replay cache, request IDs persist across reconnections. */
    if (!server->supports_replay_cache) {
        server->lastrequestid = 0;
    }

    /* A server removed while this connect ran stays removed: its state,
     * its list entry and its published descriptor change under the list
     * lock. */
    pthread_mutex_lock(&server_list_mutex);

    if (server->connect_state == SERVER_STATE_DESTROYED) {
        pthread_mutex_unlock(&server_list_mutex);
        close(fd);
        errno = ENOENT;
        return -ENOENT;
    }

    __atomic_store_n(&server->fd, fd, __ATOMIC_RELEASE);
    __atomic_store_n(&server->connect_state, SERVER_STATE_CONNECTING,
                     __ATOMIC_RELEASE);
    server->used_address = address;
    int found = 0;

    for (struct afp_server *s = server_base; s; s = s->next) {
        if (s == server) {
            found = 1;
            break;
        }
    }

    if (!found) {
        server->next = server_base;
        server_base = server;
    }

    add_fd_and_signal(fd);
    pthread_mutex_unlock(&server_list_mutex);
    return 0;
}

/*!
 * @brief Open a DSI session, once more on a fresh connection when refused
 *
 * A session refused while a status's process still counts against afpd's cap
 * opens once that process ends; another address may be another server. The
 * fresh connection reads the address list, which only a reconnect's owner
 * replaces, so it owns the server's reconnect too.
 *
 * @param[in,out] server  the server, connected
 * @returns afpc_dsi_opensession()'s result, or -EPROTO for a fresh connection
 *          at another address
 */
int afp_server_open_session(struct afp_server *server)
{
    const struct addrinfo *address = server->used_address;
    int owner = afp_server_reconnects_here(server);
    int rc = afpc_dsi_opensession(server);

    if (rc == 0 || rc == ETIMEDOUT
            || (!owner && !afp_server_reconnect_try_begin(server))) {
        return rc;
    }

    reconnecting = server;

    if (afp_server_connect(server, 0) == 0) {
        rc = -EPROTO;

        if (server->used_address == address) {
            rc = afpc_dsi_opensession(server);
        }
    }

    if (!owner) {
        reconnecting = NULL;
        afp_server_reconnect_end(server);
    }

    return rc;
}

/*!
 * @brief The highest AFP version both sides speak, at most the one asked for
 *
 * @param[in] versions   the server's version numbers, ending at a 0
 * @param[in] requested  the version asked for, or 0 for any
 * @returns the version, or NULL for none in common
 */
struct afp_versions *pick_version(unsigned char *versions,
                                  unsigned char requested)
{
    /* Pick the highest mutually supported AFP version, bounded by the
       requested version when one was supplied. */
    int i;
    char version_num = 0;
    struct afp_versions * p;

    if (requested) {
        requested = min(requested, AFP_MAX_SUPPORTED_VERSION);
    }

    i = 0;

    while (i < SERVER_MAX_VERSIONS) {
        if (!versions[i]) {
            break;
        }

        if (!requested || versions[i] <= requested) {
            version_num = max(version_num, versions[i]);
        }

        i++;
    }

    for (p = afp_versions; p->av_name; p++) {
        if (p->av_number == version_num) {
            return p;
        }
    }

    return NULL;
}

/*!
 * @brief The highest UAM both masks hold
 *
 * @param[in] uam2  one mask
 * @param[in] uam1  the other
 * @returns the UAM's bit, or -1 for none
 */
int pick_uam(unsigned int uam2, unsigned int uam1)
{
    int i;

    for (i = 15; i >= 0; i--) {
        if (((1 << i)) & (uam2 & uam1)) {
            return (1 << i);
        }
    }

    return -1;
}

/*!
 * @brief Write a server's volume names, comma-separated
 *
 * The names stop where the buffer ends, the last of them cut short there.
 *
 * @param[in]  server  the server
 * @param[out] names   receives the names and a NUL
 * @param[in]  max     capacity of names, 1 or more
 * @returns the bytes the names take in names
 */
int afp_list_volnames(struct afp_server * server, char *names, int max)
{
    int len = 0;
    names[0] = '\0';

    for (int i = 0; i < server->num_volumes && len < max - 1; i++) {
        int n = snprintf(names + len, (size_t)(max - len), "%s%s",
                         i > 0 ? ", " : "",
                         server->volumes[i].volume_name_printable);

        if (n < 0) {
            break;
        }

        /* snprintf() counts the bytes of the whole name. */
        len += n < max - len ? n : max - len - 1;
    }

    return len;
}
