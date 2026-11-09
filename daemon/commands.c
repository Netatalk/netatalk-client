/*
 *  commands.c - Stateless daemon command handlers
 *
 *  This file contains command handlers for the afpsld stateless daemon.
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The command handlers of afpsld, the stateless daemon
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "lib/afp_internal.h"
#include "lib/codepage.h"
#include "lib/compat.h"
#include "lib/dsi.h"
#include "lib/client.h"
#include "lib/mapping.h"
#include "lib/midlevel.h"
#include "lib/resource.h"
#include "lib/uam_registry.h"
#include "lib/utils.h"

#include "commands.h"
#include "daemon.h"
#include "daemon_client.h"
#include "stateless_ipc.h"

/* A 32-bit handle names an open record and the volume it belongs to. */
#define MAX_OPEN_FILES 1024
struct handle_slot {
    struct afp_file_info *fp;
    struct afp_volume *volume;
};
static struct handle_slot file_handle_table[MAX_OPEN_FILES];
static pthread_mutex_t file_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Serialize all AFP server operations to prevent concurrent DSI calls */
static pthread_mutex_t server_op_mutex = PTHREAD_MUTEX_INITIALIZER;

/*!
 * @brief The name of an afpsld command, for the log
 *
 * @param[in] command  the command
 * @returns the name, or "UNKNOWN"
 */
static const char *afpsl_ipc_command_name(unsigned char command)
{
    switch (command) {
    case AFPSL_IPC_COMMAND_ATTACH:
        return "ATTACH";

    case AFPSL_IPC_COMMAND_DETACH:
        return "DETACH";

    case AFPSL_IPC_COMMAND_STATUS:
        return "STATUS";

    case AFPSL_IPC_COMMAND_EXIT:
        return "EXIT";

    case AFPSL_IPC_COMMAND_CONNECT:
        return "CONNECT";

    case AFPSL_IPC_COMMAND_GETVOLID:
        return "GETVOLID";

    case AFPSL_IPC_COMMAND_READDIR:
        return "READDIR";

    case AFPSL_IPC_COMMAND_GETVOLS:
        return "GETVOLS";

    case AFPSL_IPC_COMMAND_STAT:
        return "STAT";

    case AFPSL_IPC_COMMAND_OPEN:
        return "OPEN";

    case AFPSL_IPC_COMMAND_READ:
        return "READ";

    case AFPSL_IPC_COMMAND_CLOSE:
        return "CLOSE";

    case AFPSL_IPC_COMMAND_SERVERINFO:
        return "SERVERINFO";

    case AFPSL_IPC_COMMAND_GET_MOUNTPOINT:
        return "GET_MOUNTPOINT";

    case AFPSL_IPC_COMMAND_WRITE:
        return "WRITE";

    case AFPSL_IPC_COMMAND_CREAT:
        return "CREAT";

    case AFPSL_IPC_COMMAND_CHMOD:
        return "CHMOD";

    case AFPSL_IPC_COMMAND_RENAME:
        return "RENAME";

    case AFPSL_IPC_COMMAND_UNLINK:
        return "UNLINK";

    case AFPSL_IPC_COMMAND_TRUNCATE:
        return "TRUNCATE";

    case AFPSL_IPC_COMMAND_MKDIR:
        return "MKDIR";

    case AFPSL_IPC_COMMAND_RMDIR:
        return "RMDIR";

    case AFPSL_IPC_COMMAND_STATFS:
        return "STATFS";

    case AFPSL_IPC_COMMAND_UTIME:
        return "UTIME";

    case AFPSL_IPC_COMMAND_DISCONNECT:
        return "DISCONNECT";

    case AFPSL_IPC_COMMAND_CHANGEPW:
        return "CHANGEPW";

    case AFPSL_IPC_COMMAND_GETXATTR:
        return "GETXATTR";

    case AFPSL_IPC_COMMAND_SETXATTR:
        return "SETXATTR";

    case AFPSL_IPC_COMMAND_LISTXATTR:
        return "LISTXATTR";

    case AFPSL_IPC_COMMAND_REMOVEXATTR:
        return "REMOVEXATTR";

    case AFPSL_IPC_COMMAND_GETFINDERINFO:
        return "GETFINDERINFO";

    case AFPSL_IPC_COMMAND_SETFINDERINFO:
        return "SETFINDERINFO";

    case AFPSL_IPC_COMMAND_REMOVEFINDERINFO:
        return "REMOVEFINDERINFO";

    case AFPSL_IPC_COMMAND_GETRESOURCEFORK:
        return "GETRESOURCEFORK";

    case AFPSL_IPC_COMMAND_SETRESOURCEFORK:
        return "SETRESOURCEFORK";

    case AFPSL_IPC_COMMAND_REMOVERESOURCEFORK:
        return "REMOVERESOURCEFORK";

    case AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK:
        return "TRUNCATERESOURCEFORK";

    case AFPSL_IPC_COMMAND_HELLO:
        return "HELLO";

    case AFPSL_IPC_COMMAND_DESKTOP_GET_COMMENT:
        return "DESKTOP_GET_COMMENT";

    case AFPSL_IPC_COMMAND_DESKTOP_GET_ICON_INFO:
        return "DESKTOP_GET_ICON_INFO";

    case AFPSL_IPC_COMMAND_DESKTOP_GET_ICON:
        return "DESKTOP_GET_ICON";

    case AFPSL_IPC_COMMAND_DESKTOP_GET_APPL:
        return "DESKTOP_GET_APPL";

    case AFPSL_IPC_COMMAND_DESKTOP_SET_COMMENT:
        return "DESKTOP_SET_COMMENT";

    default:
        return "UNKNOWN";
    }
}

/*!
 * @brief Close or continue a client's connection after a response
 *
 * @param[in,out] c                the client
 * @param[in]     send_result      send_command()'s result; a failure closes
 * @param[in]     close_requested  1 when the request asked to close
 */
static void finish_response(struct daemon_client *c, int send_result,
                            int close_requested)
{
    if (send_result < 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Failed to send response to stateless client");
        close_client_connection(c);
    } else if (close_requested) {
        close_client_connection(c);
    } else {
        continue_client_connection(c);
    }
}

/*!
 * @brief Answer an afpsld hello request, which negotiates the IPC protocol
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, or -1 to close the client for a request of the wrong size
 */
static int process_hello(struct daemon_client *c)
{
    const struct afpsl_ipc_hello_request *request = (const void *)
                                                    c->complete_packet;
    struct afpsl_ipc_hello_response response;
    int compatible;

    if ((size_t)c->completed_packet_size != sizeof(*request)) {
        return -1;
    }

    compatible = request->magic == AFPSL_IPC_PROTOCOL_MAGIC
                 && request->major == AFPSL_IPC_PROTOCOL_MAJOR;
    memset(&response, 0, sizeof(response));
    response.header.result = compatible ? AFPSL_IPC_RESULT_OK
                             : AFPSL_IPC_RESULT_ERROR;
    response.header.len = sizeof(response);
    response.magic = AFPSL_IPC_PROTOCOL_MAGIC;
    response.major = AFPSL_IPC_PROTOCOL_MAJOR;
    response.minor = AFPSL_IPC_PROTOCOL_MINOR;
    c->ipc_ready = compatible;
    finish_response(c, send_command(c, sizeof(response), (char *)&response),
                    !compatible);
    /* finish_response() already closes an incompatible connection. */
    return 0;
}

/*!
 * @brief Allocate a response with its fixed part zeroed
 *
 * @param[in] len        bytes to allocate
 * @param[in] fixed_len  leading bytes to zero
 * @returns the response, or NULL
 */
static void *alloc_response(size_t len, size_t fixed_len)
{
    void *response = malloc(len);

    if (response) {
        memset(response, 0, fixed_len);
    }

    return response;
}

/*!
 * @brief Give an open record a handle that names it and its volume
 *
 * @param[in] fp      the record
 * @param[in] volume  its volume
 * @returns the handle, or 0 when every slot is taken
 */
static int register_file_handle(struct afp_file_info *fp,
                                struct afp_volume *volume)
{
    pthread_mutex_lock(&file_handle_mutex);

    for (int i = 1; i < MAX_OPEN_FILES; i++) {
        if (file_handle_table[i].fp == NULL) {
            file_handle_table[i].fp = fp;
            file_handle_table[i].volume = volume;
            pthread_mutex_unlock(&file_handle_mutex);
            return i;
        }
    }

    pthread_mutex_unlock(&file_handle_mutex);
    return 0;
}

/*!
 * @brief Whether a record is a pseudo-file's, which no open-fork list holds
 *
 * @param[in] fp  the record
 * @returns 1 for Finder information, a comment or the server icon, 0 for a
 *          fork
 */
static int is_pseudo_file(const struct afp_file_info *fp)
{
    return fp->resource != AFP_META_NONE && fp->resource != AFP_META_RESOURCE;
}

/*!
 * @brief Whether a record is on a volume's open-fork list
 *
 * @param[in] volume  the volume
 * @param[in] fp      the record
 * @returns 1 when listed, 0 when not
 */
