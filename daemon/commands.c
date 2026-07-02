/*
 *  commands.c - Stateless daemon command handlers
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This file contains command handlers for the afpsld stateless daemon.
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp.h"
#include "afp_server.h"
#include "codepage.h"
#include "compat.h"
#include "dsi.h"
#include "libafpclient.h"
#include "map_def.h"
#include "midlevel.h"
#include "uams_def.h"
#include "utils.h"

#include "commands.h"
#include "daemon.h"
#include "daemon_client.h"

/* File handle table for mapping 32-bit IDs to 64-bit pointers */
#define MAX_OPEN_FILES 1024
static struct afp_file_info *file_handle_table[MAX_OPEN_FILES];
static pthread_mutex_t file_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Serialize all AFP server operations to prevent concurrent DSI calls */
static pthread_mutex_t server_op_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static void *alloc_response(size_t len, size_t fixed_len)
{
    void *response = malloc(len);

    if (response) {
        memset(response, 0, fixed_len);
    }

    return response;
}

static int register_file_handle(struct afp_file_info *fp)
{
    pthread_mutex_lock(&file_handle_mutex);

    for (int i = 1; i < MAX_OPEN_FILES; i++) {
        if (file_handle_table[i] == NULL) {
            file_handle_table[i] = fp;
            pthread_mutex_unlock(&file_handle_mutex);
            return i;
        }
    }

    pthread_mutex_unlock(&file_handle_mutex);
    return 0;
}

static struct afp_file_info *get_file_handle(int id)
{
    struct afp_file_info *fp = NULL;

    if (id <= 0 || id >= MAX_OPEN_FILES) {
        return NULL;
    }

    pthread_mutex_lock(&file_handle_mutex);
    fp = file_handle_table[id];
    pthread_mutex_unlock(&file_handle_mutex);
    return fp;
}

static int volume_server_is_connected(const struct afp_volume *volume)
{
    return volume && volume->server
           && volume->server->connect_state == SERVER_STATE_CONNECTED
           && volume->server->fd > 0;
}

static void release_file_handle(int id)
{
    if (id <= 0 || id >= MAX_OPEN_FILES) {
        return;
    }

    pthread_mutex_lock(&file_handle_mutex);
    file_handle_table[id] = NULL;
    pthread_mutex_unlock(&file_handle_mutex);
}

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

static unsigned char process_status(struct daemon_client * c)
{
    struct afp_server_status_request * req = (void *) c->complete_packet;
    struct afp_server_status_response * response;
    char *output_buffer;
    int output_len = 0;
    int max_len = MAX_CLIENT_RESPONSE - sizeof(struct afp_server_status_response);
    unsigned int response_len;
    int len;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_status_request)) {
        return AFP_SERVER_RESULT_ERROR;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->servername[AFP_SERVER_NAME_LEN - 1] = '\0';
    req->mountpoint[AFP_MOUNTPOINT_LEN - 1] = '\0';
    output_buffer = malloc(MAX_CLIENT_RESPONSE);

    if (!output_buffer) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory in process_status");
        return AFP_SERVER_RESULT_ERROR;
    }

    memset(output_buffer, 0, MAX_CLIENT_RESPONSE);
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
        char temp_buf[4096];
        int temp_len = sizeof(temp_buf);
        afp_status_server(s, temp_buf, &temp_len);
        int written = snprintf(output_buffer + output_len, s_len, "%s", temp_buf);

        if (written > 0) {
            output_len += written;
        }
    }

    afp_unlock_server_list();
    response_len = sizeof(struct afp_server_status_response) + output_len + 1;
    response = alloc_response(response_len, sizeof(*response));

    if (!response) {
        free(output_buffer);
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating status response");
        return AFP_SERVER_RESULT_ERROR;
    }

    response->header.result = AFP_SERVER_RESULT_OKAY;
    response->header.len = response_len;
    /* Copy text after the header */
    memcpy((char*)response + sizeof(struct afp_server_status_response),
           output_buffer, output_len + 1);
    finish_response(c, send_command(c, response_len, (char *)response),
                    req->header.close);
    free(output_buffer);
    free(response);
    return 0;
}

