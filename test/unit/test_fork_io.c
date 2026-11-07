/*
    test_fork_io.c: the reads, writes and lengths of an open fork

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/did.h"
#include "lib/lowlevel.h"
#include "tap.h"

/*!
 * @brief Check a fork's AFP 2 bounds and what a failed request reads
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct afp_versions afp2 = { "AFP2.2", 22 };
    struct afp_server server;
    struct afp_volume volume;
    struct afp_file_info fork;
    char data[16];
    size_t written;
    test_tap_init(argc, argv);
    memset(&server, 0, sizeof(server));
    memset(&volume, 0, sizeof(volume));
    dir_cache_init(&volume);
    pthread_mutex_init(&volume.open_forks_mutex, NULL);
    memset(&fork, 0, sizeof(fork));
    memset(data, 'x', sizeof(data));
    /* The volume's server is not connected: every request fails before it
     * leaves, so only a refusal decided earlier shows. */
    server.using_version = &afp2;
    server.quantum = 65536;
    volume.server = &server;
    volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
    fork.forkid = 1;
    /* AFP 2 offsets, counts and lengths are signed 32 bits: no write ends
     * past INT32_MAX. */
    written = 1;
    CHECK(ll_write(&volume, data, sizeof(data), (off_t) INT32_MAX - 8, 0,
                   &fork, &written) == -EFBIG);
    CHECK(written == 0);
    written = 1;
    CHECK(ll_write(&volume, data, sizeof(data),
                   (off_t) INT32_MAX + 1 - (off_t) sizeof(data), 0, &fork,
                   &written) == -EFBIG);
    CHECK(written == 0);
    /* A write that ends at INT32_MAX is the server's to answer; one that
     * went without an answer forgets its file's parameters and leaves the
     * fork dirty, as it may have landed. */
    {
        struct afp_dirent e;
        memset(&e, 0, sizeof(e));
        e.cnid = 5;
        e.valid = kFPNodeIDBit;
        fork.did = AFP_ROOT_DID;
        memcpy(fork.name, "w", 2);
        dir_cache_upsert(&volume, AFP_ROOT_DID, "w", &e,
                         dir_cache_changes(&volume));
        CHECK(ll_write(&volume, data, sizeof(data),
                       (off_t) INT32_MAX - (off_t) sizeof(data), 0, &fork,
                       &written) == -EIO);
        CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "w", &e)
              == DIR_CACHE_DID && fork.dirty == 1);
        fork.dirty = 0;
    }
    /* A read reaches offset INT32_MAX; past it, a fork the handle knows to
     * be longer fails the read without a request. */
    {
        char buf[16];
        int eof = 0;
        fork.size = (uint64_t) INT32_MAX + 1 + sizeof(buf);
        CHECK(ll_read(&volume, buf, sizeof(buf), (off_t) INT32_MAX, &fork,
                      &eof) == -EIO);
        CHECK(ll_read(&volume, buf, sizeof(buf), (off_t) INT32_MAX + 1,
                      &fork, &eof) == -EOVERFLOW);
        CHECK(eof == 0);
        fork.size = 0;
    }
    /* A length of INT32_MAX is the server's to answer. */
    CHECK(ll_setfork_size(&volume, &fork, INT32_MAX) == EIO);
    CHECK(ll_setfork_size(&volume, &fork, (uint64_t) INT32_MAX + 1) == EFBIG);
    /* A read whose request fails reads nothing and says so: an empty
     * result would be the end of the file. */
    {
        char buf[16];
        int eof = 0;
        CHECK(ll_read(&volume, buf, sizeof(buf), 0, &fork, &eof) == -EIO);
        CHECK(eof == 0);
    }
    pthread_mutex_destroy(&volume.open_forks_mutex);
    dir_cache_destroy(&volume);
    return test_tap_finish();
}
