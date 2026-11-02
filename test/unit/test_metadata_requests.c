/*
    test_metadata_requests.c: the requests of creates and metadata changes

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
 * @brief A lookup the server refuses stops each operation
 *
 * Each caller gets an errno, not the server's code, and sends nothing after the
 * lookup, a delete's type lookup too; a failed open hands back no record.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_lookup_stops_each_operation(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
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
    } else if (ml_unlink(&fake.volume, "/file") != -EACCES) {
        ret = 8;
    } else if (ml_open(&fake.volume, "/dir/file", O_RDONLY, &fp) != -EACCES
               || fp != NULL) {
        ret = 9;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 8 ? 0 : 10;
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
    fake.file_params = 1;

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
    } else if (ml_setxattr(&fake.volume, "/file", "user.k", "v", 1, 0)
               != -EROFS) {
        ret = 9;
    } else if (ml_removexattr(&fake.volume, "/file", "user.k") != -EROFS) {
        ret = 10;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 0 ? 0 : 11;
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
 * @param[in,out] fake     the zeroed server, and afterwards the stopped
 *                         server with its comment
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
    memset(&fake, 0, sizeof(fake));

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
    memset(&fake, 0, sizeof(fake));

    if (change_comment(&fake, "line1\nline2\n", NULL, 5, 0, &written) != 0
            || fake.comment_len != 5 || memcmp(fake.comment, "line1", 5) != 0) {
        return 2;
    }

    memset(&fake, 0, sizeof(fake));

    if (change_comment(&fake, "line1\n", NULL, 0, 0, &written) != 0) {
        return 3;
    }

    return fake.comment_len == 0 ? 0 : 4;
}

/*!
 * @brief A comment's truncation goes to the server at the read's length
 *
 * afpd answers 128 bytes of a longer comment.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_truncation_to_the_read_length_is_sent(void)
{
    struct afp_fake_server fake;
    char text[161];
    size_t written = 0;
    memset(text, 'c', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';
    memset(&fake, 0, sizeof(fake));

    if (change_comment(&fake, text, NULL, 128, 0, &written) != 0) {
        return 2;
    }

    return fake.comment_len == 128 ? 0 : 3;
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
    memset(&fake, 0, sizeof(fake));

    if (change_comment(&fake, NULL, data, sizeof(data), 0, &written) != 1
            || written != 199 || fake.comment_len != 199) {
        return 2;
    }

    memset(&fake, 0, sizeof(fake));
    return change_comment(&fake, NULL, data, 1, 199, &written) == -EFBIG
           ? 0 : 3;
}

/*!
 * @brief A comment's write sends no more of a longer comment than AFP stores
 *
 * A server other than afpd can answer all of a comment it keeps.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_write_keeps_to_its_limit(void)
{
    struct afp_fake_server fake;
    char text[221];
    size_t written = 0;
    memset(text, 'c', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';
    memset(&fake, 0, sizeof(fake));
    fake.whole_comment = 1;

    if (change_comment(&fake, text, "x", 1, 0, &written) != 1
            || written != 1) {
        return 2;
    }

    return fake.comment_len == 199 && fake.comment[0] == 'x' ? 0 : 3;
}

/*!
 * @brief An AFP 2 read past 2 GiB asks the fork's length
 *
 * The handle's length would end the file, which another client can grow
 * past the length the open read.
 *
 * @param[in] resource  whether the fork is the resource fork
 * @returns 0, or the number of the step that failed
 */
