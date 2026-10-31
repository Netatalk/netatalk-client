/*
    test_metadata_requests.c: the requests of creates and metadata changes

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/lowlevel.h"
#include "lib/mapping.h"
#include "lib/midlevel.h"
#include "lib/resource.h"
#include "tap.h"

/*!
 * @brief Drop a log line of the library
 *
 * @param[in] priv      unused
 * @param[in] logtype   unused
 * @param[in] loglevel  unused
 * @param[in] message   unused
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

/*!
 * @brief An exclusive open of a name taken fails at its soft create
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_exclusive_open_of_a_name_taken_fails(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.name_taken = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_open(&fake.volume, "/file", O_RDWR | O_CREAT | O_EXCL, &fp);
    afp_fake_server_stop(&fake);

    if (rc != -EEXIST) {
        return 2;
    }

    return fake.ncommands == 1 ? 0 : 3;
}

/*!
 * @brief An O_TRUNC open that finds the file it would create empties it
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_open_that_finds_the_file_empties_it(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.name_taken = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_open(&fake.volume, "/file", O_RDWR | O_CREAT | O_TRUNC, &fp);

    if (rc == 0) {
        rc = ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    return fake.ncommands == 4 && fake.commands[2] == afpSetForkParms
           ? 0 : 3;
}

/*!
 * @brief An open without write access creates the file it names
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_open_without_write_access_creates(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_open(&fake.volume, "/file", O_RDONLY | O_CREAT, &fp);

    if (rc == 0) {
        rc = ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    return fake.commands[0] == afpCreateFile ? 0 : 3;
}

/*!
 * @brief A create the server refuses as read-only fails with EROFS
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_only_create_fails_with_erofs(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.read_only = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_creat(&fake.volume, "/file", 0644, NULL);
    afp_fake_server_stop(&fake);
    return rc == -EROFS ? 0 : 2;
}

/*!
 * @brief An open the server refuses removes the file its create made
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_open_removes_its_file(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpOpenFork;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_open(&fake.volume, "/file", O_RDWR | O_CREAT, &fp);
    afp_fake_server_stop(&fake);

    if (rc != -EACCES) {
        return 2;
    }

    return fake.ncommands == 3 && fake.commands[2] == afpDelete ? 0 : 3;
}

/*!
 * @brief A truncation by path the server refuses closes its fork
 *
 * The fork's record leaves the volume's list too.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_truncation_closes_its_fork(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpSetForkParms;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_truncate(&fake.volume, "/file", 100);
    afp_fake_server_stop(&fake);

    if (rc != -EACCES) {
        return 2;
    }

    if (fake.ncommands != 3 || fake.commands[2] != afpCloseFork) {
        return 3;
    }

    return fake.volume.open_forks == NULL ? 0 : 4;
}

/*!
 * @brief A directory lookup the server refuses stops each operation
 *
 * The caller gets an errno, never the server's code, and nothing goes out
 * after the lookup.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_lookup_stops_each_operation(void)
{
    struct afp_fake_server fake;
    char target[16];
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpGetFileDirParms;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_creat(&fake.volume, "/dir/file", 0644, NULL) != -EACCES) {
        ret = 2;
    } else if (ml_unlink(&fake.volume, "/dir/file") != -EACCES) {
        ret = 3;
    } else if (ml_mkdir(&fake.volume, "/dir/sub", 0755) != -EACCES) {
        ret = 4;
    } else if (ml_rmdir(&fake.volume, "/dir/sub") != -EACCES) {
        ret = 5;
    } else if (ml_readlink(&fake.volume, "/dir/link", target,
                           sizeof(target)) != -EACCES) {
        ret = 6;
    } else if (ml_symlink(&fake.volume, "target", "/dir/link") != -EACCES) {
        ret = 7;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 6 ? 0 : 8;
}

/*!
 * @brief A read-only filesystem's refusals give EROFS
 *
 * An open for writing, a mkdir, a delete and a length change refused with
 * kFPVolLocked each fail with EROFS.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_only_refusals_give_erofs(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.read_only = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_truncate(&fake.volume, "/file", 100) != -EROFS) {
        ret = 2;
    } else if (ml_mkdir(&fake.volume, "/sub", 0755) != -EROFS) {
        ret = 3;
    } else if (ml_unlink(&fake.volume, "/file") != -EROFS) {
        ret = 4;
    } else if (ml_open(&fake.volume, "/file", O_RDONLY, &fp) != 0) {
        ret = 5;
    } else if (ll_setfork_size(&fake.volume, fp->forkid, 0, 100) != EROFS) {
        ret = 6;
    }

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A volume the client knows is read-only refuses changes with EROFS
 *
 * Nothing goes to the server.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_only_volume_refuses_changes(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.attributes = kReadOnly;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_creat(&fake.volume, "/file", 0644, NULL) != -EROFS) {
        ret = 2;
    } else if (ml_open(&fake.volume, "/file", O_WRONLY, &fp) != -EROFS) {
        ret = 3;
    } else if (ml_truncate(&fake.volume, "/file", 0) != -EROFS) {
        ret = 4;
    } else if (ml_chmod(&fake.volume, "/file", 0600) != -EROFS) {
        ret = 5;
    } else if (ml_unlink(&fake.volume, "/file") != -EROFS) {
        ret = 6;
    } else if (ml_mkdir(&fake.volume, "/sub", 0755) != -EROFS) {
        ret = 7;
    } else if (ml_rmdir(&fake.volume, "/sub") != -EROFS) {
        ret = 8;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 0 ? 0 : 9;
}

/*!
 * @brief A length change on a suspended session fails without a request
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_length_change_on_a_suspended_session_fails(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->connect_state = SERVER_STATE_DISCONNECTED;
    afp_server_set_suspended(fake.server, 1);
    rc = ll_setfork_size(&fake.volume, 1, 0, 100);
    afp_fake_server_stop(&fake);

    if (rc != EIO) {
        return 2;
    }

    return fake.ncommands == 0 ? 0 : 3;
}

/*!
 * @brief A time set sends one FPSetFileDirParms
 *
 * On a suspended session it fails.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_time_set_sends_one_request(void)
{
    struct afp_fake_server fake;
    struct utimbuf times = { 1000000000, 1000000000 };
    int ret = 0;
    memset(&fake, 0, sizeof(fake));

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_utime(&fake.volume, "/file", &times) != 0) {
        ret = 2;
    } else if (fake.ncommands != 1
               || fake.commands[0] != afpSetFileDirParms) {
        ret = 3;
    } else {
        fake.server->connect_state = SERVER_STATE_DISCONNECTED;
        afp_server_set_suspended(fake.server, 1);

        if (ml_utime(&fake.volume, "/file", &times) != -EIO) {
            ret = 4;
        }
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A time set refused with kFPVolLocked fails with EROFS
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_only_time_set_gives_erofs(void)
{
    struct afp_fake_server fake;
    struct utimbuf times = { 1000000000, 1000000000 };
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.read_only = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_utime(&fake.volume, "/file", &times);
    afp_fake_server_stop(&fake);
    return rc == -EROFS ? 0 : 2;
}

/*!
 * @brief Without the common mapping a change of owner takes no request
 *
 * Not even its directory's lookup goes out: the owner every file shows
 * changes nothing, and another is EPERM.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_owner_change_without_common_mapping_sends_nothing(void)
{
    struct afp_fake_server fake;
    uid_t shown;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.volume.mapping = AFP_MAPPING_LOGINIDS;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    shown = fake.server->passwd.pw_uid;

    if (ml_chown(&fake.volume, "/dir/file", shown, (gid_t) -1) != 0) {
        ret = 2;
    } else if (ml_chown(&fake.volume, "/dir/file", shown + 1, (gid_t) -1)
               != -EPERM) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 0 ? 0 : 4;
}

/*!
 * @brief A mode change refused with a time asked sends the time alone
 *
 * The call returns the time's refusal, or EPERM once the time is set.
 *
 * @param[in] time_refused  whether the server refuses the time too
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_mode_sends_the_time_alone(int time_refused)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.refuse = time_refused ? afpSetFileDirParms : 0;
    fake.refuse_privs = 1;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_setattr(&fake.volume, "/file", ML_SET_MODE | ML_SET_MTIME, 0600,
                    0, 0, 1000000000);
    afp_fake_server_stop(&fake);

    if (rc != (time_refused ? -EACCES : -EPERM)) {
        return 2;
    }

    /* The time alone is the request less the privileges' 16 bytes. */
    return fake.ncommands == 3 && fake.commands[2] == afpSetFileDirParms
           && fake.lengths[1] - fake.lengths[2] == 16 ? 0 : 3;
}

