/*
 * test_dir_cache.c: the directory cache, against a server that answers
 * nothing and against the fake server
 *
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of the directory cache and the calls that read it
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "afp_fake_server.h"
#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/client.h"
#include "lib/did.h"
#include "lib/forklist.h"
#include "lib/lowlevel.h"
#include "lib/mapping.h"
#include "lib/midlevel.h"
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

/* The cache's clock, set by the test */
static time_t fake_now;

/*!
 * @brief The cache's clock in the test
 *
 * @returns fake_now
 */
static time_t fake_clock(void)
{
    return fake_now;
}

/*!
 * @brief A record with a node ID, a type, a modification date and a length
 *
 * @param[in] cnid   the node ID
 * @param[in] isdir  1 for a directory
 * @param[in] mtime  the modification date
 * @returns the record
 */
static struct afp_dirent record(unsigned int cnid, int isdir, time_t mtime)
{
    struct afp_dirent e;
    memset(&e, 0, sizeof(e));
    e.cnid = cnid;
    e.isdir = (uint8_t) isdir;
    e.mod_ns = afp_ns_from_sec(mtime);
    e.size = 4096;
    e.valid = kFPNodeIDBit | kFPParentDirIDBit | kFPModDateBit
              | kFPExtDataForkLenBit;
    return e;
}

/*!
 * @brief A listing of named records in one directory
 *
 * @param[out] l       receives the listing, which the caller frees
 * @param[in]  parent  the directory's ID
 * @param[in]  names   the names, NULL-terminated
 * @param[in]  first   the first record's node ID; each next one adds 1
 */
static void listing_of(struct afp_listing *l, unsigned int parent,
                       const char *const *names, unsigned int first)
{
    memset(l, 0, sizeof(*l));

    for (unsigned int i = 0; names[i]; i++) {
        size_t len = strlen(names[i]) + 1U;

        if (afp_listing_reserve(l, 1, (uint32_t) len) != 0) {
            return;
        }

        l->entries[l->count] = record(first + i, 0, fake_now - 1000);
        l->entries[l->count].parent_did = parent;
        memcpy(l->pool + l->pool_used, names[i], len);
        afp_listing_commit(l);
    }
}

/* The racing threads' state; torn once a lookup meets a half-stored record */
struct race {
    struct afp_volume *volume;
    unsigned int rounds;
    int torn;
};

/*!
 * @brief Store one file's record over and over, each store a new length
 *        that its date repeats
 *
 * @param[in,out] arg  the race
 * @returns NULL
 */
static void *race_store(void *arg)
{
    struct race *r = arg;

    for (unsigned int k = 1; k <= r->rounds; k++) {
        struct afp_dirent e = record(5, 0, fake_now - 1000);
        e.size = k;
        e.mod_ns = (int64_t) k;
        dir_cache_upsert(r->volume, 10, "race", &e,
                         dir_cache_changes(r->volume));
    }

    return NULL;
}

/*!
 * @brief Forget that file's name over and over
 *
 * @param[in,out] arg  the race
 * @returns NULL
 */
static void *race_remove(void *arg)
{
    struct race *r = arg;

    for (unsigned int k = 0; k < r->rounds; k++) {
        dir_cache_remove(r->volume, 10, "race");
    }

    return NULL;
}

/*!
 * @brief Look that file up over and over: a record with parameters carries
 *        the length and the date of one store
 *
 * @param[in,out] arg  the race
 * @returns NULL
 */
static void *race_look(void *arg)
{
    struct race *r = arg;

    for (unsigned int k = 0; k < r->rounds; k++) {
        struct afp_dirent e;
        unsigned int found = dir_cache_lookup(r->volume, 10, "race", &e);

        if ((found & DIR_CACHE_STAT)
                && (e.size != (uint64_t) e.mod_ns || e.cnid != 5)) {
            r->torn = 1;
        }
    }

    return NULL;
}

/* The answer the held fetch got */
static int held_ret = -1;

/*!
 * @brief Fetch a folder's record, which the fake server holds until the
 *        test releases it
 *
 * @param[in,out] arg  the fake server
 * @returns NULL
 */
static void *fetch_in_thread(void *arg)
{
    struct afp_fake_server *fake = arg;
    struct afp_dirent held_e;
    held_ret = dir_cache_fetch(&fake->volume, AFP_ROOT_DID, "held", &held_e);
    return NULL;
}

#ifdef __GLIBC__
/* The lock two threads contend for, and the phase: 1 once the reader holds
 * it, 2 once the test lets it go */
struct lock_race {
    pthread_rwlock_t lock;
    int phase;
};

/*!
 * @brief Sleep for some milliseconds
 *
 * @param[in] ms  how many
 */
static void sleep_ms(int ms)
{
    struct timespec ts = { 0, (long) ms * 1000000L };
    nanosleep(&ts, NULL);
}

/*!
 * @brief Hold the lock for reading until the test lets it go
 *
 * @param[in,out] arg  the lock race
 * @returns NULL
 */
static void *hold_read_lock(void *arg)
{
    struct lock_race *lr = arg;
    pthread_rwlock_rdlock(&lr->lock);
    __atomic_store_n(&lr->phase, 1, __ATOMIC_RELEASE);

    while (__atomic_load_n(&lr->phase, __ATOMIC_ACQUIRE) < 2) {
        sleep_ms(1);
    }

    pthread_rwlock_unlock(&lr->lock);
    return NULL;
}

/*!
 * @brief Take the lock for writing, which waits for the reader
 *
 * @param[in,out] arg  the lock race
 * @returns NULL
 */
static void *take_write_lock(void *arg)
{
    struct lock_race *lr = arg;
    pthread_rwlock_wrlock(&lr->lock);
    pthread_rwlock_unlock(&lr->lock);
    return NULL;
}
#endif

