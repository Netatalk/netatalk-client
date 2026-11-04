/*
 *  did.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The cache of directory IDs by path, and the lookup that fills it
 */

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "lowlevel.h"

static unsigned short timeout = 10;

/*! @brief A folder's cached directory ID, under its full path */
struct did_cache_entry {
    char *dirname;               /*!< the path, without a trailing slash */
    unsigned int did;
    unsigned int parentdid;
    char *name;                  /*!< the last component, in the UNIX form */
    struct timeval time;         /*!< when it was cached */
    struct did_cache_entry *next;
};

/*!
 * @brief Free every entry of a volume's directory ID cache
 *
 * @param[in,out] volume  volume of the cache
 * @returns 0
 */
int free_entire_did_cache(struct afp_volume * volume)
{
    struct did_cache_entry * d, *p, *p2;
    pthread_mutex_lock(&volume->did_cache_mutex);
    p = volume->did_cache_base;

    for (d = volume->did_cache_base; d; d = p) {
        p2 = p;
        p = d->next;
        free(p2->dirname);
        free(p2->name);
        free(p2);
    }

    volume->did_cache_base = NULL;
    pthread_mutex_unlock(&volume->did_cache_mutex);
    return 0;
}

/*!
 * @brief Whether a cache entry is past its time
 *
 * @param[in] p    the entry
 * @param[in] now  the time of the walk
 * @returns 1 when the entry has expired, 0 otherwise
 */
static int did_entry_expired(const struct did_cache_entry *p,
                             const struct timeval *now)
{
    return now->tv_sec > p->time.tv_sec + timeout;
}

/*!
 * @brief Length of a path without its trailing slashes
 *
 * A path from afpsld may end in a slash; the root's length stays 1.
 *
 * @param[in] path  AFP path, with a leading slash
 * @returns the length
 */
static size_t did_path_length(const char *path)
{
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    return len;
}

/*!
 * @brief Where the names of a path's children begin
 *
 * @param[in] path_len  length of the path without its trailing slashes
 * @returns the offset of the slash before a child's name: beneath the root,
 *          an entry's own leading slash
 */
static size_t did_separator(size_t path_len)
{
    return path_len == 1 ? 0 : path_len;
}

/*!
 * @brief Whether a cache entry names a folder directly in a path
 *
 * @param[in] dirname   the entry's path
 * @param[in] path      AFP path of the directory
 * @param[in] path_len  its length without trailing slashes
 * @returns 1 when dirname is a direct child of path, 0 otherwise
 */
static int did_child_of(const char *dirname, const char *path,
                        size_t path_len)
{
    size_t sep = did_separator(path_len);
    return strncmp(dirname, path, sep) == 0 && dirname[sep] == '/'
           && strchr(dirname + sep + 1, '/') == NULL;
}

/*!
 * @brief The first component a walk from a directory resolves
 *
 * @param[in] path      AFP path, with a leading slash
 * @param[in] path_len  length of the directory's path within it
 * @returns the component after the directory and its slash
 */
static const char *walk_start(const char *path, size_t path_len)
{
    const char *component = path + path_len;

    if (*component == '/') {
        component++;
    }

    return component;
}

/*!
 * @brief Forget a path's cached IDs, those beneath and, with ancestors, above
 *
 * The caller holds the cache's lock; the walk drops expired entries too.
 *
 * @param[in,out] volume     volume of the cache
 * @param[in]     path       AFP path of the directory
 * @param[in]     ancestors  1 to forget the directories above path too
 * @param[in]     now        the time of the walk
 */
static void forget_locked(struct afp_volume *volume, const char *path,
                          int ancestors, const struct timeval *now)
{
    struct did_cache_entry **prev_ptr = &volume->did_cache_base;
    size_t path_len = did_path_length(path);
    size_t sep = did_separator(path_len);

    for (struct did_cache_entry *p = *prev_ptr, *next; p; p = next) {
        size_t len = strlen(p->dirname);
        size_t common = len < path_len ? len : path_len;
        int expired = did_entry_expired(p, now);
        next = p->next;

        if (expired
                || (memcmp(p->dirname, path, common) == 0
                    && (len == path_len
                        || (len > path_len && p->dirname[sep] == '/')
                        || (ancestors && len < path_len
                            && path[len] == '/')))) {
            *prev_ptr = next;

            if (expired) {
                volume->did_cache_stats.expired++;
            } else {
                volume->did_cache_stats.force_removed++;
            }

            free(p->dirname);
            free(p->name);
            free(p);
            continue;
        }

        prev_ptr = &p->next;
    }
}

/*!
 * @brief Forget a path's cached IDs under the cache's lock
 *
 * @param[in,out] volume     volume of the cache
 * @param[in]     path       AFP path of the directory
 * @param[in]     ancestors  1 to forget the directories above path too
 */
