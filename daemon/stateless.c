/*
 *  stateless.c - Stateless AFP client library
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "netatalk-client/afpsl.h"
#include "lib/afp_internal.h"
#include "lib/client.h"
#include "lib/mapping.h"
#include "lib/uam_registry.h"
#include "lib/utils.h"

#include "server.h"
#include "stateless_internal.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define default_uam "Cleartxt Passwrd"
#define AFPSLD_FILENAME "afpsld"

struct stateless_connection {
    int fd;
    unsigned int len;
    char data[AFPSL_IPC_MAX_RESPONSE + AFPSL_IPC_LOG_BUFFER_SIZE + 200];
};

static struct stateless_connection connection = { .fd = -1 };

static void close_connection(void)
{
    if (connection.fd >= 0) {
        close(connection.fd);
        connection.fd = -1;
    }
}

static void default_log_callback(void *user_data, int loglevel,
                                 const char *message)
{
    (void) user_data;
    (void) loglevel;
    fprintf(stderr, "%s\n", message);
}

static afp_sl_log_callback log_callback = default_log_callback;
static void *log_callback_data;
static int read_bytes_with_timeout(int fd, char *buf, size_t len);
static int server_result_to_errno(int result);
static int ensure_daemon_connection(void);

static void stateless_log_message(int loglevel, const char *message)
{
    char escaped[MAX_ERROR_LEN * 4];
    sanitize_text(message, escaped, sizeof(escaped));

    if (log_callback) {
        log_callback(log_callback_data, loglevel, escaped);
    }
}

static void stateless_log_errno(int loglevel, const char *context,
                                int error_number)
{
    char message[MAX_ERROR_LEN];
    snprintf(message, sizeof(message), "%s: %s", context,
             strerror(error_number));
    stateless_log_message(loglevel, message);
}

static void stateless_log_command_errno(int loglevel, unsigned int command,
                                        int error_number)
{
    char message[MAX_ERROR_LEN];
    snprintf(message, sizeof(message),
             "Write to afpsld failed for command %u: %s", command,
             strerror(error_number));
    stateless_log_message(loglevel, message);
}

void afp_sl_set_log_callback(afp_sl_log_callback callback, void *user_data)
{
    log_callback = callback;
    log_callback_data = user_data;
}

unsigned int afp_sl_default_uams(void)
{
    return default_uams_mask();
}

unsigned int afp_sl_uam_by_name(const char *name)
{
    unsigned int supported_uams;

    if (!name) {
        return 0;
    }

    supported_uams = afp_sl_default_uams() | AFP_SL_UAM_NO_USER_AUTH;
    return (unsigned int)uam_string_to_bitmap(name) & supported_uams;
}

void afp_sl_url_init(struct afpc_url *url)
{
    afp_default_url(url);
}

int afp_sl_url_parse(struct afpc_url *url, const char *text)
{
    return afp_parse_url_quiet(url, text);
}

int afp_sl_response_content_length(const char *response, size_t len,
                                   size_t *content_len)
{
    struct afpsl_ipc_log_footer footer;

    if (len < sizeof(footer)) {
        return -1;
    }

    memcpy(&footer, response + len - sizeof(footer), sizeof(footer));

    if (footer.magic != AFPSL_IPC_LOG_MAGIC
            || footer.log_len > AFPSL_IPC_LOG_BUFFER_SIZE
            || footer.log_len > len - sizeof(footer)) {
        return -1;
    }

    *content_len = len - sizeof(footer) - footer.log_len;
    return 0;
}

int afp_sl_dispatch_response_logs(const char *response, size_t len)
{
    struct afpsl_ipc_log_footer footer;
    size_t content_len;
    size_t pos;

    if (afp_sl_response_content_length(response, len, &content_len) < 0) {
        stateless_log_message(LOG_ERR,
                              "Response from afpsld has no valid log trailer");
        return -1;
    }

    memcpy(&footer, response + len - sizeof(footer), sizeof(footer));
    pos = content_len;

    while (pos < len - sizeof(footer)) {
        struct afpsl_ipc_log_record record;
        char message[AFPSL_IPC_LOG_BUFFER_SIZE + 1];

        if (len - sizeof(footer) - pos < sizeof(record)) {
            stateless_log_message(LOG_ERR,
                                  "Malformed log record from afpsld");
            return -1;
        }

        memcpy(&record, response + pos, sizeof(record));
        pos += sizeof(record);

        if (record.message_len > AFPSL_IPC_LOG_BUFFER_SIZE
                || record.message_len > len - sizeof(footer) - pos) {
            stateless_log_message(LOG_ERR,
                                  "Malformed log message from afpsld");
            return -1;
        }

        memcpy(message, response + pos, record.message_len);
        message[record.message_len] = '\0';
        stateless_log_message(record.level, message);
        pos += record.message_len;
    }

    return 0;
}

static int read_response_tail(unsigned int total_len, size_t prefix_len,
                              char **tail, size_t *content_len)
{
    size_t tail_len;

    if (total_len < prefix_len + sizeof(struct afpsl_ipc_log_footer)) {
        return -1;
    }

    tail_len = total_len - prefix_len;
    *tail = malloc(tail_len);

    if (!*tail) {
        return -1;
    }

    if (read_bytes_with_timeout(connection.fd, *tail, tail_len) < 0
            || afp_sl_response_content_length(*tail, tail_len, content_len) < 0
            || afp_sl_dispatch_response_logs(*tail, tail_len) < 0) {
        free(*tail);
        *tail = NULL;
        return -1;
    }

    return 0;
}

static int start_afpsld(void)
{
    int error_pipe[2];
    int child_errno = 0;
    ssize_t bytes_read;
    pid_t child;
    char *argv[2];
    argv[0] = AFPSLD_FILENAME;
    argv[1] = NULL;

    if (pipe(error_pipe) < 0) {
        stateless_log_errno(LOG_ERR, "Could not create afpsld startup pipe",
                            errno);
        return -1;
    }

    if (fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC) < 0
            || fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        stateless_log_errno(LOG_ERR,
                            "Could not configure afpsld startup pipe", errno);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }

    child = fork();

    if (child < 0) {
        stateless_log_errno(LOG_ERR, "Could not fork afpsld", errno);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }

    if (child == 0) {
        char filename[PATH_MAX];
        close(error_pipe[0]);
        snprintf(filename, PATH_MAX, "%s/%s", BINDIR, AFPSLD_FILENAME);
        execv(filename, argv);
        /* exec failed */
        child_errno = errno;
        (void) write(error_pipe[1], &child_errno, sizeof(child_errno));
        _exit(1);
    }

    close(error_pipe[1]);

    do {
        bytes_read = read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (bytes_read < 0 && errno == EINTR);

    close(error_pipe[0]);

    if (bytes_read < 0) {
        stateless_log_errno(LOG_ERR, "Could not read afpsld startup status",
                            errno);
        return -1;
    }

    if (bytes_read > 0) {
        (void) waitpid(child, NULL, 0);
        stateless_log_errno(LOG_ERR, "Could not execute afpsld", child_errno);
        return -1;
    }

    return 0;
}

