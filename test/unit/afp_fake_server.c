/*
    afp_fake_server.c: the server end of an AFP session for the unit tests

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/dsi.h"
#include "lib/dsi_protocol.h"
#include "lib/utils.h"

/* The largest request read; a larger one ends the server. */
#define AFP_FAKE_SERVER_MAX_REQUEST 65536U
/* A reply goes out in pieces no socket's buffer is smaller than. */
#define AFP_FAKE_SERVER_PIECE 4096U

/*!
 * @brief Read len bytes from a stream that may deliver them in pieces
 *
 * @param[in]  fd   the descriptor
 * @param[out] buf  receives the bytes
 * @param[in]  len  bytes to read
 * @returns 0, or -1 when the stream ends or fails first
 */
static int read_full(int fd, void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, (char *) buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Write len bytes to a stream that may take them in pieces
 *
 * @param[in] fd   the descriptor
 * @param[in] buf  the bytes
 * @param[in] len  bytes to write
 * @returns 0, or -1 when a write fails
 */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, (const char *) buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        done += (size_t) n;
    }

    return 0;
}

/*!
 * @brief Run the receiver on the client's end until it has read len more bytes
 *
 * It runs as the loop thread would.
 *
 * @param[in] fake  the fake server
 * @param[in] len   bytes the receiver is to read
 * @returns 0, or -1 when the receiver fails
 */
static int deliver(const struct afp_fake_server *fake, size_t len)
{
    uint64_t end = fake->server->stats.rx_bytes + len;

    while (fake->server->stats.rx_bytes < end) {
        if (afpc_dsi_recv(fake->server) < 0) {
            return -1;
        }
    }

    return 0;
}

/*!
 * @brief Write a reply's bytes and deliver them a piece at a time
 *
 * The server's thread is the one reader of the client's end, so a piece must
 * fit the socket's buffer.
 *
 * @param[in] fake  the fake server
 * @param[in] buf   the reply, DSI header first
 * @param[in] len   bytes of buf
 * @returns 0, or -1 when a write or the receiver fails
 */
static int send_reply(const struct afp_fake_server *fake, const void *buf,
                      size_t len)
{
    for (size_t done = 0; done < len;) {
        size_t piece = len - done;

        if (piece > AFP_FAKE_SERVER_PIECE) {
            piece = AFP_FAKE_SERVER_PIECE;
        }

        if (write_full(fake->peer, (const char *) buf + done, piece) < 0
                || deliver(fake, piece) < 0) {
            return -1;
        }

        done += piece;
    }

    return 0;
}

/*!
 * @brief Whether an AFP command names an open fork after its pad byte
 *
 * @param[in] command  the AFP command
 * @returns 1 for a fork's command, 0 for another
 */
static int names_a_fork(uint8_t command)
{
    switch (command) {
    case afpByteRangeLock:
    case afpCloseFork:
    case afpFlushFork:
    case afpGetForkParms:
    case afpRead:
    case afpSetForkParms:
    case afpWrite:
    case afpByteRangeLockExt:
    case afpReadExt:
    case afpWriteExt:
    case afpSyncFork:
        return 1;

    default:
        return 0;
    }
}

/*!
 * @brief Answer a request by its AFP command
 *
 * @param[in,out] fake     the fake server, which counts the forks it opens
 * @param[in]     request  the request's DSI header
 * @param[in]     payload  the request's AFP bytes
 * @param[in]     length   the length of payload
 * @returns 0, or -1 when the reply cannot be sent
 */
static int answer(struct afp_fake_server *fake,
                  const struct dsi_header *request,
                  const unsigned char *payload, uint32_t length)
{
    struct dsi_header header;
    unsigned char data[8];
    const void *body = data;
    uint32_t body_len = 0;
    int32_t result = kFPNoErr;
    uint64_t offset;
    uint64_t count;
    uint16_t value;
    uint16_t access = 0;

    if (payload[0] == afpOpenFork && length >= 12) {
        memcpy(&value, payload + 10, sizeof(value));
        access = ntohs(value);
    }

    if (names_a_fork(payload[0]) && length >= 4) {
        memcpy(&value, payload + 2, sizeof(value));
    } else {
        value = 0;
    }

    if (names_a_fork(payload[0])
            && (fake->forks_open == 0 || ntohs(value) != AFP_FAKE_SERVER_FORK)) {
        /* afpd refuses a fork it never opened, or has closed. */
        result = kFPParamErr;
    } else if (payload[0] == fake->refuse) {
        result = kFPAccessDenied;
    } else if (payload[0] == afpOpenFork && fake->read_only
               && (access & AFP_OPENFORK_ALLOWWRITE)) {
        /* A read-only volume refuses an open for writing, as afpd does. */
        result = kFPVolLocked;
    } else if (payload[0] == afpCreateFile && fake->name_taken && length >= 2
               && !(payload[1] & kFPHardCreate)) {
        /* A soft create refuses a name taken; afpd's hard create empties
         * the file the name holds. */
        result = kFPObjectExists;
    } else if (payload[0] == afpOpenFork) {
        /* No file parameters, then the fork number. */
        value = 0;
        memcpy(data, &value, sizeof(value));
        value = htons(AFP_FAKE_SERVER_FORK);
        memcpy(data + 2, &value, sizeof(value));
        body_len = 4;
        fake->forks_open++;
    } else if (payload[0] == afpCloseFork) {
        fake->forks_open--;
    } else if ((payload[0] == afpReadExt || payload[0] == afpWriteExt)
               && length >= 20) {
        memcpy(&offset, payload + 4, sizeof(offset));
        memcpy(&count, payload + 12, sizeof(count));
        offset = ntoh64(offset);
        count = ntoh64(count);

        if (payload[0] == afpWriteExt) {
            /* The end of the write. */
            offset = hton64(offset + count);
            memcpy(data, &offset, sizeof(offset));
            body_len = 8;
        } else {
            /* A link's target, which afpd reads whole: a read that asks
             * for some of it but less than the rest is refused, and one
             * that asks for more returns the rest with kFPEOFErr. */
            size_t size = fake->read_data ? strlen(fake->read_data) : 0;

            if (offset > size) {
                offset = size;
            }

            if (count != 0 && count < size - offset) {
                count = 0;
                result = kFPParamErr;
            } else if (count > size - offset) {
                count = size - offset;
                result = kFPEOFErr;
            }

            if (fake->read_data) {
                body = fake->read_data + offset;
            }

            body_len = (uint32_t) count;
        }
    }

    memset(&header, 0, sizeof(header));
    header.flags = DSI_REPLY;
    header.command = request->command;
    header.requestid = request->requestid;
    header.return_code.error_code = (int) htonl((uint32_t) result);
    header.length = htonl(body_len);

    if (send_reply(fake, &header, sizeof(header)) < 0) {
        return -1;
    }

    return body_len ? send_reply(fake, body, body_len) : 0;
}

