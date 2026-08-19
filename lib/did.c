/*
    did.c

    Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>

*/

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "afp_internal.h"
#include "afp_protocol.h"

#undef DID_CACHE_DISABLE

static unsigned short timeout = 10;

struct did_cache_entry {
    /* For the example /foo/bar/baz */
    char *dirname;                            /* full name, eg. /foo/bar/     */
    unsigned int did;            /*            eg  2323          */
    unsigned int parentdid;
    char *name;
    struct timeval time;
    struct did_cache_entry *next;
} ;

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

int remove_did_entry(struct afp_volume * volume, const char * name)
{
    struct did_cache_entry * d, *p = NULL;
    pthread_mutex_lock(&volume->did_cache_mutex);

    for (d = volume->did_cache_base; d; d = d->next) {
        if (strcmp(d->dirname, name) == 0) {
            if (p) {
                p->next = d->next;
            } else {
                volume->did_cache_base = d->next;
            }

            volume->did_cache_stats.force_removed++;
            free(d->dirname);
            free(d->name);
            free(d);
            break;
        } else {
            p = d;
        }
    }

    pthread_mutex_unlock(&volume->did_cache_mutex);
    return 0;
}


static int add_did_cache_entry(struct afp_volume * volume,
                               unsigned int parentdid, unsigned int new_did,
                               const char *path, const char *name)
{
    struct did_cache_entry * new, *old_base;
#ifdef DID_CACHE_DISABLE
    return 0;
#endif

    if ((new = malloc(sizeof(* new))) == NULL) {
        return -1;
    }

    memset(new, 0, sizeof(*new));
    new->dirname = strdup(path);

    if (!new->dirname) {
        free(new);
        return -1;
    }

    new->name = strdup(name);

    if (!new->name) {
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

/* Return a non-zero directory ID when this direct child is already cached.
 * Only directories are added to the DID cache, so a cache hit also answers
 * the type question without another FPGetFileDirParms round trip. */
static unsigned int find_cached_directory(
    struct afp_volume *volume,
    unsigned int parentdid,
    const char *name)
{
    struct did_cache_entry *next;
    struct did_cache_entry **prev_ptr;
    struct timeval now;
    unsigned int did = 0;
#ifdef DID_CACHE_DISABLE
    return 0;
#endif
    gettimeofday(&now, NULL);
    pthread_mutex_lock(&volume->did_cache_mutex);
    prev_ptr = &volume->did_cache_base;

    for (struct did_cache_entry *p = *prev_ptr; p; p = next) {
        next = p->next;

        if (now.tv_sec > p->time.tv_sec + timeout) {
            *prev_ptr = next;
            volume->did_cache_stats.expired++;
            free(p->dirname);
            free(p->name);
            free(p);
            continue;
        }

        if (p->parentdid == parentdid && strcmp(p->name, name) == 0) {
            did = p->did;
            volume->did_cache_stats.hits++;
            break;
        }

        prev_ptr = &p->next;
    }

    pthread_mutex_unlock(&volume->did_cache_mutex);
    return did;
}

unsigned char is_dir(struct afp_volume * volume,
                     unsigned int parentdid, const char *path)
{
    int ret;
    unsigned int filebitmap = 0;
    unsigned int dirbitmap = 0;
    struct afp_file_info fi;

    if (find_cached_directory(volume, parentdid, path)) {
        return 1;
    }

    volume->did_cache_stats.misses++;
    ret = afp_getfiledirparms(volume, parentdid,
                              filebitmap, dirbitmap, path, &fi);

    if (ret) {
        return 0;
    }

    return fi.isdir;
}

static unsigned int find_dirid_by_fullname(struct afp_volume * volume,
        char *path)
{
    struct did_cache_entry *p, *next;
    struct did_cache_entry **prev_ptr;
    struct timeval time;
    unsigned int found_did = 0;
#ifdef DID_CACHE_DISABLE
    return 0;
#endif
    gettimeofday(&time, NULL);
    pthread_mutex_lock(&volume->did_cache_mutex);
    prev_ptr = &volume->did_cache_base;
    p = *prev_ptr;

    while (p) {
        next = p->next;

        if (time.tv_sec > (p->time.tv_sec + timeout)) {
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


/* This calculates the dirid and basename.  It *always* gets the parent did. */

int get_dirid(struct afp_volume * volume, const char * path,
              char *basename, unsigned int *dirid)
{
    const char *last_slash;
    const char *component;
    char *slash;
    char *parent;
    char *known_parent;
    char *name;
    char *full_parent;
    size_t basename_len;
    size_t known_parent_len = 0;
    int ret = 0;
    struct afp_file_info fi;
    unsigned int filebitmap, dirbitmap;
    unsigned int newdid;
    unsigned int parent_did = AFP_ROOT_DID;

    if (!path || !dirid || !(last_slash = strrchr(path, '/'))) {
        return -1;
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

    filebitmap = kFPNodeIDBit ;
    dirbitmap = kFPNodeIDBit ;
    component = path + known_parent_len;

    if (*component == '/') {
        component++;
    }

    while ((slash = strchr(component, '/'))) {
        size_t component_len = (size_t)(slash - component);
        name = strndup(component, component_len);

        if (!name) {
            ret = -ENOMEM;
            goto out;
        }

        volume->did_cache_stats.misses++;
        ret = afp_getfiledirparms(volume, parent_did,
                                  filebitmap, dirbitmap, name, &fi);

        if (ret) {
            free(name);
            goto out;
        }

        if (fi.isdir) {
            full_parent = strndup(path, (size_t)(slash - path));

            if (!full_parent) {
                free(name);
                ret = -ENOMEM;
                goto out;
            }

            (void)add_did_cache_entry(volume, parent_did, fi.fileid,
                                      full_parent, name);
            free(full_parent);
        } else {
            free(name);
            break;
        }

        free(name);
        parent_did = fi.fileid;
        component = slash + 1U;
    }

    *dirid = parent_did;
out:
    free(parent);
    return ret;
}