int daemon_connect(unsigned int daemon_uid)
{
    int sock;
    struct sockaddr_un servaddr;
    int retries = 50;
    int ret;

    /* Check if we already have a valid connection */
    if (connection.fd >= 0) {
        /* Reuse existing connection */
        stateless_log_message(LOG_DEBUG, "Reusing connection to afpsld");
        return 0;
    }

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        stateless_log_errno(LOG_ERR, "Could not create socket", errno);
        return -1;
    }

#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;
    snprintf(servaddr.sun_path, sizeof(servaddr.sun_path), "%s-%d",
             AFPSL_IPC_SOCKET_PATH, daemon_uid);
    /* Try to connect first */
    ret = connect(sock, (struct sockaddr*) &servaddr, sizeof(servaddr));

    if (ret >= 0) {
        stateless_log_message(LOG_DEBUG, "Connected to running afpsld");
        goto done;
    }

    stateless_log_message(LOG_DEBUG, "Starting afpsld");

    if (start_afpsld() != 0) {
        stateless_log_message(LOG_ERR, "Error starting afpsld daemon");
        close(sock);
        return -1;
    }

    /* Wait for daemon to start */
    while (retries > 0) {
        struct timespec ts = {0, 100000000}; /* 100ms */
        nanosleep(&ts, NULL);
        ret = connect(sock, (struct sockaddr*) &servaddr, sizeof(servaddr));

        if (ret >= 0) {
            stateless_log_message(LOG_DEBUG,
                                  "Connected to newly started afpsld");
            goto done;
        }

        retries--;
    }

    stateless_log_errno(LOG_ERR, "Could not connect to afpsld daemon", errno);
    close(sock);
    return -1;
done:
    connection.fd = sock;
    return 0;
}

/* Read one complete length-prefixed response from afpsld. */
int afp_sl_read_framed_response(int fd, char *data, size_t capacity,
                                size_t *response_len)
{
    struct afpsl_ipc_response_header header;
    unsigned int expected_len = 0;
    size_t received = 0;
    ssize_t packetlen;
    struct timeval tv;
    fd_set rds, ords;
    int ret;

    if (!response_len) {
        return -1;
    }

    *response_len = 0;

    if (!data || fd < 0 || fd >= FD_SETSIZE
            || capacity < sizeof(header)
            + sizeof(struct afpsl_ipc_log_footer)) {
        return -1;
    }

    memset(data, 0, capacity);
    FD_ZERO(&rds);
    FD_SET(fd, &rds);

    while (1) {
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ords = rds;
        ret = select(fd + 1, &ords, NULL, NULL, &tv);

        if (ret == 0) {
            stateless_log_message(LOG_ERR,
                                  "No response from afpsld, timed out");
            return -1;
        }

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            stateless_log_errno(LOG_ERR,
                                "Error waiting for response from afpsld",
                                errno);
            return -1;
        }

        if (FD_ISSET(fd, &ords)) {
            if (received >= capacity) {
                stateless_log_message(LOG_ERR,
                                      "Response from afpsld exceeds buffer");
                return -1;
            }

            packetlen = read(fd, data + received, capacity - received);

            if (packetlen < 0) {
                if (errno == EINTR) {
                    continue;
                }

                stateless_log_errno(LOG_ERR,
                                    "Error reading response from afpsld",
                                    errno);
                return -1;
            }

            if (packetlen == 0) {
                stateless_log_message(
                    LOG_WARNING,
                    "afpsld closed connection before completing response");
                return -1;
            }

            received += (size_t) packetlen;

            if (received >= sizeof(header) && expected_len == 0) {
                memcpy(&header, data, sizeof(header));
                expected_len = header.len;

                if (expected_len < sizeof(header)
                        + sizeof(struct afpsl_ipc_log_footer)) {
                    stateless_log_message(
                        LOG_ERR, "Invalid response length from afpsld");
                    return -1;
                }
            }

            if (expected_len > capacity) {
                stateless_log_message(LOG_ERR,
                                      "Oversized response from afpsld");
                return -1;
            }

            if (received == expected_len) {
                *response_len = received;
                return 0;
            }

            if (expected_len > 0 && received > expected_len) {
                stateless_log_message(LOG_ERR,
                                      "Overlong response from afpsld");
                return -1;
            }
        }
    }
}

/* Read, validate, and dispatch one complete response from afpsld. */
static int read_answer(void)
{
    struct afpsl_ipc_response_header header;
    size_t response_len;
    connection.len = 0;

    if (afp_sl_read_framed_response(connection.fd, connection.data,
                                    sizeof(connection.data),
                                    &response_len) < 0) {
        goto error;
    }

    if (response_len > UINT_MAX) {
        goto error;
    }

    connection.len = (unsigned int) response_len;

    if (afp_sl_dispatch_response_logs(connection.data, connection.len) < 0) {
        goto error;
    }

    memcpy(&header, connection.data, sizeof(header));
    return header.result;
error:
    close_connection();
    connection.len = 0;
    return -1;
}

