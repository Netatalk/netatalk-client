/*
 * test_fork_locking.c: the locks and share modes of fork opens, against
 * the fake AFP server
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of fork locks and share modes against the fake AFP server
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/forklist.h"
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

/*!
 * @brief Count the requests of one command the fake server received
 *
 * @param[in] fake     the fake server, stopped
 * @param[in] command  the AFP command
 * @returns how many of the recorded requests are command
 */
static unsigned int count_commands(const struct afp_fake_server *fake,
                                   uint8_t command)
{
    unsigned int count = 0;

    for (unsigned int i = 0; i < fake->ncommands; i++) {
        if (fake->commands[i] == command) {
            count++;
        }
    }

    return count;
}

/*!
 * @brief Open a file on a started fake server, write 16 bytes to it and
 *        close it
 *
 * @param[in,out] fake     the fake server
 * @param[out]    written  the bytes ll_write() reports
 * @param[out]    elapsed  the milliseconds the write took
 * @returns ll_write()'s result, or -1 when the open fails
 */
static int write_file(struct afp_fake_server *fake, size_t *written,
                      long *elapsed)
{
    struct afp_file_info *fp = NULL;
    struct timespec start;
    struct timespec end;
    char data[16] = "";
    int ret;
    fake->server->quantum = 65536;

    if (ml_open(&fake->volume, "/file", O_RDWR, &fp) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    ret = ll_write(&fake->volume, data, sizeof(data), 0, 0, fp, written);
    clock_gettime(CLOCK_MONOTONIC, &end);
    *elapsed = (end.tv_sec - start.tv_sec) * 1000
               + (end.tv_nsec - start.tv_nsec) / 1000000;
    ml_close(&fake->volume, fp);
    free(fp);
    return ret;
}

/*!
 * @brief Give the access mode of the last open the fake server received
 *
 * @param[in] fake  the fake server, stopped
 * @returns the access mode, or 0 when it received no open
 */
static unsigned int open_access(const struct afp_fake_server *fake)
{
    unsigned int i = fake->ncommands;

    while (i > 0) {
        i--;

        if (fake->commands[i] == afpOpenFork && fake->lengths[i] >= 12) {
            return (unsigned int) fake->requests[i][10] << 8
                   | fake->requests[i][11];
        }
    }

    return 0;
}

/*!
 * @brief Check the locks and share modes of the library against the fake
 *        AFP server
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afp_fake_server fake;
    size_t written;
    long elapsed;
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    CHECK(afp_main_quick_startup(NULL) == 0);
    {
        /* A range another user holds stays held: the write fails with EBUSY
         * after ten tries, which 50 ms apart take far less than the five
         * seconds allowed here. */
        memset(&fake, 0, sizeof(fake));
        fake.refuse = afpByteRangeLockExt;
        fake.refusal = kFPLockErr;
        written = 1;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == -EBUSY);
        afp_fake_server_stop(&fake);
        CHECK(written == 0);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 10);
        CHECK(count_commands(&fake, afpWriteExt) == 0);
        CHECK(elapsed < 5000);
    }
    {
        /* A server with no lock left is ENOLCK after the same tries. */
        memset(&fake, 0, sizeof(fake));
        fake.refuse = afpByteRangeLockExt;
        fake.refusal = kFPNoMoreLocks;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == -ENOLCK);
        afp_fake_server_stop(&fake);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 10);
        CHECK(count_commands(&fake, afpWriteExt) == 0);
    }
    {
        /* Any other answer is final: one try, EIO. */
        memset(&fake, 0, sizeof(fake));
        fake.refuse = afpByteRangeLockExt;
        fake.refusal = kFPParamErr;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == -EIO);
        afp_fake_server_stop(&fake);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 1);
        CHECK(count_commands(&fake, afpWriteExt) == 0);
    }
    {
        /* A range that is free is locked, written and released. */
        static const uint8_t order[] = {
            afpOpenFork, afpByteRangeLockExt, afpWriteExt,
            afpByteRangeLockExt, afpCloseFork
        };
        memset(&fake, 0, sizeof(fake));
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == 0);
        afp_fake_server_stop(&fake);
        CHECK(written == 16);
        CHECK(fake.ncommands == sizeof(order)
              && memcmp(fake.commands, order, sizeof(order)) == 0);
    }
    {
        /* A volume that asks for no locks sends none. */
        memset(&fake, 0, sizeof(fake));
        fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == 0);
        afp_fake_server_stop(&fake);
        CHECK(written == 16);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 0);
    }
    {
        /* A write that a lock of another user refuses is EBUSY. */
        memset(&fake, 0, sizeof(fake));
        fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        fake.refuse = afpWriteExt;
        fake.refusal = kFPLockErr;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == -EBUSY);
        afp_fake_server_stop(&fake);
    }
    {
        /* A write whose unlock the server refuses, once its data has
         * landed, sends that unlock once and succeeds. */
        memset(&fake, 0, sizeof(fake));
        fake.refuse_unlock = 1;
        CHECK(afp_fake_server_start(&fake) == 0);
        CHECK(write_file(&fake, &written, &elapsed) == 0);
        afp_fake_server_stop(&fake);
        CHECK(written == 16);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 2);
    }
    {
        /* A write of nothing sends nothing between the open and the
         * close. */
        static const uint8_t order[] = { afpOpenFork, afpCloseFork };
        struct afp_file_info *fp = NULL;
        memset(&fake, 0, sizeof(fake));
        CHECK(afp_fake_server_start(&fake) == 0);
        fake.server->quantum = 65536;
        CHECK(ml_open(&fake.volume, "/file", O_RDWR, &fp) == 0);
        written = 1;
        CHECK(ll_write(&fake.volume, "", 0, 0, 0, fp, &written) == 0);
        CHECK(written == 0);
        ml_close(&fake.volume, fp);
        free(fp);
        afp_fake_server_stop(&fake);
        CHECK(fake.ncommands == sizeof(order)
              && memcmp(fake.commands, order, sizeof(order)) == 0);
    }
    {
        /* A read takes no lock, whether the volume locks writes or not. */
        struct afp_file_info *fp = NULL;
        char buf[16];
        int eof = 0;
        memset(&fake, 0, sizeof(fake));
        CHECK(afp_fake_server_start(&fake) == 0);
        fake.server->quantum = 65536;
        CHECK(ml_open(&fake.volume, "/file", O_RDONLY, &fp) == 0);
        CHECK(ll_read(&fake.volume, buf, sizeof(buf), 0, fp, &eof) == 0);
        CHECK(eof == 1);
        ml_close(&fake.volume, fp);
        free(fp);
        afp_fake_server_stop(&fake);
        CHECK(count_commands(&fake, afpReadExt) == 1);
        CHECK(count_commands(&fake, afpByteRangeLockExt) == 0);
    }
    {
        /* A volume or a thread that asks for it opens a file for writing
         * with the deny-write mode, as reading and writing; a file opened
         * for reading denies nothing, and so does an open neither asks
         * for. */
        static const struct {
            unsigned int volume_flags;
            int thread_deny;
            int open_flags;
            unsigned int access;
        } opens[] = {
            {
                VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, O_RDWR,
                AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE
                | AFP_OPENFORK_DENYWRITE
            },
            {
                VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, O_WRONLY,
                AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE
                | AFP_OPENFORK_DENYWRITE
            },
            {
                0, 1, O_RDWR,
                AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE
                | AFP_OPENFORK_DENYWRITE
            },
            {
                VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, O_RDONLY,
                AFP_OPENFORK_ALLOWREAD
            },
            { 0, 1, O_RDONLY, AFP_OPENFORK_ALLOWREAD },
            {
                0, 0, O_RDWR,
                AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE
            },
        };

        for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++) {
            struct afp_file_info *fp = NULL;
            memset(&fake, 0, sizeof(fake));
            fake.volume.extra_flags = opens[i].volume_flags;
            CHECK(afp_fake_server_start(&fake) == 0);
            ll_set_thread_deny_write(opens[i].thread_deny);
            CHECK(ml_open(&fake.volume, "/file", opens[i].open_flags, &fp)
                  == 0);
            ll_set_thread_deny_write(0);
            ml_close(&fake.volume, fp);
            free(fp);
            afp_fake_server_stop(&fake);
            CHECK(open_access(&fake) == opens[i].access);
        }
    }
    {
        /* An open that a deny conflict refuses is EBUSY. On a volume that
         * denies a second writer and holds another writer of the file's
         * fork, whose release may still be on its way, an open for writing
         * takes ten tries; every other open takes one: without that writer,
         * beside a reader or the other fork's writer, under a thread's mode,
         * on a volume that does not deny, and for reading. */
        enum { NONE, WRITER, READER, RESOURCE_WRITER };
        static const struct {
            unsigned int volume_flags;
            int thread_deny;
            int other_fork;
            int open_flags;
            unsigned int tries;
        } refusals[] = {
            { VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, WRITER, O_RDWR, 10 },
            { VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, NONE, O_RDWR, 1 },
            { VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, READER, O_RDWR, 1 },
            { VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, RESOURCE_WRITER, O_RDWR, 1 },
            { 0, 1, WRITER, O_RDWR, 1 },
            { 0, 0, WRITER, O_RDWR, 1 },
            { VOLUME_EXTRA_FLAGS_DENY_WRITE, 0, WRITER, O_RDONLY, 1 },
        };

        for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
            struct afp_file_info *fp = NULL;
            struct afp_file_info other;
            memset(&fake, 0, sizeof(fake));
            memset(&other, 0, sizeof(other));
            other.did = AFP_ROOT_DID;
            memcpy(other.basename, "file", sizeof("file"));
            other.writable = refusals[i].other_fork != READER;
            other.resource = refusals[i].other_fork == RESOURCE_WRITER;
            fake.volume.extra_flags = refusals[i].volume_flags;
            fake.refuse = afpOpenFork;
            fake.refusal = kFPDenyConflict;
            CHECK(afp_fake_server_start(&fake) == 0);

            if (refusals[i].other_fork) {
                add_opened_fork(&fake.volume, &other);
            }

            ll_set_thread_deny_write(refusals[i].thread_deny);
            CHECK(ml_open(&fake.volume, "/file", refusals[i].open_flags, &fp)
                  == -EBUSY);
            ll_set_thread_deny_write(0);

            if (refusals[i].other_fork) {
                remove_opened_fork(&fake.volume, &other);
            }

            afp_fake_server_stop(&fake);
            CHECK(count_commands(&fake, afpOpenFork) == refusals[i].tries);
        }
    }
    {
        /* The file that an open or a new link made stays when a deny
         * conflict refuses its open, as another client holds it open;
         * another refusal removes it. */
        static const struct {
            int link;
            int32_t refusal;
            unsigned int deletes;
        } creates[] = {
            { 0, kFPDenyConflict, 0 },
            { 0, kFPAccessDenied, 1 },
            { 1, kFPDenyConflict, 0 },
            { 1, kFPAccessDenied, 1 },
        };

        for (size_t i = 0; i < sizeof(creates) / sizeof(creates[0]); i++) {
            struct afp_file_info *fp = NULL;
            memset(&fake, 0, sizeof(fake));
            fake.refuse = afpOpenFork;
            fake.refusal = creates[i].refusal;
            CHECK(afp_fake_server_start(&fake) == 0);
            CHECK((creates[i].link
                   ? ml_symlink(&fake.volume, "target", "/file")
                   : ml_open(&fake.volume, "/file", O_RDWR | O_CREAT, &fp))
                  < 0);
            afp_fake_server_stop(&fake);
            CHECK(count_commands(&fake, afpCreateFile) == 1);
            CHECK(count_commands(&fake, afpDelete) == creates[i].deletes);
        }
    }
    return test_tap_finish();
}
