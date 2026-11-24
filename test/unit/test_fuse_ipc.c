/*
 * test_fuse_ipc.c: the checks of a mount daemon's commands
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of the check of a mount daemon's whole commands
 */

#include <limits.h>
#include <string.h>

#include "fuse/fuse_ipc.h"
#include "lib/mapping.h"
#include "tap.h"

static char command[1 + sizeof(struct afpfsd_ipc_mount_request)];
_Static_assert(sizeof(struct afpfsd_ipc_status_request)
               < sizeof(struct afpfsd_ipc_mount_request),
               "a mount's request is the longest");

struct field {
    char *text;
    size_t size;
};

/*!
 * @brief Check a command and its request
 *
 * @param[in] code     the command byte
 * @param[in] request  the request
 * @param[in] size     its size
 * @returns what afpfsd_ipc_command_valid() says of the command
 */
static int valid(unsigned char code, const void *request, size_t size)
{
    memset(command, 0, sizeof(command));
    command[0] = (char) code;
    memcpy(command + 1, request, size);
    return afpfsd_ipc_command_valid(command);
}

/*!
 * @brief Check a command once for each text field of its request, that
 *        field filled to its size without a terminator
 *
 * The rest of the request is zeros, so each other field ends.
 *
 * @param[in]     code     the command byte
 * @param[in,out] request  the request, which each check fills anew
 * @param[in]     size     its size
 * @param[in]     fields   its text fields
 * @param[in]     count    how many
 * @returns 0 when the check refuses each, or the number of the first it
 *          passes, counting from 1
 */
static int refuses_each_filled(unsigned char code, void *request, size_t size,
                               const struct field *fields, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        memset(request, 0, size);
        memset(fields[i].text, 'x', fields[i].size);

        if (valid(code, request, size)) {
            return (int) i + 1;
        }
    }

    return 0;
}

/*!
 * @brief A command whose text fields each end within their sizes passes
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_whole_commands_pass(void)
{
    struct afpfsd_ipc_mount_request mount;
    struct afpfsd_ipc_status_request status;
    struct afpfsd_ipc_unmount_request unmount;
    struct afpfsd_ipc_suspend_request suspend;
    struct afpfsd_ipc_resume_request resume;
    int ret = 0;
    memset(&mount, 0, sizeof(mount));
    memset(&status, 0, sizeof(status));
    memset(&unmount, 0, sizeof(unmount));
    memset(&suspend, 0, sizeof(suspend));
    memset(&resume, 0, sizeof(resume));
    mount.map = AFP_MAPPING_NAME;

    if (!valid(AFPFSD_IPC_COMMAND_MOUNT, &mount, sizeof(mount))) {
        ret = 1;
    } else if (!valid(AFPFSD_IPC_COMMAND_STATUS, &status, sizeof(status))) {
        ret = 2;
    } else if (!valid(AFPFSD_IPC_COMMAND_UNMOUNT, &unmount,
                      sizeof(unmount))) {
        ret = 3;
    } else if (!valid(AFPFSD_IPC_COMMAND_SUSPEND, &suspend,
                      sizeof(suspend))) {
        ret = 4;
    } else if (!valid(AFPFSD_IPC_COMMAND_RESUME, &resume, sizeof(resume))) {
        ret = 5;
    } else if (!valid(AFPFSD_IPC_COMMAND_PING, "", 0)) {
        ret = 6;
    }

    return ret;
}

/*!
 * @brief A mount whose text field fills its size is refused
 *
 * @returns 0, or the number of the field the check passed, counting from 1
 */
static int utest_mount_fields_must_end(void)
{
    struct afpfsd_ipc_mount_request r;
    const struct field fields[] = {
        { r.mountpoint, sizeof(r.mountpoint) },
        { r.fuse_options, sizeof(r.fuse_options) },
        { r.url.username, sizeof(r.url.username) },
        { r.url.uamname, sizeof(r.url.uamname) },
        { r.url.password, sizeof(r.url.password) },
        { r.url.servername, sizeof(r.url.servername) },
        { r.url.volumename, sizeof(r.url.volumename) },
        { r.url.zone, sizeof(r.url.zone) },
        { r.url.volpassword, sizeof(r.url.volpassword) },
    };
    return refuses_each_filled(AFPFSD_IPC_COMMAND_MOUNT, &r, sizeof(r),
                               fields, sizeof(fields) / sizeof(fields[0]));
}

/*!
 * @brief A mount whose mapping is past AFP_MAPPING_* is refused
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unknown_mapping_is_refused(void)
{
    struct afpfsd_ipc_mount_request mount;
    int ret = 0;
    memset(&mount, 0, sizeof(mount));
    mount.map = AFP_MAPPING_NAME + 1;

    if (valid(AFPFSD_IPC_COMMAND_MOUNT, &mount, sizeof(mount))) {
        ret = 1;
    } else {
        mount.map = UINT_MAX;
        ret = valid(AFPFSD_IPC_COMMAND_MOUNT, &mount, sizeof(mount)) ? 2 : 0;
    }

    return ret;
}

/*!
 * @brief A status whose text field fills its size is refused
 *
 * @returns 0, or the number of the field the check passed, counting from 1
 */
static int utest_status_fields_must_end(void)
{
    struct afpfsd_ipc_status_request r;
    const struct field fields[] = {
        { r.volumename, sizeof(r.volumename) },
        { r.servername, sizeof(r.servername) },
        { r.mountpoint, sizeof(r.mountpoint) },
    };
    return refuses_each_filled(AFPFSD_IPC_COMMAND_STATUS, &r, sizeof(r),
                               fields, sizeof(fields) / sizeof(fields[0]));
}

/*!
 * @brief An unmount, a suspend and a resume each need their mount point's
 *        terminator
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_mount_point_commands_must_end(void)
{
    struct afpfsd_ipc_unmount_request unmount;
    struct afpfsd_ipc_suspend_request suspend;
    struct afpfsd_ipc_resume_request resume;
    const struct field unmount_field = {
        unmount.mountpoint, sizeof(unmount.mountpoint)
    };
    const struct field suspend_field = {
        suspend.mountpoint, sizeof(suspend.mountpoint)
    };
    const struct field resume_field = {
        resume.mountpoint, sizeof(resume.mountpoint)
    };
    int ret = 0;

    if (refuses_each_filled(AFPFSD_IPC_COMMAND_UNMOUNT, &unmount,
                            sizeof(unmount), &unmount_field, 1) != 0) {
        ret = 1;
    } else if (refuses_each_filled(AFPFSD_IPC_COMMAND_SUSPEND, &suspend,
                                   sizeof(suspend), &suspend_field, 1) != 0) {
        ret = 2;
    } else if (refuses_each_filled(AFPFSD_IPC_COMMAND_RESUME, &resume,
                                   sizeof(resume), &resume_field, 1) != 0) {
        ret = 3;
    }

    return ret;
}

/*!
 * @brief Run the command checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    TEST_int(utest_whole_commands_pass(), 0,
             "a command whose text fields end within their sizes passes");
    TEST_int(utest_mount_fields_must_end(), 0,
             "a mount whose text field fills its size is refused");
    TEST_int(utest_unknown_mapping_is_refused(), 0,
             "a mount of an unknown mapping is refused");
    TEST_int(utest_status_fields_must_end(), 0,
             "a status whose text field fills its size is refused");
    TEST_int(utest_mount_point_commands_must_end(), 0,
             "an unmount, suspend or resume whose mount point fills its "
             "field is refused");
    return test_tap_finish();
}