/* Helper to write exactly len bytes */
static int write_bytes(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    ssize_t ret;

    while (total < len) {
        ret = send(fd, buf + total, len - total, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        total += ret;
    }

    return 0;
}

/* Helper to read exactly len bytes with timeout */
static int read_bytes_with_timeout(int fd, char *buf, size_t len)
{
    size_t total = 0;
    fd_set rds;
    struct timeval tv;
    int ret;

    while (total < len) {
        FD_ZERO(&rds);
        FD_SET(fd, &rds);
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ret = select(fd + 1, &rds, NULL, NULL, &tv);

        if (ret <= 0) {
            return -1;
        }

        ssize_t r = read(fd, buf + total, len - total);

        if (r < 0 && errno == EINTR) {
            continue;
        }

        if (r <= 0) {
            return -1;
        }

        total += r;
    }

    return 0;
}

static int send_command(unsigned int len, char * data, unsigned int num)
{
    int ret = write_bytes(connection.fd, data, len);

    if (ret < 0) {
        stateless_log_command_errno(LOG_ERR, num, errno);
        close_connection();
    }

    return  ret;
}

static int send_to_daemon(char * request, int req_len, char * reply,
                          int reply_len)
{
    int ret;
    unsigned int command = 0;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    if (req_len >= (int)sizeof(struct afpsl_ipc_request_header)) {
        command = ((struct afpsl_ipc_request_header *)request)->command;
    }

    if (send_command(req_len, request, command) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0 || connection.len < (unsigned int) reply_len) {
        stateless_log_message(LOG_ERR, "Error reading response from afpsld");
        close_connection();
        return -ECONNRESET;
    }

    memcpy(reply, connection.data, reply_len);
    return 0;
}

int afp_sl_exit(void)
{
    struct afpsl_ipc_exit_request req;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    req.header.command = AFPSL_IPC_COMMAND_EXIT;
    req.header.close = 1;
    req.header.len = sizeof(req);

    if (send_command(sizeof(req), (char *) &req, AFPSL_IPC_COMMAND_EXIT) < 0) {
        return -ECONNRESET;
    }

    int ret = read_answer();
    return ret < 0 ? -ECONNRESET : server_result_to_errno(ret);
}

/* afp_sl_status()
 *
 * Returns:
 * AFPSL_IPC_RESULT_DAEMON_ERROR: could not connect to afpsld
 */

int afp_sl_status(const char *volumename, const char *servername, char *text,
                  unsigned int *remaining)
{
    struct afpsl_ipc_status_request req;
    int ret;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    memset(&req, 0, sizeof(req));
    req.header.command = AFPSL_IPC_COMMAND_STATUS;
    req.header.close = 0;
    req.header.len = sizeof(req);

    if (volumename) {
        snprintf(req.volumename, AFP_VOLUME_NAME_UTF8_LEN, "%s", volumename);
    }

    if (servername) {
        snprintf(req.servername, AFP_SERVER_NAME_LEN, "%s", servername);
    }

    if (send_command(sizeof(req), (char *)&req, AFPSL_IPC_COMMAND_STATUS) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0) {
        return -ECONNRESET;
    }

    ret = server_result_to_errno(ret);

    if (ret < 0) {
        return ret;
    }

    strlcpy(text, connection.data + sizeof(struct afpsl_ipc_status_response),
            *remaining);
    return 0;
}

/* afp_sl_getvolid()
 *
 * Result header returns:
 * AFPSL_IPC_RESULT_DAEMON_ERROR
 * AFPSL_IPC_RESULT_OK
 *
 */

int afp_sl_getvolid(afpc_server_t serverid, struct afpc_url * url,
                    afpc_volume_t *volid)
{
    struct afpsl_ipc_getvolid_request req;
    struct afpsl_ipc_getvolid_response response;
    int ret;
    int retries = 10;

    if (!serverid) {
        if (!url || url->password[0] != '\0') {
            return -EACCES;
        }

        ret = afp_sl_resume(url, default_uams_mask(), &serverid, NULL);

        if (ret != 0) {
            return ret;
        }

        if (!serverid) {
            return -EIO;
        }
    }

    memset(&req, 0, sizeof(req));
    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_getvolid_request);
    req.header.command = AFPSL_IPC_COMMAND_GETVOLID;
    req.serverid = serverid;
    memcpy(&req.url, url, sizeof(*url));

    while (1) {
        ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                             sizeof(response));

        if (ret != 0) {
            return ret;
        }

        /* Volume exists but attach is still in progress from another client;
         * wait briefly and retry so callers don't see a spurious error. */
        if (response.header.result == AFPSL_IPC_RESULT_NOTATTACHED
                && --retries > 0) {
            struct timespec ts = {0, 200000000}; /* 200ms */
            nanosleep(&ts, NULL);
            continue;
        }

        break;
    }

    if (response.header.result == AFPSL_IPC_RESULT_OK) {
        memcpy(volid, &response.volumeid, sizeof(afpc_volume_t));
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_stat(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                struct stat *stat)
{
    struct afpsl_ipc_stat_request request;
    struct afpsl_ipc_stat_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_stat_request);
    request.header.command = AFPSL_IPC_COMMAND_STAT;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    ret = server_result_to_errno(response.header.result);

    if (ret == 0) {
        memcpy(stat, &response.stat, sizeof(struct stat));
    }

    return ret;
}

int afp_sl_open(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                unsigned int *fileid, unsigned int mode)
{
    struct afpsl_ipc_open_request request;
    struct afpsl_ipc_open_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_open_request);
    request.header.command = AFPSL_IPC_COMMAND_OPEN;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    ret = server_result_to_errno(response.header.result);

    if (ret == 0) {
        *fileid = response.fileid;
    }

    return ret;
}


