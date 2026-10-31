/*
    test_fork_records.c: open-fork records and symbolic links

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/midlevel.h"
#include "lib/resource.h"
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

/*!
 * @brief A truncation refused at open leaves no record listed
 *
 * The fork closes, and its record leaves the list before the open frees it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_truncation_at_open_unlists_its_record(void)
{
    struct afp_fake_server fake;
    struct afp_file_info *fp = NULL;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpSetForkParms;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_open(&fake.volume, "/file", O_RDWR | O_TRUNC, &fp);
    afp_fake_server_stop(&fake);

    if (rc != -EACCES) {
        return 2;
    }

    if (fake.volume.open_forks != NULL) {
        return 3;
    }

    return fake.ncommands == 3 && fake.commands[2] == afpCloseFork ? 0 : 4;
}

/*!
 * @brief Whether a listed record holds the open's fork number and, of the
 *        fields the open-fork list's walks read, nothing else
 *
 * @param[in] fp  the record
 * @returns 1 when it does, 0 when not
 */
static int holds_fork_alone(const struct afp_file_info *fp)
{
    return fp->forkid == AFP_FAKE_SERVER_FORK && !fp->largelist_next
           && !fp->writable && !fp->dirty && !fp->resource && !fp->did
           && fp->basename[0] == '\0';
}

/*!
 * @brief A link's read lists a record holding its fork alone
 *
 * It is listed while the read is in flight, whatever the stack held before.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_readlink_record_holds_the_fork_alone(void)
{
    struct afp_fake_server fake;
    char target[64];
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.inspect = afpReadExt;
    fake.read_data = "target";

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    afp_fake_server_dirty_stack();
    rc = ml_readlink(&fake.volume, "/link", target, sizeof(target));
    afp_fake_server_stop(&fake);

    if (rc != 0 || strcmp(target, "target") != 0) {
        return 2;
    }

    if (fake.inspected != 1) {
        return 3;
    }

    return holds_fork_alone(&fake.listed) ? 0 : 4;
}

/*!
 * @brief A link's creation lists a record holding its fork alone
 *
 * It is listed while the link's target is written.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_symlink_record_holds_the_fork_alone(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.inspect = afpWriteExt;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    afp_fake_server_dirty_stack();
    rc = ml_symlink(&fake.volume, "target", "/link");
    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    if (fake.inspected != 1) {
        return 3;
    }

    return holds_fork_alone(&fake.listed) ? 0 : 4;
}

/*!
 * @brief A resource fork's truncation lists a record holding its fork
 *
 * It is listed while the pseudo-file's fork is truncated.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_truncate_record_holds_the_fork_alone(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.inspect = afpSetForkParms;
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    afp_fake_server_dirty_stack();
    rc = appledouble_truncate(&fake.volume, "/.AppleDouble/file", 0);
    afp_fake_server_stop(&fake);

    if (rc != 1) {
        return 2;
    }

    if (fake.inspected != 1) {
        return 3;
    }

    return holds_fork_alone(&fake.listed) ? 0 : 4;
}

/*!
 * @brief Read a link from a fake server whose link target is data
 *
 * @param[in]  data    the target the server holds
 * @param[out] target  receives the target
 * @param[in]  size    capacity of target
 * @returns the call's result, or 1 when the server cannot start
 */
static int read_link(const char *data, char *target, size_t size)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.read_data = data;

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_readlink(&fake.volume, "/link", target, size);
    afp_fake_server_stop(&fake);
    return rc;
}

/*!
 * @brief A link target filling the buffer is cut to fit it
 *
 * The cut leaves room for the terminator.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_target_filling_the_buffer_is_cut(void)
{
    char target[8];
    int rc = read_link("abcdefgh", target, sizeof(target));

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return strcmp(target, "abcdefg") == 0 ? 0 : 3;
}

/*!
 * @brief A link target whose precomposed form fits comes back whole
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_target_that_fits_precomposed_is_whole(void)
{
    char target[8];
    int rc = read_link("e\xCC\x81" "e\xCC\x81" "xy", target,
                       sizeof(target));

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return strcmp(target, "\xC3\xA9" "\xC3\xA9" "xy") == 0 ? 0 : 3;
}

/*!
 * @brief A UTF-8 target that fills the buffer loses the whole character the
 *        cut would split
 *
 * The supplementary character sends the target down the verbatim path,
 * which copies the bytes as they are.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_cut_target_keeps_whole_characters(void)
{
    char target[8];
    int rc = read_link("\360\237\223\201ab\303\251", target, sizeof(target));

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return strcmp(target, "\360\237\223\201ab") == 0 ? 0 : 3;
}

/*!
 * @brief A link target keeps its separators
 *
 * A colon in a name stays a colon.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_target_keeps_its_separators(void)
{
    char target[64];
    int rc = read_link("../a:b/c", target, sizeof(target));

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return strcmp(target, "../a:b/c") == 0 ? 0 : 3;
}

/*!
 * @brief A malformed sequence that takes a separator into its replacement
 *        leaves the target's colons as they convert
 *
 * "\xE9/x" decodes to one '*', so the target has one separator fewer than
 * the server holds, and none of them is paired with the wrong one.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_malformed_target_keeps_its_colons(void)
{
    char target[64];
    int rc = read_link("caf\xE9/x:y", target, sizeof(target));

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return strcmp(target, "caf*:y") == 0 ? 0 : 3;
}

/*!
 * @brief A link reads on a volume that refuses an open for writing
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_link_reads_on_a_read_only_volume(void)
{
    struct afp_fake_server fake;
    char target[64];
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.read_only = 1;
    fake.read_data = "target";

    if (afp_fake_server_start(&fake) != 0) {
        return 1;
    }

    rc = ml_readlink(&fake.volume, "/link", target, sizeof(target));
    afp_fake_server_stop(&fake);

    if (rc != 0) {
        return 2;
    }

    return strcmp(target, "target") == 0 ? 0 : 3;
}

/*!
 * @brief Make a link on a fake server
 *
 * @param[in,out] fake  the server, which records the requests it took
 * @returns the call's result, or 1 when the server cannot start
 */