static int fork_is_listed(struct afp_volume *volume,
                          const struct afp_file_info *fp)
{
    int listed = 0;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (const struct afp_file_info *p = volume->open_forks; p;
            p = p->largelist_next) {
        if (p == fp) {
            listed = 1;
            break;
        }
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
    return listed;
}

/*!
 * @brief Find the open record a handle names on a volume
 *
 * The handle must be the volume's, and a fork's record must still be on its
 * open-fork list, as a detach frees the records the library listed.
 *
 * @param[in] id      the handle
 * @param[in] volume  the volume the request names
 * @param[in] take    1 to clear the handle, for a close, or 0
 * @returns the record, or NULL when the handle names none of volume's
 */
static struct afp_file_info *get_file_handle(int id,
                                             struct afp_volume *volume,
                                             int take)
{
    struct afp_file_info *fp = NULL;

    if (id <= 0 || id >= MAX_OPEN_FILES) {
        return NULL;
    }

    pthread_mutex_lock(&file_handle_mutex);
    fp = file_handle_table[id].fp;

    if (fp && (file_handle_table[id].volume != volume
               || (!is_pseudo_file(fp) && !fork_is_listed(volume, fp)))) {
        fp = NULL;
    }

    if (fp && take) {
        file_handle_table[id].fp = NULL;
        file_handle_table[id].volume = NULL;
    }

    pthread_mutex_unlock(&file_handle_mutex);
    return fp;
}

/*!
 * @brief Forget a volume's handles before it detaches
 *
 * The detach frees the listed records, and the table alone holds a
 * pseudo-file's; a handle left behind could name a later open's record.
 *
 * @param[in] volume  the attached volume
 */
static void release_volume_handles(struct afp_volume *volume)
{
    pthread_mutex_lock(&file_handle_mutex);

    for (int i = 1; i < MAX_OPEN_FILES; i++) {
        struct handle_slot *handle = &file_handle_table[i];

        if (handle->fp && handle->volume == volume) {
            if (is_pseudo_file(handle->fp)) {
                free(handle->fp);
            }

            handle->fp = NULL;
            handle->volume = NULL;
        }
    }

    pthread_mutex_unlock(&file_handle_mutex);
}

/*!
 * @brief Whether a volume's server has a live connection
 *
 * @param[in] volume  the volume, or NULL
 * @returns 1 when it has
 */
static int volume_server_is_connected(const struct afp_volume *volume)
{
    return volume && volume->server
           && volume->server->connect_state == SERVER_STATE_CONNECTED
           && volume->server->fd >= 0;
}

/*!
 * @brief Map a desktop database request's result to a negative errno
 *
 * @param[in] ret      the AFP result, or a negative errno from the send
 * @param[in] comment  1 for a comment request, whose kFPItemNotFound is
 *                     AFP_SL_DESKTOP_COMMENT_MISSING
 * @returns 0, or a negative errno
 */
static int desktop_error(int ret, int comment)
{
    /* AFP result codes occupy their own negative range.  Transport and
     * session failures from afpc_dsi_send() are already -errno values and
     * must reach the public desktop API unchanged.  The reply parsers still
     * use -1 for malformed replies, which remains an AFP protocol error. */
    if (ret < 0 && ret != -1 && ret > kFPNoMoreSessions) {
        return ret;
    }

    switch (ret) {
    case kFPNoErr:
        return 0;

    case kFPAccessDenied:
        return -EACCES;

    case kFPItemNotFound:
        return comment ? -AFP_SL_DESKTOP_COMMENT_MISSING : -ENOENT;

    case kFPObjectNotFound:
    case kFPDirNotFound:
        return -ENOENT;

    case kFPCallNotSupported:
        return -EOPNOTSUPP;

    case kFPParamErr:
        return -EINVAL;

    default:
        return -EPROTO;
    }
}

/*!
 * @brief Open a volume's desktop database, once per volume
 *
 * @param[in,out] volume  the volume, which keeps the reference
 * @returns 0, -ENOTCONN without a live connection, or desktop_error()'s errno
 */
static int desktop_ensure_open(struct afp_volume *volume)
{
    int ret;

    if (!volume || !volume_server_is_connected(volume)) {
        return -ENOTCONN;
    }

    if (volume->dtrefnum != 0) {
        return 0;
    }

    ret = afp_opendt(volume, &volume->dtrefnum);
    return desktop_error(ret, 0);
}

/*!
 * @brief Open a volume, logging the message the library gives
 *
 * @param[in]     c       the client the message is logged for
 * @param[in,out] volume  the volume to open
 * @returns 0 once the volume is open, or 1
 */
static int volopen(struct daemon_client * c, struct afp_volume * volume)
{
    char mesg[1024];
    unsigned int l = 0;
    memset(mesg, 0, 1024);
    int rc = afp_connect_volume(volume, volume->server, mesg, &l, 1024);

    if (mesg[0] != '\0') {
        log_for_client((void *) c, AFPFSD, LOG_ERR, "%s", mesg);
    }

    return rc;
}

/*!
 * @brief Answer an afpsld status request with the status text
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0 once it sends the response, or AFPSL_IPC_RESULT_ERROR to close
 *          the connection when it can build none
 */
static unsigned char process_status(struct daemon_client * c)
{
    struct afpsl_ipc_status_request * req = (void *) c->complete_packet;
    struct afpsl_ipc_status_response * response;
    char *output_buffer;
    int output_len = 0;
    int max_len = AFPSL_IPC_MAX_RESPONSE - sizeof(struct afpsl_ipc_status_response);
    unsigned int response_len;
    int len;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_status_request)) {
        return AFPSL_IPC_RESULT_ERROR;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->servername[AFP_SERVER_NAME_LEN - 1] = '\0';
    req->mountpoint[AFP_MOUNTPOINT_LEN - 1] = '\0';
    output_buffer = malloc(AFPSL_IPC_MAX_RESPONSE);

    if (!output_buffer) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory in process_status");
        return AFPSL_IPC_RESULT_ERROR;
    }

    memset(output_buffer, 0, AFPSL_IPC_MAX_RESPONSE);
    /* Header */
    len = max_len;

    if (afp_status_header(output_buffer, &len) >= 0) {
        output_len = strlen(output_buffer);
    }

    /* Servers - hold list lock during iteration to prevent use-after-free */
    afp_lock_server_list();

    for (struct afp_server * s = get_server_base(); s; s = s->next) {
        /* Filter if servername is specified */
        if (req->servername[0] != '\0'
                && strcmp(s->server_name_printable, req->servername) != 0) {
            continue;
        }

        int s_len = max_len - output_len;

        if (s_len <= 0) {
            break;
        }

        /* We need a temp buffer because afp_status_server overwrites the buffer */
        char temp_buf[16384];
        int temp_len = sizeof(temp_buf);
        afp_status_server(s, temp_buf, &temp_len);
        int written = snprintf(output_buffer + output_len, s_len, "%s", temp_buf);

        if (written > 0) {
            output_len += written;
        }
    }

    afp_unlock_server_list();
    response_len = sizeof(struct afpsl_ipc_status_response) + output_len + 1;
    response = alloc_response(response_len, sizeof(*response));

    if (!response) {
        free(output_buffer);
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating status response");
        return AFPSL_IPC_RESULT_ERROR;
    }

    response->header.result = AFPSL_IPC_RESULT_OK;
    response->header.len = response_len;
    /* Copy text after the header */
    memcpy((char *)response + sizeof(struct afpsl_ipc_status_response),
           output_buffer, output_len + 1);
    finish_response(c, send_command(c, response_len, (char *)response),
                    req->header.close);
    free(output_buffer);
    free(response);
    return 0;
}

/*!
 * @brief Answer an afpsld detach request by detaching a volume
 *
 * The handles of the volume's open forks go first, since the detach frees the
 * records they name.
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and its message
 */