int afp_sl_read(afpc_volume_t * volid, unsigned int fileid,
                unsigned int resource,
                unsigned long long start,
                unsigned int length, unsigned int *received,
                unsigned int *eof, char *data)
{
    struct afpsl_ipc_read_request request;
    struct afpsl_ipc_read_response response;
    char *tail = NULL;
    size_t payload_size = 0;
    int ret;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_read_request);
    request.header.command = AFPSL_IPC_COMMAND_READ;
    memcpy(&request.volumeid, volid, sizeof(afpc_volume_t));
    request.fileid = fileid;
    request.start = start;
    request.length = length;
    request.resource = resource;

    if (send_command(sizeof(request), (char *)&request,
                     AFPSL_IPC_COMMAND_READ) < 0) {
        return -ECONNRESET;
    }

    /* Read just the response header first */
    ret = read_bytes_with_timeout(connection.fd, (char *)&response,
                                  sizeof(response));

    if (ret < 0) {
        stateless_log_message(LOG_ERR,
                              "Error reading read response header");
        close_connection();
        return -ECONNRESET;
    }

    if (read_response_tail(response.header.len, sizeof(response), &tail,
                           &payload_size)
            < 0) {
        stateless_log_message(LOG_ERR,
                              "Error reading read response payload");
        close_connection();
        return -EPROTO;
    }

    if (response.header.result != AFPSL_IPC_RESULT_OK) {
        free(tail);
        *received = 0;
        *eof = 0;
        return server_result_to_errno(response.header.result);
    }

    if (payload_size != response.received || payload_size > length) {
        free(tail);
        return -EPROTO;
    }

    *received = response.received;
    *eof = response.eof;
    memcpy(data, tail, payload_size);
    free(tail);
    return 0;
}

int afp_sl_write(afpc_volume_t * volid, unsigned int fileid,
                 unsigned int resource,
                 unsigned long long offset, unsigned int size,
                 unsigned int *written, const char *data)
{
    struct afpsl_ipc_write_request request;
    struct afpsl_ipc_write_response response;
    char *tail = NULL;
    size_t payload_size = 0;
    int ret;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_write_request);
    request.header.command = AFPSL_IPC_COMMAND_WRITE;
    memcpy(&request.volumeid, volid, sizeof(afpc_volume_t));
    request.fileid = fileid;
    request.offset = offset;
    request.size = size;
    request.resource = resource;

    /* Send request header */
    if (send_command(sizeof(request), (char *)&request,
                     AFPSL_IPC_COMMAND_WRITE) < 0) {
        return -ECONNRESET;
    }

    /* Send data payload */
    ret = write_bytes(connection.fd, data, size);

    if (ret < 0) {
        stateless_log_message(LOG_ERR,
                              "Error writing data payload to afpsld");
        close_connection();
        return -ECONNRESET;
    }

    /* Read response */
    ret = read_bytes_with_timeout(connection.fd, (char *)&response,
                                  sizeof(response));

    if (ret < 0) {
        stateless_log_message(LOG_ERR, "Error reading write response");
        close_connection();
        return -ECONNRESET;
    }

    if (read_response_tail(response.header.len, sizeof(response), &tail,
                           &payload_size)
            < 0
            || payload_size != 0) {
        free(tail);
        close_connection();
        return -EPROTO;
    }

    free(tail);

    if (response.header.result != AFPSL_IPC_RESULT_OK) {
        *written = 0;
        return server_result_to_errno(response.header.result);
    }

    *written = response.written;
    return 0;
}

static int metadata_call(unsigned int command, afpc_volume_t *volid,
                         const char *path, const char *name, void *value,
                         size_t size, unsigned long long offset, int flags,
                         int send_value)
{
    struct afpsl_ipc_metadata_request *request;
    struct afpsl_ipc_metadata_response response;
    size_t base = offsetof(struct afpsl_ipc_metadata_request, data);
    size_t request_len = base + (send_value ? size : 0);
    int ret;
    size_t payload_size;
    size_t tail_content_size;
    size_t tail_size;
    size_t payload_limit = command == AFPSL_IPC_COMMAND_LISTXATTR
                           ? AFP_SL_XATTR_LIST_MAX
                           : AFP_SL_METADATA_CHUNK;
    char *tail = NULL;

    if (!volid || !path
            || strnlen(path, sizeof(request->path)) >= sizeof(request->path)
            || size > payload_limit || (name
                                        && strnlen(name, sizeof(request->name)) >= sizeof(request->name))
            || ((command == AFPSL_IPC_COMMAND_GETXATTR
                 || command == AFPSL_IPC_COMMAND_SETXATTR
                 || command == AFPSL_IPC_COMMAND_REMOVEXATTR) && (!name || name[0] == '\0'))
            || (command == AFPSL_IPC_COMMAND_SETFINDERINFO && size != 32) || (!value
                    && size > 0 && send_value)) {
        return -EINVAL;
    }

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    request = calloc(1, request_len);

    if (!request) {
        return -ENOMEM;
    }

    request->header.command = (char)command;
    request->header.len = (unsigned int)request_len;
    memcpy(&request->volumeid, volid, sizeof(afpc_volume_t));
    strlcpy(request->path, path, sizeof(request->path));

    if (name) {
        strlcpy(request->name, name, sizeof(request->name));
    }

    request->offset = offset;
    request->size = (unsigned int)size;
    request->flags = flags;

    if (send_value && size > 0) {
        memcpy(request->data, value, size);
    }

    ret = send_command((unsigned int)request_len, (char *)request, command);
    free(request);

    if (ret < 0) {
        return -ECONNRESET;
    }

    ret = read_bytes_with_timeout(connection.fd, (char *)&response,
                                  sizeof(response));

    if (ret < 0) {
        close_connection();
        return -ECONNRESET;
    }

    if (response.header.len < sizeof(response) + sizeof(struct
            afpsl_ipc_log_footer)
            || response.header.len > sizeof(response) + payload_limit +
            AFPSL_IPC_LOG_BUFFER_SIZE + sizeof(struct afpsl_ipc_log_footer)) {
        close_connection();
        return -EPROTO;
    }

    tail_size = response.header.len - sizeof(response);
    tail = malloc(tail_size);

    if (!tail || read_bytes_with_timeout(connection.fd, tail, tail_size) < 0
            || afp_sl_response_content_length(tail, tail_size, &tail_content_size) < 0
            || afp_sl_dispatch_response_logs(tail, tail_size) < 0) {
        free(tail);
        close_connection();
        return -ECONNRESET;
    }

    payload_size = tail_content_size;

    if (payload_size > 0 && response.size != payload_size) {
        close_connection();
        free(tail);
        return -EPROTO;
    }

    if (payload_size > 0) {
        if (!value || payload_size > size) {
            free(tail);
            return -ERANGE;
        }

        memcpy(value, tail, payload_size);
    }

    free(tail);

    if (response.error < 0) {
        return response.error;
    }

    return response.size;
}