static unsigned char process_detach(struct daemon_client * c)
{
    struct afp_server_detach_request * req;
    struct afp_server_detach_response response;
    struct afp_volume * v = NULL;
    req = (void *) c->complete_packet;

    /* Validate the volumeid */

    if ((v = afp_volume_find_by_pointer_hold(req->volumeid)) == NULL) {
        snprintf(response.detach_message, sizeof(response.detach_message),
                 "No such volume to detach");
        response.header.result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    if (v->attached != AFP_VOLUME_ATTACHED) {
        snprintf(response.detach_message, sizeof(response.detach_message),
                 "%s was not attached\n", v->volume_name);
        response.header.result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    afp_detach_volume(v);
    response.header.result = AFP_SERVER_RESULT_OKAY;
    snprintf(response.detach_message, 1023, "Detached volume %s.\n",
             v->volume_name);
    goto done;
done:
    response.header.len = sizeof(struct afp_server_detach_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_ping(struct daemon_client * c)
{
    log_for_client((void *) c, AFPFSD, LOG_INFO, "Ping!");
    return AFP_SERVER_RESULT_OKAY;
}

static unsigned char process_exit(struct daemon_client * c)
{
    struct afp_server_response_header response;
    /* Check before releasing our slot: count == 1 means we are the only client */
    int last_client = (count_active_clients() == 1);
    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   last_client ? "Exiting (last client)" :
                   "Exit requested but other clients active, staying up");
    response.result = AFP_SERVER_RESULT_OKAY;
    response.len = sizeof(response);
    finish_response(c, send_command(c, sizeof(response), (char *)&response), 1);

    if (last_client) {
        trigger_exit();
        signal_main_thread();
    }

    return 0;
}

static unsigned char process_changepw(struct daemon_client * c)
{
    struct afp_server_changepw_request *req = (void *)c->complete_packet;
    struct afp_server_changepw_response response;
    struct afp_server *server = NULL;
    int ret;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_changepw_request)) {
        response.header.result = AFP_SERVER_RESULT_ERROR;
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
        response.header.result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_passwd(server, req->url.username, req->oldpasswd, req->newpasswd);

    if (ret == 0) {
        response.header.result = AFP_SERVER_RESULT_OKAY;
        response.afp_error = 0;
    } else {
        log_for_client((void *)c, AFPFSD, LOG_WARNING,
                       "Password change failed for user %s (error %d)",
                       req->url.username, ret);
        response.header.result = AFP_SERVER_RESULT_ERROR;
        response.afp_error = ret;
    }

done:
    response.header.len = sizeof(struct afp_server_changepw_response);
    finish_response(c, send_command(c, response.header.len, (char *)&response),
                    req->header.close);

    if (server) {
        afp_server_release(server);
    }

    return 0;
}

static unsigned char process_disconnect(struct daemon_client * c)
{
    struct afp_server_disconnect_request *req = (void *)c->complete_packet;
    struct afp_server_disconnect_response response;
    struct afp_server *server = NULL;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_disconnect_request)) {
        response.header.result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    server = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!server) {
        log_for_client((void *)c, AFPFSD, LOG_WARNING,
                       "Disconnect request with invalid server %p",
                       req->serverid);
        response.header.result = AFP_SERVER_RESULT_OKAY;
        goto done;
    }

    log_for_client((void *)c, AFPFSD, LOG_INFO,
                   "Disconnecting from server %s", server->server_name_printable);
    afp_unmount_all_volumes(server);
    afp_logout(server, DSI_DONT_WAIT);
    afp_server_remove(server);
    response.header.result = AFP_SERVER_RESULT_OKAY;
done:
    response.header.len = sizeof(struct afp_server_disconnect_response);
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
 * AFP_SERVER_RESULT_ERROR : internal error
 * AFP_SERVER_RESULT_NOTCONNECTED: not logged in
 * AFP_SERVER_RESULT_NOTATTACHED: connected, but not attached to volume
 * AFP_SERVER_RESULT_OKAY: lookup succeeded, volumeid set
 */

static unsigned char process_getvolid(struct daemon_client * c)
{
    struct afp_volume * v = NULL;
    struct afp_server * s = NULL;
    struct afp_server_getvolid_request * req = (void *) c->complete_packet;
    struct afp_server_getvolid_response response;
    int ret = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_getvolid_request)) {
        ret = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.path[AFP_MAX_PATH - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    s = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!s || s->connect_state != SERVER_STATE_CONNECTED || s->fd <= 0) {
        ret = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    if ((v = find_volume_by_name(s, req->url.volumename)) == NULL) {
        ret = AFP_SERVER_RESULT_NOVOLUME;
        goto done;
    }

    if (v->attached != AFP_VOLUME_ATTACHED) {
        ret = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    response.volumeid = (volumeid_t) v;
    response.header.result = AFP_SERVER_RESULT_OKAY;
done:
    response.header.result = ret;
    response.header.len = sizeof(struct afp_server_getvolid_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (s) {
        afp_server_release(s);
    }

    return 0;
}

static unsigned char process_serverinfo(struct daemon_client * c)
{
    struct afp_server_serverinfo_request * req = (void *) c->complete_packet;
    struct afp_server_serverinfo_response response;
    struct afp_server * tmpserver = NULL;
    memset(&response, 0, sizeof(response));
    c->pending = 1;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_serverinfo_request)) {
        return AFP_SERVER_RESULT_ERROR;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.path[AFP_MAX_PATH - 1] = '\0';
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
        memcpy(tmpserver->basic.server_name_printable,
               tmpserver->server_name_printable, AFP_SERVER_NAME_UTF8_LEN);
        memcpy(&response.server_basic, &tmpserver->basic,
               sizeof(struct afp_server_basic));
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
            goto error;
        }

        memcpy(&response.server_basic, &tmpserver->basic,
               sizeof(struct afp_server_basic));
        afp_server_remove(tmpserver);
        tmpserver = NULL;  /* Already freed by remove, don't release again */
    }

    response.header.result = AFP_SERVER_RESULT_OKAY;
    goto done;
error:
    response.header.result = AFP_SERVER_RESULT_ERROR;
done:
    response.header.len = sizeof(struct afp_server_serverinfo_response);
    finish_response(c,
                    send_command(c, response.header.len, (char *) &response),
                    req->header.close);

    if (tmpserver) {
        afp_server_release(tmpserver);
    }

    return 0;
}

static unsigned char process_getvols(struct daemon_client * c)
{
    struct afp_server_getvols_request * request = (void *) c->complete_packet;
    struct afp_server_getvols_response * response;
    struct afp_server * server = NULL;
    const struct afp_volume * volume;
    unsigned int result = AFP_SERVER_RESULT_OKAY;
    unsigned int numvols;
    char *p;
    unsigned int len = sizeof(struct afp_server_getvols_response);
    struct afp_volume_summary * sum;

    if (((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_getvols_request)) || (request->start < 0)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto error;
    }

    /* Force null-termination of all string fields from untrusted client data */
    request->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    request->url.uamname[49] = '\0';
    request->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    request->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    request->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    request->url.path[AFP_MAX_PATH - 1] = '\0';
    request->url.zone[AFP_ZONE_LEN - 1] = '\0';
    request->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    server = find_server_by_pointer((struct afp_server *)request->serverid);

    if (!server || server->connect_state != SERVER_STATE_CONNECTED
            || server->fd <= 0) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto error;
    }

    /* find out how many there are */
    numvols = server->num_volumes;

    if ((unsigned int)request->count < numvols) {
        numvols = request->count;
    }

    if ((unsigned int)request->start > numvols) {
        result = AFP_SERVER_RESULT_ERROR;
        goto error;
    }

    len += numvols * sizeof(struct afp_volume_summary);
    response = alloc_response(len, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating volume list");
        goto error;
    }

    p = (char *) response + sizeof(struct afp_server_getvols_response);

    for (int i = request->start; i < (int)(request->start + numvols); i++) {
        volume = &server->volumes[i];
        sum = (void *) p;
        memcpy(sum->volume_name_printable, volume->volume_name_printable,
               AFP_VOLUME_NAME_UTF8_LEN);
        sum->flags = volume->flags;
        p = p + sizeof(struct afp_volume_summary);
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

        return AFP_SERVER_RESULT_ERROR;
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

static unsigned char process_open(struct daemon_client * c)
{
    struct afp_server_open_response response;
    struct afp_server_open_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;
    struct afp_file_info * fp;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_open_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Opening file '%s' mode=%d",
                   request->path, request->mode);
    ret = ml_open(v, request->path, request->mode, &fp);

    if (ret) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
        } else {
            result = AFP_SERVER_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "process_open: Failed to open file %s: %d (%s)",
                       request->path, ret, strerror(-ret));
        goto done;
    }

    int handle = register_file_handle(fp);

    if (handle == 0) {
        ml_close(v, NULL, fp);
        free(fp);
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    response.fileid = handle;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "File opened successfully, handle=%d", handle);
done:
    response.header.len = sizeof(struct afp_server_open_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_read(struct daemon_client * c)
{
    struct afp_server_read_response * response;
    struct afp_server_read_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;
    char *data;
    unsigned int eof = 0;
    unsigned int received = 0;
    unsigned int len = sizeof(struct afp_server_read_response);

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_read_request)) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFP_SERVER_RESULT_ERROR;
        }

        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFP_SERVER_RESULT_ERROR;
        }

        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        response = alloc_response(len, sizeof(*response));

        if (!response) {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Out of memory in read");
            return AFP_SERVER_RESULT_ERROR;
        }

        result = AFP_SERVER_RESULT_NOTCONNECTED;
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

        return AFP_SERVER_RESULT_ERROR;
    }

    data = ((char *) response) + sizeof(struct afp_server_read_response);
    /* Cast fileid back to file_info pointer for stateless operation */
    struct afp_file_info *fp = get_file_handle(request->fileid);

    if (!fp) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    ret = ml_read(v, NULL, data, request->length, request->start, fp, (int*)&eof);

    if (ret > 0) {
        received = ret;
    } else if (ret < 0) {
        result = AFP_SERVER_RESULT_ERROR;
    }