/*!
 * @brief Check the cache's lookups, lives, change counts and eviction
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    static const char *const listed[] = { "h", "i", NULL };
    static const char *const relisted[] = { "h", NULL };
    static const char *const three[] = { "h", "i", "j", NULL };
    struct afp_versions afp34 = { "AFP3.4", 34 };
    struct afp_server *server;
    struct afp_volume volume;
    struct afp_fake_server fake;
    struct afp_file_info fork;
    struct afp_listing l;
    struct afp_dirent e;
    struct ml_target t;
    struct stat st;
    unsigned int did;
    uint64_t sent;
    uint64_t before;
    uint64_t serial;
    uint64_t later;
    char name[AFPC_MAX_NAME_BYTES];
    char long_path[AFPC_MAX_NAME_BYTES + 8];
    test_tap_init(argc, argv);
    libafpclient_register(&quiet_client);
    CHECK(afp_main_quick_startup(NULL) == 0);
    /* A suspended server: a request fails with EIO before it leaves. */
    server = afp_server_init(NULL);
    CHECK(server != NULL);
    server->using_version = &afp34;
    afp_server_set_suspended(server, 1);
    memset(&volume, 0, sizeof(volume));
    dir_cache_init(&volume);
    pthread_mutex_init(&volume.open_forks_mutex, NULL);
    afp_open_lock_init(&volume.open_lock);
    volume.cache.clock = fake_clock;
    volume.cache.wall = fake_clock;
    fake_now = 1700000000;
    volume.server = server;
    volume.path_encoding = kFPUTF8Name;
    volume.signature = AFP_VOL_FIXED;
    volume.mapping = AFP_MAPPING_COMMON;
    /* An empty cache answers for nothing; a resolution needs the server. */
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "a", &e) == 0);
    CHECK(dir_cache_resolve(&volume, "/a/b", &did, name) == -EIO);
    /* Two directories known: a path through them resolves without a
     * request, and the root resolves to itself. */
    sent = dir_cache_changes(&volume);
    e = record(10, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "a", &e, sent);
    e = record(11, 1, fake_now - 1000);
    dir_cache_upsert(&volume, 10, "b", &e, sent);
    CHECK(dir_cache_resolve(&volume, "/a/b/c", &did, name) == 0);
    CHECK(did == 11 && strcmp(name, "c") == 0);
    CHECK(dir_cache_resolve(&volume, "/", &did, name) == 0);
    CHECK(did == AFP_ROOT_DID && name[0] == '\0');
    /* A file on the way may be another session's old object: the server
     * says what stands there. */
    e = record(12, 0, fake_now);
    dir_cache_upsert(&volume, 11, "f", &e, sent);
    CHECK(dir_cache_resolve(&volume, "/a/b/f/x", &did, name) == -EIO);
    /* An absence holds to the clock's second after next; on the way, one
     * under a cached ID is asked again, as it may be a moved folder's. */
    dir_cache_insert_negative(&volume, 11, "none", sent);
    CHECK(dir_cache_lookup(&volume, 11, "none", &e) == DIR_CACHE_NEGATIVE);
    CHECK(dir_cache_resolve(&volume, "/a/b/none/x", &did, name) == -EIO);
    dir_cache_insert_negative(&volume, AFP_ROOT_DID, "none", sent);
    CHECK(dir_cache_resolve(&volume, "/none/x", &did, name) == -ENOENT);
    fake_now += 1;
    CHECK(dir_cache_lookup(&volume, 11, "none", &e) == DIR_CACHE_NEGATIVE);
    fake_now += 1;
    CHECK(dir_cache_lookup(&volume, 11, "none", &e) == 0);
    /* A directory made through the cache resolves at once; the path
     * forgotten, its resolution needs the server again. */
    dir_cache_made_dir(&volume, AFP_ROOT_DID, "made", 20,
                       dir_cache_changes(&volume));
    CHECK(dir_cache_resolve(&volume, "/made/x", &did, name) == 0);
    CHECK(did == 20 && strcmp(name, "x") == 0);
    dir_cache_forget_path(&volume, "/made/x");
    CHECK(dir_cache_resolve(&volume, "/made/x", &did, name) == -EIO);
    /* A removal or rename after a create or listing was sent may have moved
     * the directory: its ID stays out, and an entry holding another goes. */
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, AFP_ROOT_DID, "elsewhere");
    dir_cache_made_dir(&volume, AFP_ROOT_DID, "made", 21, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "made", &e) == 0);
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "made", 22,
                         dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, AFP_ROOT_DID, "elsewhere");
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "made", 23, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "made", &e) == 0);
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "made", 24,
                         dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "made", &e) == DIR_CACHE_DID
          && e.cnid == 24);
    /* A reply a removal overtook keeps that identity, though the server
     * gives the folder's type as 0x80. */
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, AFP_ROOT_DID, "elsewhere");
    e = record(24, 0x80, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "made", &e, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "made", &e) == DIR_CACHE_DID
          && e.cnid == 24);
    /* The identity holds 120 s; the parameters a tenth of the object's
     * age, 5 s at least and 60 s at most. */
    CHECK(dir_cache_lookup(&volume, 10, "b", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    fake_now += 56;
    CHECK(dir_cache_lookup(&volume, 10, "b", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    CHECK(dir_cache_lookup(&volume, 11, "f", &e) == DIR_CACHE_DID);
    fake_now += 2;
    CHECK(dir_cache_lookup(&volume, 10, "b", &e) == DIR_CACHE_DID);
    fake_now += 60;
    CHECK(dir_cache_lookup(&volume, 10, "b", &e) == 0);
    CHECK(dir_cache_resolve(&volume, "/a/b/c", &did, name) == -EIO);
    /* The floor: a record 20 s old lives 5 s, not the 2 s a tenth of its
     * age gives, and one dated ahead of the clock lives the floor too. */
    e = record(80, 0, fake_now - 20);
    dir_cache_upsert(&volume, 10, "young", &e, dir_cache_changes(&volume));
    e = record(81, 0, fake_now + 10000);
    dir_cache_upsert(&volume, 10, "ahead", &e, dir_cache_changes(&volume));
    fake_now += 4;
    CHECK(dir_cache_lookup(&volume, 10, "young", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    CHECK(dir_cache_lookup(&volume, 10, "ahead", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    fake_now += 1;
    CHECK(dir_cache_lookup(&volume, 10, "young", &e) == DIR_CACHE_DID);
    CHECK(dir_cache_lookup(&volume, 10, "ahead", &e) == DIR_CACHE_DID);
    /* A change after a request was sent keeps its parameters out, not its
     * identity; one sent after it brings them, in the same second too. */
    fake_now += 1000;
    e = record(11, 1, fake_now - 1000);
    before = dir_cache_changes(&volume);
    dir_cache_upsert(&volume, 10, "b", &e, before);
    dir_cache_invalidate(&volume, 10, "b", 0);
    dir_cache_upsert(&volume, 10, "b", &e, before);
    CHECK(dir_cache_lookup(&volume, 10, "b", &e) == DIR_CACHE_DID);
    dir_cache_upsert(&volume, 10, "b", &e, dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 10, "b", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* A change at a name held as absent or not at all leaves an entry that
     * answers for nothing and keeps out an earlier reply's parameters. */
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, 10, "absent", 0);
    CHECK(dir_cache_lookup(&volume, 10, "absent", &e) == 0);
    e = record(15, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 10, "absent", &e, before);
    CHECK(dir_cache_lookup(&volume, 10, "absent", &e) == DIR_CACHE_DID);
    dir_cache_insert_negative(&volume, 10, "gone", dir_cache_changes(&volume));
    dir_cache_invalidate(&volume, 10, "gone", 0);
    CHECK(dir_cache_lookup(&volume, 10, "gone", &e) == 0);
    /* An absence a reply sent before the change carried stays out. */
    dir_cache_insert_negative(&volume, 10, "gone", before);
    CHECK(dir_cache_lookup(&volume, 10, "gone", &e) == 0);
    dir_cache_remove(&volume, 10, "absent");
    dir_cache_remove(&volume, 10, "gone");
    e = record(11, 1, fake_now - 1000);
    /* A removal keeps out the replies about its folder sent before it, and
     * such a reply drops an entry it contradicts. */
    dir_cache_insert_dir(&volume, 10, "c", 13, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, 10, "b");
    dir_cache_upsert(&volume, 10, "b", &e, before);
    CHECK(dir_cache_lookup(&volume, 10, "b", &e) == 0);
    dir_cache_upsert(&volume, 10, "c", &e, before);
    CHECK(dir_cache_lookup(&volume, 10, "c", &e) == 0);
    dir_cache_upsert(&volume, 10, "b", &e, dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 10, "b", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* A directory's parameters go with a create or a removal in it. */
    dir_cache_remove(&volume, 11, "f");
    CHECK(dir_cache_lookup(&volume, 10, "b", &e) == DIR_CACHE_DID);
    /* The spellings the wire takes as one name are one entry. */
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "caf\xc3\xa9", 50,
                         dir_cache_changes(&volume));
    CHECK(dir_cache_resolve(&volume, "/cafe\xcc\x81/x", &did, name) == 0);
    CHECK(did == 50 && strcmp(name, "x") == 0);
    /* A path that ends in a slash names its last directory by name. */
    CHECK(dir_cache_resolve(&volume, "/cafe\xcc\x81/", &did, name) == 0);
    CHECK(did == AFP_ROOT_DID && strcmp(name, "caf\xc3\xa9") == 0);
    /* The last component comes in both forms, in the cache's spelling; a
     * name the record does not hold is refused. */
    CHECK(ml_resolve(&volume, "/caf\xc3\xa9", &t) == 0);
    CHECK(t.did == AFP_ROOT_DID && strcmp(t.name, "caf\xc3\xa9") == 0
          && strcmp(t.afp_name, "cafe\xcc\x81") == 0);
    CHECK(ml_resolve(&volume, "/cafe\xcc\x81", &t) == 0);
    CHECK(strcmp(t.name, "caf\xc3\xa9") == 0);
    memset(long_path, 'x', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1U] = '\0';
    CHECK(ml_resolve(&volume, long_path, &t) == -ENAMETOOLONG);
    /* So is an over-long wire form; an empty path names nothing, nor does a
     * pathless listing with the AppleDouble view shown. */
    {
        char wide[1 + 341 * 2 + 1];
        wide[0] = '/';

        for (size_t k = 0; k < 341; k++) {
            wide[1 + 2 * k] = '\xc3';
            wide[2 + 2 * k] = '\xa9';
        }

        wide[sizeof(wide) - 1U] = '\0';
        CHECK(ml_resolve(&volume, wide, &t) == -ENAMETOOLONG);
    }
    CHECK(dir_cache_resolve(&volume, "", &did, name) == -ENOENT);
    volume.extra_flags |= VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    memset(&l, 0, sizeof(l));
    CHECK(ml_readdir(&volume, NULL, &l) == -EINVAL);
    volume.extra_flags &= ~VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
    /* A listing's records join under their parent and its end forgets what
     * it did not show; a page a removal overtook only marks and drops. */
    e = record(40, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "d", &e,
                     dir_cache_changes(&volume));
    e = record(39, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "old", &e, dir_cache_changes(&volume));
    e = record(41, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "h", &e, dir_cache_changes(&volume));
    e = record(99, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "i", &e, dir_cache_changes(&volume));
    dir_cache_insert_negative(&volume, 40, "none",
                              dir_cache_changes(&volume));
    serial = dir_cache_listing(&volume);
    listing_of(&l, 40, listed, 41);
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, 40, "x");
    dir_cache_insert_listing(&volume, &l, 0, before, serial);
    CHECK(dir_cache_lookup(&volume, 40, "i", &e) == 0);
    dir_cache_listed(&volume, 40, serial);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) != 0);
    CHECK(dir_cache_lookup(&volume, 40, "old", &e) == 0);
    CHECK(dir_cache_lookup(&volume, 40, "none", &e) == DIR_CACHE_NEGATIVE);
    dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                             serial);
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT) && e.cnid == 41);
    CHECK(ml_getattr(&volume, "/d/h", &st) == 0 && st.st_ino == 41);
    CHECK(dir_cache_lookup(&volume, 40, "i", &e) != 0);
    /* A listing begun later keeps what it showed when an earlier one's
     * page comes after its own, and when the earlier one ends. */
    serial = dir_cache_listing(&volume);
    later = dir_cache_listing(&volume);
    listing_of(&l, 40, relisted, 41);
    dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                             later);
    dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                             serial);
    afp_listing_free(&l);
    dir_cache_listed(&volume, 40, serial);
    CHECK(dir_cache_lookup(&volume, 40, "i", &e) == 0);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) != 0);
    dir_cache_listed(&volume, 40, later);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) != 0);
    listing_of(&l, 40, listed, 41);
    dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                             later);
    afp_listing_free(&l);
    dir_cache_listed(&volume, 40, later);
    CHECK(dir_cache_lookup(&volume, 40, "i", &e) != 0);
    /* Past 31 and past 32 bits of serials, a listing keeps what it showed
     * and forgets an object no listing showed. */
    {
        static const uint64_t starts[] = { UINT32_MAX / 2U + 1U, UINT32_MAX };

        for (size_t k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
            volume.cache.listings = starts[k];
            serial = dir_cache_listing(&volume);
            e = record(45, 0, fake_now - 1000);
            dir_cache_upsert(&volume, 40, "j", &e,
                             dir_cache_changes(&volume));
            listing_of(&l, 40, relisted, 41);
            dir_cache_insert_listing(&volume, &l, 0,
                                     dir_cache_changes(&volume), serial);
            afp_listing_free(&l);
            dir_cache_listed(&volume, 40, serial);
            CHECK(dir_cache_lookup(&volume, 40, "j", &e) == 0
                  && dir_cache_lookup(&volume, 40, "h", &e) != 0);
        }
    }
    /* A name shown for two objects, two spellings the conversion takes as
     * one, is cached for none, a third sighting included. */
    {
        static const char *const thrice[] = { "dup", "dup", "dup", NULL };
        listing_of(&l, 40, thrice, 86);
        dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                                 dir_cache_listing(&volume));
        afp_listing_free(&l);
        CHECK(dir_cache_lookup(&volume, 40, "dup", &e) == 0);
    }
    /* A removal takes every entry of its object, as each spelling of a
     * name on a server that folds case is an entry of its own. */
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "Docs", 88,
                         dir_cache_changes(&volume));
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "docs", 88,
                         dir_cache_changes(&volume));
    dir_cache_remove(&volume, AFP_ROOT_DID, "Docs");
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "docs", &e) == 0);
    /* A folder found empty goes with its objects, and the replies sent
     * before about it or them store nothing; one whose identity the cache
     * does not hold names none. */
    e = record(41, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "h", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    CHECK(dir_cache_listed_empty(&volume, AFP_ROOT_DID, "d") == 1);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "d", &e) == 0);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) == 0);
    e = record(41, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "h", &e, before);
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "d", 40, before);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "d", &e) == 0);
    CHECK(dir_cache_listed_empty(&volume, AFP_ROOT_DID, "d") == 0);
    /* A directory's own record is none of its objects, also where a
     * server gives a folder its parent's ID. */
    e = record(AFP_ROOT_DID, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "", &e,
                     dir_cache_changes(&volume));
    dir_cache_upsert(&volume, AFP_ROOT_DID, "self", &e,
                     dir_cache_changes(&volume));
    e = record(46, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "k", &e,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_listed_empty(&volume, AFP_ROOT_DID, "self") == 1);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "self", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "k", &e) == 0);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "", &e) != 0);
    /* A reply sent before a sweep stores nothing at a name the sweep
     * dropped, as either sweep counts as a removal from the folder. */
    e = record(47, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "m", &e,
                     dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, AFP_ROOT_DID, "m", 0);
    dir_cache_listed_empty(&volume, AFP_ROOT_DID, "m");
    e = record(49, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "m", &e, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "m", &e) == 0);
    e = record(48, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 47, "n", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, 47, "n", 0);
    dir_cache_listed(&volume, 47, dir_cache_listing(&volume));
    e = record(50, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 47, "n", &e, before);
    CHECK(dir_cache_lookup(&volume, 47, "n", &e) == 0);
    /* The attributes come from the cache while it holds them, and from
     * the server after a change. */
    e = record(70, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "s", &e,
                     dir_cache_changes(&volume));
    e = record(71, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
    CHECK(ml_getattr(&volume, "/s/g", &st) == 0);
    CHECK(S_ISREG(st.st_mode) && st.st_size == 4096 && st.st_ino == 71);
    CHECK(ml_getattr_fresh(&volume, "/s/g", &st) == -EIO);
    /* The Finder info comes with the record the stat presents. */
    {
        char finderinfo[32];
        e.finderinfo[8] = 0x40;
        dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
        CHECK(ml_getattr_finderinfo(&volume, "/s/g", &st, finderinfo) == 0
              && st.st_ino == 71 && finderinfo[8] == 0x40);
        e.finderinfo[8] = 0;
    }
    /* An attribute's value lives until read, 2 s at most: an earlier read
     * keeps none, a small buffer gets ERANGE, a set forgets it. */
    {
        char value[4];
        before = dir_cache_changes(&volume);
        dir_cache_forget_xattr(&volume, 70, "g");
        dir_cache_set_xattr(&volume, 70, "g", "k", "ab", 2, before);
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", NULL, 0) == -1);
        dir_cache_set_xattr(&volume, 70, "g", "k", "ab", 2,
                            dir_cache_changes(&volume));
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", NULL, 0) == 2);
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", value, 1) == -ERANGE);
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", value, sizeof(value))
              == 2 && memcmp(value, "ab", 2) == 0);
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", NULL, 0) == -1);
        dir_cache_set_xattr(&volume, 70, "g", "k", "ab", 2,
                            dir_cache_changes(&volume));
        dir_cache_forget_xattr(&volume, 70, "g");
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", NULL, 0) == -1);
        CHECK(dir_cache_lookup(&volume, 70, "g", &e)
              == (DIR_CACHE_DID | DIR_CACHE_STAT));
        dir_cache_set_xattr(&volume, 70, "g", "k", "ab", 2,
                            dir_cache_changes(&volume));
        fake_now += DIR_CACHE_XATTR_TTL;
        CHECK(dir_cache_get_xattr(&volume, 70, "g", "k", NULL, 0) == -1);
        e = record(73, 0, fake_now - 1000);
        dir_cache_upsert(&volume, 70, "v", &e, dir_cache_changes(&volume));
        dir_cache_set_xattr(&volume, 70, "v", "k", "ab", 2,
                            dir_cache_changes(&volume));
        e = record(74, 0, fake_now - 1000);
        dir_cache_upsert(&volume, 70, "v", &e, dir_cache_changes(&volume));
        CHECK(dir_cache_get_xattr(&volume, 70, "v", "k", NULL, 0) == -1);
        /* A read before a removal, or before a change at any of the
         * object's names, keeps nothing on an entry made since. */
        before = dir_cache_changes(&volume);
        dir_cache_remove(&volume, 70, "v");
        e = record(75, 0, fake_now - 1000);
        dir_cache_upsert(&volume, 70, "v", &e, dir_cache_changes(&volume));
        dir_cache_set_xattr(&volume, 70, "v", "k", "ab", 2, before);
        CHECK(dir_cache_get_xattr(&volume, 70, "v", "k", NULL, 0) == -1);
        before = dir_cache_changes(&volume);
        dir_cache_forget_xattr(&volume, 70, "x");
        e = record(76, 0, fake_now - 1000);
        dir_cache_upsert(&volume, 70, "x", &e, dir_cache_changes(&volume));
        dir_cache_set_xattr(&volume, 70, "x", "k", "ab", 2, before);
        CHECK(dir_cache_get_xattr(&volume, 70, "x", "k", NULL, 0) == -1);
        before = dir_cache_changes(&volume);
        dir_cache_forget_xattr(&volume, 70, "x");
        dir_cache_upsert(&volume, 70, "y", &e, dir_cache_changes(&volume));
        dir_cache_set_xattr(&volume, 70, "y", "k", "ab", 2, before);
        CHECK(dir_cache_get_xattr(&volume, 70, "y", "k", NULL, 0) == -1);
        /* A set or a removal at a name the cache holds as absent ends the
         * absence, as a parameter change does. */
        dir_cache_insert_negative(&volume, 70, "z", dir_cache_changes(&volume));
        dir_cache_forget_xattr(&volume, 70, "z");
        CHECK(dir_cache_lookup(&volume, 70, "z", &e) == 0);
        e = record(71, 0, fake_now - 1000);
    }
    /* A resource fork the record holds is measured on the server; the
     * record answers for one it does not hold. */
    CHECK(ml_getresourcefork(&volume, "/s/g", NULL, 0, 0) == -ENOATTR);
    e.resourcesize = 10;
    dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
    CHECK(ml_getresourcefork(&volume, "/s/g", NULL, 0, 0) == -EIO);
    dir_cache_invalidate(&volume, 70, "g", 0);
    CHECK(ml_getattr(&volume, "/s/g", &st) == -EIO);
    /* A write through a fork forgets its file under a name another
     * session's rename gave it, which a node ID finds. */
    memset(&fork, 0, sizeof(fork));
    fork.did = 70;
    fork.cnid = 71;
    memcpy(fork.name, "old", 4);
    e = record(71, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
    ll_fork_changed(&volume, &fork);
    CHECK(dir_cache_lookup(&volume, 70, "g", &e) == DIR_CACHE_DID);
    dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
    dir_cache_upsert(&volume, 70, "old", &e, dir_cache_changes(&volume));
    fork.cnid = 0;
    ll_fork_changed(&volume, &fork);
    CHECK(dir_cache_lookup(&volume, 70, "g", &e) == DIR_CACHE_DID);
    /* A reply sent before the change for a name of the object the cache
     * does not hold yet keeps its parameters out, as the node ID says. */
    before = dir_cache_changes(&volume);
    fork.cnid = 71;
    ll_fork_changed(&volume, &fork);
    dir_cache_upsert(&volume, 70, "new", &e, before);
    CHECK(dir_cache_lookup(&volume, 70, "new", &e) == DIR_CACHE_DID);
    /* A renamed directory keeps its identity and life under its new name,
     * so paths beneath it resolve; its old name goes. */
    fake_now += 100;
    dir_cache_rename(&volume, AFP_ROOT_DID, "s", AFP_ROOT_DID, "moved", 70,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_resolve(&volume, "/moved/g", &did, name) == 0
          && did == 70);
    CHECK(dir_cache_resolve(&volume, "/s/g", &did, name) == -EIO);
    fake_now += 20;
    CHECK(dir_cache_resolve(&volume, "/moved/g", &did, name) == -EIO);
    /* It carries nothing where the old name holds another object than
     * the one the request moved. */
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "r1", 89,
                         dir_cache_changes(&volume));
    dir_cache_rename(&volume, AFP_ROOT_DID, "r1", AFP_ROOT_DID, "r2", 85,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "r1", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "r2", &e) == 0);
    /* A removal counted since the move was sent keeps the carry out, as the
     * old name's entry may have met another object meanwhile. */
    e = record(86, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "ca", &e,
                     dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, AFP_ROOT_DID, "other");
    dir_cache_rename(&volume, AFP_ROOT_DID, "ca", AFP_ROOT_DID, "cb", 86,
                     before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "ca", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "cb", &e) == 0);
    e = record(86, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "ca", &e,
                     dir_cache_changes(&volume));
    dir_cache_rename(&volume, AFP_ROOT_DID, "ca", AFP_ROOT_DID, "cb", 86,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "cb", &e) == DIR_CACHE_DID);