static unsigned char process_detach(struct daemon_client * c)
{
    struct afpsl_ipc_detach_request * req;
    struct afpsl_ipc_detach_response response;
    struct afp_volume * v = NULL;
    req = (void *) c->complete_packet;

    /* Validate the volumeid */

    if ((v = afp_volume_find_by_pointer_hold(req->volumeid)) == NULL) {
        snprintf(response.detach_message, sizeof(response.detach_message),
                 "No such volume to detach");
        response.header.result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    if (v->attached != AFP_VOLUME_ATTACHED) {
        snprintf(response.detach_message, sizeof(response.detach_message),
                 "%s was not attached\n", v->volume_name);
        response.header.result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    release_volume_handles(v);
    afp_detach_volume(v);
    response.header.result = AFPSL_IPC_RESULT_OK;
    snprintf(response.detach_message, 1023, "Detached volume %s.\n",
             v->volume_name);
    goto done;
done:
    response.header.len = sizeof(struct afpsl_ipc_detach_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld exit request; the daemon exits with its last client
 *
 * @param[in,out] c  the client, whose connection closes
 * @returns 0
 */
static unsigned char process_exit(struct daemon_client * c)
{
    struct afpsl_ipc_response_header response;
    /* Check before releasing our slot: count == 1 means we are the only client */
    int last_client = (count_active_clients() == 1);
    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   last_client ? "Exiting (last client)" :
                   "Exit requested but other clients active, staying up");
    response.result = AFPSL_IPC_RESULT_OK;
    response.len = sizeof(response);
    finish_response(c, send_command(c, sizeof(response), (char *)&response), 1);

    if (last_client) {
        trigger_exit();
        signal_main_thread();
    }

    return 0;
}

/*!
 * @brief Answer an afpsld changepw request by changing a user's password
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_changepw(struct daemon_client * c)
{
    struct afpsl_ipc_changepw_request *req = (void *)c->complete_packet;
    struct afpsl_ipc_changepw_response response;
    struct afp_server *server = NULL;
    int ret;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_changepw_request)) {
        response.header.result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of string fields from untrusted client data */
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->oldpasswd[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->newpasswd[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    server = afp_server_find_by_name_hold(req->url.servername);

    if (!server) {
        log_for_client((void *)c, AFPFSD, LOG_WARNING,
                       "Password change: not connected to server %s",
                       req->url.servername);
        response.header.result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_passwd(server, req->url.username, req->oldpasswd, req->newpasswd);

    if (ret == 0) {
        response.header.result = AFPSL_IPC_RESULT_OK;
        response.afp_error = 0;
    } else {
        log_for_client((void *)c, AFPFSD, LOG_WARNING,
                       "Password change failed for user %s (error %d)",
                       req->url.username, ret);
        response.header.result = AFPSL_IPC_RESULT_ERROR;
        response.afp_error = ret;
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_changepw_response);
    finish_response(c, send_command(c, response.header.len, (char *)&response),
                    req->header.close);

    if (server) {
        afp_server_release(server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld disconnect request by ending a server's session
 *
 * The handles of the open forks of the server's volumes go first, since the
 * unmount frees the records they name; the logout then goes out without a
 * wait, and the server is removed.
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_disconnect(struct daemon_client * c)
{
    struct afpsl_ipc_disconnect_request *req = (void *)c->complete_packet;
    struct afpsl_ipc_disconnect_response response;
    struct afp_server *server = NULL;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_disconnect_request)) {
        response.header.result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    server = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!server) {
        log_for_client((void *)c, AFPFSD, LOG_WARNING,
                       "Disconnect request with invalid server %p",
                       (void *)req->serverid);
        response.header.result = AFPSL_IPC_RESULT_OK;
        goto done;
    }

    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   "Disconnecting from server %s", server->server_name_printable);

    for (int i = 0; i < server->num_volumes; i++) {
        release_volume_handles(&server->volumes[i]);
    }

    afp_unmount_all_volumes(server);
    afp_logout(server, DSI_DONT_WAIT);
    afp_server_remove(server);
    response.header.result = AFPSL_IPC_RESULT_OK;
done:
    response.header.len = sizeof(struct afpsl_ipc_disconnect_response);
    finish_response(c, send_command(c, response.header.len, (char *)&response),
                    req->header.close);

    /* Release our held reference (afp_server_remove released the list ref) */
    if (server) {
        afp_server_release(server);
    }

    return 0;
}

/* process_getvolid()
 *
 * Gets the volume id for a url provided, if it exists
 *
 * Sets the return result to be:
 * AFPSL_IPC_RESULT_ERROR : internal error
 * AFPSL_IPC_RESULT_NOTCONNECTED: not logged in
 * AFPSL_IPC_RESULT_NOTATTACHED: connected, but not attached to volume
 * AFPSL_IPC_RESULT_OK: lookup succeeded, volumeid set
 */

/*!
 * @brief Answer an afpsld getvolid request with an attached volume's ID
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and the ID
 */
static unsigned char process_getvolid(struct daemon_client * c)
{
    struct afp_volume * v = NULL;
    struct afp_server * s = NULL;
    struct afpsl_ipc_getvolid_request * req = (void *) c->complete_packet;
    struct afpsl_ipc_getvolid_response response;
    int ret = AFPSL_IPC_RESULT_OK;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_getvolid_request)) {
        ret = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    s = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!s || s->connect_state != SERVER_STATE_CONNECTED || s->fd < 0) {
        ret = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    if ((v = find_volume_by_name(s, req->url.volumename)) == NULL) {
        ret = AFPSL_IPC_RESULT_NOVOLUME;
        goto done;
    }

    if (v->attached != AFP_VOLUME_ATTACHED) {
        ret = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    response.volumeid = (afpc_volume_t) v;
    response.header.result = AFPSL_IPC_RESULT_OK;
done:
    response.header.result = ret;
    response.header.len = sizeof(struct afpsl_ipc_getvolid_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (s) {
        afp_server_release(s);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld serverinfo request with a server's details
 *
 * A server this daemon has a session with answers from it; another is asked
 * over a connection made for the request and removed after it.
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0 once it sends the response, or AFPSL_IPC_RESULT_ERROR to close
 *          the connection for a request cut short
 */
static unsigned char process_serverinfo(struct daemon_client * c)
{
    struct afpsl_ipc_serverinfo_request * req = (void *) c->complete_packet;
    struct afpsl_ipc_serverinfo_response response;
    struct afp_server * tmpserver = NULL;
    memset(&response, 0, sizeof(response));
    c->pending = 1;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_serverinfo_request)) {
        return AFPSL_IPC_RESULT_ERROR;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';

    if ((tmpserver = afp_server_find_by_name_hold(req->url.servername)) == NULL) {
        struct addrinfo *address;

        if ((address = afp_get_address((void *)c, req->url.servername,
                                       req->url.port)) != NULL) {
            tmpserver = afp_server_find_by_address_hold(address);
            freeaddrinfo(address);
        }
    }

    if (tmpserver) {
        /* We're already connected */
        afp_server_fill_basic(tmpserver, &response.server_basic);
    } else {
        struct addrinfo *address;

        if ((address = afp_get_address(NULL, req->url.servername,
                                       req->url.port)) == NULL) {
            goto error;
        }

        if ((tmpserver = afp_server_init(address)) == NULL) {
            goto error;
        }

        if (afp_server_connect(tmpserver, 1) < 0) {
            /* A failed status read leaves the server listed; one that never
             * connected is not listed, and the release below frees it. */
            if (afp_server_remove(tmpserver) == 0) {
                tmpserver = NULL;
            }

            goto error;
        }

        afp_server_fill_basic(tmpserver, &response.server_basic);
        afp_server_remove(tmpserver);
        tmpserver = NULL;  /* Already freed by remove, don't release again */
    }

    response.header.result = AFPSL_IPC_RESULT_OK;
    goto done;
error:
    response.header.result = AFPSL_IPC_RESULT_ERROR;
done:
    response.header.len = sizeof(struct afpsl_ipc_serverinfo_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (tmpserver) {
        afp_server_release(tmpserver);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld getvols request with a range of a server's volumes
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0 once it sends the response, or AFPSL_IPC_RESULT_ERROR to close
 *          the connection when it can build none
 */
static unsigned char process_getvols(struct daemon_client * c)
{
    struct afpsl_ipc_getvols_request * request = (void *) c->complete_packet;
    struct afpsl_ipc_getvols_response * response;
    struct afp_server * server = NULL;
    const struct afp_volume * volume;
    unsigned int result = AFPSL_IPC_RESULT_OK;
    unsigned int numvols;
    char *p;
    unsigned int len = sizeof(struct afpsl_ipc_getvols_response);
    struct afpc_volume_info * sum;

    if (((size_t)(c->completed_packet_size) < sizeof(struct
                                                     afpsl_ipc_getvols_request)) || (request->start < 0)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    /* Force null-termination of all string fields from untrusted client data */
    request->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    request->url.uamname[49] = '\0';
    request->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    request->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    request->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    request->url.zone[AFP_ZONE_LEN - 1] = '\0';
    request->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    server = find_server_by_pointer((struct afp_server *)request->serverid);

    if (!server || server->connect_state != SERVER_STATE_CONNECTED
            || server->fd < 0) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto error;
    }

    /* find out how many there are */
    numvols = server->num_volumes;

    if ((unsigned int)request->count < numvols) {
        numvols = request->count;
    }

    if ((unsigned int)request->start > numvols) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    len += numvols * sizeof(struct afpc_volume_info);
    response = alloc_response(len, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating volume list");
        goto error;
    }

    p = (char *) response + sizeof(struct afpsl_ipc_getvols_response);

    for (int i = request->start; i < (int)(request->start + numvols); i++) {
        volume = &server->volumes[i];
        sum = (void *) p;
        memcpy(sum->volume_name_printable, volume->volume_name_printable,
               AFP_VOLUME_NAME_UTF8_LEN);
        sum->flags = volume->flags;
        p = p + sizeof(struct afpc_volume_info);
    }

    response->num = numvols;
    goto done;
error:
    response = alloc_response(len, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in error path");

        if (server) {
            afp_server_release(server);
        }

        return AFPSL_IPC_RESULT_ERROR;
    }

done:
    response->header.len = len;
    response->header.result = result;
    finish_response(c,
                    send_command(c, response->header.len, (char *)response),
                    request->header.close);
    free(response);

    if (server) {
        afp_server_release(server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld open request by opening a fork of a file
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and the handle
 */
static unsigned char process_open(struct daemon_client * c)
{
    struct afpsl_ipc_open_response response;
    struct afpsl_ipc_open_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;
    struct afp_file_info * fp;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Opening file '%s' mode=%d",
                   path, request->mode);
    ret = ml_open(v, path, request->mode, &fp);

    if (ret) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
        } else if (ret == -EEXIST) {
            result = AFPSL_IPC_RESULT_EXIST;
        } else if (ret == -ETIMEDOUT) {
            result = AFPSL_IPC_RESULT_TIMEDOUT;
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "process_open: Failed to open file %s: %d (%s)",
                       path, ret, strerror(-ret));
        goto done;
    }

    int handle = register_file_handle(fp, v);

    if (handle == 0) {
        ml_close(v, fp);
        free(fp);
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    response.fileid = handle;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "File opened successfully, handle=%d", handle);
done:
    response.header.len = sizeof(struct afpsl_ipc_open_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld read request with data from an open file
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0 once it sends the response, or AFPSL_IPC_RESULT_ERROR to close
 *          the connection when it can build none
 */
static unsigned char process_read(struct daemon_client * c)
{
    struct afpsl_ipc_read_response * response;
    struct afpsl_ipc_read_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;
    char *data;
    unsigned int eof = 0;
    unsigned int received = 0;
    unsigned int len = sizeof(struct afpsl_ipc_read_response);

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_read_request)) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFPSL_IPC_RESULT_ERROR;
        }

        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFPSL_IPC_RESULT_ERROR;
        }

        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFPSL_IPC_RESULT_ERROR;
        }

        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    len += request->length;
    response = alloc_response(len, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating %u bytes for read", len);

        if (v) {
            afp_server_release(v->server);
        }

        return AFPSL_IPC_RESULT_ERROR;
    }

    data = ((char *) response) + sizeof(struct afpsl_ipc_read_response);
    struct afp_file_info *fp = get_file_handle(request->fileid, v, 0);

    if (!fp) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    ret = ml_read(v, data, request->length, request->start, fp, (int *)&eof);

    if (ret > 0) {
        received = ret;
    } else if (ret < 0) {
        result = AFPSL_IPC_RESULT_ERROR;
    }

done:
    response->eof = eof;
    /* Only send the actual data read, not the full requested buffer size */
    response->header.len = sizeof(struct afpsl_ipc_read_response) + received;
    response->header.result = result;
    response->received = received;
    finish_response(c, send_command(c, response->header.len, (char *) response),
                    request->header.close);
    free(response);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld write request by writing to an open file
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_write(struct daemon_client * c)
{
    struct afpsl_ipc_write_response response;
    struct afpsl_ipc_write_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;
    char *data = NULL;
    unsigned int written = 0;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_write_request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    data = malloc(request->size);

    if (!data) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating %u bytes for write",
                       request->size);
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    /* Read the data payload from client - it follows immediately after
     * the request header */
    int bytes_read = 0;
    int bytes_remaining = request->size;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Writing %u bytes at offset %llu, fileid=%u", request->size,
                   request->offset, request->fileid);

    while (bytes_remaining > 0) {
        ret = read(c->fd, data + bytes_read, bytes_remaining);

        if (ret <= 0) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Error reading write data payload");
            result = AFPSL_IPC_RESULT_ERROR;
            goto done;
        }

        bytes_read += ret;
        bytes_remaining -= ret;
    }

    struct afp_file_info *fp = get_file_handle(request->fileid, v, 0);

    if (!fp) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    ret = ml_write(v, data, request->size, request->offset, 0, fp);

    if (ret > 0) {
        written = ret;
    } else if (ret < 0) {
        result = AFPSL_IPC_RESULT_ERROR;
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_write_response);
    response.header.result = result;
    response.written = written;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (data) {
        free(data);
    }

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld create request by creating a file at its path
 *
 * A file it made gets the mode the request names, as ml_set_created_mode()
 * gives it.
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_creat(struct daemon_client * c)
{
    struct afpsl_ipc_creat_response response;
    unsigned char created = 0;
    struct afpsl_ipc_creat_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Creating file '%s' mode=%04o",
                   path, request->mode);
    ret = ml_creat(v, path, request->mode, &created);

    if (ret == 0 && created) {
        ml_set_created_mode(v, path, request->mode);
    }

    if (ret < 0) {
        if (ret == -EEXIST) {
            result = AFPSL_IPC_RESULT_EXIST;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_creat: File %s exists (EEXIST)", path);
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_creat: Permission denied creating %s (EACCES)",
                           path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_creat: Failed to create file %s: %d (%s)",
                           path, ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_creat_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld chmod request by setting an object's mode
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_chmod(struct daemon_client * c)
{
    struct afpsl_ipc_chmod_response response;
    struct afpsl_ipc_chmod_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_chmod(v, path, request->mode);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
        } else if (ret == -EACCES || ret == -EPERM) {
            result = AFPSL_IPC_RESULT_ACCESS;
        } else if (ret == -ENOSYS || ret == -ENOTSUP
                   || ret == -EOPNOTSUPP) {
            /* AFP 2.x servers, including Classic Mac OS, cannot expose
             * POSIX Unix privileges.  Preserve this capability result so
             * transfer callers can keep copying AFP metadata. */
            result = AFPSL_IPC_RESULT_NOTSUPPORTED;
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD,
                       result == AFPSL_IPC_RESULT_NOTSUPPORTED
                       ? LOG_DEBUG : LOG_ERR,
                       "Failed to chmod file %s: %d (%s)", path, ret,
                       strerror(-ret));
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_chmod_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld rename request by renaming or moving an object
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_rename(struct daemon_client * c)
{
    struct afpsl_ipc_rename_response response;
    struct afpsl_ipc_rename_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path_from;
    const char *path_to;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_from_len + 1U
                  + (size_t)request->path_to_len + 1U;

    if (request->path_from_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request->path_to_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->paths[request->path_from_len] != '\0'
            || request->paths[request->path_from_len + 1U
                              + request->path_to_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path_from = request->paths;
    path_to = request->paths + request->path_from_len + 1U;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_rename(v, path_from, path_to);

    if (ret < 0) {
        result = AFPSL_IPC_RESULT_ERROR;
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Failed to rename file from %s to %s: %d",
                       path_from, path_to, ret);
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_rename_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld unlink request by deleting a file
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_unlink(struct daemon_client * c)
{
    struct afpsl_ipc_unlink_response response;
    struct afpsl_ipc_unlink_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_unlink(v, path);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_unlink: File %s not found (ENOENT)",
                           path);
        } else if (ret == -EACCES || ret == -EPERM) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_unlink: Permission denied for %s (EACCES/EPERM)",
                           path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to unlink file %s: %d (%s)", path, ret,
                           strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_unlink_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld truncate request by setting a file's length
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_truncate(struct daemon_client * c)
{
    struct afpsl_ipc_truncate_response response;
    struct afpsl_ipc_truncate_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_truncate(v, path, request->offset);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_truncate: File %s not found (ENOENT)",
                           path);
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_truncate: Permission denied for %s (EACCES)",
                           path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to truncate file %s: %d (%s)", path, ret,
                           strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_truncate_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld utime request by setting an object's times
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_utime(struct daemon_client * c)
{
    struct afpsl_ipc_utime_response response;
    struct afpsl_ipc_utime_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_utime(v, path, &request->times);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD, LOG_ERR, "Failed to utime file %s: %d",
                       path, ret);
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_utime_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld mkdir request by creating a directory
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_mkdir(struct daemon_client * c)
{
    struct afpsl_ipc_mkdir_response response;
    struct afpsl_ipc_mkdir_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_mkdir(v, path, request->mode);

    if (ret < 0) {
        if (ret == -EEXIST) {
            result = AFPSL_IPC_RESULT_EXIST;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_mkdir: Directory %s already exists (EEXIST)",
                           path);
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_mkdir: Permission denied creating %s (EACCES)",
                           path);
        } else if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
            log_for_client(
                (void *) c, AFPFSD, LOG_ERR,
                "process_mkdir: Parent directory not found for %s (ENOENT)",
                path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to create directory %s: %d (%s)", path,
                           ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_mkdir_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld rmdir request by deleting an empty directory
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_rmdir(struct daemon_client * c)
{
    struct afpsl_ipc_rmdir_response response;
    struct afpsl_ipc_rmdir_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_rmdir(v, path);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_rmdir: Directory %s not found (ENOENT)",
                           path);
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_rmdir: Permission denied for %s (EACCES)",
                           path);
        } else if (ret == -ENOTEMPTY) {
            result = AFPSL_IPC_RESULT_ENOTEMPTY;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_rmdir: Directory %s is not empty (ENOTEMPTY)",
                           path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to remove directory %s: %d (%s)", path,
                           ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_rmdir_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld statfs request with a volume's size and free space
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and the figures
 */
static unsigned char process_statfs(struct daemon_client * c)
{
    struct afpsl_ipc_statfs_response response;
    struct afpsl_ipc_statfs_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;
    memset(&response.stat, 0, sizeof(struct statvfs));

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Querying filesystem stats for path '%s'", path);
    ret = ml_statfs(v, path, &response.stat);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFPSL_IPC_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_statfs: Path %s not found (ENOENT)",
                           path);
        } else if (ret == -EACCES) {
            result = AFPSL_IPC_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_statfs: Permission denied for %s (EACCES)",
                           path);
        } else {
            result = AFPSL_IPC_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to get filesystem stats for %s: %d (%s)",
                           path, ret, strerror(-ret));
        }
    } else {
        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "Filesystem: blocks=%llu free=%llu avail=%llu",
                       (unsigned long long)response.stat.f_blocks,
                       (unsigned long long)response.stat.f_bfree,
                       (unsigned long long)response.stat.f_bavail);
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_statfs_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld close request by closing an open fork
 *
 * The handle is cleared as it is taken, so a second close of it finds no fork.
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_close(struct daemon_client * c)
{
    struct afpsl_ipc_close_response response;
    struct afpsl_ipc_close_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret = AFPSL_IPC_RESULT_OK;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_close_request)) {
        ret = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        ret = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        ret = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    struct afp_file_info *fp = get_file_handle(request->fileid, v, 1);

    if (!fp) {
        ret = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    ret = ml_close(v, fp);
    free(fp);
done:
    response.header.len = sizeof(struct afpsl_ipc_close_response);
    response.header.result = ret;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Answer an afpsld stat request with a path's attributes
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and the attributes
 */
static unsigned char process_stat(struct daemon_client * c)
{
    struct afpsl_ipc_stat_response response;
    struct afpsl_ipc_stat_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    int ret;
    int result = AFPSL_IPC_RESULT_OK;

    if ((size_t)c->completed_packet_size < sizeof(*request)) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    request_len = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || request->header.len != request_len
            || request->path[request->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto done;
    }

    path = request->path;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Getting attributes for path '%s'", path);

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto done;
    }

    /* Fresh, as afpcmd checks a download's length against it. */
    ret = ml_getattr_fresh(v, path, &response.stat);

    if (ret < 0) {
        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "ml_getattr_fresh error for '%s': %d (%s)", path, ret,
                       strerror(-ret));
    }

    if (ret == -ENOENT) {
        result = AFPSL_IPC_RESULT_ENOENT;
    } else if (ret == -EACCES) {
        result = AFPSL_IPC_RESULT_ACCESS;
    } else if (ret < 0) {
        result = AFPSL_IPC_RESULT_ERROR;
    } else {
        result = AFPSL_IPC_RESULT_OK;
    }

done:
    response.header.len = sizeof(struct afpsl_ipc_stat_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Send a metadata response
 *
 * Without memory for the response, the connection closes.
 *
 * @param[in,out] c              the client
 * @param[in]     error          0, or the request's negative errno
 * @param[in]     data           the bytes it returns, or NULL
 * @param[in]     data_size      length of data
 * @param[in]     reported_size  the size the response reports
 */
static void send_metadata_response(struct daemon_client *c, int error,
                                   const void *data, unsigned int data_size,
                                   unsigned int reported_size)
{
    struct afpsl_ipc_metadata_response *response;
    size_t len = sizeof(*response) + data_size;
    response = calloc(1, len);

    if (!response) {
        close_client_connection(c);
        return;
    }

    response->header.result = AFPSL_IPC_RESULT_OK;
    response->header.len = (unsigned int)len;
    response->error = error;
    response->size = reported_size;

    if (data && data_size > 0) {
        memcpy(response->data, data, data_size);
    }

    finish_response(c,
                    send_command(c, response->header.len, (char *) response), 0);
    free(response);
}

/*!
 * @brief Answer an afpsld request on an attribute, FinderInfo or resource fork
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result
 */
static unsigned char process_metadata(struct daemon_client *c)
{
    struct afpsl_ipc_metadata_request *request = (void *)c->complete_packet;
    struct afp_volume *volume = NULL;
    size_t base = offsetof(struct afpsl_ipc_metadata_request, data);
    const char *path;
    const char *name;
    const char *request_data;
    char data[AFP_SL_METADATA_CHUNK];
    const char *response_data = data;
    char *allocated_data = NULL;
    unsigned int response_size = 0;
    unsigned int response_data_size = 0;
    size_t request_limit;
    int sends_data;
    size_t expected_len;
    int ret = -EINVAL;

    if ((size_t)c->completed_packet_size < base) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    sends_data = request->header.command == AFPSL_IPC_COMMAND_SETXATTR
                 || request->header.command == AFPSL_IPC_COMMAND_SETFINDERINFO
                 || request->header.command == AFPSL_IPC_COMMAND_SETRESOURCEFORK;
    request_limit = request->header.command == AFPSL_IPC_COMMAND_LISTXATTR
                    ? AFP_SL_XATTR_LIST_MAX
                    : AFP_SL_METADATA_CHUNK;

    if (request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request->name_len >= 256) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    expected_len = base + (size_t)request->path_len + 1U
                   + (size_t)request->name_len + 1U;

    if (sends_data) {
        expected_len += request->size;
    }

    if (request->size > request_limit
            || request->header.len != expected_len
            || (size_t)c->completed_packet_size != request->header.len) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    path = request->data;
    name = path + request->path_len + 1U;
    request_data = name + request->name_len + 1U;

    if (path[request->path_len] != '\0'
            || name[request->name_len] != '\0') {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if ((request->header.command == AFPSL_IPC_COMMAND_GETXATTR
            || request->header.command == AFPSL_IPC_COMMAND_SETXATTR
            || request->header.command == AFPSL_IPC_COMMAND_REMOVEXATTR)
            && name[0] == '\0') {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFPSL_IPC_COMMAND_SETFINDERINFO
            && request->size != 32) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFPSL_IPC_COMMAND_SETXATTR
            && ((request->flags & ~(kXAttrCreate | kXAttrReplace)) != 0
                || (request->flags & kXAttrCreate
                    && request->flags & kXAttrReplace))) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFPSL_IPC_COMMAND_SETRESOURCEFORK
            && (request->offset > INT_MAX
                || request->size > (size_t)(INT_MAX - request->offset))) {
        send_metadata_response(c, -EFBIG, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK
            && request->offset > INT_MAX) {
        send_metadata_response(c, -EFBIG, NULL, 0, 0);
        return 0;
    }

    volume = afp_volume_find_by_pointer_hold(request->volumeid);

    if (!volume) {
        send_metadata_response(c, -ESTALE, NULL, 0, 0);
        return 0;
    }

    if (!volume_server_is_connected(volume)) {
        send_metadata_response(c, -ENOTCONN, NULL, 0, 0);
        afp_server_release(volume->server);
        return 0;
    }

    switch (request->header.command) {
    case AFPSL_IPC_COMMAND_GETXATTR: {
        char *xattr_data = NULL;

        if (request->size) {
            xattr_data = data;
        }

        ret = ml_getxattr(volume, path, name, xattr_data,
                          request->size);
    }
    break;

    case AFPSL_IPC_COMMAND_LISTXATTR: {
        char *listxattr_data = NULL;

        if (request->size) {
            allocated_data = malloc(request->size);

            if (!allocated_data) {
                ret = -ENOMEM;
                break;
            }

            listxattr_data = allocated_data;
            response_data = allocated_data;
        }

        ret = ml_listxattr(volume, path, listxattr_data, request->size);
    }
    break;

    case AFPSL_IPC_COMMAND_GETFINDERINFO: {
        char *finderinfo_data = NULL;

        if (request->size) {
            finderinfo_data = data;
        }

        ret = ml_getfinderinfo(volume, path, finderinfo_data, request->size);
    }
    break;

    case AFPSL_IPC_COMMAND_GETRESOURCEFORK:
        if (request->offset > (unsigned long long)INT64_MAX) {
            ret = -EOVERFLOW;
        } else {
            ret = ml_getresourcefork(volume, path,
                                     request->size ? data : NULL,
                                     request->size, (off_t)request->offset);
        }

        break;

    case AFPSL_IPC_COMMAND_SETXATTR:
        ret = ml_setxattr(volume, path, name, request_data, request->size,
                          request->flags);
        break;

    case AFPSL_IPC_COMMAND_SETFINDERINFO:
        ret = ml_setfinderinfo(volume, path, request_data, request->size);
        break;

    case AFPSL_IPC_COMMAND_SETRESOURCEFORK:
        if (request->offset > (unsigned long long)INT64_MAX) {
            ret = -EOVERFLOW;
        } else {
            ret = ml_setresourcefork(volume, path, request_data,
                                     request->size, (off_t)request->offset);
        }

        break;

    case AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK:
        ret = ml_truncateresourcefork(volume, path, request->offset);
        break;

    case AFPSL_IPC_COMMAND_REMOVEXATTR:
        ret = ml_removexattr(volume, path, name);
        break;

    case AFPSL_IPC_COMMAND_REMOVEFINDERINFO:
        ret = ml_removefinderinfo(volume, path);
        break;

    case AFPSL_IPC_COMMAND_REMOVERESOURCEFORK:
        ret = ml_removeresourcefork(volume, path);
        break;

    default:
        ret = -EINVAL;
        break;
    }

    if (ret > 0) {
        response_size = (unsigned int)ret;

        if (request->size > 0) {
            if (response_size > request->size) {
                send_metadata_response(c, -ERANGE, NULL, 0, response_size);
                free(allocated_data);
                afp_server_release(volume->server);
                return 0;
            }

            response_data_size = response_size;
        }

        ret = 0;
    }

    if (response_data_size > 0) {
        send_metadata_response(c, ret, response_data, response_data_size,
                               response_size);
    } else {
        send_metadata_response(c, ret, NULL, response_data_size, response_size);
    }

    free(allocated_data);
    afp_server_release(volume->server);
    return 0;
}

/*! @brief A listing entry under its name, for the sort */
struct readdir_name {
    const char *name;
    const struct afp_dirent *e;
};

/*!
 * @brief Order two listing entries by name, case folded
 *
 * @param[in] a  a struct readdir_name
 * @param[in] b  another
 * @returns strcasecmp()'s result on their names
 */
static int compare_readdir_names(const void *a, const void *b)
{
    const struct readdir_name *na = a;
    const struct readdir_name *nb = b;
    return strcasecmp(na->name, nb->name);
}

/*!
 * @brief Answer a readdir request with a slice of the directory's listing
 *
 * Every request lists the directory whole, sorted by name case folded; the
 * slice goes out in the stat form, with the end flag at the last entry.
 *
 * @param[in] c  the client, with the request
 * @returns AFPSL_IPC_RESULT_OK, or AFPSL_IPC_RESULT_ERROR
 */
static unsigned char process_readdir(struct daemon_client * c)
{
    struct afpsl_ipc_readdir_request * req = (void *) c->complete_packet;
    struct afpsl_ipc_readdir_response * response;
    unsigned int len = sizeof(struct afpsl_ipc_readdir_response);
    unsigned int result;
    struct afp_volume * v = NULL;
    const char *path;
    size_t request_len;
    char *data, *p;
    struct afp_listing listing;
    struct readdir_name *names = NULL;
    unsigned int numfiles;
    int i;
    int ret;
    memset(&listing, 0, sizeof(listing));

    if ((size_t)c->completed_packet_size < sizeof(*req) || req->start < 0) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    request_len = sizeof(*req) + (size_t)req->path_len + 1U;

    if (req->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request_len != (size_t)c->completed_packet_size
            || req->header.len != request_len
            || req->path[req->path_len] != '\0') {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    path = req->path;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Reading directory '%s' (start=%d, count=%d)", path,
                   req->start, req->count);

    if ((v = afp_volume_find_by_pointer_hold(req->volumeid)) == NULL) {
        result = AFPSL_IPC_RESULT_ENOENT;
        goto error;
    }

    if (!volume_server_is_connected(v)) {
        result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto error;
    }

    ret = ml_readdir(v, path, &listing);

    if (ret) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    numfiles = listing.count;

    /* The names sort; the records stay in the server's order. */
    if (numfiles > 0) {
        names = malloc(numfiles * sizeof(*names));

        if (names == NULL) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Out of memory sorting file list");
            result = AFPSL_IPC_RESULT_ERROR;
            goto error;
        }

        for (i = 0; i < (int) numfiles; i++) {
            names[i].e = &listing.entries[i];
            names[i].name = afp_dirent_name(&listing, names[i].e);
        }

        qsort(names, numfiles, sizeof(*names), compare_readdir_names);
    }

    /* Make sure we're not running off the end */
    if ((unsigned int)req->start > numfiles) {
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    /* We allocate max buffer, actual length will be determined during packing */
    response = alloc_response(AFPSL_IPC_MAX_RESPONSE, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating readdir response");
        result = AFPSL_IPC_RESULT_ERROR;
        goto error;
    }

    result = AFPSL_IPC_RESULT_OK;
    /* Initialize to 0, will be set to 1 if we reach the end */
    response->eod = 0;
    data = (char *) response + sizeof(struct afpsl_ipc_readdir_response);
    const char *end = (char *) response + AFPSL_IPC_MAX_RESPONSE;
    p = data;

    /* Pack data into variable length format to save space */

    /* A negative count asks for every entry the response holds. */
    for (i = 0; (req->count < 0 || i < req->count)
            && req->start + i < (int) numfiles; i++) {
        const struct afp_dirent *e = names[req->start + i].e;
        const char *name = names[req->start + i].name;
        struct afpc_unix_privileges privs;
        struct stat st;
        int64_t creation_date;
        int64_t modification_date;
        uint64_t size;
        size_t name_len = strlen(name);
        size_t entry_size = sizeof(uint32_t) + name_len +
                            sizeof(int64_t) * 2 +
                            sizeof(struct afpc_unix_privileges) +
                            sizeof(uint64_t);

        if (p + entry_size > end) {
            /* Buffer full */
            break;
        }

        if (afp_dirent_to_stat(v, e, &st) < 0) {
            result = AFPSL_IPC_RESULT_ERROR;
            break;
        }

        privs = e->unixprivs;
        privs.uid = st.st_uid;
        privs.gid = st.st_gid;
        privs.permissions = st.st_mode;
        creation_date = st.st_ctime;
        modification_date = st.st_mtime;
        size = (uint64_t) st.st_size;
        uint32_t nl = name_len;
        memcpy(p, &nl, sizeof(uint32_t));
        p += sizeof(uint32_t);
        memcpy(p, name, name_len);
        p += name_len;
        memcpy(p, &creation_date, sizeof(creation_date));
        p += sizeof(creation_date);
        memcpy(p, &modification_date, sizeof(modification_date));
        p += sizeof(modification_date);
        memcpy(p, &privs, sizeof(privs));
        p += sizeof(privs);
        memcpy(p, &size, sizeof(size));
        p += sizeof(size);
    }

    response->numfiles = i;

    if (req->start + i >= (int) numfiles) {
        response->eod = 1;
    }

    free(names);
    afp_listing_free(&listing);
    goto done;
error:
    /* Reset len for error case */
    len = sizeof(struct afpsl_ipc_readdir_response);
    response = alloc_response(len, sizeof(*response));
    free(names);
    afp_listing_free(&listing);

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory in readdir error path");

        if (v) {
            afp_server_release(v->server);
        }

        return AFPSL_IPC_RESULT_ERROR;
    }

    response->numfiles = 0;
    p = (char *)response + len;
done:
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Directory read completed: %d files returned", response->numfiles);
    response->header.len = (p - (char *)response);
    response->header.result = result;
    finish_response(c,
                    send_command(c, response->header.len, (char *) response),
                    req->header.close);
    free(response);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

/*!
 * @brief Whether a server's connection uses one of a list's addresses
 *
 * @param[in] server   the server
 * @param[in] address  the address list, or NULL
 * @returns 1 when it does
 */
static int server_address_matches(struct afp_server *server,
                                  struct addrinfo *address)
{
    if (!address || !server->used_address || !server->used_address->ai_addr) {
        return 0;
    }

    for (struct addrinfo *p = address; p; p = p->ai_next) {
        if (p->ai_addr && server->used_address->ai_addrlen == p->ai_addrlen
                && memcmp(server->used_address->ai_addr, p->ai_addr,
                          p->ai_addrlen) == 0) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Whether a saved session's user and UAM fit a reconnect's
 *
 * @param[in] server    the server
 * @param[in] url       the reconnect's URL, with its user
 * @param[in] uam_mask  the UAMs it allows, or 0 for any
 * @returns 1 when they fit
 */
static int server_auth_matches_reconnect(struct afp_server *server,
                                         const struct afpc_url *url,
                                         unsigned int uam_mask)
{
    if (strcmp(server->username, url->username) != 0) {
        return 0;
    }

    if (uam_mask != 0 && server->using_uam != 0
            && (server->using_uam & uam_mask) == 0) {
        return 0;
    }

    return 1;
}

/*!
 * @brief Find the one live session a connect without a password may resume
 *
 * @param[in]  priv      the client, for the log
 * @param[in]  url       the URL, whose user may be empty
 * @param[in]  uam_mask  the UAMs allowed, or 0 for any
 * @param[out] error     receives EEXIST for several, EPROTO for a session
 *                       without a signature, or ENOTCONN, or NULL
 * @returns the server, held, or NULL
 */
static struct afp_server *find_resumable_server_hold(void *priv,
                                                     const struct afpc_url *url,
                                                     unsigned int uam_mask,
                                                     int *error)
{
    struct afp_server *match = NULL;
    struct addrinfo *address = NULL;
    int matches = 0;
    int invalid_identity = 0;

    if (error) {
        *error = ENOTCONN;
    }

    address = afp_get_address(priv, url->servername, url->port);
    afp_lock_server_list();

    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s->connect_state != SERVER_STATE_CONNECTED || s->fd < 0) {
            continue;
        }

        /* A server name is only a resolver input, not an identity.  A live
         * session may be reused only for its currently resolved endpoint. */
        if (!server_address_matches(s, address)) {
            continue;
        }

        /* Resume is not AFP reauthentication. A password-less resume proves
         * only that the caller can access this daemon's existing session
         * state; it may omit username/UAM and match by host when exactly one
         * live session exists. */
        if ((url->username[0] != '\0'
                && strcmp(s->username, url->username) != 0)
                || (uam_mask != 0 && s->using_uam != 0
                    && (s->using_uam & uam_mask) == 0)) {
            continue;
        }

        if (!afp_server_has_valid_signature(s)) {
            invalid_identity = 1;
            continue;
        }

        match = s;
        matches++;

        if (matches > 1) {
            break;
        }
    }

    if (matches == 1) {
        afp_server_hold(match);
    } else {
        match = NULL;

        if (error) {
            *error = matches > 1 ? EEXIST
                     : invalid_identity ? EPROTO : ENOTCONN;
        }
    }

    afp_unlock_server_list();

    if (address) {
        freeaddrinfo(address);
    }

    return match;
}

/*!
 * @brief Find the disconnected session a connect reaches, by server signature
 *
 * @param[in]  priv              the client, for the log
 * @param[in]  url               the URL
 * @param[in]  uam_mask          the UAMs allowed, or 0 for any
 * @param[out] resolved_address  receives the URL's addresses on a match, or
 *                               NULL
 * @param[out] error             receives EEXIST for several, EPROTO for a
 *                               server whose signature is missing or
 *                               another's, or ENOTCONN, or NULL
 * @returns the server, held, or NULL
 */
static struct afp_server *find_disconnected_server_hold(void *priv,
                                                        const struct afpc_url *url,
                                                        unsigned int uam_mask,
                                                        struct addrinfo **resolved_address,
                                                        int *error)
{
    struct afp_server *match = NULL;
    struct addrinfo *address = NULL;
    char signature[AFP_SIGNATURE_LEN];
    int matches = 0;
    int possible = 0;
    int identity_mismatch = 0;

    if (error) {
        *error = ENOTCONN;
    }

    if (resolved_address) {
        *resolved_address = NULL;
    }

    address = afp_get_address(priv, url->servername, url->port);

    if (!address) {
        goto done;
    }

    afp_lock_server_list();

    /* Do not turn every new connect into a GetStatus probe.  Authentication
     * narrows the search; the signature below is the actual identity match
     * and permits a server to have moved to a new address. */
    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s->connect_state == SERVER_STATE_DISCONNECTED && s->fd < 0
                && server_auth_matches_reconnect(s, url, uam_mask)) {
            possible = 1;
            break;
        }
    }

    afp_unlock_server_list();

    if (!possible) {
        goto done;
    }

    if (afp_server_probe_signature(address, signature) != 0) {
        if (error && errno == EPROTO) {
            *error = EPROTO;
        }

        goto done;
    }

    afp_lock_server_list();

    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s->connect_state != SERVER_STATE_DISCONNECTED || s->fd >= 0) {
            continue;
        }

        if (!server_auth_matches_reconnect(s, url, uam_mask)) {
            continue;
        }

        if (!afp_server_has_valid_signature(s)
                || memcmp(s->signature, signature, sizeof(signature)) != 0) {
            /* This only associates a reconnect request with a saved session
             * after the signature probe. AFP's advertised name is not a URL
             * authority, so it cannot be used here. */
            if (server_address_matches(s, address)
                    || (s->requested_port == url->port
                        && strcasecmp(s->requested_hostname,
                                      url->servername) == 0)) {
                identity_mismatch = 1;
            }

            continue;
        }

        match = s;
        matches++;

        if (matches > 1) {
            break;
        }
    }

    if (matches == 1) {
        afp_server_hold(match);

        if (resolved_address) {
            *resolved_address = address;
            address = NULL;
        }
    } else {
        match = NULL;

        if (error) {
            *error = matches > 1 ? EEXIST
                     : identity_mismatch ? EPROTO : ENOTCONN;
        }
    }

    afp_unlock_server_list();
done:

    if (address) {
        freeaddrinfo(address);
    }

    return match;
}

/*!
 * @brief Log a saved session back in at the URL's resolved address
 *
 * @param[in]     priv     the client, for the log
 * @param[in,out] server   the session's server
 * @param[in]     url      the URL, with the password
 * @param[in]     address  the address list, which the server owns from then on
 * @returns afp_server_reconnect()'s result
 */
static int reconnect_disconnected_server(void *priv,
                                         struct afp_server *server,
                                         const struct afpc_url *url,
                                         struct addrinfo *address)
{
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    int ret;
    memset(mesg, 0, sizeof(mesg));
    afp_server_replace_address(server, address);
    strlcpy(server->password, url->password, sizeof(server->password));
    errno = 0;
    ret = afp_server_reconnect(server, mesg, &len, sizeof(mesg));
    explicit_bzero(server->password, sizeof(server->password));

    if (ret != 0 && mesg[0] != '\0') {
        log_for_client(priv, AFPFSD, LOG_WARNING, "%s", mesg);
    }

    return ret;
}

/*!
 * @brief Answer an afpsld connect request: resume, reconnect or log in anew
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result, or -1 to close the client
 *          for a request cut short
 */
static int process_connect(struct daemon_client * c)
{
    struct afpsl_ipc_connect_request * req;
    struct afp_server  * s = NULL;
    struct afp_connection_request conn_req;
    struct afpsl_ipc_connect_response response;
    int ret = 0;
    int response_result;
    int error = 0;
    char loginmesg_copy[AFP_LOGINMESG_LEN];
    struct afp_server *server_copy = NULL;
    struct addrinfo *reconnect_address = NULL;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_connect_request)) {
        return -1;
    }

    req = (void *) c->complete_packet;
    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    memset(loginmesg_copy, 0, AFP_LOGINMESG_LEN);
    log_for_client((void *) c, AFPFSD, LOG_INFO, "Connecting to server %s",
                   (char *) req->url.servername);

    if (req->flags & AFPSL_IPC_CONNECT_RESUME_EXISTING) {
        if (req->url.password[0] != '\0') {
            log_for_client((void *) c, AFPFSD, LOG_WARNING,
                           "Refusing to resume server %s with supplied password",
                           req->url.servername);
            error = EACCES;
            goto error;
        }

        s = find_resumable_server_hold(c, &req->url, req->uam_mask, &error);

        if (!s) {
            log_for_client((void *) c, AFPFSD, LOG_INFO,
                           "No resumable session for server %s",
                           req->url.servername);
            goto error;
        }

        response_result = AFPSL_IPC_RESULT_ALREADY_CONNECTED;
        server_copy = s;
        memcpy(loginmesg_copy, s->loginmesg, AFP_LOGINMESG_LEN);
        afp_server_release(s);
        goto done;
    }

    s = find_disconnected_server_hold(c, &req->url, req->uam_mask,
                                      &reconnect_address, &error);

    if (s) {
        log_for_client((void *) c, AFPFSD, LOG_INFO,
                       "Recovering disconnected session for server %s",
                       req->url.servername);

        if (reconnect_disconnected_server(c, s, &req->url,
                                          reconnect_address) == 0) {
            reconnect_address = NULL;
            response_result = AFPSL_IPC_RESULT_OK;
            server_copy = s;
            memcpy(loginmesg_copy, s->loginmesg, AFP_LOGINMESG_LEN);
            afp_server_release(s);
            goto done;
        }

        error = errno ? errno : ECONNRESET;
        afp_server_release(s);
        s = NULL;
        reconnect_address = NULL;
        goto error;
    } else if (error == EEXIST || error == EPROTO) {
        goto error;
    }

    /* Initialize connection request */
    conn_req.uam_mask = req->uam_mask;
    memcpy(&conn_req.url, &req->url, sizeof(struct afpc_url));

    /*
    * Sets connect_error:
    * 0:
    *      No error
    * ENONET:
    *      could not get the address of the server
    * ENOMEM:
    *      could not allocate memory
    * ETIMEDOUT:
    *      timed out waiting for connection
    * ENETUNREACH:
    *      Server unreachable
    * EISCONN:
    *      Connection already established
    * ECONNREFUSED:
    *     Remote server has refused the connection
    * EACCES, EPERM, EADDRINUSE, EAFNOSUPPORT, EAGAIN, EALREADY, EBADF,
    * EFAULT, EINPROGRESS, EINTR, ENOTSOCK, EINVAL, EMFILE, ENFILE,
    * ENOBUFS, EPROTONOSUPPORT:
    *     Internal error
    *
    * Returns:
    * 0: No error
    * -1: An error occurred
    */

    if ((s = afp_server_full_connect(c, &conn_req)) == NULL) {
        error = errno;
        signal_main_thread();
        goto error;
    }

    /* Immediately copy data from server before it can be freed asynchronously */
    server_copy = s;

    if (s) {
        memcpy(loginmesg_copy, s->loginmesg, AFP_LOGINMESG_LEN);
    }

    response_result = AFPSL_IPC_RESULT_OK;
    goto done;
error:

    switch (error) {
    case EACCES:
    case EPERM:
        response_result = AFPSL_IPC_RESULT_NOAUTHENT;
        break;

    case ETIMEDOUT:
        response_result = AFPSL_IPC_RESULT_TIMEDOUT;
        break;
#ifdef ENONET

    case ENONET: /* Linux-specific */
#endif
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ECONNREFUSED:
        response_result = AFPSL_IPC_RESULT_NOSERVER;
        break;

    default:
        response_result = AFPSL_IPC_RESULT_ERROR;
        break;
    }

    ret = 0;
done:

    if (reconnect_address) {
        freeaddrinfo(reconnect_address);
    }

    memset(&response, 0, sizeof(response));
    /* Use copied data instead of potentially freed server pointer */
    memcpy(response.loginmesg, loginmesg_copy, AFP_LOGINMESG_LEN);
    response.header.result = (char) response_result;
    response.header.len = sizeof(response);
    response.connect_error = error;
    memcpy(&response.serverid, &server_copy, sizeof(server_copy));
    finish_response(c, send_command(c, sizeof(response), (char *) &response),
                    req->header.close);
    return ret;
}

/*!
 * @brief Answer an afpsld attach request by attaching a server's volume
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the result and the volume's ID
 */
static int process_attach(struct daemon_client * c)
{
    struct afpsl_ipc_attach_request * req = (void *) c->complete_packet;
    struct afp_server * s = NULL;
    struct afp_volume * volume = NULL;
    int response_result = AFPSL_IPC_RESULT_ERROR;
    struct afpsl_ipc_attach_response response;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
                                                    afpsl_ipc_attach_request)) {
        goto error;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    log_for_client((void *) c, AFPFSD, LOG_INFO,
                   "Attaching volume %s on server %s",
                   (char *) req->url.volumename,
                   (char *) req->url.servername);
    s = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!s || s->connect_state != SERVER_STATE_CONNECTED || s->fd < 0) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Attach request with invalid or disconnected server %p",
                       (void *)req->serverid);
        response_result = AFPSL_IPC_RESULT_NOTCONNECTED;
        goto error;
    }

    /* Always call command_sub_attach_volume, which handles:
     * - Finding the volume by name
     * - Checking if already mounted
     * - Opening AFP connection via volopen() if needed
     */
    volume = command_sub_attach_volume(c, s, req->url.volumename,
                                       req->url.volpassword, &response_result);

    if (volume == NULL && response_result == AFPSL_IPC_RESULT_ALREADY_ATTACHED) {
        /* Volume is already attached — look it up for returning its ID */
        volume = find_volume_by_name(s, req->url.volumename);

        if (!volume) {
            response_result = AFPSL_IPC_RESULT_ERROR;
            goto error;
        }

        response_result = AFPSL_IPC_RESULT_ALREADY_ATTACHED;
    } else if (volume == NULL) {
        /* command_sub_attach_volume sets response_result appropriately */
        goto error;
    }

    volume->extra_flags |= req->volume_options;
    response_result = AFPSL_IPC_RESULT_OK;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Volume '%s' attached successfully",
                   volume->volume_name_printable);
    goto done;
