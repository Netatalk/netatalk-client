/*
    test_send_failures.c: requests the transport refuses

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/lowlevel.h"
#include "lib/midlevel.h"
#include "tap.h"

/*!
 * @brief Swallow a log message
 */
static void discard_log(void *priv, enum logtypes logtype, int loglevel,
                        const char *message)
{
    (void) priv;
    (void) logtype;
    (void) loglevel;
    (void) message;
}

/* The library's log lines would otherwise land in the TAP stream. */
static struct libafpclient quiet_client = {
    .log_for_client = discard_log,
};

static struct afp_versions afp34 = { "AFP3.4", 34 };

/* Whether getpwuid() finds no user, as for an ID the database lacks. */
static int passwd_lacks_the_user;

/*!
 * @brief Find the user of an ID, or none while passwd_lacks_the_user is set
 *
 * It takes the C library's place for the library linked into this test,
 * where the dynamic linker lets the program's definition win.
 *
 * @param[in] uid  the user ID
 * @returns an entry named "test" for uid, or NULL
 */
struct passwd *getpwuid(uid_t uid)
{
    static char name[] = "test";
    static struct passwd entry;

    if (passwd_lacks_the_user) {
        return NULL;
    }

    entry.pw_name = name;
    entry.pw_uid = uid;
    entry.pw_gid = getegid();
    return &entry;
}

/*!
 * @brief A server is set up for a user the passwd database lacks
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_server_set_up_without_a_passwd_entry(void)
{
    struct afp_server *server;
    passwd_lacks_the_user = 1;
    server = afp_server_init(NULL);
    passwd_lacks_the_user = 0;

    if (server == NULL) {
        return 1;
    }

    afp_server_release(server);
    return 0;
}

/*!
 * @brief Set up a volume on a suspended server
 *
 * Every request fails before it leaves, with the transport's EIO.
 *
 * @param[out] volume  receives the volume
 * @param[out] server  receives its server, which the caller releases
 * @returns 0, or -1
 */
static int suspended_volume_open(struct afp_volume *volume,
                                 struct afp_server **server)
{
    *server = afp_server_init(NULL);

    if (*server == NULL) {
        return -1;
    }

    (*server)->using_version = &afp34;
    afp_server_set_suspended(*server, 1);
    memset(volume, 0, sizeof(*volume));
    pthread_mutex_init(&volume->did_cache_mutex, NULL);
    volume->server = *server;
    volume->path_encoding = kFPUTF8Name;
    volume->signature = AFP_VOL_FIXED;
    return 0;
}

/*!
 * @brief An unlink the transport refused returns -EIO
 *
 * A request the transport refused is a failure to every caller, never a
 * success and never a reason to send it again.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unlink_fails_with_the_transport(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    int rc;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    rc = ml_unlink(&volume, "/file");
    afp_server_release(server);
    return rc == -EIO ? 0 : 2;
}

/*!
 * @brief A mkdir the transport refused returns -EIO
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_mkdir_fails_with_the_transport(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    int rc;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    rc = ml_mkdir(&volume, "/dir", 0755);
    afp_server_release(server);
    return rc == -EIO ? 0 : 2;
}

/*!
 * @brief An rmdir the transport refused returns -EIO, not -ENOTDIR
 *
 * The lookup that tells a directory from a file is what fails.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_rmdir_fails_with_the_transport(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    int rc;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    rc = ml_rmdir(&volume, "/dir");
    afp_server_release(server);
    return rc == -EIO ? 0 : 2;
}

/*!
 * @brief A link's read the transport refused returns -EIO
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_readlink_fails_with_the_transport(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    char target[16];
    int rc;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    rc = ml_readlink(&volume, "/link", target, sizeof(target));
    afp_server_release(server);
    return rc == -EIO ? 0 : 2;
}

/*!
 * @brief A volume open the transport refused leaves it unmounted
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_volume_open_fails_unmounted(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    char mesg[256];
    unsigned int len = 0;
    int ret = 0;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    if (afp_connect_volume(&volume, server, mesg, &len, sizeof(mesg)) != 1) {
        ret = 2;
    } else if (volume.mounted == AFP_VOLUME_MOUNTED) {
        ret = 3;
    }

    afp_server_release(server);
    return ret;
}

/*!
 * @brief A listing the transport refused leaves an empty list
 *
 * The caller's pointer is emptied, whatever it held.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_failed_listing_leaves_an_empty_list(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    struct afp_file_info *files;
    int ret = 0;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    files = (struct afp_file_info *) &files;

    if (ll_readdir(&volume, "/", &files, 0) != -EIO) {
        ret = 2;
    } else if (files != NULL) {
        ret = 3;
    }

    afp_server_release(server);
    return ret;
}

/*!
 * @brief An AppleDouble listing fails with the listing beneath it
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_appledouble_listing_fails_with_the_listing(void)
{
    struct afp_server *server;
    struct afp_volume volume;
    struct afp_file_info *files;
    int ret = 0;

    if (suspended_volume_open(&volume, &server) != 0) {
        return 1;
    }

    volume.extra_flags |= VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    files = (struct afp_file_info *) &files;

    if (ml_readdir(&volume, "/.AppleDouble", &files) != -EIO) {
        ret = 2;
    } else if (files != NULL) {
        ret = 3;
    }

    afp_server_release(server);
    return ret;
}

/*!
 * @brief Run the failed send checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    libafpclient_register(&quiet_client);
    TEST_int(afp_main_quick_startup(NULL), 0, "the event loop starts");
    TEST_int(utest_server_set_up_without_a_passwd_entry(), 0,
             "a server is set up for a user without a passwd entry");
    TEST_int(utest_unlink_fails_with_the_transport(), 0,
             "an unlink the transport refused returns -EIO");
    TEST_int(utest_mkdir_fails_with_the_transport(), 0,
             "a mkdir the transport refused returns -EIO");
    TEST_int(utest_rmdir_fails_with_the_transport(), 0,
             "an rmdir the transport refused returns -EIO");
    TEST_int(utest_readlink_fails_with_the_transport(), 0,
             "a link's read the transport refused returns -EIO");
    TEST_int(utest_volume_open_fails_unmounted(), 0,
             "a volume open the transport refused leaves it unmounted");
    TEST_int(utest_failed_listing_leaves_an_empty_list(), 0,
             "a listing the transport refused leaves an empty list");
    TEST_int(utest_appledouble_listing_fails_with_the_listing(), 0,
             "an AppleDouble listing fails with the listing beneath it");
    return test_tap_finish();
}