static void forget_did_paths(struct afp_volume *volume, const char *path,
                             int ancestors)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    pthread_mutex_lock(&volume->did_cache_mutex);
    forget_locked(volume, path, ancestors, &now);
    pthread_mutex_unlock(&volume->did_cache_mutex);
}

/*!
 * @brief Forget the cached directory IDs on a path's line
 *
 * A refusal under a cached ID leaves open which directory above the path
 * held the stale ID: the path's entry, its ancestors' and descendants' go.
 *
 * @param[in,out] volume  volume of the cache
 * @param[in]     path    AFP path of the directory
 */
void remove_did_lineage(struct afp_volume *volume, const char *path)
{
    forget_did_paths(volume, path, 1);
}

/*!
 * @brief Forget the cached IDs of a path and of the folders beneath it
 *
 * A folder replaced or moved has another ID under its path, and so has
 * every folder beneath it; the directories above it resolved it and stay.
 *
 * @param[in,out] volume  volume of the cache
 * @param[in]     path    AFP path of the directory
 */
void remove_did_subtree(struct afp_volume *volume, const char *path)
{
    forget_did_paths(volume, path, 0);
}

/*!
 * @brief Free entries taken from the cache, with the folders beneath them
 *
 * The caller holds the cache's lock and has unlinked the taken entries.
 *
 * @param[in,out] volume  volume of the cache
 * @param[in]     taken   the entries, linked through next
 */
static void forget_taken(struct afp_volume *volume,
                         struct did_cache_entry *taken)
{
    struct timeval now;

    if (taken == NULL) {
        return;
    }

    gettimeofday(&now, NULL);

    for (struct did_cache_entry *p = taken, *next; p; p = next) {
        next = p->next;
        forget_locked(volume, p->dirname, 0, &now);
        volume->did_cache_stats.force_removed++;
        free(p->dirname);
        free(p->name);
        free(p);
    }
}

/*! @brief A listed folder's name and node ID, for the search of a listing */
struct listed_folder {
    const char *name;
    uint32_t cnid;
};

/*!
 * @brief Order two listed folders by name
 *
 * @param[in] a  one folder
 * @param[in] b  the other
 * @returns strcmp(3)'s order of their names
 */
static int listed_folder_cmp(const void *a, const void *b)
{
    return strcmp(((const struct listed_folder *) a)->name,
                  ((const struct listed_folder *) b)->name);
}

/*!
 * @brief Index a listing's folders by name
 *
 * The folders whose entry carries a node ID, sorted for bsearch(3).
 *
 * @param[in]  l      the listing, which holds an entry
 * @param[out] count  receives the number of folders indexed
 * @returns the index, which the caller frees, or NULL without memory
 */
static struct listed_folder *index_folders(const struct afp_listing *l,
                                           size_t *count)
{
    struct listed_folder *folders = malloc(l->count * sizeof(*folders));
    size_t n = 0;

    if (folders == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < l->count; i++) {
        const struct afp_dirent *e = &l->entries[i];

        if (e->isdir && (e->valid & kFPNodeIDBit)) {
            folders[n].name = afp_dirent_name(l, e);
            folders[n].cnid = e->cnid;
            n++;
        }
    }

    qsort(folders, n, sizeof(*folders), listed_folder_cmp);
    *count = n;
    return folders;
}

/*!
 * @brief Whether an indexed listing holds a folder of a name and an ID
 *
 * @param[in] folders  the index
 * @param[in] count    its size
 * @param[in] name     the folder's name, in the UNIX form
 * @param[in] did      its ID
 * @returns 1 when a folder of that name is indexed with that node ID, 0
 *          otherwise
 */
static int listing_holds_folder(const struct listed_folder *folders,
                                size_t count, const char *name,
                                unsigned int did)
{
    struct listed_folder key = { name, 0 };
    const struct listed_folder *f = count == 0 ? NULL
                                    : bsearch(&key, folders, count,
                                              sizeof(*folders),
                                              listed_folder_cmp);
    return f != NULL && f->cnid == did;
}

/*!
 * @brief Whether the cache holds a direct child of a folder
 *
 * @param[in,out] volume    volume of the cache
 * @param[in]     path      AFP path of the folder
 * @param[in]     path_len  its length, as did_path_length() gives it
 * @returns 1 when it holds one, 0 otherwise
 */
static int has_cached_child(struct afp_volume *volume, const char *path,
                            size_t path_len)
{
    int found = 0;
    pthread_mutex_lock(&volume->did_cache_mutex);

    for (const struct did_cache_entry *p = volume->did_cache_base;
            p != NULL && !found; p = p->next) {
        found = did_child_of(p->dirname, path, path_len);
    }

    pthread_mutex_unlock(&volume->did_cache_mutex);
    return found;
}