static int utest_afp2_read_past_the_bound_asks_the_length(int resource)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_fake_server fake;
    struct afp_file_info fork;
    uint32_t *length;
    char buf[16];
    int eof = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    memset(&fork, 0, sizeof(fork));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
    fake.file_params = 1;
    length = resource ? &fake.resource_length : &fake.data_length;
    *length = (uint32_t) INT32_MAX + 1 + sizeof(buf);

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    fake.server->quantum = 65536;
    fork.forkid = AFP_FAKE_SERVER_FORK;
    fork.did = AFP_ROOT_DID;
    fork.resource = resource ? AFP_META_RESOURCE : 0;
    memcpy(fork.basename, "file", sizeof("file"));
    fork.size = (uint64_t) INT32_MAX + 1;

    if (ll_read(&fake.volume, buf, sizeof(buf), (off_t) INT32_MAX + 1, &fork,
                &eof) != -EOVERFLOW || eof) {
        ret = 2;
    } else {
        *length = (uint32_t) INT32_MAX + 1;

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
 * @brief An AFP 2 read past a fork of 4 GiB or more fails with EOVERFLOW
 *
 * afpd answers 0xFFFFFFFF as the length of such a fork.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_afp2_read_past_a_saturated_length_overflows(void)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_fake_server fake;
    struct afp_file_info fork;
    char buf[16];
    int eof = 0;
    int rc;
    memset(&fake, 0, sizeof(fake));
    memset(&fork, 0, sizeof(fork));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
    fake.file_params = 1;
    fake.data_length = UINT32_MAX;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    fake.server->quantum = 65536;
    fork.forkid = AFP_FAKE_SERVER_FORK;
    fork.did = AFP_ROOT_DID;
    memcpy(fork.basename, "file", sizeof("file"));
    fork.size = UINT32_MAX;
    rc = ll_read(&fake.volume, buf, sizeof(buf), (off_t) UINT32_MAX, &fork,
                 &eof);
    afp_fake_server_stop(&fake);
    return rc == -EOVERFLOW && !eof ? 0 : 2;
}

/*!
 * @brief An AFP 2 open of a directory fails with EISDIR
 *
 * No fork open goes out.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_afp2_open_of_a_directory_fails_with_eisdir(void)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_dir = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    rc = ml_open(&fake.volume, "/dir", O_RDONLY, &fp);

    if (rc == 0) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return rc == -EISDIR && fake.ncommands == 1 ? 0 : 2;
}

/*!
 * @brief A name's length counts in the volume's encoding
 *
 * On AFP 2 a name of 28 Mac Roman bytes, 34 in UTF-8, is within the 31
 * bytes a name holds: mkdir and rmdir of it go to the server.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_name_length_counts_in_the_volume_encoding(void)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    static const char path[] = "/\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                               "\xc3\xa9" "abcdefghijklmnopqrstuv";
    struct afp_fake_server fake;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_dir = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    fake.volume.path_encoding = kFPLongName;

    if (ml_mkdir(&fake.volume, path, 0755) != 0) {
        ret = 2;
    } else if (ml_rmdir(&fake.volume, path) != 0) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A locked file's refusals give EPERM
 *
 * afpd refuses the delete and a write open of a file the Finder locks with
 * kFPObjectLocked.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_locked_file_refusals_give_eperm(void)
{
    static const uint8_t refused[] = { afpDelete, afpOpenFork };

    for (size_t i = 0; i < sizeof(refused); i++) {
        struct afp_fake_server fake;
        struct afp_file_info *fp = NULL;
        int rc;
        memset(&fake, 0, sizeof(fake));
        fake.refuse = refused[i];
        fake.refusal = kFPObjectLocked;
        fake.file_params = 1;

        if (afp_fake_server_start(&fake) != 0) {
            return 1;
        }

        rc = refused[i] == afpDelete ? ml_unlink(&fake.volume, "/file")
             : ml_open(&fake.volume, "/file", O_WRONLY, &fp);
        afp_fake_server_stop(&fake);

        if (rc != -EPERM) {
            return 2 + (int) i;
        }
    }

    return 0;
}

/*!
 * @brief A mode the server does not set sends the time asked alone
 *
 * The call returns ENOSYS once the time is set.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_unset_mode_sends_the_time_alone(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.refuse_privs = 1;
    fake.refusal = kFPBitmapErr;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_setattr(&fake.volume, "/file", ML_SET_MODE | ML_SET_MTIME, 0600,
                    0, 0, 1000000000);
    afp_fake_server_stop(&fake);

    if (rc != -ENOSYS) {
        return 2;
    }

    /* The time alone is the request less the privileges' 16 bytes. */
    return fake.ncommands == 3 && fake.commands[2] == afpSetFileDirParms
           && fake.lengths[1] - fake.lengths[2] == 16 ? 0 : 3;
}