static int make_link(struct afp_fake_server *fake)
{
    int rc;

    if (afp_fake_server_start(fake) != 0) {
        return 1;
    }

    rc = ml_symlink(&fake->volume, "../target", "/link");
    afp_fake_server_stop(fake);
    return rc;
}

/*!
 * @brief A link over a file fails and leaves the file
 *
 * The create is a soft one, and nothing follows it.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_link_over_a_file_leaves_it(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.name_taken = 1;
    rc = make_link(&fake);

    if (rc != -EEXIST) {
        return rc == 1 ? 1 : 2;
    }

    return fake.ncommands == 1 ? 0 : 3;
}

/*!
 * @brief A link whose write is refused is deleted
 *
 * Its fork closes before the file its create made is deleted.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_write_deletes_the_link(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpWriteExt;
    rc = make_link(&fake);

    if (rc != -EACCES) {
        return rc == 1 ? 1 : 2;
    }

    if (fake.ncommands != 5) {
        return 3;
    }

    return fake.commands[3] == afpCloseFork && fake.commands[4] == afpDelete
           ? 0 : 4;
}

/*!
 * @brief A link whose Finder information is refused is deleted
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_refused_finder_info_deletes_the_link(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpSetFileDirParms;
    rc = make_link(&fake);

    if (rc != -EPERM) {
        return rc == 1 ? 1 : 2;
    }

    if (fake.ncommands != 6) {
        return 3;
    }

    return fake.commands[5] == afpDelete ? 0 : 4;
}

/*!
 * @brief A link the server makes whole stays
 *
 * Five requests go out, and none is a delete.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_whole_link_stays(void)
{
    struct afp_fake_server fake;
    int rc;
    memset(&fake, 0, sizeof(fake));
    rc = make_link(&fake);

    if (rc != 0) {
        return rc == 1 ? 1 : 2;
    }

    return fake.ncommands == 5 ? 0 : 3;
}

/*!
 * @brief Run the open-fork record and link checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    TEST_int(afp_main_quick_startup(NULL), 0, "the event loop starts");
    TEST_int(utest_refused_truncation_at_open_unlists_its_record(), 0,
             "a truncation refused at open leaves no record listed");
    TEST_int(utest_readlink_record_holds_the_fork_alone(), 0,
             "a link's read lists a record holding its fork alone");
    TEST_int(utest_symlink_record_holds_the_fork_alone(), 0,
             "a link's creation lists a record holding its fork alone");
    TEST_int(utest_truncate_record_holds_the_fork_alone(), 0,
             "a resource fork's truncation lists a record holding its fork");
    TEST_int(utest_target_filling_the_buffer_is_cut(), 0,
             "a link target filling the buffer is cut to fit it");
    TEST_int(utest_target_that_fits_precomposed_is_whole(), 0,
             "a link target whose precomposed form fits comes back whole");
    TEST_int(utest_cut_target_keeps_whole_characters(), 0,
             "a cut UTF-8 link target keeps whole characters");
    TEST_int(utest_target_keeps_its_separators(), 0,
             "a link target keeps its separators");
    TEST_int(utest_malformed_target_keeps_its_colons(), 0,
             "a malformed link target keeps the colons it converts to");
    TEST_int(utest_link_reads_on_a_read_only_volume(), 0,
             "a link reads on a volume that refuses an open for writing");
    TEST_int(utest_link_over_a_file_leaves_it(), 0,
             "a link over a file fails and leaves the file");
    TEST_int(utest_refused_write_deletes_the_link(), 0,
             "a link whose write is refused is deleted");
    TEST_int(utest_refused_finder_info_deletes_the_link(), 0,
             "a link whose Finder information is refused is deleted");
    TEST_int(utest_whole_link_stays(), 0,
             "a link the server makes whole stays");
    return test_tap_finish();
}