error:
done:
    signal_main_thread();
    memset(&response, 0, sizeof(response));
    response.header.result = (char) response_result;
    response.header.len = sizeof(response);

    if (volume) {
        response.volumeid = (afpc_volume_t) volume;
    }

    finish_response(c, send_command(c, sizeof(response), (char *) &response),
                    (size_t) c->completed_packet_size >= sizeof(struct
                                                                afpsl_ipc_request_header) && req->header.close);

    if (s) {
        afp_server_release(s);
    }

    return 0;
}


/*!
 * @brief Send a desktop database response
 *
 * Data past AFPSL_IPC_MAX_RESPONSE goes as -E2BIG; without memory for the
 * response, the connection closes.
 *
 * @param[in,out] c      the client
 * @param[in]     error  0, or the request's negative errno
 * @param[in]     data   the bytes it returns, or NULL
 * @param[in]     size   length of data
 */
static void send_desktop_data_response(struct daemon_client *c, int error,
                                       const void *data, uint32_t size)
{
    struct afpsl_ipc_desktop_data_response *response;
    size_t len = sizeof(*response) + size;

    if (len > AFPSL_IPC_MAX_RESPONSE) {
        error = -E2BIG;
        size = 0;
        len = sizeof(*response);
    }

    response = calloc(1, len);

    if (!response) {
        close_client_connection(c);
        return;
    }

    response->header.result = AFPSL_IPC_RESULT_OK;
    response->header.len = (unsigned int)len;
    response->error = error;
    response->size = size;

    if (data && size) {
        memcpy(response->data, data, size);
    }

    finish_response(c, send_command(c, response->header.len, (char *)response),
                    0);
    free(response);
}

