/*
 * did.c: the directory cache by parent directory and name
 *
 * Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief The directory cache: objects and absences by directory and name
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "did.h"
#include "lowlevel.h"
#include "resource.h"

/* An object, or its absence, at a name in a listing's spelling; a name
 * beyond U+FFFF keeps each spelling, as its conversion copies it as is. */
struct dir_cache_entry {
    struct dir_cache_entry *next_name;
    struct dir_cache_entry *next_cnid;
    /*! The next entry on its parent's chain */
    struct dir_cache_entry *next_sibling;
    /*! What points at this entry on that chain, for an unlink in place */
    struct dir_cache_entry **pprev_sibling;
    /*! The parameters; cnid and isdir are the object's identity */
    struct afp_dirent e;
    /*! The identity holds until then, or the absence does */
    time_t did_expires;
    /*! The parameters hold until then, 0 when not held */
    time_t stat_expires;
    /*! The change a reply must postdate to store parameters: this object's,
     * or for a new entry the latest a dropped entry carried */
    uint64_t invalidated;
    /*! The number of the request whose reply the entry holds */
    uint64_t stored;
    /*! The serial of the latest listing that showed the object, 0 for none */
    uint64_t listed;
    unsigned int parent_did;
    uint32_t hash;
    uint16_t name_len;
    /*! An absence: no identity, no parameters */
    uint8_t negative;
    /*! Used since the clock hand passed */
    uint8_t referenced;
    /*! One attribute's value, for the kernel's second question */
    char *xattr_name;
    char *xattr_value;
    size_t xattr_len;
    time_t xattr_expires;
    char name[];
};

/*!
 * @brief Free an entry's attribute value
 *
 * @param[in,out] entry  the entry
 */
static void clear_xattr(struct dir_cache_entry *entry)
{
    free(entry->xattr_name);
    free(entry->xattr_value);
    entry->xattr_name = NULL;
    entry->xattr_value = NULL;
    entry->xattr_len = 0;
    entry->xattr_expires = 0;
}

/*!
 * @brief The lives' clock, which no step of the wall clock moves
 *
 * Linux's CLOCK_BOOTTIME counts a suspend; FreeBSD's stops in one.
 *
 * @returns CLOCK_BOOTTIME's seconds where defined, else CLOCK_MONOTONIC's
 */
static time_t clock_now(void)
{
    struct timespec ts;
#ifdef CLOCK_BOOTTIME

    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
        return ts.tv_sec;
    }

#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/*!
 * @brief The wall clock an object's age is measured on
 *
 * @returns time(2)
 */
static time_t wall_now(void)
{
    return time(NULL);
}

/*!
 * @brief Count a change, under the write lock, or number a request
 *
 * A request takes its number without the lock, so one count orders the
 * changes and the requests.
 *
 * @param[in,out] c  the cache
 * @returns the number
 */
static uint64_t count_change(struct dir_cache *c)
{
    return __atomic_add_fetch(&c->changes, 1, __ATOMIC_RELAXED);
}

/*!
 * @brief FNV-1a over the parent's ID and the name
 *
 * @param[in] parent_did  the directory's ID
 * @param[in] name        the name
 * @param[in] len         its length
 * @returns the hash
 */
static uint32_t hash_name(unsigned int parent_did, const char *name,
                          size_t len)
{
    uint32_t h = UINT32_C(2166136261);
    size_t i;

    for (i = 0; i < 4; i++) {
        h ^= (parent_did >> (8 * i)) & 0xffU;
        h *= UINT32_C(16777619);
    }

    for (i = 0; i < len; i++) {
        h ^= (unsigned char) name[i];
        h *= UINT32_C(16777619);
    }

    return h;
}

/*!
 * @brief Set up a volume's cache, empty
 *
 * @param[in,out] volume  the volume
 */
void dir_cache_init(struct afp_volume *volume)
{
    struct dir_cache *c = &volume->cache;
    memset(c, 0, sizeof(*c));
    c->max_entries = DIR_CACHE_MAX_ENTRIES;
    c->clock = clock_now;
    c->wall = wall_now;
    pthread_rwlock_init(&c->lock, NULL);
}

/*!
 * @brief Make the three tables at the first insert
 *
 * @param[in,out] c  the cache
 * @returns 0, or -ENOMEM
 */
static int make_tables(struct dir_cache *c)
{
    uint32_t n = 1;

    if (c->by_name) {
        return 0;
    }

    while (n < c->max_entries * 2U) {
        n <<= 1;
    }

    c->by_name = calloc(n, sizeof(*c->by_name));
    c->by_cnid = calloc(n, sizeof(*c->by_cnid));
    c->by_parent = calloc(n, sizeof(*c->by_parent));

    if (!c->by_name || !c->by_cnid || !c->by_parent) {
        free(c->by_name);
        free(c->by_cnid);
        free(c->by_parent);
        c->by_name = NULL;
        c->by_cnid = NULL;
        c->by_parent = NULL;
        return -ENOMEM;
    }

    c->buckets = n;
    return 0;
}

/*!
 * @brief Find a name's entry
 *
 * @param[in] c           the cache
 * @param[in] parent_did  the directory's ID
 * @param[in] name        the name
 * @param[in] len         its length
 * @param[in] hash        hash_name() of the two
 * @returns the entry, or NULL
 */
static struct dir_cache_entry *find_name(const struct dir_cache *c,
                                         unsigned int parent_did,
                                         const char *name, size_t len,
                                         uint32_t hash)
{
    if (c->by_name == NULL) {
        return NULL;
    }

    for (struct dir_cache_entry *p = c->by_name[hash & (c->buckets - 1)];
            p; p = p->next_name) {
        if (p->hash == hash && p->parent_did == parent_did
                && p->name_len == len && memcmp(p->name, name, len) == 0) {
            return p;
        }
    }

    return NULL;
}

/*!
 * @brief Find a name's entry by the name alone
 *
 * @param[in] c           the cache
 * @param[in] parent_did  the directory's ID
 * @param[in] name        the name
 * @returns the entry, or NULL
 */
static struct dir_cache_entry *find(const struct dir_cache *c,
                                    unsigned int parent_did, const char *name)
{
    size_t len = strlen(name);
    return find_name(c, parent_did, name, len,
                     hash_name(parent_did, name, len));
}

/*!
 * @brief Put an entry on its node ID's chain
 *
 * @param[in,out] c      the cache
 * @param[in,out] entry  the entry
 */