done:
    response->eof = eof;
    /* Only send the actual data read, not the full requested buffer size */
    response->header.len = sizeof(struct afp_server_read_response) + received;
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

static unsigned char process_write(struct daemon_client * c)
{
    struct afp_server_write_response response;
    struct afp_server_write_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;
    char *data = NULL;
    unsigned int written = 0;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_write_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    data = malloc(request->size);

    if (!data) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating %u bytes for write",
                       request->size);
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Read the data payload from client - it follows immediately after
     * the request header */
    int bytes_read = 0;
    int bytes_remaining = request->size;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Writing %u bytes at offset %llu, fileid=%u", request->size,
                   request->offset, request->fileid);

    /* Consume any data already read into the incoming buffer
     * by process_command */
    if (c->incoming_size > 0) {
        int to_copy = min(bytes_remaining, c->incoming_size);
        memcpy(data + bytes_read, c->incoming_string, to_copy);

        /* Shift remaining data in buffer if any */
        if (c->incoming_size > to_copy) {
            memmove(c->incoming_string, c->incoming_string + to_copy,
                    c->incoming_size - to_copy);
        }

        c->incoming_size -= to_copy;
        c->a = c->incoming_string + c->incoming_size;
        bytes_read += to_copy;
        bytes_remaining -= to_copy;
    }

    while (bytes_remaining > 0) {
        ret = read(c->fd, data + bytes_read, bytes_remaining);

        if (ret <= 0) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Error reading write data payload");
            result = AFP_SERVER_RESULT_ERROR;
            goto done;
        }

        bytes_read += ret;
        bytes_remaining -= ret;
    }

    /* Get file handle */
    struct afp_file_info *fp = get_file_handle(request->fileid);

    if (!fp) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Perform the write operation - ml_write wants uid/gid, using 0 for now */
    ret = ml_write(v, NULL, data, request->size, request->offset, fp, 0, 0);

    if (ret > 0) {
        written = ret;
    } else if (ret < 0) {
        result = AFP_SERVER_RESULT_ERROR;
    }

done:
    response.header.len = sizeof(struct afp_server_write_response);
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

static unsigned char process_creat(struct daemon_client * c)
{
    struct afp_server_creat_response response;
    struct afp_server_creat_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_creat_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Creating file '%s' mode=%04o",
                   request->path, request->mode);
    ret = ml_creat(v, request->path, request->mode);

    if (ret < 0) {
        if (ret == -EEXIST) {
            result = AFP_SERVER_RESULT_EXIST;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_creat: File %s exists (EEXIST)", request->path);
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_creat: Permission denied creating %s (EACCES)",
                           request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_creat: Failed to create file %s: %d (%s)",
                           request->path, ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afp_server_creat_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_chmod(struct daemon_client * c)
{
    struct afp_server_chmod_response response;
    struct afp_server_chmod_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_chmod_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_chmod(v, request->path, request->mode);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
        } else if (ret == -EACCES || ret == -EPERM) {
            result = AFP_SERVER_RESULT_ACCESS;
        } else {
            result = AFP_SERVER_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Failed to chmod file %s: %d (%s)", request->path, ret,
                       strerror(-ret));
    }

done:
    response.header.len = sizeof(struct afp_server_chmod_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_rename(struct daemon_client * c)
{
    struct afp_server_rename_response response;
    struct afp_server_rename_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_rename_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path fields from untrusted client data */
    request->path_from[AFP_MAX_PATH - 1] = '\0';
    request->path_to[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_rename(v, request->path_from, request->path_to);

    if (ret < 0) {
        result = AFP_SERVER_RESULT_ERROR;
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Failed to rename file from %s to %s: %d",
                       request->path_from, request->path_to, ret);
    }

done:
    response.header.len = sizeof(struct afp_server_rename_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_unlink(struct daemon_client * c)
{
    struct afp_server_unlink_response response;
    struct afp_server_unlink_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_unlink_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_unlink(v, request->path);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_unlink: File %s not found (ENOENT)",
                           request->path);
        } else if (ret == -EACCES || ret == -EPERM) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_unlink: Permission denied for %s (EACCES/EPERM)",
                           request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to unlink file %s: %d (%s)", request->path, ret,
                           strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afp_server_unlink_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_truncate(struct daemon_client * c)
{
    struct afp_server_truncate_response response;
    struct afp_server_truncate_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_truncate_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_truncate(v, request->path, request->offset);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_truncate: File %s not found (ENOENT)",
                           request->path);
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_truncate: Permission denied for %s (EACCES)",
                           request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to truncate file %s: %d (%s)", request->path, ret,
                           strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afp_server_truncate_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_utime(struct daemon_client * c)
{
    struct afp_server_utime_response response;
    struct afp_server_utime_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_utime_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_utime(v, request->path, &request->times);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
        } else {
            result = AFP_SERVER_RESULT_ERROR;
        }

        log_for_client((void *) c, AFPFSD, LOG_ERR, "Failed to utime file %s: %d",
                       request->path, ret);
    }

done:
    response.header.len = sizeof(struct afp_server_utime_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_mkdir(struct daemon_client * c)
{
    struct afp_server_mkdir_response response;
    struct afp_server_mkdir_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_mkdir_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_mkdir(v, request->path, request->mode);

    if (ret < 0) {
        if (ret == -EEXIST) {
            result = AFP_SERVER_RESULT_EXIST;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_mkdir: Directory %s already exists (EEXIST)",
                           request->path);
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_mkdir: Permission denied creating %s (EACCES)",
                           request->path);
        } else if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
            log_for_client(
                (void *) c, AFPFSD, LOG_ERR,
                "process_mkdir: Parent directory not found for %s (ENOENT)",
                request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to create directory %s: %d (%s)", request->path,
                           ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afp_server_mkdir_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_rmdir(struct daemon_client * c)
{
    struct afp_server_rmdir_response response;
    struct afp_server_rmdir_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_rmdir_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_rmdir(v, request->path);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_rmdir: Directory %s not found (ENOENT)",
                           request->path);
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_rmdir: Permission denied for %s (EACCES)",
                           request->path);
        } else if (ret == -ENOTEMPTY) {
            result = AFP_SERVER_RESULT_ENOTEMPTY;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_rmdir: Directory %s is not empty (ENOTEMPTY)",
                           request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to remove directory %s: %d (%s)", request->path,
                           ret, strerror(-ret));
        }
    }