/*!
 * @brief Answer an afpsld desktop request for an object's Finder comment
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the errno and the comment
 */
static unsigned char process_desktop_path(struct daemon_client *c)
{
    struct afpsl_ipc_desktop_path_request *request = (void *)c->complete_packet;
    struct afp_volume *volume = NULL;
    struct afp_comment comment;
    struct stat st;
    struct ml_target t;
    char data[AFP_SL_DESKTOP_COMMENT_MAX];
    size_t expected;
    int ret;

    if ((size_t)c->completed_packet_size < sizeof(*request)
            || request->path_len >= AFP_MAX_UTF8_PATH_STORAGE) {
        send_desktop_data_response(c, -EINVAL, NULL, 0);
        return 0;
    }

    expected = sizeof(*request) + (size_t)request->path_len + 1U;

    if (request->header.len != expected
            || (size_t)c->completed_packet_size != expected
            || request->path[request->path_len] != '\0') {
        send_desktop_data_response(c, -EINVAL, NULL, 0);
        return 0;
    }

    volume = afp_volume_find_by_pointer_hold(request->volumeid);

    if (!volume) {
        send_desktop_data_response(c, -ESTALE, NULL, 0);
        return 0;
    }

    ret = desktop_ensure_open(volume);

    /* A fresh stat of the target keeps a missing comment distinct from a
     * missing or inaccessible path. */
    if (ret == 0) {
        ret = ml_getattr_fresh(volume, request->path, &st);
    }

    if (ret == 0) {
        ret = ml_resolve(volume, request->path, &t);
    }

    if (ret == 0) {
        comment.data = data;
        comment.maxsize = sizeof(data);
        comment.size = 0;
        ret = desktop_error(afp_getcomment(volume, t.did, t.afp_name,
                                           &comment), 1);
    }

    send_desktop_data_response(c, ret, ret == 0 ? data : NULL,
                               ret == 0 ? comment.size : 0);
    afp_server_release(volume->server);
    return 0;
}

