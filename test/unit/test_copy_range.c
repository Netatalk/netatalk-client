/*
 * test_copy_range.c: the limit of a copy through the client, and the target
 * a copy on the server leaves, against the fake AFP server
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of ml_copy_range_within()'s limit and of a copy on the
 *        server, against the fake AFP server
 */

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "afp_fake_server.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/lowlevel.h"
#include "lib/midlevel.h"
#include "tap.h"

/* A source the server copies, its data fork a whole MiB */
#define SOURCE_DATA (1024U * 1024U)

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
 * @brief Close an open fork and free its record
 *
 * @param[in]     fake  the fake server, running
 * @param[in,out] fp    the record, or NULL
 */
static void close_fork(struct afp_fake_server *fake, struct afp_file_info *fp)
{
    if (fp) {
        ml_close(&fake->volume, fp);
        free(fp);
    }
}

/*!
 * @brief Run the copy checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    struct afp_fake_server fake;
    struct afp_file_info *in = NULL;
    struct afp_file_info *out = NULL;
    test_tap_init(argc, argv);
    /* A write to a server end that has gone must fail, not end the test. */
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    CHECK(afp_main_quick_startup(NULL) == 0);
    /* A copy through the client moves at most the caller's limit, to the
     * byte, and without one, sixteen quanta of a longer source. */
    memset(&fake, 0, sizeof(fake));
    fake.read_file = 1;
    fake.data_length = 32U * 1024U;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->quantum = 1024;
    CHECK(ml_open(&fake.volume, "/source", O_RDONLY, &in) == 0);
    CHECK(ml_open(&fake.volume, "/target", O_WRONLY, &out) == 0);

    if (in && out) {
        CHECK(ml_copy_range_within(&fake.volume, in, 0, out, 0, 32U * 1024U,
                                   1500) == 1500);
        CHECK(ml_copy_range_within(&fake.volume, in, 0, out, 0, 32U * 1024U,
                                   SIZE_MAX) == 16 * 1024);
    }

    close_fork(&fake, in);
    close_fork(&fake, out);
    afp_fake_server_stop(&fake);
    /* A copy on the server reopens its new target with the deny mode the
     * target opened with, whatever the thread's mode at the copy. */
    in = NULL;
    out = NULL;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 7;
    fake.data_length = SOURCE_DATA;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->server_type = AFPC_SERVER_TYPE_NETATALK;
    fake.server->flags = kSupportsCopyfile;
    CHECK(ml_open(&fake.volume, "/source", O_RDONLY, &in) == 0);
    ll_set_thread_deny_write(1);
    CHECK(ml_open(&fake.volume, "/target", O_WRONLY | O_CREAT, &out) == 0);
    ll_set_thread_deny_write(0);
    /* The copy reads the source's parameters, then the target's, which a
     * new file has empty. */
    fake.params_empty = (uint16_t)(1U << (fake.params_answers + 1));

    if (in && out) {
        /* The fake reports one length for every open; a new file's is 0. */
        out->size = 0;
        (void) ml_copy_range_within(&fake.volume, in, 0, out, 0, SOURCE_DATA,
                                    SIZE_MAX);
    }

    close_fork(&fake, in);
    close_fork(&fake, out);
    afp_fake_server_stop(&fake);
    CHECK(count_commands(&fake, afpCopyFile) == 1);
    CHECK(count_commands(&fake, afpOpenFork) == 3);
    CHECK(afp_fake_server_open_access(&fake) & AFP_OPENFORK_DENYWRITE);
    /* On a volume with Unix privileges, a source its owner cannot write goes
     * through the client on an Apple server, whose copy may keep that mode,
     * and copies on Netatalk's. */
    static const struct {
        enum afpc_server_type type;
        unsigned int copies;
    } modes[] = {
        { AFPC_SERVER_TYPE_MACINTOSH, 0 },
        { AFPC_SERVER_TYPE_NETATALK, 1 },
    };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        in = NULL;
        out = NULL;
        memset(&fake, 0, sizeof(fake));
        fake.file_params = 1;
        fake.params_cnid = 7;
        fake.params_mode = 0100444;
        fake.params_access = kUserIsOwner | kUserCanRead | kUserCanWrite;
        fake.data_length = SOURCE_DATA;
        CHECK(afp_fake_server_start(&fake) == 0);
        fake.server->server_type = modes[i].type;
        fake.server->flags = kSupportsCopyfile;
        fake.volume.extra_flags |= VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
        CHECK(ml_open(&fake.volume, "/source", O_RDONLY, &in) == 0);
        CHECK(ml_open(&fake.volume, "/target", O_WRONLY | O_CREAT, &out) == 0);
        fake.params_empty = (uint16_t)(1U << (fake.params_answers + 1));

        if (in && out) {
            out->size = 0;
            (void) ml_copy_range_within(&fake.volume, in, 0, out, 0,
                                        SOURCE_DATA, SIZE_MAX);
        }

        close_fork(&fake, in);
        close_fork(&fake, out);
        afp_fake_server_stop(&fake);
        CHECK(count_commands(&fake, afpCopyFile) == modes[i].copies);
    }

    /* A copy whose reopen the server refuses, as Netatalk 2 refuses one that
     * kept a read-only source's mode, goes: a new file takes its name, and
     * the client copies into it. */
    in = NULL;
    out = NULL;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 7;
    fake.read_file = 1;
    fake.data_length = SOURCE_DATA;
    fake.refuse = afpOpenFork;
    fake.refusal = kFPLockErr;
    fake.refuse_skip = 2;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->quantum = 1024;
    fake.server->server_type = AFPC_SERVER_TYPE_NETATALK;
    fake.server->flags = kSupportsCopyfile;
    CHECK(ml_open(&fake.volume, "/source", O_RDONLY, &in) == 0);
    CHECK(ml_open(&fake.volume, "/target", O_WRONLY | O_CREAT, &out) == 0);
    fake.params_empty = (uint16_t)(1U << (fake.params_answers + 1));

    if (in && out) {
        out->size = 0;
        CHECK(ml_copy_range_within(&fake.volume, in, 0, out, 0, SOURCE_DATA,
                                   SIZE_MAX) > 0);
        CHECK(out->forkid != 0);
    }

    close_fork(&fake, in);
    close_fork(&fake, out);
    afp_fake_server_stop(&fake);
    CHECK(count_commands(&fake, afpCopyFile) == 1);
    CHECK(count_commands(&fake, afpDelete) == 2);
    CHECK(count_commands(&fake, afpOpenFork) == 4);
    /* A source a byte short of the offload's threshold copies through the
     * client, though the server could copy it. */
    in = NULL;
    out = NULL;
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 7;
    fake.read_file = 1;
    fake.data_length = SOURCE_DATA - 1;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->quantum = 1024;
    fake.server->server_type = AFPC_SERVER_TYPE_NETATALK;
    fake.server->flags = kSupportsCopyfile;
    CHECK(ml_open(&fake.volume, "/source", O_RDONLY, &in) == 0);
    CHECK(ml_open(&fake.volume, "/target", O_WRONLY | O_CREAT, &out) == 0);
    fake.params_empty = (uint16_t)(1U << (fake.params_answers + 1));

    if (in && out) {
        out->size = 0;
        CHECK(ml_copy_range_within(&fake.volume, in, 0, out, 0,
                                   SOURCE_DATA - 1, SIZE_MAX) > 0);
    }

    close_fork(&fake, in);
    close_fork(&fake, out);
    afp_fake_server_stop(&fake);
    CHECK(count_commands(&fake, afpCopyFile) == 0);
    return test_tap_finish();
}