done:
    response.header.len = sizeof(struct afp_server_rmdir_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_statfs(struct daemon_client * c)
{
    struct afp_server_statfs_response response;
    struct afp_server_statfs_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;
    memset(&response.stat, 0, sizeof(struct statvfs));

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_statfs_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Querying filesystem stats for path '%s'", request->path);
    ret = ml_statfs(v, request->path, &response.stat);

    if (ret < 0) {
        if (ret == -ENOENT) {
            result = AFP_SERVER_RESULT_ENOENT;
            log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                           "process_statfs: Path %s not found (ENOENT)",
                           request->path);
        } else if (ret == -EACCES) {
            result = AFP_SERVER_RESULT_ACCESS;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "process_statfs: Permission denied for %s (EACCES)",
                           request->path);
        } else {
            result = AFP_SERVER_RESULT_ERROR;
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Failed to get filesystem stats for %s: %d (%s)",
                           request->path, ret, strerror(-ret));
        }
    } else {
        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "Filesystem: blocks=%llu free=%llu avail=%llu",
                       (unsigned long long)response.stat.f_blocks,
                       (unsigned long long)response.stat.f_bfree,
                       (unsigned long long)response.stat.f_bavail);
    }

done:
    response.header.len = sizeof(struct afp_server_statfs_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_close(struct daemon_client * c)
{
    struct afp_server_close_response response;
    struct afp_server_close_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret = AFP_SERVER_RESULT_OKAY;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_close_request)) {
        ret = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        ret = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        ret = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    struct afp_file_info *fp = get_file_handle(request->fileid);

    if (!fp) {
        ret = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    ret = ml_close(v, NULL, fp);
    free(fp);
    release_file_handle(request->fileid);
done:
    response.header.len = sizeof(struct afp_server_close_response);
    response.header.result = ret;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static unsigned char process_stat(struct daemon_client * c)
{
    struct afp_server_stat_response response;
    struct afp_server_stat_request * request = (void *) c->complete_packet;
    struct afp_volume * v = NULL;
    int ret;
    int result = AFP_SERVER_RESULT_OKAY;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Getting attributes for path '%s'", request->path);

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_stat_request)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto done;
    }

    /* Force null-termination of path field from untrusted client data */
    request->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(request->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_NOTATTACHED;
        goto done;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto done;
    }

    ret = ml_getattr(v, request->path, &response.stat);

    if (ret < 0) {
        log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                       "ml_getattr error for '%s': %d (%s)", request->path, ret,
                       strerror(-ret));
    }

    if (ret == -ENOENT) {
        result = AFP_SERVER_RESULT_ENOENT;
    } else if (ret < 0) {
        result = AFP_SERVER_RESULT_ERROR;
    } else {
        result = AFP_SERVER_RESULT_OKAY;
    }

done:
    response.header.len = sizeof(struct afp_server_stat_response);
    response.header.result = result;
    finish_response(c, send_command(c, response.header.len, (char *) &response),
                    request->header.close);

    if (v) {
        afp_server_release(v->server);
    }

    return 0;
}

