#ifndef NETATALK_CLIENT_FUSE_IPC_H
#define NETATALK_CLIENT_FUSE_IPC_H

#include <limits.h>

#include "netatalk-client/types.h"

#define AFPFSD_IPC_SOCKET_PATH "/tmp/afp_server"

#define AFPFSD_IPC_MAX_RESPONSE 16384
#define AFPFSD_IPC_MOUNTPOINT_LEN 255
#define AFPFSD_IPC_FUSE_OPTIONS_LEN 256

/* FUSE manager daemon command codes. */
#define AFPFSD_IPC_COMMAND_MOUNT 1
#define AFPFSD_IPC_COMMAND_STATUS 4
#define AFPFSD_IPC_COMMAND_UNMOUNT 6
#define AFPFSD_IPC_COMMAND_SUSPEND 8
#define AFPFSD_IPC_COMMAND_RESUME 9
#define AFPFSD_IPC_COMMAND_PING 11
#define AFPFSD_IPC_COMMAND_EXIT 12
#define AFPFSD_IPC_COMMAND_SPAWN_MOUNT 100

/* FUSE manager daemon result codes. */
#define AFPFSD_IPC_RESULT_OK 0
#define AFPFSD_IPC_RESULT_ERROR 1

struct afpfsd_ipc_resume_request {
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
};

struct afpfsd_ipc_suspend_request {
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
};

struct afpfsd_ipc_unmount_request {
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
};

struct afpfsd_ipc_mount_request {
    struct afpc_url url;
    unsigned int uam_mask;
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
    unsigned int volume_options;
    unsigned int map;
    int changeuid;
    char fuse_options[AFPFSD_IPC_FUSE_OPTIONS_LEN];
};

struct afpfsd_ipc_status_request {
    char volumename[AFPC_VOLUME_NAME_UTF8_LEN];
    char servername[AFPC_SERVER_NAME_UTF8_LEN];
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
};

struct afpfsd_ipc_spawn_mount_request {
    char mountpoint[AFPFSD_IPC_MOUNTPOINT_LEN];
    char socket_id[PATH_MAX];
    char volumename[AFPC_VOLUME_NAME_UTF8_LEN];
};

struct afpfsd_ipc_response {
    char result;
    unsigned int len;
};

#endif