int afp_sl_getxattr(afpc_volume_t *volid, const char *path, const char *name,
                    void *value, size_t size)
{
    return metadata_call(AFPSL_IPC_COMMAND_GETXATTR, volid, path, name, value,
                         size, 0, 0, 0);
}

int afp_sl_setxattr(afpc_volume_t *volid, const char *path, const char *name,
                    void *value, size_t size, int flags)
{
    int wire_flags = 0;

    if ((flags & ~(AFP_SL_XATTR_CREATE | AFP_SL_XATTR_REPLACE)) != 0
            || (flags & AFP_SL_XATTR_CREATE
                && flags & AFP_SL_XATTR_REPLACE)) {
        return -EINVAL;
    }

    if (flags & AFP_SL_XATTR_CREATE) {
        wire_flags |= kXAttrCreate;
    }

    if (flags & AFP_SL_XATTR_REPLACE) {
        wire_flags |= kXAttrReplace;
    }

    return metadata_call(AFPSL_IPC_COMMAND_SETXATTR, volid, path, name,
                         value, size, 0, wire_flags, 1);
}

int afp_sl_listxattr(afpc_volume_t *volid, const char *path, char *list,
                     size_t size)
{
    return metadata_call(AFPSL_IPC_COMMAND_LISTXATTR, volid, path, NULL, list,
                         size, 0, 0, 0);
}

int afp_sl_removexattr(afpc_volume_t *volid, const char *path, const char *name)
{
    return metadata_call(AFPSL_IPC_COMMAND_REMOVEXATTR, volid, path, name, NULL,
                         0, 0, 0, 0);
}

int afp_sl_getfinderinfo(afpc_volume_t *volid, const char *path, void *value,
                         size_t size)
{
    return metadata_call(AFPSL_IPC_COMMAND_GETFINDERINFO, volid, path, NULL,
                         value, size, 0, 0, 0);
}

int afp_sl_setfinderinfo(afpc_volume_t *volid, const char *path,
                         const void *value,
                         size_t size)
{
    return metadata_call(AFPSL_IPC_COMMAND_SETFINDERINFO, volid, path, NULL,
                         (void *)value, size, 0, 0, 1);
}

int afp_sl_removefinderinfo(afpc_volume_t *volid, const char *path)
{
    return metadata_call(AFPSL_IPC_COMMAND_REMOVEFINDERINFO, volid, path, NULL,
                         NULL, 0, 0, 0, 0);
}

int afp_sl_getresourcefork(afpc_volume_t *volid, const char *path, void *value,
                           size_t size, unsigned long long offset)
{
    return metadata_call(AFPSL_IPC_COMMAND_GETRESOURCEFORK, volid, path, NULL,
                         value, size, offset, 0, 0);
}

int afp_sl_setresourcefork(afpc_volume_t *volid, const char *path,
                           const void *value, size_t size,
                           unsigned long long offset)
{
    if (offset > INT_MAX || size > (size_t)(INT_MAX - offset)) {
        return -EFBIG;
    }

    return metadata_call(AFPSL_IPC_COMMAND_SETRESOURCEFORK, volid, path, NULL,
                         (void *)value, size, offset, 0, 1);
}

int afp_sl_truncateresourcefork(afpc_volume_t *volid, const char *path,
                                unsigned long long size)
{
    if (size > INT_MAX) {
        return -EFBIG;
    }

    return metadata_call(AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK, volid, path,
                         NULL, NULL, 0, size, 0, 0);
}

int afp_sl_removeresourcefork(afpc_volume_t *volid, const char *path)
{
    return metadata_call(AFPSL_IPC_COMMAND_REMOVERESOURCEFORK, volid, path, NULL,
                         NULL, 0, 0, 0, 0);
}

enum afp_sl_recovery_action afp_sl_recovery_for_error(int result)
{
    if (result == -ESTALE) {
        return AFP_SL_RECOVERY_REATTACH;
    }

    if (result == -ECONNRESET || result == -ECONNREFUSED
            || result == -ENOTCONN || result == -ETIMEDOUT
            || result == -EHOSTUNREACH) {
        return AFP_SL_RECOVERY_RECONNECT;
    }

    return AFP_SL_RECOVERY_NONE;
}