static void link_cnid(struct dir_cache *c, struct dir_cache_entry *entry)
{
    struct dir_cache_entry **head = &c->by_cnid[entry->e.cnid
                                                & (c->buckets - 1)];
    entry->next_cnid = *head;
    *head = entry;
}

/*!
 * @brief Take an entry off its node ID's chain; an absence is on none
 *
 * @param[in,out] c      the cache
 * @param[in,out] entry  the entry
 */
static void unlink_cnid(struct dir_cache *c, struct dir_cache_entry *entry)
{
    uint32_t slot = entry->e.cnid & (c->buckets - 1);

    if (entry->negative) {
        return;
    }

    for (struct dir_cache_entry **pp = &c->by_cnid[slot]; *pp;
            pp = &(*pp)->next_cnid) {
        if (*pp == entry) {
            *pp = entry->next_cnid;
            return;
        }
    }
}

/*!
 * @brief Take an entry out of the three tables and free it
 *
 * The entries made after it start from the change it carried.
 *
 * @param[in,out] c      the cache
 * @param[in]     entry  the entry
 */
static void drop(struct dir_cache *c, struct dir_cache_entry *entry)
{
    uint32_t slot = entry->hash & (c->buckets - 1);

    if (entry->invalidated > c->dropped_change) {
        c->dropped_change = entry->invalidated;
    }

    for (struct dir_cache_entry **pp = &c->by_name[slot]; *pp;
            pp = &(*pp)->next_name) {
        if (*pp == entry) {
            *pp = entry->next_name;
            break;
        }
    }

    unlink_cnid(c, entry);
    *entry->pprev_sibling = entry->next_sibling;

    if (entry->next_sibling) {
        entry->next_sibling->pprev_sibling = entry->pprev_sibling;
    }

    clear_xattr(entry);
    free(entry);
    c->count--;
}

/*!
 * @brief Evict one entry
 *
 * The clock hand clears the marks it passes and takes the first unmarked.
 *
 * @param[in,out] c  the cache
 */
static void evict_one(struct dir_cache *c)
{
    for (uint32_t steps = 0; steps < c->buckets * 2U; steps++) {
        struct dir_cache_entry **pp = &c->by_name[c->clock_hand];

        while (*pp) {
            if (__atomic_exchange_n(&(*pp)->referenced, 0,
                                    __ATOMIC_RELAXED)) {
                pp = &(*pp)->next_name;
                continue;
            }

            /* Moving on keeps this bucket's cleared marks for a full round. */
            drop(c, *pp);
            c->stats.evictions++;
            c->clock_hand = (c->clock_hand + 1U) & (c->buckets - 1);
            return;
        }

        c->clock_hand = (c->clock_hand + 1U) & (c->buckets - 1);
    }
}

/*!
 * @brief Make the entry for a name, evicting one when the cache is full
 *
 * It starts as an absence carrying the latest change a dropped entry had.
 *
 * @param[in,out] c           the cache
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name
 * @param[in]     len         its length
 * @param[in]     hash        hash_name() of the two
 * @returns the entry, or NULL without memory
 */
static struct dir_cache_entry *make(struct dir_cache *c,
                                    unsigned int parent_did,
                                    const char *name, size_t len,
                                    uint32_t hash)
{
    struct dir_cache_entry *entry;
    struct dir_cache_entry **head;

    if (make_tables(c)) {
        return NULL;
    }

    if (c->count >= c->max_entries) {
        evict_one(c);
    }

    entry = calloc(1, sizeof(*entry) + len + 1U);

    if (entry == NULL) {
        return NULL;
    }

    entry->parent_did = parent_did;
    entry->hash = hash;
    entry->name_len = (uint16_t) len;
    entry->negative = 1;
    entry->invalidated = c->dropped_change;
    memcpy(entry->name, name, len + 1U);
    head = &c->by_name[hash & (c->buckets - 1)];
    entry->next_name = *head;
    *head = entry;
    head = &c->by_parent[parent_did & (c->buckets - 1)];
    entry->next_sibling = *head;

    if (*head) {
        (*head)->pprev_sibling = &entry->next_sibling;
    }

    *head = entry;
    entry->pprev_sibling = head;
    c->count++;
    c->stats.inserts++;
    return entry;
}

/*!
 * @brief Find a name's entry, or make it
 *
 * @param[in,out] c           the cache
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name
 * @returns the entry, or NULL without memory
 */
static struct dir_cache_entry *find_or_make(struct dir_cache *c,
                                            unsigned int parent_did,
                                            const char *name)
{
    size_t len = strlen(name);
    uint32_t hash = hash_name(parent_did, name, len);
    struct dir_cache_entry *entry = find_name(c, parent_did, name, len,
                                              hash);
    return entry ? entry : make(c, parent_did, name, len, hash);
}

/*!
 * @brief Stamp a change on a name's entry, under the write lock
 *
 * A name the cache lacks, or holds as absent, gets an entry that answers for
 * nothing to carry the stamp; without memory it goes to dropped_change.
 *
 * @param[in,out] c           the cache
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name
 * @returns the entry, or NULL without memory
 */
static struct dir_cache_entry *stamp_name(struct dir_cache *c,
                                          unsigned int parent_did,
                                          const char *name)
{
    struct dir_cache_entry *entry = find_or_make(c, parent_did, name);

    if (entry) {
        if (entry->negative) {
            entry->did_expires = 0;
        }

        entry->invalidated = count_change(c);
        /* The stamp outlives the clock hand's next pass. */
        __atomic_store_n(&entry->referenced, 1, __ATOMIC_RELAXED);
        clear_xattr(entry);
    } else {
        c->dropped_change = count_change(c);
    }

    return entry;
}

/*!
 * @brief The life of an object's parameters
 *
 * @param[in] e     the record
 * @param[in] wall  the wall clock's time
 * @returns the life, in seconds
 */
static time_t stat_ttl(const struct afp_dirent *e, time_t wall)
{
    /* 64 bits: an old date's age overflows a 32-bit time_t */
    int64_t ttl = 0;

    if (e->valid & kFPModDateBit) {
        ttl = ((int64_t) wall - e->mod_ns / AFP_NS_PER_SEC) / 10;
    }

    if (ttl < DIR_CACHE_STAT_TTL_MIN) {
        return DIR_CACHE_STAT_TTL_MIN;
    }

    return ttl > DIR_CACHE_STAT_TTL_MAX ? DIR_CACHE_STAT_TTL_MAX : (time_t) ttl;
}

