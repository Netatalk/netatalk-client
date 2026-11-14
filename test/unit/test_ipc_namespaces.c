/*
 * test_ipc_namespaces.c: the commands, results and request layouts of the
 * two IPC protocols, checked by the compiler
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Compile-time checks of the IPC protocols' commands and layouts
 */

#include <stddef.h>

#include "fuse/fuse_ipc.h"
#include "daemon/stateless_ipc.h"

#ifdef AFP_SERVER_COMMAND_MOUNT
#error "legacy shared IPC command namespace is still visible"
#endif

#ifdef AFP_SERVER_RESULT_OKAY
#error "legacy shared IPC result namespace is still visible"
#endif

#ifdef AFPSL_IPC_COMMAND_MOUNT
#error "stateless IPC must not define FUSE mount commands"
#endif

#ifdef AFPSL_IPC_COMMAND_UNMOUNT
#error "stateless IPC must not define FUSE unmount commands"
#endif

#ifdef AFPSL_IPC_COMMAND_SUSPEND
#error "stateless IPC must not define FUSE suspend commands"
#endif

#ifdef AFPSL_IPC_COMMAND_RESUME
#error "stateless IPC must not define FUSE resume commands"
#endif

struct legacy_response_header {
    char result;
    unsigned int len;
};

struct legacy_request_header {
    char command;
    unsigned int len;
    unsigned int close;
};

struct legacy_fuse_mount_request {
    struct afpc_url url;
    unsigned int uam_mask;
    char mountpoint[255];
    unsigned int volume_options;
    unsigned int map;
    int changeuid;
    char fuse_options[256];
};

#define ASSERT_SAME_LAYOUT(current, legacy) \
    _Static_assert(sizeof(current) == sizeof(legacy), "wire size changed"); \
    _Static_assert(_Alignof(current) == _Alignof(legacy), "wire alignment changed")

ASSERT_SAME_LAYOUT(struct afpfsd_ipc_response,
                   struct legacy_response_header);
ASSERT_SAME_LAYOUT(struct afpsl_ipc_response_header,
                   struct legacy_response_header);
ASSERT_SAME_LAYOUT(struct afpsl_ipc_request_header,
                   struct legacy_request_header);
ASSERT_SAME_LAYOUT(struct afpfsd_ipc_mount_request,
                   struct legacy_fuse_mount_request);

_Static_assert(offsetof(struct afpfsd_ipc_response, len)
               == offsetof(struct legacy_response_header, len),
               "FUSE response field offset changed");
_Static_assert(offsetof(struct afpsl_ipc_request_header, close)
               == offsetof(struct legacy_request_header, close),
               "stateless request field offset changed");
_Static_assert(offsetof(struct afpfsd_ipc_mount_request, fuse_options)
               == offsetof(struct legacy_fuse_mount_request, fuse_options),
               "FUSE mount field offset changed");

_Static_assert(AFPFSD_IPC_COMMAND_MOUNT == 1, "FUSE mount command changed");
_Static_assert(AFPFSD_IPC_COMMAND_STATUS == 4, "FUSE status command changed");
_Static_assert(AFPFSD_IPC_COMMAND_UNMOUNT == 6, "FUSE unmount command changed");
_Static_assert(AFPFSD_IPC_COMMAND_SUSPEND == 8, "FUSE suspend command changed");
_Static_assert(AFPFSD_IPC_COMMAND_RESUME == 9, "FUSE resume command changed");
_Static_assert(AFPFSD_IPC_COMMAND_PING == 11, "FUSE ping command changed");
_Static_assert(AFPFSD_IPC_COMMAND_EXIT == 12, "FUSE exit command changed");
_Static_assert(AFPFSD_IPC_COMMAND_SPAWN_MOUNT == 100,
               "FUSE spawn command changed");
_Static_assert(AFPFSD_IPC_RESULT_OK == 0, "FUSE success result changed");
_Static_assert(AFPFSD_IPC_RESULT_ERROR == 1, "FUSE error result changed");