static int server_result_to_errno(int result)
{
    switch (result) {
    case AFPSL_IPC_RESULT_OK:
        return 0;

    case AFPSL_IPC_RESULT_ENOENT:
    case AFPSL_IPC_RESULT_MOUNTPOINT_NOEXIST:
        return -ENOENT;

    case AFPSL_IPC_RESULT_NOTCONNECTED:
        return -ENOTCONN;

    case AFPSL_IPC_RESULT_NOTATTACHED:
        return -ESTALE;

    case AFPSL_IPC_RESULT_NOVOLUME:
        return -ENODEV;

    case AFPSL_IPC_RESULT_ALREADY_CONNECTED:
    case AFPSL_IPC_RESULT_ALREADY_ATTACHED:
    case AFPSL_IPC_RESULT_ALREADY_MOUNTED:
        return 0;

    case AFPSL_IPC_RESULT_NOAUTHENT:
    case AFPSL_IPC_RESULT_VOLPASS_NEEDED:
    case AFPSL_IPC_RESULT_ACCESS:
    case AFPSL_IPC_RESULT_MOUNTPOINT_PERM:
        return -EACCES;

    case AFPSL_IPC_RESULT_EXIST:
        return -EEXIST;

    case AFPSL_IPC_RESULT_ENOTEMPTY:
        return -ENOTEMPTY;

    case AFPSL_IPC_RESULT_TIMEDOUT:
        return -ETIMEDOUT;

    case AFPSL_IPC_RESULT_DAEMON_ERROR:
        return -ECONNREFUSED;

    case AFPSL_IPC_RESULT_NOSERVER:
        return -EHOSTUNREACH;

    case AFPSL_IPC_RESULT_NOTSUPPORTED:
        return -ENOTSUP;

    default:
        return -EIO;
    }
}

