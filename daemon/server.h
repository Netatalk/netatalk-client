#ifndef _AFP_SERVER_H_
#define _AFP_SERVER_H_

#include <limits.h>

#include "netatalk-client/afpsl.h"
#include "lib/afp_protocol.h"

#include "ipc.h"

struct afpsl_ipc_response_header {
    char result;
    unsigned int len;
};

struct afpsl_ipc_request_header {
    char command;
    unsigned int len;
    unsigned int close;
};

#define AFPSL_IPC_CONNECT_RESUME_EXISTING 0x1U

struct afpsl_ipc_attach_request {
    struct afpsl_ipc_request_header header;
    afpc_server_t serverid;
    struct afpc_url url;
    unsigned int volume_options;
};

struct afpsl_ipc_attach_response {
    struct afpsl_ipc_response_header header;
    afpc_volume_t volumeid;
};

struct afpsl_ipc_detach_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
};

struct afpsl_ipc_detach_response {
    struct afpsl_ipc_response_header header;
    char detach_message[1024];
};

struct afpsl_ipc_connect_request {
    struct afpsl_ipc_request_header header;
    struct afpc_url url;
    unsigned int uam_mask;
    unsigned int flags;
};

struct afpsl_ipc_connect_response {
    struct afpsl_ipc_response_header header;
    afpc_server_t serverid;
    char loginmesg[AFP_LOGINMESG_LEN];
    int connect_error;
};

struct afpsl_ipc_disconnect_request {
    struct afpsl_ipc_request_header header;
    afpc_server_t serverid;
};

struct afpsl_ipc_disconnect_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_getvolid_request {
    struct afpsl_ipc_request_header header;
    afpc_server_t serverid;
    struct afpc_url url;
};

struct afpsl_ipc_getvolid_response {
    struct afpsl_ipc_response_header header;
    afpc_volume_t volumeid;
};

struct afpsl_ipc_readdir_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    int start;
    int count;
};

struct afpsl_ipc_readdir_response {
    struct afpsl_ipc_response_header header;
    unsigned int numfiles;
    char eod;
};

struct afpsl_ipc_getvols_request {
    struct afpsl_ipc_request_header header;
    afpc_server_t serverid;
    struct afpc_url url;
    int start;
    int count;
};

struct afpsl_ipc_getvols_response {
    struct afpsl_ipc_response_header header;
    unsigned int num;
    char endlist;
};

struct afpsl_ipc_stat_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afpsl_ipc_stat_response {
    struct afpsl_ipc_response_header header;
    struct stat stat;
};

struct afpsl_ipc_open_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    int mode;
};

struct afpsl_ipc_open_response {
    struct afpsl_ipc_response_header header;
    unsigned int fileid;
};

struct afpsl_ipc_read_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    unsigned int fileid;
    unsigned long long start;
    unsigned int length;
    unsigned int resource;
};

struct afpsl_ipc_read_response {
    struct afpsl_ipc_response_header header;
    unsigned int received;
    unsigned int eof;
};

struct afpsl_ipc_write_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    unsigned int fileid;
    unsigned long long offset;
    unsigned int size;
    unsigned int resource;
};

struct afpsl_ipc_write_response {
    struct afpsl_ipc_response_header header;
    unsigned int written;
};

struct afpsl_ipc_close_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    unsigned int fileid;
};

struct afpsl_ipc_close_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_creat_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afpsl_ipc_creat_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_chmod_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afpsl_ipc_chmod_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_rename_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path_from[AFP_MAX_PATH];
    char path_to[AFP_MAX_PATH];
};

struct afpsl_ipc_rename_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_unlink_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afpsl_ipc_unlink_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_truncate_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    unsigned long long offset;
};

struct afpsl_ipc_truncate_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_mkdir_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afpsl_ipc_mkdir_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_rmdir_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afpsl_ipc_rmdir_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_statfs_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afpsl_ipc_statfs_response {
    struct afpsl_ipc_response_header header;
    struct statvfs stat;
};

struct afpsl_ipc_utime_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    struct utimbuf times;
};

struct afpsl_ipc_utime_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_serverinfo_request {
    struct afpsl_ipc_request_header header;
    struct afpc_url url;
};

struct afpsl_ipc_serverinfo_response {
    struct afpsl_ipc_response_header header;
    struct afpc_server_info server_basic;
};

struct afpsl_ipc_status_request {
    struct afpsl_ipc_request_header header;
    char volumename[AFP_VOLUME_NAME_UTF8_LEN];
    char servername[AFP_SERVER_NAME_LEN];
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afpsl_ipc_status_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_exit_request {
    struct afpsl_ipc_request_header header;
};

struct afpsl_ipc_changepw_request {
    struct afpsl_ipc_request_header header;
    struct afpc_url url;
    char oldpasswd[AFP_MAX_PASSWORD_LEN];
    char newpasswd[AFP_MAX_PASSWORD_LEN];
};

struct afpsl_ipc_changepw_response {
    struct afpsl_ipc_response_header header;
    int afp_error;
};

struct afpsl_ipc_metadata_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    char path[AFP_MAX_PATH];
    char name[256];
    unsigned long long offset;
    unsigned int size;
    int flags;
    char data[];
};

struct afpsl_ipc_metadata_response {
    struct afpsl_ipc_response_header header;
    int error;
    unsigned int size;
    char data[];
};

#endif