/*!
 * @brief Answer an afpsld desktop request that sets an object's Finder comment
 *
 * @param[in,out] c  the client, whose complete packet holds the request
 * @returns 0, the response carrying the errno
 */
static unsigned char process_desktop_set_comment(struct daemon_client *c)
{
    struct afpsl_ipc_desktop_set_comment_request *request =
        (void *)c->complete_packet;
    struct afpsl_ipc_desktop_set_comment_response response;
    struct afp_volume *volume = NULL;
    struct stat st;
    struct ml_target t;
    size_t expected;
    size_t written = 0;
    uint8_t truncated = 0;
    int ret = -EINVAL;
    memset(&response, 0, sizeof(response));

    if ((size_t)c->completed_packet_size < sizeof(*request)
            || request->path_len >= AFP_MAX_UTF8_PATH_STORAGE
            || request->text_len > AFP_SL_DESKTOP_COMMENT_MAX) {
        goto done;
    }

    expected = sizeof(*request) + (size_t)request->path_len + 1U
               + (size_t)request->text_len;

    if (request->header.len != expected
            || (size_t)c->completed_packet_size != expected
            || request->data[request->path_len] != '\0') {
        goto done;
    }

    volume = afp_volume_find_by_pointer_hold(request->volumeid);

    if (!volume) {
        ret = -ESTALE;
        goto done;
    }

    ret = desktop_ensure_open(volume);

    if (ret == 0) {
        ret = ml_getattr_fresh(volume, request->data, &st);
    }

    if (ret == 0) {
        ret = ml_resolve(volume, request->data, &t);
    }

    written = request->text_len;

    if (ret == 0 && volume->server->using_version
            && volume->server->using_version->av_number >= 20
            && volume->server->using_version->av_number <= 22
            && written > AFP_SL_DESKTOP_COMMENT_AFP2_MAX) {
        written = AFP_SL_DESKTOP_COMMENT_AFP2_MAX;
        truncated = 1;
    }

    if (ret == 0) {
        ret = desktop_error(afp_addcomment_sized(volume, t.did, t.afp_name,
                                                 request->data + request->path_len + 1U,
                                                 written), 0);
    }

done:
    response.header.result = AFPSL_IPC_RESULT_OK;
    response.header.len = sizeof(response);
    response.error = ret;

    if (ret == 0) {
        response.written = (uint32_t)written;
        response.truncated = truncated;
    }

    finish_response(c, send_command(c, sizeof(response), (char *)&response),
                    0);

    if (volume) {
        afp_server_release(volume->server);
    }

    return 0;
}

