#ifndef NETATALK_CLIENT_DAEMON_STATELESS_IPC_H
#define NETATALK_CLIENT_DAEMON_STATELESS_IPC_H

#include <limits.h>
#include <stdint.h>

#include "netatalk-client/afpsl.h"
#include "lib/afp_protocol.h"

/* Desktop icons may use the AFP uint16_t maximum, so the framing limit must
 * accommodate one raw icon plus the fixed response and log trailer. */
#define AFPSL_IPC_MAX_RESPONSE (UINT16_MAX + 1024U)
#define AFPSL_IPC_PROTOCOL_MAGIC UINT32_C(0x41465053)
#define AFPSL_IPC_PROTOCOL_MAJOR 2U
#define AFPSL_IPC_PROTOCOL_MINOR 2U

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
#define AFPSL_IPC_COMMAND_HELLO 50
#define AFPSL_IPC_COMMAND_DESKTOP_GET_COMMENT 51
#define AFPSL_IPC_COMMAND_DESKTOP_GET_ICON_INFO 52
#define AFPSL_IPC_COMMAND_DESKTOP_GET_ICON 53
#define AFPSL_IPC_COMMAND_DESKTOP_GET_APPL 54
#define AFPSL_IPC_COMMAND_DESKTOP_SET_COMMENT 55

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

struct afpsl_ipc_hello_request {
    struct afpsl_ipc_request_header header;
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t features;
};

struct afpsl_ipc_hello_response {
    struct afpsl_ipc_response_header header;
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t features;
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
    int start;
    int count;
    uint32_t path_len;
    char path[];
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
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_stat_response {
    struct afpsl_ipc_response_header header;
    struct stat stat;
};

struct afpsl_ipc_open_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    int mode;
    uint32_t path_len;
    char path[];
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
    mode_t mode;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_creat_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_chmod_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    mode_t mode;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_chmod_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_rename_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_from_len;
    uint32_t path_to_len;
    char paths[];
};

struct afpsl_ipc_rename_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_unlink_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_unlink_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_truncate_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    unsigned long long offset;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_truncate_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_mkdir_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    mode_t mode;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_mkdir_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_rmdir_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_rmdir_response {
    struct afpsl_ipc_response_header header;
};

struct afpsl_ipc_statfs_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_statfs_response {
    struct afpsl_ipc_response_header header;
    struct statvfs stat;
};

struct afpsl_ipc_utime_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    struct utimbuf times;
    uint32_t path_len;
    char path[];
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
    unsigned long long offset;
    unsigned int size;
    int flags;
    uint32_t path_len;
    uint32_t name_len;
    char data[];
};

struct afpsl_ipc_metadata_response {
    struct afpsl_ipc_response_header header;
    int error;
    unsigned int size;
    char data[];
};

struct afpsl_ipc_desktop_path_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_len;
    char path[];
};

struct afpsl_ipc_desktop_set_comment_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t path_len;
    uint32_t text_len;
    char data[]; /* NUL-terminated path followed by text_len raw bytes. */
};

struct afpsl_ipc_desktop_code_request {
    struct afpsl_ipc_request_header header;
    afpc_volume_t volumeid;
    uint32_t creator;
    uint32_t type;
    uint16_t index;
    uint8_t icon_type;
    uint8_t pad;
    uint32_t size;
};

struct afpsl_ipc_desktop_data_response {
    struct afpsl_ipc_response_header header;
    int error;
    uint32_t size;
    char data[];
};

struct afpsl_ipc_desktop_set_comment_response {
    struct afpsl_ipc_response_header header;
    int error;
    uint32_t written;
    uint8_t truncated;
    uint8_t pad[3];
};

struct afpsl_ipc_desktop_icon_info_response {
    struct afpsl_ipc_response_header header;
    int error;
    uint32_t tag;
    uint32_t type;
    uint16_t size;
    uint8_t icon_type;
    uint8_t pad;
};

struct afpsl_ipc_desktop_appl_response {
    struct afpsl_ipc_response_header header;
    int error;
    uint16_t file_bitmap;
    uint16_t pad;
    uint32_t tag;
    uint32_t directory_id;
    uint32_t file_id;
    int64_t creation_date;
    int64_t modification_date;
    uint64_t data_fork_size;
    uint64_t resource_fork_size;
    char pathname[AFPC_MAX_NAME_BYTES];
};

#endif