/*!
 * @brief Forget the cached IDs a folder's ID, or its listing, contradicts
 *
 * The folder's entry under another ID, and a child under another parent ID
 * or missing from the listing, go with the folders beneath them.
 *
 * @param[in,out] volume  volume of the cache
 * @param[in]     path    AFP path of the folder
 * @param[in]     did     its ID in the server's reply, or 0 for none
 * @param[in]     l       its complete listing, or NULL
 */
void check_did_entry(struct afp_volume *volume, const char *path,
                     unsigned int did, const struct afp_listing *l)
{
    struct did_cache_entry **prev_ptr = &volume->did_cache_base;
    struct did_cache_entry *taken = NULL;
    size_t path_len = did_path_length(path);
    struct listed_folder *folders = NULL;
    size_t nfolders = 0;

    /* Indexed outside the lock, as a child cached meanwhile resolved after
     * the pages; without memory the children wait for the next listing. */
    if (l != NULL && l->count > 0
            && (!has_cached_child(volume, path, path_len)
                || (folders = index_folders(l, &nfolders)) == NULL)) {
        l = NULL;
    }

    pthread_mutex_lock(&volume->did_cache_mutex);

    for (struct did_cache_entry *p = *prev_ptr, *next; p; p = next) {
        int stale = 0;
        next = p->next;

        /* An expired entry still tells what resolved beneath it. */
        if (did != 0 && strncmp(p->dirname, path, path_len) == 0
                && p->dirname[path_len] == '\0') {
            stale = p->did != did;
        } else if (did_child_of(p->dirname, path, path_len)) {
            stale = (did != 0 && p->parentdid != did)
                    || (l != NULL
                        && !listing_holds_folder(folders, nfolders, p->name,
                                                 p->did));
        }

        if (stale) {
            *prev_ptr = next;
            p->next = taken;
            taken = p;
            continue;
        }

        prev_ptr = &p->next;
    }

    forget_taken(volume, taken);
    pthread_mutex_unlock(&volume->did_cache_mutex);
    free(folders);
}

/*!
 * @brief Cache a folder's ID under its full path
 *
 * @param[in,out] volume     volume of the cache
 * @param[in]     parentdid  the ID of the folder's parent
 * @param[in]     new_did    the folder's ID
 * @param[in]     path       AFP path of the folder
 * @param[in]     name       its last component, in the volume's form
 * @returns 0, or -1 without memory or for a name that does not convert
 */
static int add_did_cache_entry(struct afp_volume * volume,
                               unsigned int parentdid, unsigned int new_did,
                               const char *path, const char *name)
{
    struct did_cache_entry * new, *old_base;
    char unix_name[AFPC_MAX_NAME_BYTES];

    if ((new = malloc(sizeof(* new))) == NULL) {
        return -1;
    }

    memset(new, 0, sizeof(*new));
    new->dirname = strdup(path);

    if (!new->dirname) {
        free(new);
        return -1;
    }

    if (convert_name_to_unix(volume->path_encoding, name, strlen(name),
                             unix_name, sizeof(unix_name)) != 0
            || (new->name = strdup(unix_name)) == NULL) {
        free(new->dirname);
        free(new);
        return -1;
    }

    new->did = new_did;
    new->parentdid = parentdid;
    gettimeofday(&new->time, NULL);
    pthread_mutex_lock(&volume->did_cache_mutex);
    old_base = volume->did_cache_base;
    volume->did_cache_base = new;
    new->next = old_base;
    pthread_mutex_unlock(&volume->did_cache_mutex);
    return 0;
}

/*!
 * @brief Find a folder's cached ID by its full path
 *
 * The walk drops the entries past their time as it goes.
 *
 * @param[in,out] volume  volume of the cache
 * @param[in]     path    AFP path of the folder
 * @returns the ID, or 0 when the cache holds none live
 */
static unsigned int find_dirid_by_fullname(struct afp_volume * volume,
                                           char *path)
{
    struct did_cache_entry *p, *next;
    struct did_cache_entry **prev_ptr;
    struct timeval time;
    unsigned int found_did = 0;
    gettimeofday(&time, NULL);
    pthread_mutex_lock(&volume->did_cache_mutex);
    prev_ptr = &volume->did_cache_base;
    p = *prev_ptr;

    while (p) {
        next = p->next;

        if (did_entry_expired(p, &time)) {
            volume->did_cache_stats.expired++;

            /* If this is the one we are looking for, we found it but it is expired.
             * We remove it and return 0 (not found). */
            if (strcmp(p->dirname, path) == 0) {
                *prev_ptr = next;
                free(p->dirname);
                free(p->name);
                free(p);
                goto out;
            }

            /* Remove expired entry */
            *prev_ptr = next;
            free(p->dirname);
            free(p->name);
            free(p);
            p = next;
            continue;
        }

        if (strcmp(p->dirname, path) == 0) {
            found_did = p->did;
            volume->did_cache_stats.hits++;
            goto out;
        }

        prev_ptr = &p->next;
        p = next;
    }

out:
    pthread_mutex_unlock(&volume->did_cache_mutex);
    return found_did;
}


