/*
    test_dir_cache.c: the directory cache, against a server that answers
    nothing and against the fake server

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
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
    pthread_rwlock_init(&volume.open_lock, NULL);
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
    /* An absence is remembered to the clock's second after next; on the
     * way, one held under a cached ID is asked again, as a moved folder's
     * may be. */
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
    /* A removal or a rename after a directory's create or listing was sent
     * may have moved it away: its ID stays out, and an entry holding
     * another goes. */
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
    /* A change found after a request was sent keeps that request's
     * parameters out; its identity comes in. A request sent after the
     * change brings its parameters, in the change's second too. */
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
    /* A change to a name the cache does not hold, or holds as absent,
     * leaves an entry that answers for nothing and keeps out the
     * parameters of a reply sent before it. */
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
    /* A removal keeps out every request sent before it, and such a reply
     * drops an entry it contradicts. */
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
    /* The resolution's last component comes in both forms, in the one
     * spelling the cache keeps, and a name the volume refuses is
     * refused. */
    CHECK(ml_resolve(&volume, "/caf\xc3\xa9", &t) == 0);
    CHECK(t.did == AFP_ROOT_DID && strcmp(t.name, "caf\xc3\xa9") == 0
          && strcmp(t.afp_name, "cafe\xcc\x81") == 0);
    CHECK(ml_resolve(&volume, "/cafe\xcc\x81", &t) == 0);
    CHECK(strcmp(t.name, "caf\xc3\xa9") == 0);
    memset(long_path, 'x', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1U] = '\0';
    CHECK(ml_resolve(&volume, long_path, &t) == -ENAMETOOLONG);
    /* So is a name whose wire form outgrows the record; an empty path
     * names nothing, and a listing libfuse could give no path refuses,
     * the AppleDouble view shown. */
    {
        char wide[1 + 341 * 2 + 1];
        size_t k;
        wide[0] = '/';

        for (k = 0; k < 341; k++) {
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
    /* A listing's records join under their parent, and a listing to its
     * end forgets the folder's objects it did not show; an absence stays.
     * A page sent before a removal stores nothing: it marks the objects it
     * shows for the sweep, and drops an entry it contradicts. */
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
        size_t k;

        for (k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
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
    /* A name a listing shows for two objects, as two spellings of it the
     * conversion takes as one, is cached for none of them, a third
     * sighting included. */
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
    /* A folder found empty goes with its objects, and nothing counts as
     * changed; one whose identity the cache does not hold names none. */
    e = record(41, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 40, "h", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    CHECK(dir_cache_listed_empty(&volume, AFP_ROOT_DID, "d") == 1);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "d", &e) == 0);
    CHECK(dir_cache_lookup(&volume, 40, "h", &e) == 0);
    CHECK(dir_cache_changes(&volume) == before);
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
    /* A change a dropped entry carried keeps out the parameters of a reply
     * sent before it, whichever drop took the entry: an empty listing's or
     * a sweep's. */
    e = record(47, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "m", &e,
                     dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, AFP_ROOT_DID, "m", 0);
    dir_cache_listed_empty(&volume, AFP_ROOT_DID, "m");
    e = record(49, 1, fake_now - 1000);
    dir_cache_upsert(&volume, AFP_ROOT_DID, "m", &e, before);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "m", &e) == DIR_CACHE_DID);
    e = record(48, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 47, "n", &e, dir_cache_changes(&volume));
    before = dir_cache_changes(&volume);
    dir_cache_invalidate(&volume, 47, "n", 0);
    dir_cache_listed(&volume, 47, dir_cache_listing(&volume));
    e = record(50, 0, fake_now - 1000);
    dir_cache_upsert(&volume, 47, "n", &e, before);
    CHECK(dir_cache_lookup(&volume, 47, "n", &e) == DIR_CACHE_DID);
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
    /* An attribute's value stays on its object's entry until it is read,
     * two seconds of the clock at most. A read sent before a change keeps
     * nothing, a size too small answers ERANGE, a set or a removal forgets
     * the value and leaves the parameters, and another object at the name
     * holds no value. */
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
        /* A value read before a removal, before a change at a name the
         * cache does not hold, or before a change under another of the
         * object's names keeps nothing on an entry made since. */
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
     * session's rename gave it, which a node ID finds: the fork's own, or
     * the one the cache holds at the fork's name. */
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
    /* A renamed directory keeps its identity under its new name, with the
     * life of the reply that carried it, so the paths beneath it resolve
     * as before; its old name is gone. */
    fake_now += 100;
    dir_cache_rename(&volume, AFP_ROOT_DID, "s", AFP_ROOT_DID, "moved", 70);
    CHECK(dir_cache_resolve(&volume, "/moved/g", &did, name) == 0
          && did == 70);
    CHECK(dir_cache_resolve(&volume, "/s/g", &did, name) == -EIO);
    fake_now += 20;
    CHECK(dir_cache_resolve(&volume, "/moved/g", &did, name) == -EIO);
    /* It carries nothing where the old name holds another object than
     * the one the request moved. */
    dir_cache_insert_dir(&volume, AFP_ROOT_DID, "r1", 89,
                         dir_cache_changes(&volume));
    dir_cache_rename(&volume, AFP_ROOT_DID, "r1", AFP_ROOT_DID, "r2", 85);
    CHECK(dir_cache_lookup(&volume, AFP_ROOT_DID, "r1", &e) == 0
          && dir_cache_lookup(&volume, AFP_ROOT_DID, "r2", &e) == 0);
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
    /* A request the server never answered may have been carried out, so
     * the names it went to are forgotten: a create's, a move's and an
     * exchange's. */
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
        size_t alone;
        size_t k;

        for (k = 0; k < 4; k++) {
            dir_cache_insert_dir(&volume, AFP_ROOT_DID, made[k], 31U + k,
                                 dir_cache_changes(&volume));
        }

        dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e5", 35,
                             dir_cache_changes(&volume));
        CHECK(volume.cache.count == 4 && volume.cache.stats.evictions > 0);

        /* The fifth's insert takes one of four used entries and leaves
         * the others unused; of those, the hand takes the one no lookup
         * met since, whichever it is. */
        for (alone = 0; alone < 4; alone++) {
            unsigned int met = 0;
            dir_cache_purge(&volume);

            for (k = 0; k < 4; k++) {
                dir_cache_insert_dir(&volume, AFP_ROOT_DID, made[k], 31U + k,
                                     dir_cache_changes(&volume));
            }

            dir_cache_insert_dir(&volume, AFP_ROOT_DID, "e5", 35,
                                 dir_cache_changes(&volume));

            for (k = 0; k < 4; k++) {
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
    /* The hand moves past the bucket it took an entry from, so the next
     * eviction leaves the used entry whose mark it cleared there: "h5"
     * and "h14" share the first of the eight buckets a cache of three
     * entries makes, and "h3" is in the third. */
    {
        struct afp_volume tiny;
        memset(&tiny, 0, sizeof(tiny));
        dir_cache_init(&tiny);
        tiny.cache.clock = fake_clock;
        tiny.cache.wall = fake_clock;
        tiny.cache.max_entries = 3;
        dir_cache_insert_negative(&tiny, AFP_ROOT_DID, "h14",
                                  dir_cache_changes(&tiny));
        dir_cache_insert_dir(&tiny, AFP_ROOT_DID, "h5", 91,
                             dir_cache_changes(&tiny));
        dir_cache_insert_negative(&tiny, AFP_ROOT_DID, "h3",
                                  dir_cache_changes(&tiny));
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
    /* Against the fake server: a component the server does not find under
     * a cached ID restarts the walk from the root, and a create that finds
     * the name taken ends the absence held for it. */
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
    CHECK(ml_creat(&fake.volume, "/taken", 0644, NULL) == -EEXIST);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "taken", &e) == 0);
    /* A folder renamed by a path that ends in a slash, which names it as
     * the path without the slash does, leaves no entry under its old
     * name. */
    dir_cache_insert_dir(&fake.volume, AFP_ROOT_DID, "olddir", 81,
                         dir_cache_changes(&fake.volume));
    CHECK(ml_rename(&fake.volume, "/olddir/", "/newdir") == 0);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "olddir", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 5 && fake.commands[0] == afpGetFileDirParms
          && fake.commands[1] == afpGetFileDirParms
          && fake.commands[2] == afpCreateFile
          && fake.commands[4] == afpMoveAndRename);
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
    /* A source the cache holds as a file and the server as a folder, moved
     * over what the server holds as a file at a name the move finds
     * taken, fails with ENOTDIR and deletes nothing: the second read, the
     * source's, answers a folder. */
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
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpMoveAndRename);
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
    CHECK(ml_exchange(&fake.volume, "/xa", "/xb") == 0);
    CHECK(dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "xa", &e) == 0
          && dir_cache_lookup(&fake.volume, AFP_ROOT_DID, "xb", &e) == 0);
    afp_fake_server_stop(&fake);
    CHECK(fake.ncommands == 1 && fake.commands[0] == afpExchangeFiles);
    pthread_rwlock_destroy(&volume.open_lock);
    pthread_mutex_destroy(&volume.open_forks_mutex);
    dir_cache_destroy(&volume);
    afp_server_release(server);
    return test_tap_finish();
}