/*!
 * @brief Drop a directory's child folders, under the write lock
 *
 * The drop counts as a removal from the directory, so a reply sent before
 * it stores none of them again.
 *
 * @param[in,out] c    the cache
 * @param[in]     did  the directory's ID
 */
static void drop_child_folders(struct dir_cache *c, unsigned int did)
{
    struct dir_cache_entry *next;
    int dropped = 0;

    if (c->by_parent == NULL) {
        return;
    }

    for (struct dir_cache_entry *p = c->by_parent[did & (c->buckets - 1)];
            p; p = next) {
        next = p->next_sibling;

        if (p->parent_did == did && !p->negative && p->e.isdir
                && p->e.cnid != did) {
            drop(c, p);
            dropped = 1;
        }
    }

    if (dropped) {
        c->removals[did % DIR_CACHE_CNID_SLOTS] = count_change(c);
    }
}

/*!
 * @brief Store a record in its entry: an identity, with or without parameters
 *
 * The reply to a request numbered before the one whose reply the entry
 * holds stores nothing.
 *
 * @param[in,out] c          the cache
 * @param[in,out] entry      the entry
 * @param[in]     e          the record
 * @param[in]     sent       dir_cache_changes() before the request was sent
 * @param[in]     now        the cache's clock
 * @param[in]     wall       the wall clock's time
 * @param[in]     with_stat  whether the parameters hold
 */
static void store(struct dir_cache *c, struct dir_cache_entry *entry,
                  const struct afp_dirent *e, uint64_t sent, time_t now,
                  time_t wall, int with_stat)
{
    if (sent < entry->stored) {
        return;
    }

    /* An attribute's value is its object's alone. */
    if (entry->negative || entry->e.cnid != e->cnid) {
        clear_xattr(entry);
    }

    /* A folder dated later may hold a child folder another session
     * replaced: the walk asks the server for its child folders again. */
    if (!entry->negative && e->isdir && entry->e.cnid == e->cnid
            && (entry->e.valid & e->valid & kFPModDateBit)
            && e->mod_ns > entry->e.mod_ns) {
        drop_child_folders(c, e->cnid);
    }

    unlink_cnid(c, entry);
    entry->e = *e;
    entry->negative = 0;
    entry->stored = sent;
    entry->did_expires = now + DIR_CACHE_DID_TTL;
    entry->stat_expires = with_stat ? now + stat_ttl(e, wall) : 0;
    link_cnid(c, entry);
    __atomic_store_n(&entry->referenced, 1, __ATOMIC_RELAXED);
}

/*!
 * @brief Forget the parameters of every entry with a node ID, under the
 * write lock
 *
 * The slot stamps replies for the object's names the cache lacks yet.
 *
 * @param[in,out] c    the cache
 * @param[in]     did  the node ID
 */
static void invalidate_cnid(struct dir_cache *c, unsigned int did)
{
    c->cnid_changes[did % DIR_CACHE_CNID_SLOTS] = count_change(c);

    if (c->by_cnid == NULL) {
        return;
    }

    for (struct dir_cache_entry *p = c->by_cnid[did & (c->buckets - 1)];
            p; p = p->next_cnid) {
        if (p->e.cnid == did) {
            p->stat_expires = 0;
            p->invalidated = count_change(c);
            clear_xattr(p);
        }
    }
}

/*!
 * @brief Whether no change hit the reply's name or node ID after its request
 *
 * @param[in] c      the cache
 * @param[in] entry  the entry at the reply's name
 * @param[in] e      the reply's record
 * @param[in] sent   dir_cache_changes() before the request was sent
 * @returns 1 when they may, 0 otherwise
 */
static int params_hold(const struct dir_cache *c,
                       const struct dir_cache_entry *entry,
                       const struct afp_dirent *e, uint64_t sent)
{
    return entry->invalidated <= sent
           && c->cnid_changes[e->cnid % DIR_CACHE_CNID_SLOTS] <= sent;
}

/*!
 * @brief Whether a removal came after a request about a folder's object
 *
 * @param[in] c           the cache
 * @param[in] parent_did  the reply's folder
 * @param[in] cnid        the reply's object, or 0
 * @param[in] sent        dir_cache_changes() before the request was sent
 * @returns 1 when one did, or a purge
 */
static int removed_since(const struct dir_cache *c, unsigned int parent_did,
                         unsigned int cnid, uint64_t sent)
{
    return c->last_removal > sent
           || c->removals[parent_did % DIR_CACHE_CNID_SLOTS] > sent
           || (cnid != 0
               && c->cnid_removals[cnid % DIR_CACHE_CNID_SLOTS] > sent);
}

/*!
 * @brief Drop the objects a directory's entries hold, under the write lock
 *
 * @param[in,out] c       the cache
 * @param[in]     did     the directory's ID
 * @param[in]     serial  a listing's serial, whose entries stay with those
 *                        of the listings begun after it, or 0
 */
static void drop_children(struct dir_cache *c, unsigned int did,
                          uint64_t serial)
{
    struct dir_cache_entry *next;

    if (c->by_parent == NULL) {
        return;
    }

    for (struct dir_cache_entry *p = c->by_parent[did & (c->buckets - 1)];
            p; p = next) {
        next = p->next_sibling;

        /* A directory's own record is none of its objects. */
        if (p->parent_did == did && !p->negative && p->e.cnid != did
                && (serial == 0 || p->listed < serial)) {
            drop(c, p);
        }
    }
}

/*!
 * @brief Look a name up in its directory at a time of the cache's clock
 *
 * @param[in]  volume      volume of the directory
 * @param[in]  parent_did  the directory's ID
 * @param[in]  name        the name, as dir_cache_resolve() gives it
 * @param[out] e           receives the record when its identity holds
 * @param[in]  now         the cache's clock
 * @returns dir_cache_lookup()'s answer
 */
static unsigned int lookup_at(struct afp_volume *volume,
                              unsigned int parent_did, const char *name,
                              struct afp_dirent *e, time_t now)
{
    struct dir_cache *c = &volume->cache;
    size_t len = strlen(name);
    uint32_t hash = hash_name(parent_did, name, len);
    struct dir_cache_entry *entry;
    unsigned int found = 0;
    pthread_rwlock_rdlock(&c->lock);
    entry = find_name(c, parent_did, name, len, hash);