/*!
 * @brief A comment's write sends the bytes written
 *
 * What follows them in the buffer stays out of the request.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_write_sends_the_bytes_written(void)
{
    struct afp_fake_server fake;
    struct afp_file_info comment;
    size_t written = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.dtrefnum = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    memset(&comment, 0, sizeof(comment));
    comment.resource = AFP_META_COMMENT;
    comment.did = 2;
    memcpy(comment.basename, "file", sizeof("file"));

    if (appledouble_write(&fake.volume, &comment, "hi", 2, 0, &written)
            != 1) {
        ret = 2;
    } else if (appledouble_write(&fake.volume, &comment, "hi there", 2, 0,
                                 &written) != 1) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    /* Each write reads the comment it changes first. */
    if (written != 2 || fake.ncommands != 4
            || fake.commands[3] != afpAddComment) {
        return 4;
    }

    return fake.lengths[1] == fake.lengths[3] ? 0 : 5;
}

/*!
 * @brief Write a comment pseudo-file on a fake server whose comment is text
 *
 * @param[in,out] fake     the stopped server afterwards, with its comment
 * @param[in]     text     the comment the server starts with, or NULL
 * @param[in]     data     bytes to write, or NULL to truncate
 * @param[in]     size     length of data, or the length to truncate to
 * @param[in]     offset   offset of the write
 * @param[out]    written  bytes written
 * @returns the write's or the truncation's result, or 2 when the server
 *          cannot start
 */