static void send_metadata_response(struct daemon_client *c, int error,
                                   const void *data, unsigned int data_size,
                                   unsigned int reported_size)
{
    struct afp_server_metadata_response *response;
    size_t len = sizeof(*response) + data_size;
    response = calloc(1, len);

    if (!response) {
        close_client_connection(c);
        return;
    }

    response->header.result = AFP_SERVER_RESULT_OKAY;
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

static unsigned char process_metadata(struct daemon_client *c)
{
    struct afp_server_metadata_request *request = (void *)c->complete_packet;
    struct afp_volume *volume = NULL;
    size_t base = offsetof(struct afp_server_metadata_request, data);
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

    sends_data = request->header.command == AFP_SERVER_COMMAND_SETXATTR
                 || request->header.command == AFP_SERVER_COMMAND_SETFINDERINFO
                 || request->header.command == AFP_SERVER_COMMAND_SETRESOURCEFORK;
    request_limit = request->header.command == AFP_SERVER_COMMAND_LISTXATTR
                    ? AFP_SL_XATTR_LIST_MAX
                    : AFP_SL_METADATA_CHUNK;
    expected_len = base;

    if (sends_data) {
        expected_len += request->size;
    }

    if (request->size > request_limit
            || request->header.len != expected_len
            || (size_t)c->completed_packet_size != request->header.len) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (!memchr(request->path, '\0', sizeof(request->path))
            || !memchr(request->name, '\0', sizeof(request->name))) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if ((request->header.command == AFP_SERVER_COMMAND_GETXATTR
            || request->header.command == AFP_SERVER_COMMAND_SETXATTR
            || request->header.command == AFP_SERVER_COMMAND_REMOVEXATTR)
            && request->name[0] == '\0') {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFP_SERVER_COMMAND_SETFINDERINFO
            && request->size != 32) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFP_SERVER_COMMAND_SETXATTR
            && ((request->flags & ~(kXAttrCreate | kXAttrReplace)) != 0
                || (request->flags & kXAttrCreate
                    && request->flags & kXAttrReplace))) {
        send_metadata_response(c, -EINVAL, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFP_SERVER_COMMAND_SETRESOURCEFORK
            && (request->offset > INT_MAX
                || request->size > (size_t)(INT_MAX - request->offset))) {
        send_metadata_response(c, -EFBIG, NULL, 0, 0);
        return 0;
    }

    if (request->header.command == AFP_SERVER_COMMAND_TRUNCATERESOURCEFORK
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
    case AFP_SERVER_COMMAND_GETXATTR: {
        char *xattr_data = NULL;

        if (request->size) {
            xattr_data = data;
        }

        ret = ml_getxattr(volume, request->path, request->name, xattr_data,
                          request->size);
    }
    break;

    case AFP_SERVER_COMMAND_LISTXATTR: {
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

        ret = ml_listxattr(volume, request->path, listxattr_data, request->size);
    }
    break;

    case AFP_SERVER_COMMAND_GETFINDERINFO: {
        char *finderinfo_data = NULL;

        if (request->size) {
            finderinfo_data = data;
        }

        ret = ml_getfinderinfo(volume, request->path, finderinfo_data, request->size);
    }
    break;

    case AFP_SERVER_COMMAND_GETRESOURCEFORK:
        if (request->offset > (unsigned long long)INT64_MAX) {
            ret = -EOVERFLOW;
        } else {
            ret = ml_getresourcefork(volume, request->path,
                                     request->size ? data : NULL,
                                     request->size, (off_t)request->offset);
        }

        break;

    case AFP_SERVER_COMMAND_SETXATTR:
        ret = ml_setxattr(volume, request->path, request->name,
                          request->data, request->size, request->flags);
        break;

    case AFP_SERVER_COMMAND_SETFINDERINFO:
        ret = ml_setfinderinfo(volume, request->path,
                               request->data, request->size);
        break;

    case AFP_SERVER_COMMAND_SETRESOURCEFORK:
        if (request->offset > (unsigned long long)INT64_MAX) {
            ret = -EOVERFLOW;
        } else {
            ret = ml_setresourcefork(volume, request->path, request->data,
                                     request->size, (off_t)request->offset);
        }

        break;

    case AFP_SERVER_COMMAND_TRUNCATERESOURCEFORK:
        ret = ml_truncateresourcefork(volume, request->path, request->offset);
        break;

    case AFP_SERVER_COMMAND_REMOVEXATTR:
        ret = ml_removexattr(volume, request->path, request->name);
        break;

    case AFP_SERVER_COMMAND_REMOVEFINDERINFO:
        ret = ml_removefinderinfo(volume, request->path);
        break;

    case AFP_SERVER_COMMAND_REMOVERESOURCEFORK:
        ret = ml_removeresourcefork(volume, request->path);
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

static int compare_afp_file_info(const void *a, const void *b)
{
    const struct afp_file_info *fa = *(struct afp_file_info * const *)a;
    const struct afp_file_info *fb = *(struct afp_file_info * const *)b;
    return strcasecmp(fa->name, fb->name);
}

static unsigned char process_readdir(struct daemon_client * c)
{
    struct afp_server_readdir_request * req = (void *) c->complete_packet;
    struct afp_server_readdir_response * response;
    unsigned int len = sizeof(struct afp_server_readdir_response);
    unsigned int result;
    struct afp_volume * v = NULL;
    char *data, *p;
    struct afp_file_info *filebase, *fp;
    unsigned int numfiles = 0;
    int i;
    int ret;
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "Reading directory '%s' (start=%d, count=%d)", req->path,
                   req->start, req->count);

    if (((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_readdir_request)) || (req->start < 0)) {
        result = AFP_SERVER_RESULT_ERROR;
        goto error;
    }

    /* Force null-termination of path field from untrusted client data */
    req->path[AFP_MAX_PATH - 1] = '\0';

    if ((v = afp_volume_find_by_pointer_hold(req->volumeid)) == NULL) {
        result = AFP_SERVER_RESULT_ENOENT;
        goto error;
    }

    if (!volume_server_is_connected(v)) {
        result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto error;
    }

    ret = ml_readdir(v, req->path, &filebase);

    if (ret) {
        result = AFP_SERVER_RESULT_ERROR;
        goto error;
    }

    for (fp = filebase; fp; fp = fp->next) {
        numfiles++;
    }

    /* Sort the file list alphabetically */
    if (numfiles > 1) {
        struct afp_file_info **file_array = malloc(numfiles * sizeof(
                                                struct afp_file_info *));

        if (file_array) {
            int idx = 0;

            for (fp = filebase; fp; fp = fp->next) {
                file_array[idx++] = fp;
            }

            qsort(file_array, numfiles, sizeof(struct afp_file_info *),
                  compare_afp_file_info);
            /* Rebuild linked list */
            filebase = file_array[0];

            for (i = 0; i < (int)numfiles - 1; i++) {
                file_array[i]->next = file_array[i + 1];
            }

            file_array[numfiles - 1]->next = NULL;
            free(file_array);
        } else {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Out of memory sorting file list");
        }
    }

    /* Make sure we're not running off the end */
    if ((unsigned int)req->start > numfiles) {
        result = AFP_SERVER_RESULT_ERROR;
        goto error;
    }

    /* Make sure we don't respond with more than asked */
    if (numfiles > (unsigned int)req->count) {
        numfiles = req->count;
    }

    /* We allocate max buffer, actual length will be determined during packing */
    response = alloc_response(MAX_CLIENT_RESPONSE, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory allocating readdir response");
        afp_ml_filebase_free(&filebase);
        return AFP_SERVER_RESULT_ERROR;
    }

    result = AFP_SERVER_RESULT_OKAY;
    /* Initialize to 0, will be set to 1 if we reach the end */
    response->eod = 0;
    data = (char *) response + sizeof(struct afp_server_readdir_response);
    const char *end = (char *) response + MAX_CLIENT_RESPONSE;
    p = data;
    fp = filebase;

    /* Advance to the first one */
    for (i = 0; i < req->start; i++) {
        if (!fp) {
            response->eod = 1;
            response->numfiles = 0;
            afp_ml_filebase_free(&filebase);
            goto done;
        }

        fp = fp->next;
    }

    /* Pack data into variable length format to save space */

    for (i = 0; i < (int)numfiles; i++) {
        if (!fp) {
            response->eod = 1;
            break;
        }

        size_t name_len = strlen(fp->name);
        size_t entry_size = sizeof(uint32_t) + name_len +
                            sizeof(uint32_t) * 2 +
                            sizeof(struct afp_unixprivs) +
                            sizeof(uint64_t);

        if (p + entry_size > end) {
            /* Buffer full */
            break;
        }

        uint32_t nl = name_len;
        memcpy(p, &nl, sizeof(uint32_t));
        p += sizeof(uint32_t);
        memcpy(p, fp->name, name_len);
        p += name_len;
        memcpy(p, &fp->creation_date, sizeof(uint32_t));
        p += sizeof(uint32_t);
        memcpy(p, &fp->modification_date, sizeof(uint32_t));
        p += sizeof(uint32_t);
        memcpy(p, &fp->unixprivs, sizeof(struct afp_unixprivs));
        p += sizeof(struct afp_unixprivs);
        memcpy(p, &fp->size, sizeof(uint64_t));
        p += sizeof(uint64_t);
        fp = fp->next;
    }

    response->numfiles = i;

    if (!fp) {
        response->eod = 1;
    }

    afp_ml_filebase_free(&filebase);
    goto done;
error:
    /* Reset len for error case */
    len = sizeof(struct afp_server_readdir_response);
    response = alloc_response(len, sizeof(*response));

    if (!response) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Out of memory in readdir error path");
        afp_ml_filebase_free(&filebase);
        return AFP_SERVER_RESULT_ERROR;
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

static int server_name_matches_url(struct afp_server *server,
                                   const struct afp_url *url)
{
    return strcmp(server->server_name_utf8, url->servername) == 0
           || strcmp(server->server_name, url->servername) == 0
           || strcmp(server->server_name_printable, url->servername) == 0;
}

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

static int server_auth_matches_resume(struct afp_server *server,
                                      const struct afp_url *url,
                                      unsigned int uam_mask)
{
    /*
     * Resume is not AFP reauthentication. A password-less resume proves only
     * that the caller can access this daemon's existing session state; it may
     * omit username/UAM and match by host when exactly one live session exists.
     */
    if (url->username[0] != '\0'
            && strcmp(server->username, url->username) != 0) {
        return 0;
    }

    if (uam_mask != 0 && server->using_uam != 0
            && (server->using_uam & uam_mask) == 0) {
        return 0;
    }

    return 1;
}

static int server_auth_matches_reconnect(struct afp_server *server,
        const struct afp_url *url, unsigned int uam_mask)
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

static struct afp_server *find_resumable_server_hold(void *priv,
        const struct afp_url *url, unsigned int uam_mask, int *error)
{
    struct afp_server *match = NULL;
    struct addrinfo *address = NULL;
    int matches = 0;

    if (error) {
        *error = ENOTCONN;
    }

    address = afp_get_address(priv, url->servername, url->port);
    afp_lock_server_list();

    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s->connect_state != SERVER_STATE_CONNECTED || s->fd <= 0) {
            continue;
        }

        if (!server_name_matches_url(s, url)
                && !server_address_matches(s, address)) {
            continue;
        }

        if (!server_auth_matches_resume(s, url, uam_mask)) {
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

        if (error && matches > 1) {
            *error = EEXIST;
        }
    }

    afp_unlock_server_list();

    if (address) {
        freeaddrinfo(address);
    }

    return match;
}

static struct afp_server *find_disconnected_server_hold(void *priv,
        const struct afp_url *url, unsigned int uam_mask, int *error)
{
    struct afp_server *match = NULL;
    struct addrinfo *address = NULL;
    int matches = 0;

    if (error) {
        *error = ENOTCONN;
    }

    address = afp_get_address(priv, url->servername, url->port);
    afp_lock_server_list();

    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s->connect_state != SERVER_STATE_DISCONNECTED || s->fd > 0) {
            continue;
        }

        if (!server_name_matches_url(s, url)
                && !server_address_matches(s, address)) {
            continue;
        }

        if (!server_auth_matches_reconnect(s, url, uam_mask)) {
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

        if (error && matches > 1) {
            *error = EEXIST;
        }
    }

    afp_unlock_server_list();

    if (address) {
        freeaddrinfo(address);
    }

    return match;
}

static int reconnect_disconnected_server(void *priv, struct afp_server *server,
        const struct afp_url *url)
{
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    int ret;
    memset(mesg, 0, sizeof(mesg));
    strlcpy(server->password, url->password, sizeof(server->password));
    errno = 0;
    ret = afp_server_reconnect(server, mesg, &len, sizeof(mesg));
    explicit_bzero(server->password, sizeof(server->password));

    if (ret != 0 && mesg[0] != '\0') {
        log_for_client(priv, AFPFSD, LOG_WARNING, "%s", mesg);
    }

    return ret;
}

static int process_connect(struct daemon_client * c)
{
    struct afp_server_connect_request * req;
    struct afp_server  * s = NULL;
    struct afp_connection_request conn_req;
    struct afp_server_connect_response response;
    int ret = 0;
    int response_result;
    int error = 0;
    char loginmesg_copy[AFP_LOGINMESG_LEN];
    struct afp_server *server_copy = NULL;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_connect_request)) {
        return -1;
    }

    req = (void *) c->complete_packet;
    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.path[AFP_MAX_PATH - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    memset(loginmesg_copy, 0, AFP_LOGINMESG_LEN);
    log_for_client((void *) c, AFPFSD, LOG_INFO, "Connecting to server %s",
                   (char *) req->url.servername);

    if (req->flags & AFP_SERVER_CONNECT_RESUME_EXISTING) {
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

        response_result = AFP_SERVER_RESULT_ALREADY_CONNECTED;
        server_copy = s;
        memcpy(loginmesg_copy, s->loginmesg, AFP_LOGINMESG_LEN);
        afp_server_release(s);
        goto done;
    }

    s = find_disconnected_server_hold(c, &req->url, req->uam_mask, &error);

    if (s) {
        log_for_client((void *) c, AFPFSD, LOG_INFO,
                       "Recovering disconnected session for server %s",
                       req->url.servername);

        if (reconnect_disconnected_server(c, s, &req->url) == 0) {
            response_result = AFP_SERVER_RESULT_OKAY;
            server_copy = s;
            memcpy(loginmesg_copy, s->loginmesg, AFP_LOGINMESG_LEN);
            afp_server_release(s);
            goto done;
        }

        error = errno ? errno : ECONNRESET;
        afp_server_release(s);
        s = NULL;
    } else if (error == EEXIST) {
        goto error;
    }

    /* Initialize connection request */
    conn_req.uam_mask = req->uam_mask;
    memcpy(&conn_req.url, &req->url, sizeof(struct afp_url));

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

    response_result = AFP_SERVER_RESULT_OKAY;
    goto done;
error:

    switch (error) {
    case EACCES:
    case EPERM:
        response_result = AFP_SERVER_RESULT_NOAUTHENT;
        break;

    case ETIMEDOUT:
        response_result = AFP_SERVER_RESULT_TIMEDOUT;
        break;
#ifdef ENONET

    case ENONET: /* Linux-specific */
#endif
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ECONNREFUSED:
        response_result = AFP_SERVER_RESULT_NOSERVER;
        break;

    default:
        response_result = AFP_SERVER_RESULT_ERROR;
        break;
    }

    ret = 0;
done:
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

static int process_attach(struct daemon_client * c)
{
    struct afp_server_attach_request * req = (void *) c->complete_packet;
    struct afp_server * s = NULL;
    struct afp_volume * volume = NULL;
    int response_result = AFP_SERVER_RESULT_ERROR;
    struct afp_server_attach_response response;

    if ((size_t)(c->completed_packet_size) < sizeof(struct
            afp_server_attach_request)) {
        goto error;
    }

    /* Force null-termination of all string fields from untrusted client data */
    req->url.username[AFP_MAX_USERNAME_LEN - 1] = '\0';
    req->url.uamname[49] = '\0';
    req->url.password[AFP_MAX_PASSWORD_LEN - 1] = '\0';
    req->url.servername[AFP_SERVER_NAME_UTF8_LEN - 1] = '\0';
    req->url.volumename[AFP_VOLUME_NAME_UTF8_LEN - 1] = '\0';
    req->url.path[AFP_MAX_PATH - 1] = '\0';
    req->url.zone[AFP_ZONE_LEN - 1] = '\0';
    req->url.volpassword[AFP_VOLPASS_LEN] = '\0';
    log_for_client((void *) c, AFPFSD, LOG_INFO,
                   "Attaching volume %s on server %s",
                   (char *) req->url.volumename,
                   (char *) req->url.servername);
    s = find_server_by_pointer((struct afp_server *)req->serverid);

    if (!s || s->connect_state != SERVER_STATE_CONNECTED || s->fd <= 0) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Attach request with invalid or disconnected server %p",
                       req->serverid);
        response_result = AFP_SERVER_RESULT_NOTCONNECTED;
        goto error;
    }

    /* Always call command_sub_attach_volume, which handles:
     * - Finding the volume by name
     * - Checking if already mounted
     * - Opening AFP connection via volopen() if needed
     */
    volume = command_sub_attach_volume(c, s, req->url.volumename,
                                       req->url.volpassword, &response_result);

    if (volume == NULL && response_result == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
        /* Volume is already attached — look it up for returning its ID */
        volume = find_volume_by_name(s, req->url.volumename);

        if (!volume) {
            response_result = AFP_SERVER_RESULT_ERROR;
            goto error;
        }

        response_result = AFP_SERVER_RESULT_ALREADY_ATTACHED;
    } else if (volume == NULL) {
        /* command_sub_attach_volume sets response_result appropriately */
        goto error;
    }

    volume->extra_flags |= req->volume_options;
    response_result = AFP_SERVER_RESULT_OKAY;
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
        response.volumeid = (volumeid_t) volume;
    }

    finish_response(c, send_command(c, sizeof(response), (char *) &response),
                    (size_t) c->completed_packet_size >= sizeof(struct
                            afp_server_request_header) && req->header.close);

    if (s) {
        afp_server_release(s);
    }

    return 0;
}