    if (entry && now < entry->did_expires) {
        /* Testing first keeps a hot entry's cache line shared by readers. */
        if (!__atomic_load_n(&entry->referenced, __ATOMIC_RELAXED)) {
            __atomic_store_n(&entry->referenced, 1, __ATOMIC_RELAXED);
        }

        if (entry->negative) {
            found = DIR_CACHE_NEGATIVE;
        } else {
            found = DIR_CACHE_DID;

            if (now < entry->stat_expires) {
                found |= DIR_CACHE_STAT;
            }

            *e = entry->e;
        }
    }

    pthread_rwlock_unlock(&c->lock);
    __atomic_add_fetch(found & DIR_CACHE_NEGATIVE ? &c->stats.negative_hits
                       : found ? &c->stats.hits : &c->stats.misses,
                       1, __ATOMIC_RELAXED);

    if (found & DIR_CACHE_STAT) {
        __atomic_add_fetch(&c->stats.stat_hits, 1, __ATOMIC_RELAXED);
    }

    return found;
}

/*!
 * @brief Look a name up in its directory
 *
 * @param[in]  volume      volume of the directory
 * @param[in]  parent_did  the directory's ID
 * @param[in]  name        the name, as dir_cache_resolve() gives it
 * @param[out] e           receives the record when its identity holds
 * @returns DIR_CACHE_DID when the object's identity holds, with
 *          DIR_CACHE_STAT when its parameters do too; DIR_CACHE_NEGATIVE
 *          for a name known to hold nothing; 0 for one the cache cannot
 *          answer for
 */
unsigned int dir_cache_lookup(struct afp_volume *volume,
                              unsigned int parent_did, const char *name,
                              struct afp_dirent *e)
{
    return lookup_at(volume, parent_did, name, e, volume->cache.clock());
}

/*!
 * @brief Number a request ahead of its send
 *
 * The number is above every change counted and every request numbered
 * before it. The reply's store drops what a removal or a change counted
 * since may have outdated, and stores nothing over a later request's reply.
 *
 * @param[in] volume  the volume
 * @returns the number
 */
uint64_t dir_cache_changes(struct afp_volume *volume)
{
    return count_change(&volume->cache);
}

/*!
 * @brief Whether a record contradicts the entry at its name
 *
 * @param[in] entry  the entry
 * @param[in] e      the record
 * @returns 1 when the entry holds an absence, another object or another
 *          type, 0 otherwise
 */
static int contradicts(const struct dir_cache_entry *entry,
                       const struct afp_dirent *e)
{
    /* A reply gives a directory's type as 0x80, the cache's own record 1. */
    return entry->negative || entry->e.cnid != e->cnid
           || !entry->e.isdir != !e->isdir;
}

/*!
 * @brief Store a reply's record at its name
 *
 * A reply a removal overtook stores nothing and drops an entry it contradicts;
 * one a change overtook stores the identity alone.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name, as dir_cache_resolve() gives it
 * @param[in]     e           the record
 * @param[in]     sent        dir_cache_changes() before the request was sent
 */