int afp_sl_creat(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                 mode_t mode)
{
    struct afpsl_ipc_creat_request request;
    struct afpsl_ipc_creat_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_creat_request);
    request.header.command = AFPSL_IPC_COMMAND_CREAT;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_chmod(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                 mode_t mode)
{
    struct afpsl_ipc_chmod_request request;
    struct afpsl_ipc_chmod_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_chmod_request);
    request.header.command = AFPSL_IPC_COMMAND_CHMOD;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_rename(afpc_volume_t *volid, const char *path_from,
                  const char *path_to,
                  struct afpc_url *url)
{
    struct afpsl_ipc_rename_request request;
    struct afpsl_ipc_rename_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_rename_request);
    request.header.command = AFPSL_IPC_COMMAND_RENAME;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path_from, path_from, AFP_MAX_PATH);
    strlcpy(request.path_to, path_to, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_unlink(afpc_volume_t *volid, const char *path, struct afpc_url *url)
{
    struct afpsl_ipc_unlink_request request;
    struct afpsl_ipc_unlink_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_unlink_request);
    request.header.command = AFPSL_IPC_COMMAND_UNLINK;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_truncate(afpc_volume_t *volid, const char *path,
                    struct afpc_url *url,
                    unsigned long long offset)
{
    struct afpsl_ipc_truncate_request request;
    struct afpsl_ipc_truncate_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_truncate_request);
    request.header.command = AFPSL_IPC_COMMAND_TRUNCATE;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.offset = offset;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_utime(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                 struct utimbuf *times)
{
    struct afpsl_ipc_utime_request request;
    struct afpsl_ipc_utime_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_utime_request);
    request.header.command = AFPSL_IPC_COMMAND_UTIME;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    memcpy(&request.times, times, sizeof(struct utimbuf));
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_mkdir(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                 mode_t mode)
{
    struct afpsl_ipc_mkdir_request request;
    struct afpsl_ipc_mkdir_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_mkdir_request);
    request.header.command = AFPSL_IPC_COMMAND_MKDIR;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_rmdir(afpc_volume_t *volid, const char *path, struct afpc_url *url)
{
    struct afpsl_ipc_rmdir_request request;
    struct afpsl_ipc_rmdir_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_rmdir_request);
    request.header.command = AFPSL_IPC_COMMAND_RMDIR;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_statfs(afpc_volume_t *volid, const char *path, struct afpc_url *url,
                  struct statvfs *stat)
{
    struct afpsl_ipc_statfs_request request;
    struct afpsl_ipc_statfs_response response;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afpsl_ipc_statfs_request);
    request.header.command = AFPSL_IPC_COMMAND_STATFS;

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    /* Use current directory if no path specified */
    if (tmppath == NULL || tmppath[0] == '\0') {
        tmppath = "/";
    }

    memcpy(&request.volumeid, volid_p, sizeof(afpc_volume_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    if (response.header.result == AFPSL_IPC_RESULT_OK) {
        memcpy(stat, &response.stat, sizeof(struct statvfs));
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_close(afpc_volume_t *volid, unsigned int fileid)
{
    struct afpsl_ipc_close_request request;
    struct afpsl_ipc_close_response response;
    int ret;
    request.header.close = 0; /* Keep connection open - just closing file */
    request.header.len = sizeof(struct afpsl_ipc_close_request);
    request.header.command = AFPSL_IPC_COMMAND_CLOSE;
    memcpy(&request.volumeid, volid, sizeof(afpc_volume_t));
    request.fileid = fileid;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return server_result_to_errno(response.header.result);
}

int afp_sl_readdir(afpc_volume_t * volid, const char * path,
                   struct afpc_url * url,
                   int start, int count, unsigned int *numfiles,
                   struct afpc_file_info **data, int *eod)
{
    struct afpsl_ipc_readdir_request req;
    struct afpsl_ipc_readdir_response * mainrep;
    int ret;
    const char *tmppath = path;
    unsigned int size;
    afpc_volume_t *volid_p = volid;
    afpc_volume_t tmpvolid;
    unsigned int total_files = 0;
    unsigned int requested_count;
    struct afpc_file_info *buffer = NULL;
    int current_start = start;
    int eod_flag = 0;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    if (volid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    if (tmppath == NULL) {
        tmppath = "/";
    }

    /* Ensure we request at least 256 files
     * if the user asks for fewer (but > 0) */
    requested_count = (count > 0 && count < 256) ? 256 : count;

    /* Loop to fetch all requested files or until EOD */
    while (total_files < requested_count && !eod_flag) {
        unsigned int batch_count = requested_count - total_files;
        memset(&req, 0, sizeof(req));
        req.header.close = 0;
        req.header.len = sizeof(struct afpsl_ipc_readdir_request);
        req.header.command = AFPSL_IPC_COMMAND_READDIR;
        req.start = current_start;
        req.count = batch_count;
        memcpy(&req.volumeid, volid_p, sizeof(afpc_volume_t));
        strlcpy(req.path, tmppath, AFP_MAX_PATH);

        if (send_command(sizeof(req), (char *)&req, AFPSL_IPC_COMMAND_READDIR) < 0) {
            if (buffer) {
                free(buffer);
            }

            return -ECONNRESET;
        }

        ret = read_answer();

        if (ret < 0) {
            if (buffer) {
                free(buffer);
            }

            return -ECONNRESET;
        }

        mainrep = (void *) connection.data;

        if (mainrep->header.result) {
            if (buffer) {
                free(buffer);
            }

            return server_result_to_errno(mainrep->header.result);
        }

        unsigned int batch_received = mainrep->numfiles;

        if (batch_received > 0) {
            /* Resize buffer */
            size = (total_files + batch_received) * sizeof(struct afpc_file_info);
            struct afpc_file_info *new_buffer = realloc(buffer, size);

            if (!new_buffer) {
                if (buffer) {
                    free(buffer);
                }

                return -ENOMEM;
            }

            buffer = new_buffer;
            /* Unpack variable length data */
            const char *p = ((char *) mainrep) + sizeof(struct afpsl_ipc_readdir_response);
            struct afpc_file_info *current_basic = buffer + total_files;

            for (unsigned int i = 0; i < batch_received; i++) {
                uint32_t name_len;
                memcpy(&name_len, p, sizeof(uint32_t));
                p += sizeof(uint32_t);

                if (name_len >= AFP_MAX_PATH) {
                    name_len = AFP_MAX_PATH - 1;
                }

                memcpy(current_basic->name, p, name_len);
                current_basic->name[name_len] = '\0';
                p += name_len;
                memcpy(&current_basic->creation_date, p, sizeof(uint32_t));
                p += sizeof(uint32_t);
                memcpy(&current_basic->modification_date, p, sizeof(uint32_t));
                p += sizeof(uint32_t);
                memcpy(&current_basic->unixprivs, p, sizeof(struct afpc_unix_privileges));
                p += sizeof(struct afpc_unix_privileges);
                memcpy(&current_basic->size, p, sizeof(uint64_t));
                p += sizeof(uint64_t);
                current_basic++;
            }

            total_files += batch_received;
            current_start += batch_received;
        }

        if (mainrep->eod) {
            eod_flag = 1;
        }

        /* Safety break if daemon returns 0 files
         * but no EOD to prevent infinite loop */
        if (batch_received == 0 && !eod_flag) {
            break;
        }
    }

    *numfiles = total_files;
    *data = buffer;

    if (eod) {
        *eod = eod_flag;
    }

    return 0;
}

int afp_sl_getvols(afpc_server_t serverid, struct afpc_url *url,
                   unsigned int start, unsigned int count, unsigned int *numvols,
                   struct afpc_volume_info *vols)
{
    struct afpsl_ipc_getvols_request req;
    int ret;
    struct afpsl_ipc_getvols_response * response;

    if (!serverid) {
        return -EINVAL;
    }

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_getvols_request);
    req.header.command = AFPSL_IPC_COMMAND_GETVOLS;
    req.serverid = serverid;
    req.start = start;
    req.count = count;
    memcpy(&req.url, url, sizeof(*url));

    if (send_command(sizeof(req), (char *) &req, AFPSL_IPC_COMMAND_GETVOLS) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0) {
        return -ECONNRESET;
    }

    response = (void *) connection.data;
    ret = server_result_to_errno(response->header.result);

    if (ret == 0) {
        memcpy(vols, ((char *) response) + sizeof(struct afpsl_ipc_getvols_response),
               response->num * sizeof(struct afpc_volume_info));
        *numvols = response->num;
    }

    return ret;
}

static int afp_sl_connect_with_flags(struct afpc_url *url,
                                     unsigned int uam_mask,
                                     unsigned int flags, afpc_server_t *id,
                                     char *loginmesg)
{
    struct afpsl_ipc_connect_request req;
    const struct afpsl_ipc_connect_response *resp;
    int ret;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_connect_request);
    req.header.command = AFPSL_IPC_COMMAND_CONNECT;
    memcpy(&req.url, url, sizeof(struct afpc_url));
    req.uam_mask = uam_mask;
    req.flags = flags;

    if (send_command(sizeof(req), (char *)&req, AFPSL_IPC_COMMAND_CONNECT) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0) {
        return -ECONNRESET;
    }

    resp = (void *) connection.data;

    /* Treat "already connected" as idempotent success. */
    if (resp->header.result == AFPSL_IPC_RESULT_OK
            || resp->header.result == AFPSL_IPC_RESULT_ALREADY_CONNECTED) {
        if (id) {
            memcpy(id, &resp->serverid, sizeof(afpc_server_t));
        }

        if (loginmesg) {
            memcpy(loginmesg, resp->loginmesg, AFP_LOGINMESG_LEN);
        }

        return 0;
    }

    if (resp->connect_error != 0) {
        return resp->connect_error > 0 ? -resp->connect_error
               : resp->connect_error;
    }

    return server_result_to_errno(resp->header.result);
}

int afp_sl_connect(struct afpc_url *url, unsigned int uam_mask,
                   afpc_server_t *id,
                   char *loginmesg)
{
    return afp_sl_connect_with_flags(url, uam_mask, 0, id, loginmesg);
}

int afp_sl_resume(struct afpc_url *url, unsigned int uam_mask,
                  afpc_server_t *id,
                  char *loginmesg)
{
    return afp_sl_connect_with_flags(url, uam_mask,
                                     AFPSL_IPC_CONNECT_RESUME_EXISTING, id,
                                     loginmesg);
}

int afp_sl_attach(afpc_server_t serverid, struct afpc_url * url,
                  unsigned int volume_options, afpc_volume_t *volumeid,
                  enum afp_sl_attach_status *status)
{
    struct afpsl_ipc_attach_request req;
    struct afpsl_ipc_attach_response * response;
    int ret;

    if (status) {
        *status = AFP_SL_ATTACH_STATUS_NONE;
    }

    if (!serverid) {
        return -EINVAL;
    }

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    response = (void *) connection.data;
    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_attach_request);
    req.header.command = AFPSL_IPC_COMMAND_ATTACH;
    req.serverid = serverid;
    memcpy(&req.url, url, sizeof(struct afpc_url));
    req.volume_options = volume_options;

    if (send_command(sizeof(req), (char *)&req, AFPSL_IPC_COMMAND_ATTACH) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0) {
        return -ECONNRESET;
    }

    if (connection.len < sizeof(struct afpsl_ipc_attach_response)) {
        return -EPROTO;
    }

    /* Treat "already attached" as success since we have a valid volumeid */
    if (ret == AFPSL_IPC_RESULT_OK
            || ret == AFPSL_IPC_RESULT_ALREADY_ATTACHED) {
        if (volumeid) {
            memcpy(volumeid, &response->volumeid, sizeof(afpc_volume_t));
        }

        return 0;
    }

    if (ret == AFPSL_IPC_RESULT_VOLPASS_NEEDED && status) {
        *status = AFP_SL_ATTACH_STATUS_PASSWORD_REQUIRED;
    }

    return server_result_to_errno(ret);
}

int afp_sl_detach(afpc_volume_t *volumeid, struct afpc_url * url)
{
    struct afpsl_ipc_detach_request req;
    int ret;
    afpc_volume_t tmpvolid;
    afpc_volume_t *volid_p = volumeid;

    if (ensure_daemon_connection()) {
        return -ECONNREFUSED;
    }

    if (volumeid == NULL) {
        ret = afp_sl_getvolid(NULL, url, &tmpvolid);

        if (ret) {
            return ret;
        }

        volid_p = &tmpvolid;
    }

    req.header.close = 1;
    req.header.len = sizeof(struct afpsl_ipc_detach_request);
    req.header.command = AFPSL_IPC_COMMAND_DETACH;
    memcpy(&req.volumeid, volid_p, sizeof(afpc_volume_t));

    if (send_command(sizeof(req), (char *)&req, AFPSL_IPC_COMMAND_DETACH) < 0) {
        return -ECONNRESET;
    }

    ret = read_answer();

    if (ret < 0) {
        return -ECONNRESET;
    }

    /* DETACH uses header.close=1, so daemon closed its side.
     * Close our side too to avoid reusing a stale connection. */
    close_connection();
    return server_result_to_errno(ret);
}

int afp_sl_changepw(struct afpc_url *url, const char *old_password,
                    const char *new_password,
                    enum afp_sl_password_change_status *status)
{
    struct afpsl_ipc_changepw_request req;
    struct afpsl_ipc_changepw_response response;
    int ret;

    if (!url || !old_password || !new_password) {
        return -EINVAL;
    }

    if (status) {
        *status = AFP_SL_PASSWORD_CHANGE_STATUS_NONE;
    }

    memset(&req, 0, sizeof(req));
    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_changepw_request);
    req.header.command = AFPSL_IPC_COMMAND_CHANGEPW;
    memcpy(&req.url, url, sizeof(struct afpc_url));
    strlcpy(req.oldpasswd, old_password, AFP_MAX_PASSWORD_LEN);
    strlcpy(req.newpasswd, new_password, AFP_MAX_PASSWORD_LEN);
    ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    if (response.header.result == AFPSL_IPC_RESULT_OK) {
        return 0;
    }

    if (response.header.result != AFPSL_IPC_RESULT_ERROR) {
        return server_result_to_errno(response.header.result);
    }

    switch (response.afp_error) {
    case kFPAccessDenied:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_ACCESS_DENIED;
        }

        return -EACCES;

    case kFPUserNotAuth:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_INCORRECT_OLD_PASSWORD;
        }

        return -EACCES;

    case kFPBadUAM:
    case kFPCallNotSupported:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_UNSUPPORTED_AUTHENTICATION;
        }

        return -ENOTSUP;

    case kFPPwdSameErr:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_UNCHANGED;
        }

        return -EINVAL;

    case kFPPwdTooShortErr:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_TOO_SHORT;
        }

        return -EINVAL;

    case kFPPwdExpiredErr:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_EXPIRED;
        }

        return -EACCES;

    case kFPPwdPolicyErr:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_POLICY_VIOLATION;
        }

        return -EINVAL;

    case kFPParamErr:
        if (status) {
            *status = AFP_SL_PASSWORD_CHANGE_STATUS_INVALID_PARAMETER;
        }

        return -EINVAL;

    default:
        return -EIO;
    }
}