/*!
 * @brief Answer the client's requests until the session ends
 *
 * Each request gets the answer fake's fields ask for, or kFPNoErr alone; the
 * receiver runs on each reply, as the loop does for a listed server.
 *
 * @param[in,out] arg  the fake server
 * @returns NULL
 */
static void *serve(void *arg)
{
    struct afp_fake_server *fake = arg;
    struct dsi_header header;

    while (read_full(fake->peer, &header, sizeof(header)) == 0) {
        uint32_t length = ntohl(header.length);
        unsigned char *payload;
        int rc;

        if (length == 0 || length > AFP_FAKE_SERVER_MAX_REQUEST) {
            break;
        }

        payload = malloc(length);

        if (!payload || read_full(fake->peer, payload, length) < 0) {
            free(payload);
            break;
        }

        if (fake->ncommands < sizeof(fake->commands)) {
            fake->commands[fake->ncommands++] = payload[0];
        }

        if (payload[0] == fake->inspect) {
            pthread_mutex_lock(&fake->volume.open_forks_mutex);

            if (fake->volume.open_forks) {
                memcpy(&fake->listed, fake->volume.open_forks,
                       sizeof(fake->listed));
                fake->inspected = 1;
            }

            pthread_mutex_unlock(&fake->volume.open_forks_mutex);
        }

        rc = answer(fake, &header, payload, length);
        free(payload);

        if (rc < 0) {
            break;
        }
    }

    /* The server's end closes, so a request after the end fails at once
     * where it would wait unread. */
    close(fake->peer);
    fake->peer = -1;
    return NULL;
}

/*!
 * @brief Close the session's ends and release its server and the volume's
 *        locks
 *
 * @param[in,out] fake  the fake server
 */
static void release(struct afp_fake_server *fake)
{
    if (fake->peer >= 0) {
        close(fake->peer);
    }

    close(fake->server->fd);
    fake->server->fd = -1;
    afp_server_release(fake->server);
    fake->server = NULL;
    fake->volume.server = NULL;
    pthread_rwlock_destroy(&fake->volume.open_lock);
    pthread_mutex_destroy(&fake->volume.open_forks_mutex);
    pthread_mutex_destroy(&fake->volume.did_cache_mutex);
}

/*!
 * @brief Open a session on the fake server and start the server's thread
 *
 * The start gives the server AFP 3.4, and the volume its server, its locks,
 * UTF-8 names and a fixed signature; the caller zeroes the fake and sets
 * refuse, inspect, read_only, name_taken, read_data and the volume's other
 * fields first.
 *
 * @param[in,out] fake  the fake server
 * @returns 0, or -1 when the session cannot be made
 */
int afp_fake_server_start(struct afp_fake_server *fake)
{
    static struct afp_versions afp34 = { "AFP3.4", 34 };
    int fds[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return -1;
    }

    fake->server = afp_server_init(NULL);

    if (!fake->server) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    fake->server->using_version = &afp34;
    fake->server->fd = fds[0];
    fake->server->connect_state = SERVER_STATE_CONNECTED;
    fake->peer = fds[1];
    pthread_mutex_init(&fake->volume.did_cache_mutex, NULL);
    pthread_mutex_init(&fake->volume.open_forks_mutex, NULL);
    pthread_rwlock_init(&fake->volume.open_lock, NULL);
    fake->volume.server = fake->server;
    fake->volume.path_encoding = kFPUTF8Name;
    fake->volume.signature = AFP_VOL_FIXED;

    if (pthread_create(&fake->thread, NULL, serve, fake) != 0) {
        release(fake);
        return -1;
    }

    return 0;
}

/*!
 * @brief End the client's requests, wait for the server's thread, and release
 *        the session
 *
 * The server's thread reads the requests' end and returns; the volume's
 * open-fork list stays as the client left it.
 *
 * @param[in,out] fake  the fake server
 */
void afp_fake_server_stop(struct afp_fake_server *fake)
{
    shutdown(fake->server->fd, SHUT_WR);
    pthread_join(fake->thread, NULL);
    release(fake);
}

/*!
 * @brief Fill the stack below the caller's frame with a pattern
 *
 * A record the function the caller calls next declares and leaves
 * uninitialized then holds the pattern, not zeroes. It lives outside the
 * caller's file, so no build without link-time optimization inlines it into
 * the caller's frame.
 */
void afp_fake_server_dirty_stack(void)
{
    unsigned char area[65536];
    volatile unsigned char *p = area;

    for (size_t i = 0; i < sizeof(area); i++) {
        p[i] = 0xa5;
    }
}