void dir_cache_upsert(struct afp_volume *volume, unsigned int parent_did,
                      const char *name, const struct afp_dirent *e,
                      uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    time_t wall = c->wall();
    struct dir_cache_entry *entry;
    pthread_rwlock_wrlock(&c->lock);

    if (removed_since(c, parent_did, e->cnid, sent)) {
        entry = find(c, parent_did, name);

        if (entry && contradicts(entry, e)) {
            drop(c, entry);
        }
    } else if ((entry = find_or_make(c, parent_did, name)) != NULL) {
        store(c, entry, e, sent, now, wall, params_hold(c, entry, e, sent));
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Remember that a name holds nothing
 *
 * A removal or a change at the name since the request, or a later
 * request's reply, keeps it out, the removal dropping the name's object;
 * the clock hand takes it first.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name, as dir_cache_resolve() gives it
 * @param[in]     sent        dir_cache_changes() before the request was sent
 */
void dir_cache_insert_negative(struct afp_volume *volume,
                               unsigned int parent_did, const char *name,
                               uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    struct dir_cache_entry *entry;
    pthread_rwlock_wrlock(&c->lock);

    if (removed_since(c, parent_did, 0, sent)) {
        entry = find(c, parent_did, name);

        if (entry && !entry->negative) {
            drop(c, entry);
        }
    } else if ((entry = find_or_make(c, parent_did, name)) != NULL
               && entry->invalidated <= sent && entry->stored <= sent) {
        unlink_cnid(c, entry);
        memset(&entry->e, 0, sizeof(entry->e));
        entry->negative = 1;
        entry->stored = sent;
        entry->did_expires = now + DIR_CACHE_NEGATIVE_TTL;
        entry->stat_expires = 0;
        __atomic_store_n(&entry->referenced, 0, __ATOMIC_RELAXED);
        clear_xattr(entry);
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Store a directory's identity at its name, under the write lock
 *
 * An entry whose identity holds that ID keeps its parameters.
 *
 * @param[in,out] c           the cache
 * @param[in]     parent_did  its parent's ID
 * @param[in]     name        its name
 * @param[in]     did         its ID
 * @param[in]     sent        dir_cache_changes() before the request was sent
 * @param[in]     now         the cache's clock
 */
static void store_dir(struct dir_cache *c, unsigned int parent_did,
                      const char *name, unsigned int did, uint64_t sent,
                      time_t now)
{
    struct afp_dirent e;
    struct dir_cache_entry *entry = find_or_make(c, parent_did, name);
    memset(&e, 0, sizeof(e));
    e.cnid = did;
    e.isdir = 1;
    e.valid = kFPNodeIDBit;

    if (entry && (entry->negative || now >= entry->did_expires
                  || entry->e.cnid != did)) {
        store(c, entry, &e, sent, now, 0, 0);
    }
}

/*!
 * @brief Remember the ID a listing gives a directory
 *
 * A removal since the listing began keeps the ID out and drops an entry
 * holding another ID or an absence, as one of the two is stale.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  its parent's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @param[in]     did         its ID
 * @param[in]     sent        dir_cache_changes() before the listing began
 */
void dir_cache_insert_dir(struct afp_volume *volume, unsigned int parent_did,
                          const char *name, unsigned int did, uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    struct dir_cache_entry *entry;
    pthread_rwlock_wrlock(&c->lock);

    if (!removed_since(c, parent_did, did, sent)) {
        store_dir(c, parent_did, name, did, sent, now);
    } else if ((entry = find(c, parent_did, name)) != NULL
               && (entry->negative || entry->e.cnid != did)) {
        drop(c, entry);
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Begin a listing
 *
 * @param[in,out] volume  volume of the directory
 * @returns the serial the listing's pages stamp their entries with, above
 *          every serial before it
 */
uint64_t dir_cache_listing(struct afp_volume *volume)
{
    struct dir_cache *c = &volume->cache;
    uint64_t serial;
    pthread_rwlock_wrlock(&c->lock);
    serial = ++c->listings;
    pthread_rwlock_unlock(&c->lock);
    return serial;
}

/*!
 * @brief Store a page of a listing, in one hold of the write lock
 *
 * A record changed since the page was sent keeps its identity alone; a page
 * a removal overtook only marks what it shows and drops what it contradicts.
 *
 * @param[in,out] volume  volume of the directory
 * @param[in]     l       the listing
 * @param[in]     first   the page's first entry in l
 * @param[in]     sent    dir_cache_changes() before the page's request
 * @param[in]     serial  dir_cache_listing()'s serial for the listing
 */
void dir_cache_insert_listing(struct afp_volume *volume,
                              const struct afp_listing *l, uint32_t first,
                              uint64_t sent, uint64_t serial)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    time_t wall = c->wall();
    pthread_rwlock_wrlock(&c->lock);

    for (uint32_t i = first; i < l->count; i++) {
        const struct afp_dirent *e = &l->entries[i];
        const char *name = afp_dirent_name(l, e);
        struct dir_cache_entry *entry;
        int kept_out;

        if (e->kind != AFP_META_NONE || name[0] == '\0'
                || (e->valid & (kFPNodeIDBit | kFPParentDirIDBit))
                != (kFPNodeIDBit | kFPParentDirIDBit)) {
            continue;
        }

        kept_out = removed_since(c, e->parent_did, e->cnid, sent);
        entry = kept_out || i >= c->max_entries / 2
                ? find(c, e->parent_did, name)
                : find_or_make(c, e->parent_did, name);

        if (entry == NULL) {
            continue;
        }

        /* A second object at the name, two spellings the conversion takes
         * as one, leaves an entry that answers for nothing. */
        if (entry->listed >= serial && contradicts(entry, e)) {
            unlink_cnid(c, entry);
            memset(&entry->e, 0, sizeof(entry->e));
            entry->negative = 1;
            entry->did_expires = 0;
            entry->stat_expires = 0;
            clear_xattr(entry);
            continue;
        }

        if (!kept_out) {
            store(c, entry, e, sent, now, wall,
                  params_hold(c, entry, e, sent));
        } else if (contradicts(entry, e)) {
            drop(c, entry);
            continue;
        }

        if (entry->listed < serial) {
            entry->listed = serial;
        }
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Forget the objects a complete listing did not show
 *
 * The sweep counts as a removal from the directory: a reply about its
 * objects sent before may show one the listing did not.
 *
 * @param[in,out] volume  volume of the directory
 * @param[in]     did     the listed directory's ID
 * @param[in]     serial  the listing's serial
 */
void dir_cache_listed(struct afp_volume *volume, unsigned int did,
                      uint64_t serial)
{
    struct dir_cache *c = &volume->cache;
    pthread_rwlock_wrlock(&c->lock);
    drop_children(c, did, serial);
    c->removals[did % DIR_CACHE_CNID_SLOTS] = count_change(c);
    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Forget a directory a listing found empty, and its objects
 *
 * An empty listing carries no ID to check the cached one. The forget counts
 * as a removal from the folder and from its parent, as a reply about the
 * objects of either sent before may be stale.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  its parent's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @returns 1 when the cache held the folder's identity, 0 when the folder's
 *          ID must come from the server for its objects to go
 */
int dir_cache_listed_empty(struct afp_volume *volume,
                           unsigned int parent_did, const char *name)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    struct dir_cache_entry *entry;
    uint64_t stamp;
    int found = 0;
    pthread_rwlock_wrlock(&c->lock);
    entry = find(c, parent_did, name);
    stamp = count_change(c);
    c->removals[parent_did % DIR_CACHE_CNID_SLOTS] = stamp;

    if (entry) {
        if (!entry->negative && now < entry->did_expires) {
            drop_children(c, entry->e.cnid, 0);
            c->removals[entry->e.cnid % DIR_CACHE_CNID_SLOTS] = stamp;
            found = 1;
        }

        drop(c, entry);
    }

    pthread_rwlock_unlock(&c->lock);
    return found;
}

/*!
 * @brief Drop every entry of an object, under the write lock
 *
 * Each spelling of a name on a case-folding server is an entry of its own.
 *
 * @param[in,out] c     the cache
 * @param[in]     cnid  the object's node ID
 */
static void drop_object(struct dir_cache *c, unsigned int cnid)
{
    struct dir_cache_entry *next;

    if (c->by_cnid == NULL) {
        return;
    }

    for (struct dir_cache_entry *p = c->by_cnid[cnid & (c->buckets - 1)];
            p; p = next) {
        next = p->next_cnid;

        if (p->e.cnid == cnid) {
            drop(c, p);
        }
    }
}

/*!
 * @brief Forget a name and its object's entries, under the write lock
 *
 * The directory's parameters go, and the removal stamps the directory's
 * slot and the object's.
 *
 * @param[in,out] c           the cache
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name
 */
static void remove_name(struct dir_cache *c, unsigned int parent_did,
                        const char *name)
{
    struct dir_cache_entry *entry = find(c, parent_did, name);
    unsigned int cnid = entry && !entry->negative ? entry->e.cnid : 0;
    uint64_t stamp;

    if (entry) {
        drop(c, entry);
    }

    if (cnid) {
        drop_object(c, cnid);
    }

    invalidate_cnid(c, parent_did);
    stamp = count_change(c);
    c->removals[parent_did % DIR_CACHE_CNID_SLOTS] = stamp;

    if (cnid) {
        c->cnid_removals[cnid % DIR_CACHE_CNID_SLOTS] = stamp;
    }

    c->stats.invalidations++;
}

/*!
 * @brief Forget a name: its object is gone, or one is at it now
 *
 * Its directory's parameters go too, as its date and entry count changed.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name, as dir_cache_resolve() gives it
 */
void dir_cache_remove(struct afp_volume *volume, unsigned int parent_did,
                      const char *name)
{
    struct dir_cache *c = &volume->cache;
    pthread_rwlock_wrlock(&c->lock);
    remove_name(c, parent_did, name);
    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Remember the directory a create made at a name
 *
 * Forgets as dir_cache_remove() does, then keeps the ID unless a removal
 * counted since the create was sent may have moved the directory away.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  its parent's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @param[in]     did         its ID, as the create's reply gives it
 * @param[in]     sent        dir_cache_changes() before the create was sent
 */
void dir_cache_made_dir(struct afp_volume *volume, unsigned int parent_did,
                        const char *name, unsigned int did, uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    int kept_out;
    pthread_rwlock_wrlock(&c->lock);
    kept_out = removed_since(c, parent_did, did, sent);
    remove_name(c, parent_did, name);

    if (!kept_out) {
        store_dir(c, parent_did, name, did, sent, now);
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Forget a directory's parameters: a request dated it
 *
 * @param[in,out] volume  volume of the directory
 * @param[in]     did     the directory's ID
 */
void dir_cache_forget_dir(struct afp_volume *volume, unsigned int did)
{
    struct dir_cache *c = &volume->cache;
    pthread_rwlock_wrlock(&c->lock);
    invalidate_cnid(c, did);
    c->stats.invalidations++;
    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Move an object's entry to its new directory and name
 *
 * The entries beneath a folder stay under its ID; its other names, the new
 * name's old entry and the parameters, both directories' too, go.
 *
 * @param[in,out] volume    volume of the directories
 * @param[in]     from_did  the old directory's ID
 * @param[in]     from      the old name, as dir_cache_resolve() gives it
 * @param[in]     to_did    the new directory's ID
 * @param[in]     to        the new name, as dir_cache_resolve() gives it
 * @param[in]     cnid      the object's node ID the cache held at the old
 *                          name before the request, or 0
 * @param[in]     sent      dir_cache_changes() before the move was sent
 */
void dir_cache_rename(struct afp_volume *volume, unsigned int from_did,
                      const char *from, unsigned int to_did, const char *to,
                      unsigned int cnid, uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    time_t now = c->clock();
    struct afp_dirent e;
    struct dir_cache_entry *entry;
    time_t expires = 0;
    memset(&e, 0, sizeof(e));
    pthread_rwlock_wrlock(&c->lock);
    entry = find(c, from_did, from);

    if (entry && cnid && !entry->negative && entry->e.cnid == cnid
            && now < entry->did_expires
            && !removed_since(c, from_did, cnid, sent)
            && !removed_since(c, to_did, 0, sent)) {
        e = entry->e;
        expires = entry->did_expires;
    }

    remove_name(c, from_did, from);

    if (cnid) {
        drop_object(c, cnid);
    }

    remove_name(c, to_did, to);

    if (expires && (entry = find_or_make(c, to_did, to)) != NULL) {
        e.parent_did = to_did;
        store(c, entry, &e, sent, now, 0, 0);
        entry->did_expires = expires;
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Forget an object's parameters: a request changed them
 *
 * The node ID finds the object under a name another session gave it.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name, as dir_cache_resolve() gives it
 * @param[in]     cnid        the object's node ID, or 0 for the cache's
 */
void dir_cache_invalidate(struct afp_volume *volume, unsigned int parent_did,
                          const char *name, unsigned int cnid)
{
    struct dir_cache *c = &volume->cache;
    struct dir_cache_entry *entry;
    pthread_rwlock_wrlock(&c->lock);
    entry = stamp_name(c, parent_did, name);

    if (entry) {
        entry->stat_expires = 0;

        if (cnid == 0 && !entry->negative) {
            cnid = entry->e.cnid;
        }
    }

    if (cnid) {
        invalidate_cnid(c, cnid);
    }

    c->stats.invalidations++;
    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Check a file's record against what its open reported
 *
 * A record of another object goes, and one of another length forgets its
 * parameters, so a re-opened file another session replaced or grew reads
 * whole, as NFS's close-to-open consistency gives.
 *
 * @param[in,out] volume      volume of the file
 * @param[in]     parent_did  its directory's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @param[in]     cnid        the node ID the open reported, or 0
 * @param[in]     size        the data fork's length the open reported
 * @returns 1 when the record held another object or length, 0 otherwise
 */
int dir_cache_check_open(struct afp_volume *volume, unsigned int parent_did,
                         const char *name, unsigned int cnid, uint64_t size)
{
    struct dir_cache *c = &volume->cache;
    const struct dir_cache_entry *entry;
    int moved;
    int stale;
    pthread_rwlock_rdlock(&c->lock);
    entry = find(c, parent_did, name);
    moved = entry && !entry->negative && cnid != 0 && entry->e.cnid != cnid;
    stale = moved || (entry && !entry->negative && entry->stat_expires != 0
                      && entry->e.size != size);
    pthread_rwlock_unlock(&c->lock);

    if (moved) {
        dir_cache_remove(volume, parent_did, name);
    } else if (stale) {
        dir_cache_invalidate(volume, parent_did, name, 0);
    }

    return stale;
}

/*!
 * @brief Forget an object's attribute value after a set or a removal
 *
 * The parameters stay, as no attribute is one of them; the stamp keeps out
 * a value read before, under every name of the object.
 *
 * @param[in,out] volume      volume of the object
 * @param[in]     parent_did  its directory's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 */
void dir_cache_forget_xattr(struct afp_volume *volume,
                            unsigned int parent_did, const char *name)
{
    struct dir_cache *c = &volume->cache;
    const struct dir_cache_entry *entry;
    pthread_rwlock_wrlock(&c->lock);
    entry = stamp_name(c, parent_did, name);

    if (entry && !entry->negative) {
        unsigned int cnid = entry->e.cnid;
        c->cnid_changes[cnid % DIR_CACHE_CNID_SLOTS] = count_change(c);

        for (struct dir_cache_entry *p = c->by_cnid[cnid & (c->buckets - 1)];
                p; p = p->next_cnid) {
            if (p->e.cnid == cnid) {
                p->invalidated = count_change(c);
                clear_xattr(p);
            }
        }
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Read an attribute's value from its object's entry
 *
 * The value goes once read, as it serves the kernel's second question.
 *
 * @param[in,out] volume      volume of the object
 * @param[in]     parent_did  its directory's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @param[in]     xattr       the attribute's AFP name
 * @param[out]    value       receives the value, or NULL
 * @param[in]     size        capacity of value; 0 asks for the length alone
 * @returns the value's length, -ERANGE when value is too small, or -1
 *          when the entry holds no fresh value of that attribute
 */
int dir_cache_get_xattr(struct afp_volume *volume, unsigned int parent_did,
                        const char *name, const char *xattr, void *value,
                        size_t size)
{
    struct dir_cache *c = &volume->cache;
    struct dir_cache_entry *entry;
    int ret = -1;
    pthread_rwlock_wrlock(&c->lock);
    entry = find(c, parent_did, name);

    if (entry && !entry->negative && entry->xattr_name
            && strcmp(entry->xattr_name, xattr) == 0
            && c->clock() < entry->xattr_expires) {
        if (size == 0) {
            ret = (int) entry->xattr_len;
        } else if (size < entry->xattr_len) {
            ret = -ERANGE;
        } else {
            memcpy(value, entry->xattr_value, entry->xattr_len);
            ret = (int) entry->xattr_len;
            clear_xattr(entry);
        }
    }

    pthread_rwlock_unlock(&c->lock);
    return ret;
}

/*!
 * @brief Keep an attribute's value on its object's entry
 *
 * Nothing is kept for an object the cache lacks, or for a reply a removal
 * or a change of the object overtook.
 *
 * @param[in,out] volume      volume of the object
 * @param[in]     parent_did  its directory's ID
 * @param[in]     name        its name, as dir_cache_resolve() gives it
 * @param[in]     xattr       the attribute's AFP name
 * @param[in]     value       the value
 * @param[in]     len         bytes of value
 * @param[in]     sent        dir_cache_changes() before the request was sent
 */
void dir_cache_set_xattr(struct afp_volume *volume, unsigned int parent_did,
                         const char *name, const char *xattr,
                         const void *value, size_t len, uint64_t sent)
{
    struct dir_cache *c = &volume->cache;
    struct dir_cache_entry *entry;
    char *copy_name;
    char *copy_value;
    pthread_rwlock_wrlock(&c->lock);
    entry = find(c, parent_did, name);

    if (entry && !entry->negative
            && !removed_since(c, parent_did, entry->e.cnid, sent)
            && params_hold(c, entry, &entry->e, sent)) {
        copy_name = strdup(xattr);
        copy_value = malloc(len ? len : 1U);

        if (copy_name && copy_value) {
            clear_xattr(entry);
            memcpy(copy_value, value, len);
            entry->xattr_name = copy_name;
            entry->xattr_value = copy_value;
            entry->xattr_len = len;
            entry->xattr_expires = c->clock() + DIR_CACHE_XATTR_TTL;
        } else {
            free(copy_name);
            free(copy_value);
        }
    }

    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Read the cache's counters
 *
 * @param[in]  volume  the volume
 * @param[out] counts  receives the counters
 * @returns the entries held
 */
uint32_t dir_cache_counts(struct afp_volume *volume,
                          struct dir_cache_counts *counts)
{
    struct dir_cache *c = &volume->cache;
    uint32_t count;
    pthread_rwlock_rdlock(&c->lock);
    count = c->count;
    counts->hits = __atomic_load_n(&c->stats.hits, __ATOMIC_RELAXED);
    counts->misses = __atomic_load_n(&c->stats.misses, __ATOMIC_RELAXED);
    counts->stat_hits = __atomic_load_n(&c->stats.stat_hits,
                                        __ATOMIC_RELAXED);
    counts->negative_hits = __atomic_load_n(&c->stats.negative_hits,
                                            __ATOMIC_RELAXED);
    counts->fetches = __atomic_load_n(&c->stats.fetches, __ATOMIC_RELAXED);
    counts->inserts = c->stats.inserts;
    counts->evictions = c->stats.evictions;
    counts->invalidations = c->stats.invalidations;
    pthread_rwlock_unlock(&c->lock);
    return count;
}

/*!
 * @brief Forget every entry
 *
 * @param[in,out] volume  the volume
 */
void dir_cache_purge(struct afp_volume *volume)
{
    struct dir_cache *c = &volume->cache;
    pthread_rwlock_wrlock(&c->lock);

    for (uint32_t i = 0; c->by_name && i < c->buckets; i++) {
        while (c->by_name[i]) {
            drop(c, c->by_name[i]);
        }
    }

    c->last_removal = count_change(c);
    pthread_rwlock_unlock(&c->lock);
}

/*!
 * @brief Free a volume's cache
 *
 * @param[in,out] volume  the volume
 */
void dir_cache_destroy(struct afp_volume *volume)
{
    struct dir_cache *c = &volume->cache;
    dir_cache_purge(volume);
    free(c->by_name);
    free(c->by_cnid);
    free(c->by_parent);
    c->by_name = NULL;
    c->by_cnid = NULL;
    c->by_parent = NULL;
    pthread_rwlock_destroy(&c->lock);
}

/*!
 * @brief Read an object's record from the server and store it
 *
 * A reply without a node ID holds no identity and is not stored.
 *
 * @param[in,out] volume      volume of the directory
 * @param[in]     parent_did  the directory's ID
 * @param[in]     name        the name, as dir_cache_resolve() gives it
 * @param[out]    e           receives the record
 * @returns ll_fetch_entry()'s result
 */
int dir_cache_fetch(struct afp_volume *volume, unsigned int parent_did,
                    const char *name, struct afp_dirent *e)
{
    uint64_t sent = dir_cache_changes(volume);
    int ret = ll_fetch_entry(volume, parent_did, name, e);
    __atomic_add_fetch(&volume->cache.stats.fetches, 1, __ATOMIC_RELAXED);

    if (ret == 0 && (e->valid & kFPNodeIDBit)) {
        dir_cache_upsert(volume, parent_did, name, e, sent);
    } else if (ret == -ENOENT) {
        dir_cache_insert_negative(volume, parent_did, name, sent);
    }

    return ret;
}

/*!
 * @brief Read a path's next component in the spelling the cache keeps
 *
 * A name outside ASCII goes to the wire form and back, so the spellings
 * the wire takes as one become a listing's; beyond U+FFFF they stay apart.
 *
 * @param[in]     volume     volume of the path
 * @param[in,out] p          the rest of the path, moved past the component
 * @param[out]    component  receives the component, AFPC_MAX_NAME_BYTES
 *                           long
 * @returns 1 for a component a slash follows, 0 for the last one, or a
 *          negative errno: -ENAMETOOLONG for one the record does not hold
 *          or a name not in ASCII the volume refuses, -EINVAL for one the
 *          volume's encoding does not hold, or -ENOMEM
 */
static int next_component(const struct afp_volume *volume, const char **p,
                          char *component)
{
    char wire[AFPC_MAX_NAME_BYTES];
    const char *s = *p + strspn(*p, "/");
    const char *slash = strchr(s, '/');
    size_t len = slash ? (size_t)(slash - s) : strlen(s);
    int ret;

    if (len >= AFPC_MAX_NAME_BYTES) {
        return -ENAMETOOLONG;
    }

    memcpy(component, s, len);
    component[len] = '\0';
    *p = s + len;

    if (!utf8_is_ascii(component, len)) {
        ret = ll_wire_name(volume, component, wire);

        if (ret) {
            return ret;
        }

        if (convert_name_to_unix(volume->path_encoding, wire, strlen(wire),
                                 component, AFPC_MAX_NAME_BYTES) < 0) {
            return -EINVAL;
        }
    }

    return slash != NULL;
}

/*!
 * @brief Walk a path to its parent's directory ID and its last component
 *
 * @param[in,out] volume      volume of the path
 * @param[in]     path        UTF-8 path, from the root
 * @param[out]    parent_did  receives the parent's directory ID
 * @param[out]    name        receives the last component
 * @param[in]     fresh       1 to read every component from the server,
 *                            0 to take the ones the cache answers for
 * @returns 0, or a negative errno, as dir_cache_resolve() gives them
 */
static int resolve_path(struct afp_volume *volume, const char *path,
                        unsigned int *parent_did, char *name, int fresh)
{
    struct afp_dirent e;
    const char *p = path;
    unsigned int did = AFP_ROOT_DID;
    time_t now = volume->cache.clock();
    int cached = 0;
    int restarted = fresh;
    int ret;

    /* An empty path, macFUSE's "/(null)" cut to nothing, names nothing. */
    if (path != NULL && path[0] == '\0') {
        return -ENOENT;
    }

    if (path == NULL || path[0] != '/') {
        return -EINVAL;
    }

    while ((ret = next_component(volume, &p, name)) == 1) {
        unsigned int found = restarted ? 0
                             : lookup_at(volume, did, name, &e, now)
                             & (DIR_CACHE_DID | DIR_CACHE_NEGATIVE);

        /* An absence under a cached ID may be a moved folder's. */
        if (found == DIR_CACHE_NEGATIVE && !cached) {
            return -ENOENT;
        }

        /* A file on the way may be another session's old object. */
        if (found == DIR_CACHE_DID && e.isdir) {
            cached = 1;
        } else {
            ret = dir_cache_fetch(volume, did, name, &e);
            now = volume->cache.clock();

            /* A cached ID may name a folder another session moved. */
            if (ret == -ENOENT && cached && !restarted) {
                dir_cache_forget_path(volume, path);
                restarted = 1;
                cached = 0;
                did = AFP_ROOT_DID;
                p = path;
                continue;
            }

            if (ret) {
                return ret;
            }

            /* A reply without the node ID asked for names no folder. */
            if (!(e.valid & kFPNodeIDBit)) {
                return -EIO;
            }

            if (!e.isdir) {
                return -ENOTDIR;
            }
        }

        /* A trailing slash names the last directory by name. */
        if (p[strspn(p, "/")] == '\0') {
            break;
        }

        did = e.cnid;
    }

    if (ret < 0) {
        return ret;
    }

    *parent_did = did;
    return 0;
}

/*!
 * @brief Resolve a path to its parent's directory ID and its last component
 *
 * One lookup per component, and a fetch for each the cache cannot answer.
 * A component missing under a cached ID restarts the walk once, all fresh.
 *
 * @param[in,out] volume      volume of the path
 * @param[in]     path        UTF-8 path, from the root
 * @param[out]    parent_did  receives the parent's directory ID
 * @param[out]    name        receives the last component in the spelling
 *                            the cache keeps, AFPC_MAX_NAME_BYTES long;
 *                            empty for the root; a path that ends in a
 *                            slash gives its last directory's
 * @returns 0, or a negative errno: -ENOENT or -ENOTDIR on the parent's
 *          path, or for an empty path, -ENAMETOOLONG for a component the
 *          record does not hold or one not in ASCII the volume refuses,
 *          -EINVAL for a path that does not start at the root or a
 *          component the volume's encoding does not hold, -ENOMEM, or a
 *          fetch's error
 */
int dir_cache_resolve(struct afp_volume *volume, const char *path,
                      unsigned int *parent_did, char *name)
{
    return resolve_path(volume, path, parent_did, name, 0);
}

/*!
 * @brief Resolve a path as dir_cache_resolve() does, reading every
 * component from the server
 *
 * For a listing refused under cached IDs, where a forget may leave a stale
 * ID below the first component the cache lacks.
 *
 * @param[in,out] volume      volume of the path
 * @param[in]     path        UTF-8 path, from the root
 * @param[out]    parent_did  receives the parent's directory ID
 * @param[out]    name        receives the last component, as
 *                            dir_cache_resolve() gives it
 * @returns 0, or a negative errno, as dir_cache_resolve() gives them
 */
int dir_cache_resolve_fresh(struct afp_volume *volume, const char *path,
                            unsigned int *parent_did, char *name)
{
    return resolve_path(volume, path, parent_did, name, 1);
}

/*!
 * @brief Forget a path and the directories on it
 *
 * For a request refused under a cached ID: the walk passes lapsed identities,
 * stops at the first name the cache lacks, and counts a removal from each
 * folder it reads.
 *
 * @param[in,out] volume  volume of the path
 * @param[in]     path    UTF-8 path, from the root
 */
void dir_cache_forget_path(struct afp_volume *volume, const char *path)
{
    struct dir_cache *c = &volume->cache;
    char component[AFPC_MAX_NAME_BYTES];
    const char *p = path;
    unsigned int did = AFP_ROOT_DID;
    uint64_t stamp;
    int more = 1;

    if (path == NULL || path[0] != '/') {
        return;
    }

    pthread_rwlock_wrlock(&c->lock);
    stamp = count_change(c);

    while (more == 1 && (more = next_component(volume, &p, component)) >= 0
            && component[0] != '\0') {
        struct dir_cache_entry *entry = find(c, did, component);
        c->removals[did % DIR_CACHE_CNID_SLOTS] = stamp;

        if (entry == NULL || entry->negative) {
            break;
        }

        did = entry->e.cnid;
        drop(c, entry);
    }

    pthread_rwlock_unlock(&c->lock);
}