/*!
 * @brief Answer a desktop database request
 *
 * The answer, its errno included, goes to the client from here.
 *
 * @param[in] c  the client, with the request
 * @returns 0
 */
static unsigned char process_desktop_code(struct daemon_client *c)
{
    struct afpsl_ipc_desktop_code_request *request = (void *)c->complete_packet;
    struct afp_volume *volume = NULL;
    int ret;

    if ((size_t)c->completed_packet_size != sizeof(*request)
            || request->header.len != sizeof(*request)) {
        send_desktop_data_response(c, -EINVAL, NULL, 0);
        return 0;
    }

    volume = afp_volume_find_by_pointer_hold(request->volumeid);

    if (!volume) {
        send_desktop_data_response(c, -ESTALE, NULL, 0);
        return 0;
    }

    ret = desktop_ensure_open(volume);

    if (request->header.command == AFPSL_IPC_COMMAND_DESKTOP_GET_ICON_INFO) {
        struct afpsl_ipc_desktop_icon_info_response response;
        struct afp_icon_info info;
        memset(&response, 0, sizeof(response));

        if (ret == 0) {
            ret = desktop_error(afp_geticoninfo(volume, request->creator,
                                                request->index, &info), 0);
        }

        response.header.result = AFPSL_IPC_RESULT_OK;
        response.header.len = sizeof(response);
        response.error = ret;

        if (ret == 0) {
            response.tag = info.tag;
            response.type = info.filetype;
            response.size = info.size;
            response.icon_type = info.icontype;
        }

        finish_response(c, send_command(c, sizeof(response), (char *)&response),
                        0);
    } else if (request->header.command == AFPSL_IPC_COMMAND_DESKTOP_GET_ICON) {
        struct afp_icon icon;
        char *data = NULL;

        if (request->size > AFP_SL_DESKTOP_ICON_MAX) {
            ret = -E2BIG;
        } else if (ret == 0 && request->size > 0) {
            data = malloc(request->size);

            if (!data) {
                ret = -ENOMEM;
            }
        }

        if (ret == 0) {
            icon.data = data;
            icon.maxsize = request->size;
            icon.size = 0;
            ret = desktop_error(afp_geticon(volume, request->creator,
                                            request->type, request->icon_type,
                                            (uint16_t)request->size, &icon), 0);

            if (ret == 0) {
                send_desktop_data_response(c, 0, data, icon.size);
            }
        }

        if (ret != 0) {
            send_desktop_data_response(c, ret, NULL, 0);
        }

        free(data);
    } else {
        struct afpsl_ipc_desktop_appl_response response;
        struct afp_appl appl;
        const unsigned short bitmap = kFPParentDirIDBit | kFPCreateDateBit
                                      | kFPModDateBit | kFPLongNameBit | kFPNodeIDBit
                                      | kFPDataForkLenBit | kFPRsrcForkLenBit;
        memset(&response, 0, sizeof(response));

        if (ret == 0) {
            ret = desktop_error(afp_getappl(volume, request->creator,
                                            request->index, bitmap, &appl), 0);
        }

        response.header.result = AFPSL_IPC_RESULT_OK;
        response.header.len = sizeof(response);
        response.error = ret;

        if (ret == 0) {
            struct timespec created = afp_dirent_time(volume, &appl.file,
                                                      kFPCreateDateBit,
                                                      appl.file.create_ns);
            struct timespec modified = afp_dirent_time(volume, &appl.file,
                                                       kFPModDateBit,
                                                       appl.file.mod_ns);
            response.file_bitmap = appl.bitmap;
            response.tag = appl.tag;
            response.directory_id = appl.file.parent_did;
            response.file_id = appl.file.cnid;
            response.creation_date = created.tv_sec;
            response.modification_date = modified.tv_sec;
            response.data_fork_size = appl.file.size;
            response.resource_fork_size = appl.file.resourcesize;
            strlcpy(response.pathname, appl.name, sizeof(response.pathname));
        }

        finish_response(c, send_command(c, sizeof(response), (char *)&response),
                        0);
    }

    afp_server_release(volume->server);
    return 0;
}