/*!
 * @brief A negative length fails with EINVAL without a request
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_negative_length_fails_with_einval(void)
{
    struct afp_fake_server fake;
    struct afp_file_info fork;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    memset(&fork, 0, sizeof(fork));

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fork.forkid = AFP_FAKE_SERVER_FORK;

    if (ml_truncate(&fake.volume, "/file", -1) != -EINVAL) {
        ret = 2;
    } else if (ml_ftruncate(&fake.volume, &fork, -1) != -EINVAL) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 0 ? 0 : 4;
}

/*!
 * @brief The comment pseudo-file of a file without a comment is empty
 *
 * A truncation of a comment to nothing leaves it so.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_of_a_file_without_one_is_empty(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    struct stat st;
    char buf[16];
    int eof = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    fake.volume.mapping = AFP_MAPPING_LOGINIDS;
    fake.volume.dtrefnum = 1;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (appledouble_getattr(&fake.volume, "/.AppleDouble/file.comment", &st)
            != 1 || !S_ISREG(st.st_mode) || st.st_size != 0) {
        ret = 2;
    } else if (ml_open(&fake.volume, "/.AppleDouble/file.comment", O_RDONLY,
                       &fp) != 0) {
        ret = 3;
    } else if (ml_read(&fake.volume, buf, sizeof(buf), 0, fp, &eof) != 0
               || !eof) {
        ret = 4;
    }

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A directory's Finder information pseudo-file is a regular file
 *
 * The directory has no resource fork to show.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_directory_finder_info_is_a_regular_file(void)
{
    struct afp_fake_server fake;
    struct stat st;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE
                              | VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.volume.mapping = AFP_MAPPING_LOGINIDS;
    fake.file_params = 1;
    fake.params_dir = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (appledouble_getattr(&fake.volume, "/.AppleDouble/dir.finderinfo",
                            &st) != 1 || !S_ISREG(st.st_mode)) {
        ret = 2;
    } else if (appledouble_getattr(&fake.volume, "/.AppleDouble/dir", &st)
               != -ENOENT) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief Give the started fake server a machine type and identify it
 *
 * @param[in,out] fake          the fake server
 * @param[in]     machine_type  the type, such as Netatalk4.2.3
 */
static void identify_as(struct afp_fake_server *fake, const char *machine_type)
{
    snprintf(fake->server->machine_type, sizeof(fake->server->machine_type),
             "%s", machine_type);
    afp_server_identify(fake->server);
}

/*!
 * @brief A request that sets a mode on Netatalk carries ids of -1
 *
 * @param[in] created  whether the mode is a new file's, or a change
 * @returns 0, or the number of the step that failed
 */
static int utest_mode_requests_carry_unset_ids(int created)
{
    struct afp_fake_server fake;
    unsigned char made = 0;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    identify_as(&fake, "Netatalk4.2.3");

    if (created) {
        rc = ml_creat(&fake.volume, "/file", 0640, &made);
        ml_set_created_mode(&fake.volume, "/file", 0640);
    } else {
        rc = ml_chmod(&fake.volume, "/file", 0600);
    }

    afp_fake_server_stop(&fake);

    if (rc != 0 || (created && !made)) {
        return 2;
    }

    return (fake.set_bitmap & kFPUnixPrivsBit) && fake.set_uid == UINT32_MAX
           && fake.set_gid == UINT32_MAX ? 0 : 3;
}