#define ASSERT_VALUE(name, value) \
    _Static_assert(name == value, #name " wire value changed")

ASSERT_VALUE(AFPSL_IPC_COMMAND_ATTACH, 2);
ASSERT_VALUE(AFPSL_IPC_COMMAND_DETACH, 3);
ASSERT_VALUE(AFPSL_IPC_COMMAND_STATUS, 4);
ASSERT_VALUE(AFPSL_IPC_COMMAND_EXIT, 12);
ASSERT_VALUE(AFPSL_IPC_COMMAND_CONNECT, 14);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GETVOLID, 16);
ASSERT_VALUE(AFPSL_IPC_COMMAND_READDIR, 19);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GETVOLS, 20);
ASSERT_VALUE(AFPSL_IPC_COMMAND_STAT, 21);
ASSERT_VALUE(AFPSL_IPC_COMMAND_OPEN, 22);
ASSERT_VALUE(AFPSL_IPC_COMMAND_READ, 23);
ASSERT_VALUE(AFPSL_IPC_COMMAND_CLOSE, 24);
ASSERT_VALUE(AFPSL_IPC_COMMAND_SERVERINFO, 25);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GET_MOUNTPOINT, 26);
ASSERT_VALUE(AFPSL_IPC_COMMAND_WRITE, 27);
ASSERT_VALUE(AFPSL_IPC_COMMAND_CREAT, 28);
ASSERT_VALUE(AFPSL_IPC_COMMAND_CHMOD, 29);
ASSERT_VALUE(AFPSL_IPC_COMMAND_RENAME, 30);
ASSERT_VALUE(AFPSL_IPC_COMMAND_UNLINK, 31);
ASSERT_VALUE(AFPSL_IPC_COMMAND_TRUNCATE, 32);
ASSERT_VALUE(AFPSL_IPC_COMMAND_MKDIR, 33);
ASSERT_VALUE(AFPSL_IPC_COMMAND_RMDIR, 34);
ASSERT_VALUE(AFPSL_IPC_COMMAND_STATFS, 35);
ASSERT_VALUE(AFPSL_IPC_COMMAND_UTIME, 36);
ASSERT_VALUE(AFPSL_IPC_COMMAND_DISCONNECT, 37);
ASSERT_VALUE(AFPSL_IPC_COMMAND_CHANGEPW, 38);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GETXATTR, 39);
ASSERT_VALUE(AFPSL_IPC_COMMAND_SETXATTR, 40);
ASSERT_VALUE(AFPSL_IPC_COMMAND_LISTXATTR, 41);
ASSERT_VALUE(AFPSL_IPC_COMMAND_REMOVEXATTR, 42);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GETFINDERINFO, 43);
ASSERT_VALUE(AFPSL_IPC_COMMAND_SETFINDERINFO, 44);
ASSERT_VALUE(AFPSL_IPC_COMMAND_REMOVEFINDERINFO, 45);
ASSERT_VALUE(AFPSL_IPC_COMMAND_GETRESOURCEFORK, 46);
ASSERT_VALUE(AFPSL_IPC_COMMAND_SETRESOURCEFORK, 47);
ASSERT_VALUE(AFPSL_IPC_COMMAND_REMOVERESOURCEFORK, 48);
ASSERT_VALUE(AFPSL_IPC_COMMAND_TRUNCATERESOURCEFORK, 49);
ASSERT_VALUE(AFPSL_IPC_COMMAND_HELLO, 50);
ASSERT_VALUE(AFPSL_IPC_COMMAND_COPY, 56);

ASSERT_VALUE(AFPSL_IPC_RESULT_OK, 0);
ASSERT_VALUE(AFPSL_IPC_RESULT_ERROR, 1);
ASSERT_VALUE(AFPSL_IPC_RESULT_TRYING, 2);
ASSERT_VALUE(AFPSL_IPC_RESULT_WARNING, 3);
ASSERT_VALUE(AFPSL_IPC_RESULT_ENOENT, 4);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOTCONNECTED, 5);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOTATTACHED, 6);
ASSERT_VALUE(AFPSL_IPC_RESULT_ALREADY_CONNECTED, 7);
ASSERT_VALUE(AFPSL_IPC_RESULT_ALREADY_ATTACHED, 8);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOAUTHENT, 9);
ASSERT_VALUE(AFPSL_IPC_RESULT_ERROR_UNKNOWN, 10);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOVOLUME, 14);
ASSERT_VALUE(AFPSL_IPC_RESULT_ALREADY_MOUNTED, 15);
ASSERT_VALUE(AFPSL_IPC_RESULT_VOLPASS_NEEDED, 16);
ASSERT_VALUE(AFPSL_IPC_RESULT_MOUNTPOINT_NOEXIST, 17);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOSERVER, 18);
ASSERT_VALUE(AFPSL_IPC_RESULT_MOUNTPOINT_PERM, 19);
ASSERT_VALUE(AFPSL_IPC_RESULT_TIMEDOUT, 20);
ASSERT_VALUE(AFPSL_IPC_RESULT_DAEMON_ERROR, 21);
ASSERT_VALUE(AFPSL_IPC_RESULT_NOTSUPPORTED, 22);
ASSERT_VALUE(AFPSL_IPC_RESULT_ACCESS, 23);
ASSERT_VALUE(AFPSL_IPC_RESULT_EXIST, 24);
ASSERT_VALUE(AFPSL_IPC_RESULT_ENOTEMPTY, 25);
ASSERT_VALUE(AFPSL_IPC_RESULT_BUSY, 26);
ASSERT_VALUE(AFPSL_IPC_FEATURE_DENY_WRITE, 0x1);
ASSERT_VALUE(AFPSL_IPC_FEATURE_COPY, 0x2);

/* The daemon finds the server of a command in the leading field of its
 * request, whichever request it is. */
#define ASSERT_VOLUME_FIRST(type) \
    _Static_assert(offsetof(struct type, volumeid) \
                   == offsetof(struct afpsl_ipc_volume_request, volumeid), \
                   #type " has no volumeid where the daemon reads it")
#define ASSERT_SERVER_FIRST(type) \
    _Static_assert(offsetof(struct type, serverid) \
                   == offsetof(struct afpsl_ipc_server_request, serverid), \
                   #type " has no serverid where the daemon reads it")

ASSERT_VOLUME_FIRST(afpsl_ipc_detach_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_readdir_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_stat_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_open_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_read_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_write_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_close_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_creat_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_chmod_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_rename_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_unlink_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_truncate_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_mkdir_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_rmdir_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_statfs_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_utime_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_metadata_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_desktop_path_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_desktop_set_comment_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_desktop_code_request);
ASSERT_VOLUME_FIRST(afpsl_ipc_copy_request);

ASSERT_SERVER_FIRST(afpsl_ipc_attach_request);
ASSERT_SERVER_FIRST(afpsl_ipc_disconnect_request);
ASSERT_SERVER_FIRST(afpsl_ipc_getvolid_request);
ASSERT_SERVER_FIRST(afpsl_ipc_getvols_request);

int main(void)
{
    return 0;
}
