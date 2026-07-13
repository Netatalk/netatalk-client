#ifndef NETATALK_CLIENT_DAEMON_STATELESS_IPC_H
#define NETATALK_CLIENT_DAEMON_STATELESS_IPC_H

#include <limits.h>
#include <stdint.h>

#include "netatalk-client/afpsl.h"
#include "lib/afp_protocol.h"

#define AFPSL_IPC_MAX_RESPONSE 16384

/* Unix socket shared by afpsld and libafpsl. */
#define AFPSL_IPC_SOCKET_PATH "/tmp/afp_sl"

/* Structured logging trailer appended to stateless daemon responses. */
#define AFPSL_IPC_LOG_MAGIC UINT32_C(0x4146504c)
#define AFPSL_IPC_LOG_BUFFER_SIZE 4096

struct afpsl_ipc_log_record {
    int32_t level;
    uint32_t message_len;
};

struct afpsl_ipc_log_footer {
    uint32_t magic;
    uint32_t log_len;
};

/* Stateless IPC command codes. */
#define AFPSL_IPC_COMMAND_ATTACH 2
#define AFPSL_IPC_COMMAND_DETACH 3
#define AFPSL_IPC_COMMAND_STATUS 4
#define AFPSL_IPC_COMMAND_EXIT 12
#define AFPSL_IPC_COMMAND_CONNECT 14
#define AFPSL_IPC_COMMAND_GETVOLID 16
#define AFPSL_IPC_COMMAND_READDIR 19
#define AFPSL_IPC_COMMAND_GETVOLS 20
#define AFPSL_IPC_COMMAND_STAT 21
#define AFPSL_IPC_COMMAND_OPEN 22
#define AFPSL_IPC_COMMAND_READ 23
#define AFPSL_IPC_COMMAND_CLOSE 24
#define AFPSL_IPC_COMMAND_SERVERINFO 25
#define AFPSL_IPC_COMMAND_GET_MOUNTPOINT 26
#define AFPSL_IPC_COMMAND_WRITE 27
#define AFPSL_IPC_COMMAND_CREAT 28
#define AFPSL_IPC_COMMAND_CHMOD 29
#define AFPSL_IPC_COMMAND_RENAME 30
#define AFPSL_IPC_COMMAND_UNLINK 31
#define AFPSL_IPC_COMMAND_TRUNCATE 32
#define AFPSL_IPC_COMMAND_MKDIR 33
#define AFPSL_IPC_COMMAND_RMDIR 34
#define AFPSL_IPC_COMMAND_STATFS 35
#define AFPSL_IPC_COMMAND_UTIME 36
#define AFPSL_IPC_COMMAND_DISCONNECT 37
#define AFPSL_IPC_COMMAND_CHANGEPW 38
#define AFPSL_IPC_COMMAND_GETXATTR 39
#define AFPSL_IPC_COMMAND_SETXATTR 40
#define AFPSL_IPC_COMMAND_LISTXATTR 41
#define AFPSL_IPC_COMMAND_REMOVEXATTR 42
#define AFPSL_IPC_COMMAND_GETFINDERINFO 43
#define AFPSL_IPC_COMMAND_SETFINDERINFO 44
#define AFPSL_IPC_COMMAND_REMOVEFINDERINFO 45
#define AFPSL_IPC_COMMAND_GETRESOURCEFORK 46
#define AFPSL_IPC_COMMAND_SETRESOURCEFORK 47
#define AFPSL_IPC_COMMAND_REMOVERESOURCEFORK 48
#define AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK 49

/* Stateless IPC result codes. */
#define AFPSL_IPC_RESULT_OK 0
#define AFPSL_IPC_RESULT_ERROR 1
#define AFPSL_IPC_RESULT_TRYING 2
#define AFPSL_IPC_RESULT_WARNING 3
#define AFPSL_IPC_RESULT_ENOENT 4
#define AFPSL_IPC_RESULT_NOTCONNECTED 5
#define AFPSL_IPC_RESULT_NOTATTACHED 6
#define AFPSL_IPC_RESULT_ALREADY_CONNECTED 7
#define AFPSL_IPC_RESULT_ALREADY_ATTACHED 8
#define AFPSL_IPC_RESULT_NOAUTHENT 9
#define AFPSL_IPC_RESULT_ERROR_UNKNOWN 10
#define AFPSL_IPC_RESULT_NOVOLUME 14
#define AFPSL_IPC_RESULT_ALREADY_MOUNTED 15
#define AFPSL_IPC_RESULT_VOLPASS_NEEDED 16
#define AFPSL_IPC_RESULT_MOUNTPOINT_NOEXIST 17
#define AFPSL_IPC_RESULT_NOSERVER 18
#define AFPSL_IPC_RESULT_MOUNTPOINT_PERM 19
#define AFPSL_IPC_RESULT_TIMEDOUT 20
#define AFPSL_IPC_RESULT_DAEMON_ERROR 21
#define AFPSL_IPC_RESULT_NOTSUPPORTED 22
#define AFPSL_IPC_RESULT_ACCESS 23
#define AFPSL_IPC_RESULT_EXIST 24
#define AFPSL_IPC_RESULT_ENOTEMPTY 25

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