static int ensure_daemon_connection(void)
{
    return daemon_connect(geteuid()) == 0 ? 0 : -ECONNREFUSED;
}

int afp_sl_serverinfo(struct afpc_url * url, struct afpc_server_info * basic)
{
    struct afpsl_ipc_serverinfo_request req;
    struct afpsl_ipc_serverinfo_response response;
    int ret;
    req.header.close = 0;
    req.header.len = sizeof(struct afpsl_ipc_serverinfo_request);
    req.header.command = AFPSL_IPC_COMMAND_SERVERINFO;
    memcpy(&req.url, url, sizeof(struct afpc_url));
    ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    ret = server_result_to_errno(response.header.result);

    if (ret == 0) {
        memcpy(basic, &response.server_basic, sizeof(struct afpc_server_info));
    }

    return ret;
}

int afp_sl_disconnect(afpc_server_t *id)
{
    struct afpsl_ipc_disconnect_request req;
    struct afpsl_ipc_disconnect_response response;
    int ret;

    if (!id || !*id) {
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));
    req.header.command = AFPSL_IPC_COMMAND_DISCONNECT;
    req.header.len = sizeof(req);
    /* Close socket after this command */
    req.header.close = 1;
    req.serverid = *id;
    ret = send_to_daemon((char *) &req, sizeof(req), (char *) &response,
                         sizeof(response));

    if (ret < 0) {
        return ret;
    }

    close_connection();
    ret = server_result_to_errno(response.header.result);

    if (ret == 0) {
        *id = NULL;
    }

    return ret;
}
