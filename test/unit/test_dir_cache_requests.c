/*
 * test_dir_cache_requests.c: the requests the directory cache saves,
 * counted at a fake server through the mid level's calls alone
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests counting the requests the directory cache saves
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "afp_fake_server.h"
#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/mapping.h"
#include "lib/midlevel.h"
#include "tap.h"

/*!
 * @brief Swallow a log message
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
 * @brief Count a command among the requests a fake server recorded
 *
 * @param[in] fake     the fake server, stopped
 * @param[in] command  the AFP command
 * @returns the count
 */
static unsigned int count_of(const struct afp_fake_server *fake,
                             uint8_t command)
{
    unsigned int n = 0;

    for (unsigned int i = 0; i < fake->ncommands && i < 16U; i++) {
        n += fake->commands[i] == command;
    }

    return n;
}

/*!
 * @brief Count the requests the directory cache saves, without its header
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afp_fake_server fake;
    struct stat st;
    test_tap_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    libafpclient_register(&quiet_client);
    CHECK(afp_main_quick_startup(NULL) == 0);
    /* A second stat of a file costs no request, and an unlink after them
     * sends FPDelete alone. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 50;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.volume.mapping = AFP_MAPPING_COMMON;
    CHECK(ml_getattr(&fake.volume, "/f", &st) == 0);
    CHECK(ml_getattr(&fake.volume, "/f", &st) == 0);
    CHECK(ml_unlink(&fake.volume, "/f") == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && count_of(&fake, afpGetFileDirParms) == 1
          && count_of(&fake, afpDelete) == 1);
    /* A move into a folder carries the open fork's record to the folder. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 95;
    CHECK(afp_fake_server_start(&fake) == 0);
    {
        struct afp_file_info *fp = NULL;
        CHECK(ml_open(&fake.volume, "/mf", O_RDONLY, &fp) == 0);

        if (fp) {
            CHECK(ml_rename(&fake.volume, "/mf", "/dst") == 0);
            CHECK(fp->did == 95);
            ml_close(&fake.volume, fp);
            free(fp);
        }
    }
    afp_fake_server_stop(&fake);
    /* The size and then the value of an attribute cost one FPGetExtAttr. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 60;
    fake.xattr_value = "value";
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.volume.mapping = AFP_MAPPING_COMMON;
    fake.volume.attributes |= kSupportsExtAttrs;
    {
        char value[8];
        CHECK(ml_getattr(&fake.volume, "/x", &st) == 0);
        CHECK(ml_getxattr(&fake.volume, "/x", "user.k", NULL, 0) == 5);
        CHECK(ml_getxattr(&fake.volume, "/x", "user.k", value,
                          sizeof(value)) == 5
              && memcmp(value, "value", 5) == 0);
    }
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && count_of(&fake, afpGetExtAttr) == 1);
    return test_tap_finish();
}