static void *process_command_thread(void * other)
{
    struct daemon_client * c = other;
    int ret = 0;
    const struct afp_server_request_header * req = (void *) c->complete_packet;
    /* Clear the outgoing log buffer from any previous command on this connection */
    pthread_mutex_lock(&c->command_string_mutex);
    c->outgoing_string[0] = '\0';
    c->outgoing_string_len = 0;
    pthread_mutex_unlock(&c->command_string_mutex);
    log_for_client((void *) c, AFPFSD, LOG_DEBUG,
                   "******* processing command %d", req->command);
    pthread_mutex_lock(&server_op_mutex);

    switch (req->command) {
    case AFP_SERVER_COMMAND_SERVERINFO:
        ret = process_serverinfo(c);
        break;

    case AFP_SERVER_COMMAND_CONNECT:
        ret = process_connect(c);
        break;

    case AFP_SERVER_COMMAND_ATTACH:
        ret = process_attach(c);
        break;

    case AFP_SERVER_COMMAND_DETACH:
        ret = process_detach(c);
        break;

    case AFP_SERVER_COMMAND_PING:
        ret = process_ping(c);
        break;

    case AFP_SERVER_COMMAND_GETVOLID:
        ret = process_getvolid(c);
        break;

    case AFP_SERVER_COMMAND_READDIR:
        ret = process_readdir(c);
        break;

    case AFP_SERVER_COMMAND_GETVOLS:
        ret = process_getvols(c);
        break;

    case AFP_SERVER_COMMAND_STAT:
        ret = process_stat(c);
        break;

    case AFP_SERVER_COMMAND_OPEN:
        ret = process_open(c);
        break;

    case AFP_SERVER_COMMAND_READ:
        ret = process_read(c);
        break;

    case AFP_SERVER_COMMAND_WRITE:
        ret = process_write(c);
        break;

    case AFP_SERVER_COMMAND_CREAT:
        ret = process_creat(c);
        break;

    case AFP_SERVER_COMMAND_CHMOD:
        ret = process_chmod(c);
        break;

    case AFP_SERVER_COMMAND_RENAME:
        ret = process_rename(c);
        break;

    case AFP_SERVER_COMMAND_UNLINK:
        ret = process_unlink(c);
        break;

    case AFP_SERVER_COMMAND_TRUNCATE:
        ret = process_truncate(c);
        break;

    case AFP_SERVER_COMMAND_UTIME:
        ret = process_utime(c);
        break;

    case AFP_SERVER_COMMAND_MKDIR:
        ret = process_mkdir(c);
        break;

    case AFP_SERVER_COMMAND_RMDIR:
        ret = process_rmdir(c);
        break;

    case AFP_SERVER_COMMAND_STATFS:
        ret = process_statfs(c);
        break;

    case AFP_SERVER_COMMAND_CLOSE:
        ret = process_close(c);
        break;

    case AFP_SERVER_COMMAND_EXIT:
        ret = process_exit(c);
        break;

    case AFP_SERVER_COMMAND_STATUS:
        ret = process_status(c);
        break;

    case AFP_SERVER_COMMAND_DISCONNECT:
        ret = process_disconnect(c);
        break;

    case AFP_SERVER_COMMAND_CHANGEPW:
        ret = process_changepw(c);
        break;

    case AFP_SERVER_COMMAND_GETXATTR:
    case AFP_SERVER_COMMAND_SETXATTR:
    case AFP_SERVER_COMMAND_LISTXATTR:
    case AFP_SERVER_COMMAND_REMOVEXATTR:
    case AFP_SERVER_COMMAND_GETFINDERINFO:
    case AFP_SERVER_COMMAND_SETFINDERINFO:
    case AFP_SERVER_COMMAND_REMOVEFINDERINFO:
    case AFP_SERVER_COMMAND_GETRESOURCEFORK:
    case AFP_SERVER_COMMAND_SETRESOURCEFORK:
    case AFP_SERVER_COMMAND_TRUNCATERESOURCEFORK:
    case AFP_SERVER_COMMAND_REMOVERESOURCEFORK:
        ret = process_metadata(c);
        break;

    case AFP_SERVER_COMMAND_MOUNT:
    case AFP_SERVER_COMMAND_UNMOUNT:
    case AFP_SERVER_COMMAND_GET_MOUNTPOINT:
    case AFP_SERVER_COMMAND_SUSPEND:
    case AFP_SERVER_COMMAND_RESUME:
        log_for_client((void *)c, AFPFSD, LOG_ERR,
                       "Command %d not supported by afpsld (stateless daemon). "
                       "Use afpfsd (FUSE daemon) instead.", req->command);
        ret = AFP_SERVER_RESULT_NOTSUPPORTED;
        break;

    default:
        log_for_client((void *)c, AFPFSD, LOG_ERR, "Unknown command %d", req->command);
        ret = AFP_SERVER_RESULT_ERROR;
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

int process_command(struct daemon_client * c)
{
    ssize_t ret;
    const struct afp_server_request_header * header;
    pthread_attr_t attr;

    if (c->incoming_size == 0) {
        /* We're at the start of the packet */
        c->a = c->incoming_string;
        ret = read(c->fd, c->incoming_string,
                   sizeof(struct afp_server_request_header));

        if (ret == 0) {
            return -1;
        }

        if (ret < 0) {
            perror("error reading command");
            return -1;
        }

        c->incoming_size += (int) ret;
        c->a += (size_t) ret;

        if ((size_t)ret < sizeof(struct afp_server_request_header)) {
            /* incomplete header, continue to read */
            return 2;
        }

        header = (const struct afp_server_request_header *) c->incoming_string;

        if (header->len < sizeof(*header)
                || header->len > sizeof(c->incoming_string)) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Invalid command length %u", header->len);
            return -1;
        }

        if ((unsigned int)c->incoming_size == header->len) {
            goto havefullone;
        }

        /* incomplete header, continue to read */
        return 2;
    }

    /* Okay, we're continuing to read */
    ret = read(c->fd, c->a, AFP_CLIENT_INCOMING_BUF - c->incoming_size);

    if (ret <= 0) {
        perror("reading command 2");
        return -1;
    }

    c->a += (size_t) ret;
    c->incoming_size += (int) ret;

    if ((size_t)c->incoming_size < sizeof(*header)) {
        /* incomplete header, continue to read */
        return 0;
    }

    header = (const struct afp_server_request_header *) c->incoming_string;

    if (header->len < sizeof(*header)
            || header->len > sizeof(c->incoming_string)) {
        log_for_client((void *) c, AFPFSD, LOG_ERR,
                       "Invalid command length %u", header->len);
        return -1;
    }

    if ((unsigned int)c->incoming_size < header->len) {
        return 0;
    }

havefullone:
    /* Okay, so we have a full one.  Copy the buffer. */
    header = (const struct afp_server_request_header *) c->incoming_string;
    /* do the copy */
    c->completed_packet_size = header->len;
    memcpy(c->complete_packet, c->incoming_string, c->completed_packet_size);
    /* shift things back */
    c->a -= c->completed_packet_size;
    memmove(c->incoming_string, c->incoming_string + c->completed_packet_size,
            c->incoming_size - c->completed_packet_size);
    c->incoming_size -= c->completed_packet_size;
    memset(c->incoming_string + c->incoming_size, 0,
           AFP_CLIENT_INCOMING_BUF - c->incoming_size);
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

/* command_sub_attach_volume()
 *
 * Attaches to a volume and returns a created volume structure.
 *
 * Returns:
 * NULL if it could not attach
 *
 * Sets response_result to:
 *
 * AFP_SERVER_RESULT_OKAY:
 * 	Attached properly
 * AFP_SERVER_RESULT_NOVOLUME:
 * 	No volume exists by that name
 * AFP_SERVER_RESULT_ALREADY_ATTACHED:
 * 	Volume's AFP session is already active
 * AFP_SERVER_RESULT_VOLPASS_NEEDED:
 * 	A volume password is needed
 * AFP_SERVER_RESULT_ERROR_UNKNOWN:
 * 	An unknown error occured when attaching.
 *
 */

struct afp_volume *command_sub_attach_volume(struct daemon_client *c,
        struct afp_server *server,
        char *volname,
        char *volpassword,
        int *response_result)
{
    struct afp_volume *using_volume;

    if (response_result) {
        *response_result = AFP_SERVER_RESULT_OKAY;
    }

    using_volume = find_volume_by_name(server, volname);

    if (!using_volume) {
        if (volname && volname[0]) {
            log_for_client((void *) c, AFPFSD, LOG_ERR,
                           "Volume %s does not exist on server %s.", volname,
                           server->basic.server_name_printable);
        }

        if (response_result) {
            *response_result = AFP_SERVER_RESULT_NOVOLUME;
        }

        if (server->num_volumes) {
            char names[1024];
            afp_list_volnames(server, names, 1024);
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Choose a volume from: %s",
                           names);
        } else {
            log_for_client((void *)c, AFPFSD, LOG_ERR,
                           "No volumes available on server %s.",
                           server->basic.server_name_printable);
        }

        goto error;
    }

    if (using_volume->attached == AFP_VOLUME_ATTACHED) {
        log_for_client((void *)c, AFPFSD, LOG_DEBUG,
                       "Volume %s is already attached", volname);

        if (response_result) {
            *response_result = AFP_SERVER_RESULT_ALREADY_ATTACHED;
        }

        goto error;
    }

    if (using_volume->flags & HasPassword) {
        if (!volpassword || volpassword[0] == '\0') {
            log_for_client((void *) c, AFPFSD, LOG_ERR, "Volume password needed");

            if (response_result) {
                *response_result = AFP_SERVER_RESULT_VOLPASS_NEEDED;
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
            *response_result = AFP_SERVER_RESULT_ERROR_UNKNOWN;
        }

        goto error;
    }

    afp_detect_mapping(using_volume);
    log_for_client((void *) c, AFPFSD, LOG_DEBUG, "Volume mapping detected: %d",
                   using_volume->mapping);
    return using_volume;
error:
    return NULL;
}