/*!
 * @brief A set-ID file keeps its owner without a request
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_set_id_file_keeps_its_owner(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.volume.mapping = AFP_MAPPING_COMMON;
    fake.file_params = 1;
    fake.params_mode = 0104755;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_chown(&fake.volume, "/file", 1001, (gid_t) -1);
    afp_fake_server_stop(&fake);

    if (rc != -EPERM) {
        return 2;
    }

    return fake.ncommands == 1 && fake.commands[0] == afpGetFileDirParms
           ? 0 : 3;
}

/*!
 * @brief Netatalk 2 reads back a directory's special bit it kept
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_netatalk2_directory_special_bit_is_read_back(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.file_params = 1;
    fake.params_dir = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    identify_as(&fake, "Netatalk2.4.10");
    rc = ml_chmod(&fake.volume, "/dir", 041755);
    afp_fake_server_stop(&fake);

    if (rc != -EPERM) {
        return 2;
    }

    return fake.ncommands == 3 && fake.commands[2] == afpGetFileDirParms
           ? 0 : 3;
}

/*!
 * @brief A mode change carries no time the server would misplace
 *
 * @param[in] variant  1 for an unset date, 2 for one before 1970, 3 for a
 *                     symlink's
 * @returns 0, or the number of the step that failed
 */
static int utest_misplaced_time_is_not_carried(int variant)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.file_params = 1;

    if (variant == 1) {
        fake.params_date = htonl(UINT32_C(0x80000000));
    } else if (variant == 2) {
        fake.params_date = htonl((uint32_t) - (AD_DATE_DELTA + 1));
    } else {
        fake.params_mode = 0120777;
    }

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_chmod(&fake.volume, "/file", 0600);
    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    return (fake.set_bitmap & kFPUnixPrivsBit)
           && !(fake.set_bitmap & kFPModDateBit) ? 0 : 3;
}

/*!
 * @brief A pseudo-file's create sends nothing
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_pseudo_file_create_sends_nothing(void)
{
    struct afp_fake_server fake;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_creat(&fake.volume, "/dir/.AppleDouble/file.comment", 0644, NULL)
            != 0) {
        ret = 2;
    } else if (ml_creat(&fake.volume, "/dir/.AppleDouble/file.finderinfo",
                        0644, NULL) != 0) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    return fake.ncommands == 0 ? 0 : 4;
}

/*!
 * @brief The server icon opens for reading alone
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_server_icon_opens_read_only(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_open(&fake.volume, "/.servericon", O_WRONLY, &fp) != -EPERM) {
        ret = 2;
    } else if (ml_open(&fake.volume, "/.servericon", O_RDONLY | O_TRUNC, &fp)
               != -EPERM) {
        ret = 3;
    } else if (ml_open(&fake.volume, "/.servericon", O_RDONLY, &fp) != 0) {
        ret = 4;
    }

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A comment reads from the offset asked, as often as asked
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_reads_at_an_offset(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    char buf[16];
    int eof = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    fake.volume.dtrefnum = 1;
    fake.comment_len = 12;
    memcpy(fake.comment, "line1\nline2\n", 12);

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (ml_open(&fake.volume, "/.AppleDouble/file.comment", O_RDONLY, &fp)
            != 0) {
        ret = 2;
    } else if (ml_read(&fake.volume, buf, sizeof(buf), 6, fp, &eof) != 6
               || !eof || memcmp(buf, "line2\n", 6) != 0) {
        ret = 3;
    } else if (ml_read(&fake.volume, buf, 5, 0, fp, &eof) != 5 || eof
               || memcmp(buf, "line1", 5) != 0) {
        ret = 4;
    }

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A file's owner change on Netatalk carries the group as -1
 *
 * The fake keeps the owner, which the read-back reports.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_owner_change_carries_the_group_as_unset(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.volume.mapping = AFP_MAPPING_COMMON;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    identify_as(&fake, "Netatalk4.2.3");
    rc = ml_chown(&fake.volume, "/file", 1001, (gid_t) -1);
    afp_fake_server_stop(&fake);

    if (rc != -EPERM) {
        return 2;
    }

    return (fake.set_bitmap & kFPUnixPrivsBit) && fake.set_uid == 1001
           && fake.set_gid == UINT32_MAX ? 0 : 3;
}

/*!
 * @brief A delete measures the name before it looks anything up
 *
 * On AFP 2 a name of 40 bytes is past the 31 a name holds.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_delete_measures_the_name_first(void)
{
    static struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_fake_server fake;
    char path[42];
    int rc;
    memset(&fake, 0, sizeof(fake));
    memset(path, 'a', sizeof(path) - 1);
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.server->using_version = &afp22;
    fake.volume.path_encoding = kFPLongName;
    rc = ml_unlink(&fake.volume, path);
    afp_fake_server_stop(&fake);

    if (rc != -ENAMETOOLONG) {
        return 2;
    }

    return fake.ncommands == 0 ? 0 : 3;
}

/*!
 * @brief A comment's truncation by path and its O_TRUNC open reach the server
 *
 * @param[in] trunc_open  whether to open with O_TRUNC, or truncate to 5
 * @returns 0, or the number of the step that failed
 */
