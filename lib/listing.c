/*
    listing.c: directory listings, their records and the stat form

    Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
    Copyright (C) 2026 Andy Lemin (andylemin)

    The stat form is extracted from lowlevel.c.

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief Directory listings: the records, their name pool and the stat form
 *
 * A member holds a value when its bit is set in valid; a record holds its
 * name's offset in the pool, as both arrays move when they grow.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "resource.h"
#include "users.h"
#include "utils.h"

/* A listing's first allocation, in entries */
#define AFP_LISTING_MIN_ENTRIES 64U

/*!
 * @brief Grow an array to hold need elements, at least doubling it
 *
 * @param[in]     arr    the array, or NULL
 * @param[in,out] cap    its capacity in elements, updated on success
 * @param[in]     need   the elements it must hold
 * @param[in]     elem   bytes per element
 * @param[in]     least  the smallest capacity to grow to
 * @returns the grown array, or NULL without memory or for an array of
 *          more than UINT32_MAX bytes
 */
static void *grown_array(void *arr, uint32_t *cap, uint64_t need, size_t elem,
                         uint32_t least)
{
    uint64_t capacity = max(need, *cap * UINT64_C(2));
    void *grown;
    capacity = min(max(capacity, least), UINT32_MAX / elem);

    if (need > capacity) {
        return NULL;
    }

    grown = realloc(arr, (size_t) capacity * elem);

    if (grown != NULL) {
        *cap = (uint32_t) capacity;
    }

    return grown;
}

/*!
 * @brief Make room for more entries and name bytes
 *
 * @param[in,out] l        the listing
 * @param[in]     entries  entries about to be added
 * @param[in]     bytes    pool bytes about to be added
 * @returns 0, or -ENOMEM with the listing as it was
 */
int afp_listing_reserve(struct afp_listing *l, uint32_t entries,
                        size_t bytes)
{
    uint64_t need = (uint64_t) l->count + entries;

    if (need > l->capacity) {
        struct afp_dirent *grown = grown_array(l->entries, &l->capacity, need,
                                               sizeof(*l->entries),
                                               AFP_LISTING_MIN_ENTRIES);

        if (grown == NULL) {
            return -ENOMEM;
        }

        l->entries = grown;
    }

    need = (uint64_t) l->pool_used + bytes;

    if (need > l->pool_cap) {
        char *grown = grown_array(l->pool, &l->pool_cap, need, 1, 0);

        if (grown == NULL) {
            return -ENOMEM;
        }

        l->pool = grown;
    }

    return 0;
}

/*!
 * @brief Append a copy of an entry under its name with a suffix
 *
 * The AppleDouble view lists each pseudo-file as such a copy.
 *
 * @param[in,out] l       the listing
 * @param[in]     e       the entry to copy, one of l's, whose name is in
 *                        l's pool
 * @param[in]     suffix  appended to the entry's name
 * @param[in]     kind    the AFP_META_* kind of the copy
 * @param[in]     size    the copy's size
 * @returns 0, or -ENOMEM with the listing as it was
 */
int afp_listing_append(struct afp_listing *l, const struct afp_dirent *e,
                       const char *suffix, unsigned int kind, uint64_t size)
{
    struct afp_dirent copy = *e;
    size_t name_len = strlen(afp_dirent_name(l, e));
    size_t suffix_len = strlen(suffix);
    char *name;
    int ret = afp_listing_reserve(l, 1, name_len + suffix_len + 1U);

    if (ret) {
        return ret;
    }

    /* The arrays may have moved; the offsets have not. */
    name = l->pool + l->pool_used;
    memcpy(name, afp_dirent_name(l, &copy), name_len);
    memcpy(name + name_len, suffix, suffix_len + 1U);
    copy.kind = (uint8_t) kind;
    copy.size = size;
    l->entries[l->count] = copy;
    afp_listing_commit(l);
    return 0;
}

/*!
 * @brief Commit the record at the listing's end, named at its pool's end
 *
 * The producer wrote the record at entries[count] and the name, terminated,
 * at pool + pool_used, within what it reserved.
 *
 * @param[in,out] l  the listing
 */
void afp_listing_commit(struct afp_listing *l)
{
    struct afp_dirent *e = &l->entries[l->count++];
    e->name_off = l->pool_used;
    l->pool_used += (uint32_t) strlen(l->pool + e->name_off) + 1U;
}

/*!
 * @brief Drop the entries whose name decodes to nothing
 *
 * The kernel fails a page that holds an empty name, with the entries after
 * it.
 *
 * @param[in,out] l  the listing
 */
void afp_listing_drop_unnamed(struct afp_listing *l)
{
    uint32_t kept = 0;

    for (uint32_t i = 0; i < l->count; i++) {
        if (afp_dirent_name(l, &l->entries[i])[0] != '\0') {
            l->entries[kept++] = l->entries[i];
        }
    }

    l->count = kept;
}

/*!
 * @brief Free a listing's arrays and leave it empty
 *
 * @param[in,out] l  the listing
 */
void afp_listing_free(struct afp_listing *l)
{
    free(l->entries);
    free(l->pool);
    memset(l, 0, sizeof(*l));
}