/*!
 * @brief Find the directory ID of a path's parent, and the path's last name
 *
 * The cache answers for a known parent, a walk from the nearest cached
 * directory for another, once more from the root if a component is missing.
 *
 * @param[in]  volume    volume of the path
 * @param[in]  path      AFP path, with a leading slash
 * @param[out] basename  receives the last component, or NULL
 * @param[out] dirid     receives the parent's directory ID, set on success
 *                       alone
 * @returns 0, or a negative errno: -ENOTDIR for a component that is a
 *          file, -ENOENT for one that does not exist, -EACCES for one the
 *          session may not read, -EIO for an unexpected answer or a request
 *          without one, -EINVAL for a path without a slash, -ENAMETOOLONG
 *          for a last name too long, or -ENOMEM
 */
int get_dirid(struct afp_volume * volume, const char *path,
              char *basename, unsigned int *dirid)
{
    const char *last_slash;
    const char *component;
    const char *component_end;
    char *slash;
    char *parent;
    char *known_parent;
    char *name;
    char *full_parent;
    size_t basename_len;
    size_t known_parent_len = 0;
    int ret = 0;
    struct afp_dirent fi;
    unsigned int filebitmap, dirbitmap;
    unsigned int newdid;
    unsigned int parent_did = AFP_ROOT_DID;

    if (!path || !dirid || !(last_slash = strrchr(path, '/'))) {
        return -EINVAL;
    }

    basename_len = strlen(last_slash + 1U);

    if (basename_len >= AFPC_MAX_NAME_BYTES) {
        return -ENAMETOOLONG;
    }

    if (basename) {
        memcpy(basename, last_slash + 1U, basename_len + 1U);
    }

    if (last_slash == path) {
        *dirid = AFP_ROOT_DID;
        return 0;
    }

    parent = strndup(path, (size_t)(last_slash - path));

    if (!parent) {
        return -ENOMEM;
    }

    if ((newdid = find_dirid_by_fullname(volume, parent))) {
        *dirid = newdid;
        free(parent);
        return 0;
    }

    known_parent = parent;

    while ((slash = strrchr(known_parent, '/'))) {
        if (slash == known_parent) {
            parent_did = AFP_ROOT_DID;
            known_parent_len = 1;
            break;
        }

        *slash = '\0';

        if ((parent_did = find_dirid_by_fullname(volume, known_parent))) {
            known_parent_len = strlen(known_parent);
            break;
        }
    }

    filebitmap = kFPNodeIDBit;
    dirbitmap = kFPNodeIDBit;
    component = walk_start(path, known_parent_len);

    while ((component_end = strchr(component, '/'))) {
        size_t component_len = (size_t)(component_end - component);
        name = strndup(component, component_len);

        if (!name) {
            ret = -ENOMEM;
            goto out;
        }

        pthread_mutex_lock(&volume->did_cache_mutex);
        volume->did_cache_stats.misses++;
        pthread_mutex_unlock(&volume->did_cache_mutex);
        ret = afp_getfiledirparms(volume, parent_did,
                                  filebitmap, dirbitmap, name, &fi);

        /* A cached ancestor's ID may name a folder that is gone:
         * kFPDirNotFound, or kFPObjectNotFound from afpd. */
        if ((ret == kFPDirNotFound || ret == kFPObjectNotFound)
                && known_parent_len > 1) {
            free(name);
            remove_did_lineage(volume, path);
            known_parent_len = 1;
            parent_did = AFP_ROOT_DID;
            component = walk_start(path, known_parent_len);
            continue;
        }

        ret = -ll_answer_errno(ret);

        /* A reply without the node ID asked for names no folder. */
        if (ret == 0 && !(fi.valid & kFPNodeIDBit)) {
            ret = -EIO;
        }

        if (ret == 0 && !fi.isdir) {
            ret = -ENOTDIR;
        }

        if (ret) {
            free(name);
            goto out;
        }

        full_parent = strndup(path, (size_t)(component_end - path));

        if (!full_parent) {
            free(name);
            ret = -ENOMEM;
            goto out;
        }

        check_did_entry(volume, full_parent, fi.cnid, NULL);
        (void)add_did_cache_entry(volume, parent_did, fi.cnid,
                                  full_parent, name);
        free(full_parent);
        free(name);
        parent_did = fi.cnid;
        component = component_end + 1U;
    }

    *dirid = parent_did;
out:
    free(parent);
    return ret;
}