static int utest_comment_path_changes_reach_the_server(int trunc_open)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    fake.volume.dtrefnum = 1;
    fake.comment_len = 12;
    memcpy(fake.comment, "line1\nline2\n", 12);

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = trunc_open ? ml_open(&fake.volume, "/.AppleDouble/file.comment",
                              O_WRONLY | O_TRUNC, &fp)
         : ml_truncate(&fake.volume, "/.AppleDouble/file.comment", 5);

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    return fake.comment_len == (trunc_open ? 0 : 5) ? 0 : 3;
}

/*!
 * @brief A lookup the server refuses fails the file's pseudo-files
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_lookup_fails_the_pseudo_files(void)
{
    struct afp_fake_server fake;
    struct stat st;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    fake.refuse = afpGetFileDirParms;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    if (appledouble_getattr(&fake.volume, "/.AppleDouble/file", &st)
            != -EACCES) {
        ret = 2;
    } else if (appledouble_getattr(&fake.volume,
                                   "/.AppleDouble/file.finderinfo", &st)
               != -EACCES) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A pseudo-file's name goes to the server in the volume's encoding
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_pseudo_file_name_takes_the_volume_encoding(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    fake.volume.path_encoding = kFPLongName;

    if (ml_open(&fake.volume, "/.AppleDouble/caf\xc3\xa9.finderinfo",
                O_RDONLY, &fp) != 0) {
        ret = 2;
    } else if (strcmp(fp->basename, "caf\x8e") != 0) {
        ret = 3;
    }

    if (fp) {
        ml_close(&fake.volume, fp);
        free(fp);
    }

    afp_fake_server_stop(&fake);
    return ret;
}

/*!
 * @brief A read-only filesystem's refusal of Finder information is EROFS
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_read_only_finder_info_change_gives_erofs(void)
{
    struct afp_fake_server fake;
    struct afp_file_info info;
    size_t written = 0;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.read_only = 1;
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    memset(&info, 0, sizeof(info));
    info.resource = AFP_META_FINDERINFO;
    info.did = 2;
    memcpy(info.basename, "file", sizeof("file"));
    rc = appledouble_write(&fake.volume, &info, "TEXTttxt", 8, 0, &written);
    afp_fake_server_stop(&fake);
    return rc == -EROFS ? 0 : 2;
}

/*!
 * @brief Finder information that makes a file a symlink sends no time
 *
 * afpd would set the time on the link's target.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_symlink_finder_info_sends_no_time(void)
{
    struct afp_fake_server fake;
    struct afp_file_info info;
    size_t written = 0;
    int ret = 0;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    memset(&info, 0, sizeof(info));
    info.resource = AFP_META_FINDERINFO;
    info.did = 2;
    memcpy(info.basename, "file", sizeof("file"));

    if (appledouble_write(&fake.volume, &info, "TEXTttxt", 8, 0, &written)
            != 1) {
        ret = 2;
    } else if (appledouble_write(&fake.volume, &info, "slnkrhap", 8, 0,
                                 &written) != 1) {
        ret = 3;
    }

    afp_fake_server_stop(&fake);

    if (ret != 0) {
        return ret;
    }

    /* The time is four bytes of a set's request. */
    return fake.ncommands == 4 && fake.commands[3] == afpSetFileDirParms
           && fake.lengths[1] - fake.lengths[3] == 4 ? 0 : 4;
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
             "a refused lookup stops each operation with EACCES");
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
    TEST_int(utest_comment_truncation_to_the_read_length_is_sent(), 0,
             "a comment's truncation goes to the server at the read's length");
    TEST_int(utest_comment_write_keeps_to_its_limit(), 0,
             "a comment's write sends no more than AFP stores");
    TEST_int(utest_afp2_read_past_the_bound_asks_the_length(0), 0,
             "an AFP 2 read past 2 GiB asks the fork's length");
    TEST_int(utest_afp2_read_past_the_bound_asks_the_length(1), 0,
             "an AFP 2 read past 2 GiB of a resource fork asks its length");
    TEST_int(utest_afp2_read_past_a_saturated_length_overflows(), 0,
             "an AFP 2 read past a fork of 4 GiB fails with EOVERFLOW");
    TEST_int(utest_afp2_open_of_a_directory_fails_with_eisdir(), 0,
             "an AFP 2 open of a directory fails with EISDIR");
    TEST_int(utest_name_length_counts_in_the_volume_encoding(), 0,
             "a name's length counts in the volume's encoding");
    TEST_int(utest_locked_file_refusals_give_eperm(), 0,
             "a locked file's delete and write open fail with EPERM");
    TEST_int(utest_unset_mode_sends_the_time_alone(), 0,
             "a mode the server does not set sends the time asked alone");
    TEST_int(utest_negative_length_fails_with_einval(), 0,
             "a negative length fails with EINVAL without a request");
    TEST_int(utest_comment_of_a_file_without_one_is_empty(), 0,
             "a file without a comment shows an empty one, which reads as "
             "empty");
    TEST_int(utest_mode_requests_carry_unset_ids(1), 0,
             "a created file's mode request carries ids of -1 on Netatalk");
    TEST_int(utest_mode_requests_carry_unset_ids(0), 0,
             "a file's mode change carries ids of -1 on Netatalk");
    TEST_int(utest_set_id_file_keeps_its_owner(), 0,
             "a set-ID file keeps its owner without a request");
    TEST_int(utest_netatalk2_directory_special_bit_is_read_back(), 0,
             "Netatalk 2 reads back a directory's special bit it kept");
    TEST_int(utest_misplaced_time_is_not_carried(1), 0,
             "a mode change carries no unset time");
    TEST_int(utest_misplaced_time_is_not_carried(2), 0,
             "a mode change carries no time before 1970");
    TEST_int(utest_misplaced_time_is_not_carried(3), 0,
             "a symlink's mode change carries no time");
    TEST_int(utest_pseudo_file_create_sends_nothing(), 0,
             "a pseudo-file's create sends nothing");
    TEST_int(utest_server_icon_opens_read_only(), 0,
             "the server icon opens for reading alone");
    TEST_int(utest_comment_reads_at_an_offset(), 0,
             "a comment reads from the offset asked, as often as asked");
    TEST_int(utest_owner_change_carries_the_group_as_unset(), 0,
             "a file's owner change on Netatalk carries the group as -1");
    TEST_int(utest_delete_measures_the_name_first(), 0,
             "a delete measures the name before it looks anything up");
    TEST_int(utest_directory_finder_info_is_a_regular_file(), 0,
             "a directory's Finder information is a regular file, and it "
             "has no resource fork");
    TEST_int(utest_comment_path_changes_reach_the_server(0), 0,
             "a comment's truncation by path sets its length");
    TEST_int(utest_comment_path_changes_reach_the_server(1), 0,
             "a comment's O_TRUNC open empties it");
    TEST_int(utest_refused_lookup_fails_the_pseudo_files(), 0,
             "a refused lookup fails the file's pseudo-files");
    TEST_int(utest_pseudo_file_name_takes_the_volume_encoding(), 0,
             "a pseudo-file's name takes the volume's encoding");
    TEST_int(utest_read_only_finder_info_change_gives_erofs(), 0,
             "a read-only filesystem's refusal of Finder information is EROFS");
    TEST_int(utest_symlink_finder_info_sends_no_time(), 0,
             "Finder information that makes a file a symlink sends no time");
    return test_tap_finish();
}