/*!
 * @brief Run a client's complete request under the server operation lock
 *
 * A handler's failure closes the connection.
 *
 * @param[in,out] other  the client
 * @returns NULL
 */
static void *process_command_thread(void *other)
{
    struct daemon_client * c = other;
    int ret = 0;
    const struct afpsl_ipc_request_header * req = (void *) c->complete_packet;
    /* Clear the outgoing log buffer from any previous command on this connection */
    pthread_mutex_lock(&c->command_string_mutex);
    c->outgoing_string[0] = '\0';
    c->outgoing_string_len = 0;
    pthread_mutex_unlock(&c->command_string_mutex);
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "******* processing command %s (%u)",
                   afpsl_ipc_command_name((unsigned char)req->command),
                   (unsigned char)req->command);
    pthread_mutex_lock(&server_op_mutex);

    if (!c->ipc_ready && req->command != AFPSL_IPC_COMMAND_HELLO) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Client did not negotiate the afpsld IPC protocol");
        ret = -1;
    } else switch (req->command) {
        case AFPSL_IPC_COMMAND_HELLO:
            ret = process_hello(c);
            break;

        case AFPSL_IPC_COMMAND_SERVERINFO:
            ret = process_serverinfo(c);
            break;

        case AFPSL_IPC_COMMAND_CONNECT:
            ret = process_connect(c);
            break;

        case AFPSL_IPC_COMMAND_ATTACH:
            ret = process_attach(c);
            break;

        case AFPSL_IPC_COMMAND_DETACH:
            ret = process_detach(c);
            break;

        case AFPSL_IPC_COMMAND_GETVOLID:
            ret = process_getvolid(c);
            break;

        case AFPSL_IPC_COMMAND_READDIR:
            ret = process_readdir(c);
            break;

        case AFPSL_IPC_COMMAND_GETVOLS:
            ret = process_getvols(c);
            break;

        case AFPSL_IPC_COMMAND_STAT:
            ret = process_stat(c);
            break;

        case AFPSL_IPC_COMMAND_OPEN:
            ret = process_open(c);
            break;

        case AFPSL_IPC_COMMAND_READ:
            ret = process_read(c);
            break;

        case AFPSL_IPC_COMMAND_WRITE:
            ret = process_write(c);
            break;

        case AFPSL_IPC_COMMAND_CREAT:
            ret = process_creat(c);
            break;

        case AFPSL_IPC_COMMAND_CHMOD:
            ret = process_chmod(c);
            break;

        case AFPSL_IPC_COMMAND_RENAME:
            ret = process_rename(c);
            break;

        case AFPSL_IPC_COMMAND_UNLINK:
            ret = process_unlink(c);
            break;

        case AFPSL_IPC_COMMAND_TRUNCATE:
            ret = process_truncate(c);
            break;

        case AFPSL_IPC_COMMAND_UTIME:
            ret = process_utime(c);
            break;

        case AFPSL_IPC_COMMAND_MKDIR:
            ret = process_mkdir(c);
            break;

        case AFPSL_IPC_COMMAND_RMDIR:
            ret = process_rmdir(c);
            break;

        case AFPSL_IPC_COMMAND_STATFS:
            ret = process_statfs(c);
            break;

        case AFPSL_IPC_COMMAND_CLOSE:
            ret = process_close(c);
            break;

        case AFPSL_IPC_COMMAND_EXIT:
            ret = process_exit(c);
            break;

        case AFPSL_IPC_COMMAND_STATUS:
            ret = process_status(c);
            break;

        case AFPSL_IPC_COMMAND_DISCONNECT:
            ret = process_disconnect(c);
            break;

        case AFPSL_IPC_COMMAND_CHANGEPW:
            ret = process_changepw(c);
            break;

        case AFPSL_IPC_COMMAND_GETXATTR:
        case AFPSL_IPC_COMMAND_SETXATTR:
        case AFPSL_IPC_COMMAND_LISTXATTR:
        case AFPSL_IPC_COMMAND_REMOVEXATTR:
        case AFPSL_IPC_COMMAND_GETFINDERINFO:
        case AFPSL_IPC_COMMAND_SETFINDERINFO:
        case AFPSL_IPC_COMMAND_REMOVEFINDERINFO:
        case AFPSL_IPC_COMMAND_GETRESOURCEFORK:
        case AFPSL_IPC_COMMAND_SETRESOURCEFORK:
        case AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK:
        case AFPSL_IPC_COMMAND_REMOVERESOURCEFORK:
            ret = process_metadata(c);
            break;

        case AFPSL_IPC_COMMAND_DESKTOP_GET_COMMENT:
            ret = process_desktop_path(c);
            break;

        case AFPSL_IPC_COMMAND_DESKTOP_SET_COMMENT:
            ret = process_desktop_set_comment(c);
            break;

        case AFPSL_IPC_COMMAND_DESKTOP_GET_ICON_INFO:
        case AFPSL_IPC_COMMAND_DESKTOP_GET_ICON:
        case AFPSL_IPC_COMMAND_DESKTOP_GET_APPL:
            ret = process_desktop_code(c);
            break;

        case AFPSL_IPC_COMMAND_GET_MOUNTPOINT:
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Command %d not supported by afpsld", req->command);
            ret = AFPSL_IPC_RESULT_NOTSUPPORTED;
            break;

        default:
            log_for_client((void *)c, AFPFSD, LOG_ERR, "Unknown command %d", req->command);
            ret = AFPSL_IPC_RESULT_ERROR;
        }

    /* Shift back */
    if (ret != 0) {
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Command processing failed (ret=%d), closing connection", ret);
        close_client_connection(c);
    }

    pthread_mutex_unlock(&server_op_mutex);
    return NULL;
}

/*!
 * @brief Read a client's request, and start a thread for it once complete
 *
 * @param[in,out] c  the client
 * @returns 0 once the thread starts, 2 while the request is incomplete, or
 *          -1 to close the client
 */
int process_command(struct daemon_client * c)
{
    ssize_t ret;
    const struct afpsl_ipc_request_header * header;
    pthread_attr_t attr;

    if (!c->complete_packet) {
        ret = read(c->fd, c->incoming_header + c->incoming_header_size,
                   sizeof(c->incoming_header) - c->incoming_header_size);

        if (ret == 0) {
            return -1;
        }

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return 2;
            }

            perror("error reading command header");
            return -1;
        }

        c->incoming_header_size += (size_t)ret;

        if (c->incoming_header_size < sizeof(c->incoming_header)) {
            return 2;
        }

        header = (const struct afpsl_ipc_request_header *)c->incoming_header;

        if (header->len < sizeof(*header)
                || header->len > AFPSL_IPC_MAX_REQUEST) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Invalid command length %u", header->len);
            return -1;
        }

        c->complete_packet = malloc(header->len);

        if (!c->complete_packet) {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "Out of memory allocating command of %u bytes",
                           header->len);
            return -1;
        }

        memcpy(c->complete_packet, c->incoming_header,
               sizeof(c->incoming_header));
        c->completed_packet_size = (int)sizeof(c->incoming_header);
    }

    header = (const struct afpsl_ipc_request_header *)c->complete_packet;

    if ((unsigned int)c->completed_packet_size < header->len) {
        ret = read(c->fd, c->complete_packet + c->completed_packet_size,
                   header->len - (unsigned int)c->completed_packet_size);

        if (ret == 0) {
            return -1;
        }

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return 2;
            }

            perror("error reading command body");
            return -1;
        }

        c->completed_packet_size += (int)ret;

        if ((unsigned int)c->completed_packet_size < header->len) {
            return 2;
        }
    }

    rm_fd_and_signal(c->fd);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&c->processing_thread, &attr, process_command_thread,
                       c) < 0) {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

/*!
 * @brief Attach to a volume of a server
 *
 * @param[in]  c                the client asking, for its log
 * @param[in]  server           the server of the volume
 * @param[in]  volname          the volume's name
 * @param[in]  volpassword      its password, NULL or empty for none
 * @param[out] response_result  AFPSL_IPC_RESULT_OK, or _NOVOLUME,
 *                              _ALREADY_ATTACHED, _VOLPASS_NEEDED or
 *                              _ERROR_UNKNOWN; may be NULL
 * @returns the volume, or NULL when it could not be attached
 */
struct afp_volume *command_sub_attach_volume(struct daemon_client *c,
                                             struct afp_server *server,
                                             char *volname,
                                             char *volpassword,
                                             int *response_result)
{
    struct afp_volume *using_volume;

    if (response_result) {
        *response_result = AFPSL_IPC_RESULT_OK;
    }

    using_volume = find_volume_by_name(server, volname);

    if (!using_volume) {
        if (volname && volname[0]) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Volume %s does not exist on server %s.", volname,
                           server->server_name_printable);
        }

        if (response_result) {
            *response_result = AFPSL_IPC_RESULT_NOVOLUME;
        }

        if (server->num_volumes) {
            char names[1024];
            afp_list_volnames(server, names, 1024);
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Choose a volume from: %s",
                           names);
        } else {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "No volumes available on server %s.",
                           server->server_name_printable);
        }

        goto error;
    }

    if (using_volume->attached == AFP_VOLUME_ATTACHED) {
        log_for_client((void *)c, AFPFSD, LOG_DEBUG,
                       "Volume %s is already attached", volname);

        if (response_result) {
            *response_result = AFPSL_IPC_RESULT_ALREADY_ATTACHED;
        }

        goto error;
    }

    if (using_volume->flags & HasPassword) {
        if (!volpassword || volpassword[0] == '\0') {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Volume password needed");

            if (response_result) {
                *response_result = AFPSL_IPC_RESULT_VOLPASS_NEEDED;
            }

            goto error;
        }

        memcpy(using_volume->volpassword, volpassword, AFP_VOLPASS_LEN);
    } else {
        memset(using_volume->volpassword, 0, AFP_VOLPASS_LEN);
    }

    using_volume->server = server;

    if (volopen(c, using_volume)) {
        log_for_client((void *) c, AFPFSD, LOG_ERR, "Could not attach volume %s",
                       volname);

        if (response_result) {
            *response_result = AFPSL_IPC_RESULT_ERROR_UNKNOWN;
        }

        goto error;
    }

    afp_detect_mapping(using_volume);
    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Volume mapping detected: %d",
                   using_volume->mapping);
    /* The root's dates show the attach's time. */
    using_volume->mount_time = time(NULL);
    return using_volume;
error:
    return NULL;
}