/*!
 * @brief Read a record's date as a timespec
 *
 * @param[in] volume  volume of the entry
 * @param[in] e       the record
 * @param[in] bit     the date's bitmap bit
 * @param[in] ns      the date
 * @returns the date, or the connection's time for one the server reports
 *          unset
 */
struct timespec afp_dirent_time(const struct afp_volume *volume,
                                const struct afp_dirent *e, uint32_t bit,
                                int64_t ns)
{
    struct timespec ts;

    if (!(e->valid & bit)) {
        ts.tv_sec = volume->server->connect_time;
        ts.tv_nsec = 0;
        return ts;
    }

    ts.tv_sec = afp_sec_from_ns(ns);
    ts.tv_nsec = ns % AFP_NS_PER_SEC;

    /* The division truncates; a date before 1970 borrows a second. */
    if (ts.tv_nsec < 0) {
        ts.tv_sec--;
        ts.tv_nsec += AFP_NS_PER_SEC;
    }

    return ts;
}

/*!
 * @brief The 512-byte blocks a length takes
 *
 * @param[in] size  the length
 * @returns the blocks, a partly used last one counted
 */
static blkcnt_t blocks_of(off_t size)
{
    return size / 512 + (size % 512 != 0);
}

/*!
 * @brief Make an object's attributes those of a pseudo-file over it
 *
 * The AppleDouble view shows one for each entry's Finder information and
 * comment, and one for the server icon.
 *
 * @param[in,out] stbuf  the object's attributes, made the pseudo-file's
 * @param[in]     size   the pseudo-file's length
 */
void afp_pseudo_file_stat(struct stat *stbuf, off_t size)
{
    stbuf->st_mode = (stbuf->st_mode & ~S_IFMT) | S_IFREG;
    stbuf->st_nlink = 1;
    stbuf->st_size = size;
    stbuf->st_blocks = blocks_of(size);
}

/*!
 * @brief The mode a record shows
 *
 * @param[in] e  the record
 * @returns its UNIX mode when the server sent a nonzero one, else 0700 for
 *          a directory and 0600 for a file; a pseudo-file's is a regular
 *          file's with its object's permissions
 */
mode_t afp_dirent_mode(const struct afp_dirent *e)
{
    mode_t mode = e->isdir ? (0700 | S_IFDIR) : (0600 | S_IFREG);

    if ((e->valid & kFPUnixPrivsBit) && e->unixprivs.permissions != 0) {
        mode = e->unixprivs.permissions;
    }

    /* A folder's pseudo-files too, as appledouble_getattr() shows them. */
    if (e->kind == AFP_META_FINDERINFO || e->kind == AFP_META_COMMENT) {
        mode = (mode & ~S_IFMT) | S_IFREG;
    }

    return mode;
}

/*!
 * @brief Present a record as a struct stat
 *
 * A directory's size is the one Mac OS X 10.2 showed.
 *
 * @param[in]  volume  volume of the entry
 * @param[in]  e       the record
 * @param[out] stbuf   receives the attributes, cleared first
 * @returns 0, or -EIO when the id mapping cannot express the owner
 */
int afp_dirent_to_stat(struct afp_volume *volume, const struct afp_dirent *e,
                       struct stat *stbuf)
{
    unsigned int uid = e->unixprivs.uid;
    unsigned int gid = e->unixprivs.gid;
    struct timespec create = afp_dirent_time(volume, e, kFPCreateDateBit,
                                             e->create_ns);
    struct timespec mod = afp_dirent_time(volume, e, kFPModDateBit,
                                          e->mod_ns);
    memset(stbuf, 0, sizeof(*stbuf));

    if (translate_uidgid_to_client(volume, &uid, &gid)) {
        return -EIO;
    }

    stbuf->st_mode = afp_dirent_mode(e);
    stbuf->st_ino = e->cnid;
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;
    stbuf->st_blksize = 4096;

    if (S_ISDIR(stbuf->st_mode)) {
        stbuf->st_nlink = e->offspring + 2;

        /* macOS's nlink_t is 16 bits: a count it cannot hold shows as 1,
         * "not kept". */
        if (stbuf->st_nlink < 2) {
            stbuf->st_nlink = 1;
        }

        stbuf->st_size = e->kind == AFP_META_RESOURCE
                         ? 0 : (e->offspring * 34) + 24;
    } else {
        /* A fork's length is the server's, unchecked; off_t stops at
         * INT64_MAX. */
        uint64_t len = e->kind == AFP_META_RESOURCE ? e->resourcesize
                       : e->size;
        stbuf->st_nlink = 1;
        stbuf->st_size = (off_t) min(len, (uint64_t) INT64_MAX);
    }

    stbuf->st_blocks = blocks_of(stbuf->st_size);
#ifdef __linux__
    stbuf->st_ctim = create;
    stbuf->st_mtim = mod;
#else
    stbuf->st_ctime = create.tv_sec;
    stbuf->st_mtime = mod.tv_sec;
#endif
#ifdef __APPLE__
    /* The Finder shows the birth time (stat_to_darwin_attr()). */
    stbuf->st_birthtimespec = create;
#endif
    return 0;
}