#ifdef __GLIBC__
    /* A waiting writer takes the open lock ahead of new readers, so a
     * stream of opens does not starve a rename. */
    {
        struct lock_race lr;
        pthread_t reader;
        pthread_t writer;
        int refused = 0;
        afp_open_lock_init(&lr.lock);
        lr.phase = 0;
        CHECK(pthread_create(&reader, NULL, hold_read_lock, &lr) == 0);

        while (__atomic_load_n(&lr.phase, __ATOMIC_ACQUIRE) < 1) {
            sleep_ms(1);
        }

        CHECK(pthread_create(&writer, NULL, take_write_lock, &lr) == 0);

        for (int i = 0; i < 10000 && !refused; i++) {
            if (pthread_rwlock_tryrdlock(&lr.lock) == EBUSY) {
                refused = 1;
            } else {
                pthread_rwlock_unlock(&lr.lock);
                sleep_ms(1);
            }
        }

        __atomic_store_n(&lr.phase, 2, __ATOMIC_RELEASE);
        pthread_join(reader, NULL);
        pthread_join(writer, NULL);
        pthread_rwlock_destroy(&lr.lock);
        CHECK(refused);
    }
#endif
    /* An exchange without an answer may have swapped the files, so an
     * open fork of either name keeps its name and no node ID. */
    {
        struct afp_file_info xf;
        memset(&xf, 0, sizeof(xf));
        xf.did = AFP_ROOT_DID;
        xf.cnid = 54;
        memcpy(xf.name, "xa", 3);
        memcpy(xf.basename, "xa", 3);
        add_opened_fork(&volume, &xf);
        CHECK(ml_exchange(&volume, "/xa", "/xb") == -EIO);
        CHECK(xf.cnid == 0 && strcmp(xf.name, "xa") == 0);
        remove_opened_fork(&volume, &xf);
    }
    /* So does a rename's: it fails rather than move the source over the
     * destination, and both names are forgotten. */
    {
        struct afp_file_info rf;
        memset(&rf, 0, sizeof(rf));
        rf.did = AFP_ROOT_DID;
        rf.cnid = 55;
        memcpy(rf.name, "ra", 3);
        memcpy(rf.basename, "ra", 3);
        e = record(55, 0, fake_now - 1000);
        dir_cache_upsert(&volume, AFP_ROOT_DID, "ra", &e,
                         dir_cache_changes(&volume));
        e = record(56, 0, fake_now - 1000);
        dir_cache_upsert(&volume, AFP_ROOT_DID, "rb", &e,
                         dir_cache_changes(&volume));
        add_opened_fork(&volume, &rf);
        CHECK(ml_rename(&volume, "/ra", "/rb") == -EIO);
        CHECK(rf.cnid == 0 && strcmp(rf.name, "ra") == 0);
        CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "ra", &e) == 0
              && dir_cache_lookup(&volume, AFP_ROOT_DID, "rb", &e) == 0);
        remove_opened_fork(&volume, &rf);
    }
    /* An unanswered request may have happened, so a create's, a move's and
     * an exchange's names are forgotten. */
    dir_cache_insert_negative(&volume, AFP_ROOT_DID, "un",
                              dir_cache_changes(&volume));
    CHECK(ml_creat(&volume, "/un", 0644, NULL) == -EIO);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "un", &e) == 0);
    e = record(51, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "ua", &e,
                     dir_cache_changes(&volume));
    e = record(52, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "ub", &e,
                     dir_cache_changes(&volume));
    CHECK(ml_rename(&volume, "/ua", "/uc") == -EIO);
    CHECK(ml_exchange(&volume, "/ub", "/ud") == -EIO);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "ua", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "ub", &e) == 0);
    /* So are a delete's, and a length change through a fork forgets its
     * file's parameters. */
    e = record(53, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "ue", &e,
                     dir_cache_changes(&volume));
    CHECK(ml_unlink(&volume, "/ue") == -EIO);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "ue", &e) == 0);
    e = record(71, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 70, "g", &e, dir_cache_changes(&volume));
    fork.cnid = 0;
    memcpy(fork.name, "g", 2);
    CHECK(ll_setfork_size(&volume, &fork, 10) == EIO);
    CHECK(dir_cache_lookup(&volume, 70, "g", &e) == DIR_CACHE_DID);
    /* The cache holds max_entries; the clock hand takes an unused one. */
    dir_cache_purge(&volume);
    volume.cache.max_entries = 4;
    {
        static const char *const made[] = { "e1", "e2", "e3", "e4" };

        for (unsigned int k = 0; k < 4; k++) {
            dir_cache_insert_dir(&volume, AFP_ROOT_DID, made[k], 31U + k,
                                 dir_cache_changes(&volume));
        }

        dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e5", 35,
                             dir_cache_changes(&volume));
        CHECK(volume.cache.count == 4 && volume.cache.stats.evictions > 0);

        /* The fifth insert cleared every mark and took one entry; of the
         * rest, the hand takes the one no lookup met since. */
        for (unsigned int alone = 0; alone < 4; alone++) {
            unsigned int met = 0;
            dir_cache_purge(&volume);

            for (unsigned int k = 0; k < 4; k++) {
                dir_cache_insert_dir(&volume, AFP_ROOT_DID, made[k], 31U + k,
                                     dir_cache_changes(&volume));
            }

            dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e5", 35,
                                 dir_cache_changes(&volume));

            for (unsigned int k = 0; k < 4; k++) {
                if (k != alone && dir_cache_lookup(&volume, AFP_ROOT_DID,
                                                   made[k], &e) != 0) {
                    met++;
                }
            }

            /* The fifth's insert took the one left alone. */
            if (met == 3) {
                continue;
            }

            dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e6", 36,
                                 dir_cache_changes(&volume));
            CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, made[alone], &e)
                  == 0);
        }
    }
    /* The hand moves past the bucket it evicted from, so the next eviction
     * spares the used entry whose mark it cleared there. */
    {
        struct afp_volume tiny;
        const struct dir_cache_entry *first;
        const struct dir_cache_entry *head;
        memset(&tiny, 0, sizeof(tiny));
        dir_cache_init(&tiny);
        tiny.cache.clock = fake_clock;
        tiny.cache.wall = fake_clock;
        tiny.cache.max_entries = 3;
        dir_cache_insert_negative(&tiny, AFP_ROOT_DID, "h14",
                                  dir_cache_changes(&tiny));
        first = tiny.cache.by_name[0];
        dir_cache_insert_dir(&tiny, AFP_ROOT_DID, "h5", 91,
                             dir_cache_changes(&tiny));
        head = tiny.cache.by_name[0];
        dir_cache_insert_negative(&tiny, AFP_ROOT_DID, "h3",
                                  dir_cache_changes(&tiny));
        /* The layout the check rests on: eight buckets, "h14" and then "h5"
         * at the head of the first, "h3" elsewhere. */
        CHECK(tiny.cache.buckets == 8 && first != NULL && head != first
              && tiny.cache.by_name[0] == head);
        dir_cache_insert_dir(&tiny, AFP_ROOT_DID, "h4", 92,
                             dir_cache_changes(&tiny));
        dir_cache_insert_dir(&tiny, AFP_ROOT_DID, "h7", 93,
                             dir_cache_changes(&tiny));
        CHECK(dir_cache_lookup(&tiny, AFP_ROOT_DID, "h5", &e) != 0
              && dir_cache_lookup(&tiny, AFP_ROOT_DID, "h3", &e) == 0);
        dir_cache_destroy(&tiny);
    }
    /* One listing makes at most half the cache's entries: two of a cache
     * of four. */
    dir_cache_purge(&volume);
    volume.cache.max_entries = 4;
    listing_of(&l, 40, three, 61);
    dir_cache_insert_listing(&volume, &l, 0, dir_cache_changes(&volume),
                             dir_cache_listing(&volume));
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) != 0
          && dir_cache_lookup(&volume, 40, "j", &e) == 0);
    /* The cap counts positions across pages: a later page's third makes no
     * entry, but a held entry there takes the record. */
    dir_cache_insert_dir(&volume, 40, "j", 63, dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 40, "j", &e) == DIR_CACHE_DID);
    listing_of(&l, 40, three, 61);
    dir_cache_insert_listing(&volume, &l, 2, dir_cache_changes(&volume),
                             dir_cache_listing(&volume));
    CHECK(dir_cache_lookup(&volume, 40, "j", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    dir_cache_purge(&volume);
    dir_cache_insert_listing(&volume, &l, 2, dir_cache_changes(&volume),
                             dir_cache_listing(&volume));
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&volume, 40, "j", &e) == 0);
    /* A listed folder's record keeps its parameters where its entry holds
     * the listing's ID already. */
    e = record(45, 1, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "kept", &e, dir_cache_changes(&volume));
    dir_cache_insert_dir(&volume, 40, "kept", 45, dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 40, "kept", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* An absence whose request a removal overtook is not stored, and a
     * record at its name goes. */
    e = record(46, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "x", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, 40, "y");
    dir_cache_insert_negative(&volume, 40, "x", before);
    dir_cache_insert_negative(&volume, 40, "w", before);
    CHECK(dir_cache_lookup(&volume, 40, "x", &e) == 0
          && dir_cache_lookup(&volume, 40, "w", &e) == 0);
    /* A change an evicted entry carried keeps out the parameters of a
     * reply sent before it. */
    dir_cache_purge(&volume);
    volume.cache.max_entries = 1;
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, AFP_ROOT_DID, "e0", 0);
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e6", 36,
                         dir_cache_changes(&volume));
    e = record(30, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "e0", &e, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "e0", &e) == DIR_CACHE_DID);
    volume.cache.max_entries = DIR_CACHE_MAX_ENTRIES;
    /* Three threads store, forget and look up one name at once: no lookup
     * meets a half-stored record. */
    {
        struct race r = { &volume, 2000, 0 };
        pthread_t storer;
        pthread_t remover;
        pthread_t looker;
        CHECK(pthread_create(&storer, NULL, race_store, &r) == 0);
        CHECK(pthread_create(&remover, NULL, race_remove, &r) == 0);
        CHECK(pthread_create(&looker, NULL, race_look, &r) == 0);
        pthread_join(storer, NULL);
        pthread_join(remover, NULL);
        pthread_join(looker, NULL);
        CHECK(r.torn == 0);
        dir_cache_remove(&volume, 10, "race");
    }
    /* Against the fake server: a name missing under a cached ID restarts
     * the walk; a create finding the name taken ends only its absence. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpGetFileDirParms;
    fake.refusal = kFPObjectNotFound;
    fake.name_taken = 1;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(60, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "a", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_getattr(&fake.volume, "/a/b/x", &st) == -ENOENT);
    dir_cache_insert_negative(&fake.volume, AFP_ROOT_DID, "taken",
                              dir_cache_changes(&fake.volume));
    before = dir_cache_changes(&fake.volume);
    CHECK(ml_creat(&fake.volume, "/taken", 0644, NULL) == -EEXIST);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "taken", &e) == 0);
    e = record(61, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "near", &e, before);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "near", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* A folder renamed by a path with a trailing slash leaves no entry
     * under its old name. */
    dir_cache_insert_dir(&fake.volume, AFP_ROOT_DID, "olddir", 81,
                         dir_cache_changes(&fake.volume));
    CHECK(ml_rename(&fake.volume, "/olddir/", "/newdir") == 0);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "olddir", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 5 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpGetFileDirParms
          && fake.commands[2] == afpCreateFile
          && fake.commands[4] == afpMoveAndRename);
    /* So does a directory create or an O_CREAT open finding the name taken:
     * a reply sent before either keeps its parameters. */
    memset(&fake, 0, sizeof(fake));
    fake.name_taken = 1;
    fake.refuse = afpCreateDir;
    fake.refusal = kFPObjectExists;
    CHECK(afp_fake_server_start(&fake) == 0);
    before = dir_cache_changes(&fake.volume);
    CHECK(ml_mkdir(&fake.volume, "/taken", 0755) == -EEXIST);
    {
        struct afp_file_info *fp = NULL;
        CHECK(ml_open(&fake.volume, "/file", O_RDWR | O_CREAT, &fp) == 0);

        if (fp) {
            ml_close(&fake.volume, fp);
            free(fp);
        }
    }
    e = record(62, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "near", &e, before);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "near", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    afp_fake_server_stop(&fake);
    /* An append's recorded length is the end the server reports, after
     * one chunk or two, and after a write past the end. */
    memset(&fake, 0, sizeof(fake));
    fake.data_length = 100;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->quantum = 65536;
    {
        struct afp_file_info *fp = NULL;
        CHECK(ml_open(&fake.volume, "/log", O_RDWR, &fp) == 0);

        if (fp) {
            CHECK(ml_write(&fake.volume, "abc", 3, 0, 1, fp) == 3);
            CHECK(fp->size == 103);
            fake.server->quantum = 2;
            CHECK(ml_write(&fake.volume, "defg", 4, 0, 1, fp) == 4
                  && fp->size == 107);
            CHECK(ml_write(&fake.volume, "h", 1, 107, 0, fp) == 1
                  && ml_write(&fake.volume, "i", 1, 0, 1, fp) == 1
                  && fp->size == 109);
            ml_close(&fake.volume, fp);
            free(fp);
        }
    }
    afp_fake_server_stop(&fake);
    /* The count is noted before a fetch goes: a removal while the server
     * holds the reply keeps the reply out. */
    memset(&fake, 0, sizeof(fake));
    fake.hold = afpGetFileDirParms;
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 70;
    CHECK(afp_fake_server_start(&fake) == 0);
    {
        pthread_t asker;
        CHECK(pthread_create(&asker, NULL, fetch_in_thread, &fake) == 0);
        CHECK(afp_fake_server_wait_held(&fake) == 0);
        dir_cache_remove(&fake.volume, AFP_ROOT_DID, "held");
        afp_fake_server_release(&fake);
        pthread_join(asker, NULL);
    }
    CHECK(held_ret == 0);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "held", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpGetFileDirParms);
    /* An empty listing forgets the root's objects, and the objects of a
     * folder whose identity the cache does not hold, read for its ID. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpEnumerateExt2;
    fake.refusal = kFPObjectNotFound;
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 77;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(76, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "z", &e,
                     dir_cache_changes(&fake.volume));
    dir_cache_upsert(&fake.volume, 77, "z", &e,
                     dir_cache_changes(&fake.volume));
    memset(&l, 0, sizeof(l));
    CHECK(ml_readdir(&fake.volume, "/", &l) == 0 && l.count == 0);
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "z", &e) == 0);
    memset(&l, 0, sizeof(l));
    CHECK(ml_readdir(&fake.volume, "/e", &l) == 0 && l.count == 0);
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&fake.volume, 77, "z", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 3 && fake.commands[0] == afpEnumerateExt2
          && fake.commands[1] == afpEnumerateExt2
          && fake.commands[2] == afpGetFileDirParms);
    /* A file on a path's way is no folder. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 78;
    CHECK(afp_fake_server_start(&fake) == 0);
    CHECK(ml_getattr(&fake.volume, "/f/x", &st) == -ENOTDIR);
    afp_fake_server_stop(&fake);
    /* A fresh resolution, a refused listing's retry, reads a folder the
     * cache holds from the server. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 79;
    CHECK(afp_fake_server_start(&fake) == 0);
    dir_cache_insert_dir(&fake.volume, AFP_ROOT_DID, "held", 80,
                         dir_cache_changes(&fake.volume));
    CHECK(dir_cache_resolve_fresh(&fake.volume, "/held/x", &did, name) == 0
          && did == 79);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpGetFileDirParms);
    /* A listing that finds a file at the folder's name forgets the name. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpEnumerateExt2;
    fake.refusal = kFPObjectTypeErr;
    CHECK(afp_fake_server_start(&fake) == 0);
    dir_cache_insert_dir(&fake.volume, AFP_ROOT_DID, "a", 60,
                         dir_cache_changes(&fake.volume));
    dir_cache_insert_dir(&fake.volume, 60, "f", 62,
                         dir_cache_changes(&fake.volume));
    memset(&l, 0, sizeof(l));
    CHECK(ml_readdir(&fake.volume, "/a/f", &l) == -ENOTDIR);
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&fake.volume, 60, "f", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpEnumerateExt2);
    /* A listing the same parent refuses again forgets the folder's name. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpEnumerateExt2;
    fake.refusal = kFPDirNotFound;
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 60;
    CHECK(afp_fake_server_start(&fake) == 0);
    dir_cache_insert_dir(&fake.volume, AFP_ROOT_DID, "a", 60,
                         dir_cache_changes(&fake.volume));
    dir_cache_insert_dir(&fake.volume, 60, "b", 61,
                         dir_cache_changes(&fake.volume));
    memset(&l, 0, sizeof(l));
    CHECK(ml_readdir(&fake.volume, "/a/b", &l) == -ENOENT);
    afp_listing_free(&l);
    CHECK(dir_cache_lookup(&fake.volume, 60, "b", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && fake.commands[0] == afpEnumerateExt2
          && fake.commands[1] == afpGetFileDirParms);
    /* An open the server refuses for want of a file forgets the record
     * that sent the kernel there. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpOpenFork;
    fake.refusal = kFPObjectNotFound;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(64, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "gone", &e,
                     dir_cache_changes(&fake.volume));
    {
        struct afp_file_info *opened = NULL;
        CHECK(ml_open(&fake.volume, "/gone", O_RDONLY, &opened) == -ENOENT);
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "gone", &e) == 0);
    /* So does a link's read. */
    e = record(67, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "lnk", &e,
                     dir_cache_changes(&fake.volume));
    {
        char target[64];
        CHECK(ml_readlink(&fake.volume, "/lnk", target, sizeof(target))
              == -ENOENT);
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "lnk", &e) == 0);
    afp_fake_server_stop(&fake);
    /* A file's privileges change forgets its folder's parameters, which
     * afpd dates, and keeps the folder's identity. */
    memset(&fake, 0, sizeof(fake));
    fake.volume.extra_flags = VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    fake.file_params = 1;
    fake.params_cnid = 69;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(68, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "d", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_chmod(&fake.volume, "/d/f", 0600) == 0);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "d", &e)
          == DIR_CACHE_DID);
    afp_fake_server_stop(&fake);
    /* A source the cache holds as a file but the server as a folder, moved
     * over a file, fails with ENOTDIR and deletes nothing. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpMoveAndRename;
    fake.refusal = kFPObjectExists;
    fake.file_params = 1;
    fake.params_cnid = 66;
    fake.params_dirs = 1U << 1;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.volume.attributes |= kNoExchangeFiles;
    e = record(65, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "srcdir", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_rename(&fake.volume, "/srcdir", "/now") == -ENOTDIR);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 4 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpMoveAndRename
          && fake.commands[2] == afpGetFileDirParms
          && fake.commands[3] == afpGetFileDirParms);
    /* An exclusive rename sends the move alone, whose answer finds the
     * name taken where a cached absence would not. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpMoveAndRename;
    fake.refusal = kFPObjectExists;
    CHECK(afp_fake_server_start(&fake) == 0);
    dir_cache_insert_negative(&fake.volume, AFP_ROOT_DID, "taken",
                              dir_cache_changes(&fake.volume));
    CHECK(ml_rename_excl(&fake.volume, "/src", "/taken") == -EEXIST);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpMoveAndRename);
    /* A move the cache's record steered that the server refuses forgets
     * the names it went to: the folder's own, and the source's inside it. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpMoveAndRename;
    fake.refusal = kFPObjectNotFound;
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 95;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(95, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "dst", &e,
                     dir_cache_changes(&fake.volume));
    e = record(96, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, 95, "src", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_rename(&fake.volume, "/src", "/dst") == -ENOENT);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "dst", &e) == 0
          && dir_cache_lookup(&fake.volume, 95, "src", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpMoveAndRename);
    /* A rename onto a folder's record asks the server, which holds nothing
     * there now: the source moves to the name. */
    memset(&fake, 0, sizeof(fake));
    fake.refuse = afpGetFileDirParms;
    fake.refusal = kFPObjectNotFound;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(97, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "gone_dir", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_rename(&fake.volume, "/src", "/gone_dir") == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpMoveAndRename);
    /* A move into a folder carries the source's open fork to the folder's
     * ID; another name's fork keeps its fields. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_dir = 1;
    fake.params_cnid = 95;
    CHECK(afp_fake_server_start(&fake) == 0);
    {
        struct afp_file_info mf;
        struct afp_file_info other;
        memset(&mf, 0, sizeof(mf));
        mf.did = AFP_ROOT_DID;
        mf.cnid = 97;
        memcpy(mf.name, "mf", 3);
        memcpy(mf.basename, "mf", 3);
        memset(&other, 0, sizeof(other));
        other.did = AFP_ROOT_DID;
        other.cnid = 98;
        memcpy(other.name, "other", 6);
        memcpy(other.basename, "other", 6);
        e = record(95, 1, fake_now - 1000);
        dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "dst", &e,
                         dir_cache_changes(&fake.volume));
        add_opened_fork(&fake.volume, &mf);
        add_opened_fork(&fake.volume, &other);
        CHECK(ml_rename(&fake.volume, "/mf", "/dst") == 0);
        CHECK(mf.did == 95 && strcmp(mf.name, "mf") == 0
              && strcmp(mf.basename, "mf") == 0);
        CHECK(other.did == AFP_ROOT_DID && strcmp(other.name, "other") == 0);
        remove_opened_fork(&fake.volume, &other);
        remove_opened_fork(&fake.volume, &mf);
    }
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpMoveAndRename);
    /* An exchange forgets both names, which hold each other's contents
     * after it. */
    memset(&fake, 0, sizeof(fake));
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(90, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "xa", &e,
                     dir_cache_changes(&fake.volume));
    e = record(91, 0, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "xb", &e,
                     dir_cache_changes(&fake.volume));
    {
        /* An open fork of either name takes the other's and loses its node
         * ID, which the names keep over swapped contents. */
        struct afp_file_info xf;
        memset(&xf, 0, sizeof(xf));
        xf.did = AFP_ROOT_DID;
        xf.cnid = 90;
        memcpy(xf.name, "xa", 3);
        memcpy(xf.basename, "xa", 3);
        add_opened_fork(&fake.volume, &xf);
        CHECK(ml_exchange(&fake.volume, "/xa", "/xb") == 0);
        CHECK(strcmp(xf.name, "xb") == 0 && xf.cnid == 0);
        remove_opened_fork(&fake.volume, &xf);
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "xa", &e) == 0
          && dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "xb", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpExchangeFiles);
    /* An open that reports another length than the record's forgets the
     * record's parameters, and one that reports another object the name;
     * one that reports the same keeps them. The record says which found it
     * stale. */
    memset(&fake, 0, sizeof(fake));
    fake.data_length = 20;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->server_type = AFPC_SERVER_TYPE_NETATALK;
    e = record(110, 0, fake_now - 1000);
    e.size = 20;
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "same", &e,
                     dir_cache_changes(&fake.volume));
    e = record(111, 0, fake_now - 1000);
    e.size = 10;
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "grown", &e,
                     dir_cache_changes(&fake.volume));
    {
        static const char *const opened[] = { "/same", "/grown" };

        for (size_t i = 0; i < 2; i++) {
            struct afp_file_info *fp = NULL;
            CHECK(ml_open(&fake.volume, opened[i], O_RDONLY, &fp) == 0
                  && fp->stale == (i == 1));

            if (fp) {
                ml_close(&fake.volume, fp);
                free(fp);
            }
        }
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "same", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "grown", &e)
          == DIR_CACHE_DID);
    afp_fake_server_stop(&fake);
    memset(&fake, 0, sizeof(fake));
    fake.data_length = 20;
    fake.params_cnid = 113;
    CHECK(afp_fake_server_start(&fake) == 0);
    fake.server->server_type = AFPC_SERVER_TYPE_NETATALK;
    e = record(112, 0, fake_now - 1000);
    e.size = 20;
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "replaced", &e,
                     dir_cache_changes(&fake.volume));
    {
        struct afp_file_info *fp = NULL;
        CHECK(ml_open(&fake.volume, "/replaced", O_RDONLY, &fp) == 0
              && fp->stale == 1);

        if (fp) {
            ml_close(&fake.volume, fp);
            free(fp);
        }
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "replaced", &e) == 0);
    afp_fake_server_stop(&fake);
    /* AFP 2's open reads the length first, which checks the record. */
    {
        static struct afp_versions afp22 = { "AFP2.2", 22 };
        struct afp_file_info *fp = NULL;
        memset(&fake, 0, sizeof(fake));
        fake.file_params = 1;
        fake.params_cnid = 111;
        fake.data_length = 20;
        CHECK(afp_fake_server_start(&fake) == 0);
        fake.server->using_version = &afp22;
        e = record(111, 0, fake_now - 1000);
        e.size = 10;
        dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "grown", &e,
                         dir_cache_changes(&fake.volume));
        CHECK(ml_open(&fake.volume, "/grown", O_RDONLY, &fp) == 0
              && fp->stale == 1);

        if (fp) {
            ml_close(&fake.volume, fp);
            free(fp);
        }

        CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "grown", &e)
              == DIR_CACHE_DID);
        afp_fake_server_stop(&fake);
    }
    /* A folder dated later drops its child folders' identities, not its
     * files', and keeps out the replies about its objects sent before; one
     * dated the same keeps them. */
    e = record(120, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "dated", &e,
                     dir_cache_changes(&volume));
    dir_cache_insert_dir(&volume, 120, "sub", 121, dir_cache_changes(&volume));
    e = record(122, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 120, "file", &e, dir_cache_changes(&volume));
    e = record(120, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "dated", &e,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 120, "sub", &e) & DIR_CACHE_DID);
    before = dir_cache_changes(&volume);
    e = record(120, 1, fake_now - 999);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "dated", &e,
                     dir_cache_changes(&volume));
    CHECK(dir_cache_lookup(&volume, 120, "sub", &e) == 0);
    CHECK(dir_cache_lookup(&volume, 120, "file", &e) & DIR_CACHE_DID);
    dir_cache_insert_dir(&volume, 120, "sub", 121, before);
    CHECK(dir_cache_lookup(&volume, 120, "sub", &e) == 0);
    /* A removal keeps out the replies sent before it about its folder or
     * its object under another name, and no other folder's. */
    e = record(131, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 130, "gone", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_remove(&volume, 130, "gone");
    e = record(132, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 130, "near", &e, before);
    CHECK(dir_cache_lookup(&volume, 130, "near", &e) == 0);
    e = record(131, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 140, "alias", &e, before);
    CHECK(dir_cache_lookup(&volume, 140, "alias", &e) == 0);
    e = record(133, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 141, "far", &e, before);
    CHECK(dir_cache_lookup(&volume, 141, "far", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* A listing's sweep keeps out the replies about the folder's objects
     * sent before it, which may show one the listing did not. */
    before = dir_cache_changes(&volume);
    dir_cache_listed(&volume, 150, dir_cache_listing(&volume));
    e = record(151, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 150, "ghost", &e, before);
    CHECK(dir_cache_lookup(&volume, 150, "ghost", &e) == 0);
    /* A forgotten path keeps out the replies sent before it about the
     * folders it read, and no other folder's. */
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "fp", 160,
                         dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_forget_path(&volume, "/fp/x");
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "fp", 160, before);
    e = record(162, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 161, "other", &e, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "fp", &e) == 0
          && dir_cache_lookup(&volume, 161, "other", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* A reply to an earlier request stores nothing over a later one's: a
     * listing's folder ID, noted before the listing, over the ID a fetch
     * during it found, an object over a later absence, and an absence over
     * a later object. */
    sent = dir_cache_changes(&volume);
    e = record(171, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "big", &e,
                     dir_cache_changes(&volume));
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "big", 170, sent);
    CHECK((dir_cache_lookup(&volume, AFP_ROOT_DID, "big", &e) & DIR_CACHE_DID)
          && e.cnid == 171);
    sent = dir_cache_changes(&volume);
    dir_cache_insert_negative(&volume, AFP_ROOT_DID, "went",
                              dir_cache_changes(&volume));
    e = record(172, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "went", &e, sent);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "went", &e)
          == DIR_CACHE_NEGATIVE);
    sent = dir_cache_changes(&volume);
    e = record(173, 0, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "came", &e,
                     dir_cache_changes(&volume));
    dir_cache_insert_negative(&volume, AFP_ROOT_DID, "came", sent);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "came", &e)
          == (DIR_CACHE_DID | DIR_CACHE_STAT));
    /* An attribute call, or the read a mode change starts with, that finds
     * no object forgets the name. */
    {
        static const uint8_t calls[] = {
            afpGetExtAttr, afpSetExtAttr, afpListExtAttrs, afpRemoveExtAttr,
            afpGetFileDirParms
        };

        for (size_t i = 0; i < sizeof(calls); i++) {
            char value[8];
            int ret;
            memset(&fake, 0, sizeof(fake));
            fake.refuse = calls[i];
            fake.refusal = kFPObjectNotFound;
            CHECK(afp_fake_server_start(&fake) == 0);
            fake.volume.attributes |= kSupportsExtAttrs;
            fake.volume.extra_flags |= VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
            e = record(180, 0, fake_now - 1000);
            dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "x", &e,
                             dir_cache_changes(&fake.volume));

            switch (calls[i]) {
            case afpGetExtAttr:
                ret = ml_getxattr(&fake.volume, "/x", "user.k", NULL, 0);
                break;

            case afpSetExtAttr:
                ret = ml_setxattr(&fake.volume, "/x", "user.k", "v", 1, 0);
                break;

            case afpListExtAttrs:
                ret = ml_listxattr(&fake.volume, "/x", value, sizeof(value));
                break;

            case afpRemoveExtAttr:
                ret = ml_removexattr(&fake.volume, "/x", "user.k");
                break;

            default:
                ret = ml_setattr(&fake.volume, "/x", ML_SET_MODE, 0600, 0, 0,
                                 0);
                break;
            }

            CHECK(ret == -ENOENT
                  && dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "x", &e)
                  == 0);
            afp_fake_server_stop(&fake);
        }
    }
    /* An unlink the record refuses as a folder's asks the server, which
     * holds a file there now. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    fake.params_cnid = 191;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(190, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "was_dir", &e,
                     dir_cache_changes(&fake.volume));
    CHECK(ml_unlink(&fake.volume, "/was_dir") == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 2 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpDelete);
    /* A Finder info that makes a file a link forgets its folder's
     * parameters, as the link dates the folder. */
    memset(&fake, 0, sizeof(fake));
    fake.file_params = 1;
    CHECK(afp_fake_server_start(&fake) == 0);
    e = record(200, 1, fake_now - 1000);
    dir_cache_upsert(&fake.volume, AFP_ROOT_DID, "links", &e,
                     dir_cache_changes(&fake.volume));
    {
        char finderinfo[32] = ML_SYMLINK_FINDERINFO;
        CHECK(ml_setfinderinfo(&fake.volume, "/links/f", finderinfo,
                               sizeof(finderinfo)) == 0);
    }
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "links", &e)
          == DIR_CACHE_DID);
    afp_fake_server_stop(&fake);
    pthread_rwlock_destroy(&volume.open_lock);
    pthread_mutex_destroy(&volume.open_forks_mutex);
    dir_cache_destroy(&volume);
    afp_server_release(server);
    return test_tap_finish();
}