static int change_comment(struct afp_fake_server *fake, const char *text,
                          const char *data, size_t size, off_t offset,
                          size_t *written)
{
    struct afp_file_info comment;
    int rc;
    memset(fake, 0, sizeof(*fake));
    fake->volume.dtrefnum = 1;

    if (text) {
        fake->comment_len = (uint8_t) strlen(text);
        memcpy(fake->comment, text, fake->comment_len);
    }

    if (afp_fake_server_start(fake) != 0) {
        return 2;
    }

    memset(&comment, 0, sizeof(comment));
    comment.resource = AFP_META_COMMENT;
    comment.did = 2;
    memcpy(comment.basename, "file", sizeof("file"));
    rc = data ? appledouble_write(&fake->volume, &comment, data, size, offset,
                                  written)
         : ml_ftruncate(&fake->volume, &comment, (off_t) size);
    afp_fake_server_stop(fake);
    return rc;
}

/*!
 * @brief A comment's write at an offset keeps the bytes before it
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_write_at_an_offset_keeps_the_start(void)
{
    struct afp_fake_server fake;
    size_t written = 0;

    if (change_comment(&fake, "line1\n", "line2\n", 6, 6, &written) != 1
            || written != 6) {
        return 2;
    }

    return fake.comment_len == 12
           && memcmp(fake.comment, "line1\nline2\n", 12) == 0 ? 0 : 3;
}

/*!
 * @brief A comment's truncation sends the comment's first bytes
 *
 * A truncation to nothing empties the comment.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_truncation_sends_the_start(void)
{
    struct afp_fake_server fake;
    size_t written = 0;

    if (change_comment(&fake, "line1\nline2\n", NULL, 5, 0, &written) != 0
            || fake.comment_len != 5 || memcmp(fake.comment, "line1", 5) != 0) {
        return 2;
    }

    if (change_comment(&fake, "line1\n", NULL, 0, 0, &written) != 0) {
        return 3;
    }

    return fake.comment_len == 0 ? 0 : 4;
}

/*!
 * @brief A comment's write stops at the 199 bytes AFP stores
 *
 * One that starts there fails with EFBIG.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_write_stops_at_its_limit(void)
{
    struct afp_fake_server fake;
    char data[240];
    size_t written = 0;
    memset(data, 'c', sizeof(data));

    if (change_comment(&fake, NULL, data, sizeof(data), 0, &written) != 1
            || written != 199 || fake.comment_len != 199) {
        return 2;
    }

    return change_comment(&fake, NULL, data, 1, 199, &written) == -EFBIG
           ? 0 : 3;
}

/*!
 * @brief An AFP 2 read past 2 GiB asks the fork's length where the
 *        handle's would end the file
 *
 * Another client can grow the fork past the length the open read.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_afp2_read_past_the_bound_asks_the_length(void)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_fake_server fake;
    struct afp_file_info fork;
    char buf[16];
    int eof = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    memset(&fork, 0, sizeof(fork));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
    fake.file_params = 1;
    fake.data_length = (uint32_t) INT32_MAX + 1 + sizeof(buf);

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    fake.server->quantum = 65536;
    fork.forkid = AFP_FAKE_SERVER_FORK;
    fork.did = AFP_ROOT_DID;
    memcpy(fork.basename, "file", sizeof("file"));
    fork.size = (uint64_t) INT32_MAX + 1;

    if (ll_read(&fake.volume, buf, sizeof(buf), (off_t) INT32_MAX + 1, &fork,
                &eof) != -EOVERFLOW || eof) {
        ret = 2;
    } else {
        fake.data_length = (uint32_t) INT32_MAX + 1;

        if (ll_read(&fake.volume, buf, sizeof(buf), (off_t) INT32_MAX + 1,
                    &fork, &eof) != 0 || !eof) {
            ret = 3;
        }
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 2 && fake.commands[0] == afpGetFileDirParms
           && fake.commands[1] == afpGetFileDirParms ? 0 : 4;
}

/*!
 * @brief Check the requests each operation sends and what it returns
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    TEST_int(afp_main_quick_startup(NULL), 0, "the event loop starts");
    TEST_int(utest_exclusive_open_of_a_name_taken_fails(), 0,
             "an exclusive open of a name taken fails at its soft create");
    TEST_int(utest_open_that_finds_the_file_empties_it(), 0,
             "an O_TRUNC open that finds the file it would create empties it");
    TEST_int(utest_open_without_write_access_creates(), 0,
             "an open without write access creates the file it names");
    TEST_int(utest_read_only_create_fails_with_erofs(), 0,
             "a create the server refuses as read-only fails with EROFS");
    TEST_int(utest_refused_open_removes_its_file(), 0,
             "an open the server refuses removes the file its create made");
    TEST_int(utest_refused_truncation_closes_its_fork(), 0,
             "a truncation by path the server refuses closes its fork");
    TEST_int(utest_refused_lookup_stops_each_operation(), 0,
             "a refused directory lookup stops each operation with EACCES");
    TEST_int(utest_read_only_refusals_give_erofs(), 0,
             "an open, a mkdir, a delete and a length change refused as "
             "read-only give EROFS");
    TEST_int(utest_read_only_volume_refuses_changes(), 0,
             "a volume the client knows is read-only refuses changes with "
             "EROFS");
    TEST_int(utest_length_change_on_a_suspended_session_fails(), 0,
             "a length change on a suspended session fails without a "
             "request");
    TEST_int(utest_time_set_sends_one_request(), 0,
             "a time set sends one request, and fails on a suspended session");
    TEST_int(utest_read_only_time_set_gives_erofs(), 0,
             "a time set refused with kFPVolLocked fails with EROFS");
    TEST_int(utest_owner_change_without_common_mapping_sends_nothing(), 0,
             "without the common mapping a change of owner takes no "
             "request");
    TEST_int(utest_refused_mode_sends_the_time_alone(1), 0,
             "a mode refused with a time asked gives the time's own refusal");
    TEST_int(utest_refused_mode_sends_the_time_alone(0), 0,
             "a mode refused with a time asked sets the time, then EPERM");
    TEST_int(utest_comment_write_sends_the_bytes_written(), 0,
             "a comment's write sends the bytes written");
    TEST_int(utest_comment_write_at_an_offset_keeps_the_start(), 0,
             "a comment's write at an offset keeps the bytes before it");
    TEST_int(utest_comment_truncation_sends_the_start(), 0,
             "a comment's truncation sends the comment's first bytes");
    TEST_int(utest_comment_write_stops_at_its_limit(), 0,
             "a comment's write stops at the 199 bytes AFP stores");
    TEST_int(utest_afp2_read_past_the_bound_asks_the_length(), 0,
             "an AFP 2 read past 2 GiB asks the fork's length");
    return test_tap_finish();
}
