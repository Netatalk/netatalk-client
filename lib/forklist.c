/*
 * forklist.c: some functions which help record which forks were opened.
 *
 * Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief A volume's list of open forks, and the flushes and checks it serves
 *
 * The list of a volume's open forks, outside libafpclient's layers.
 * Lane: shared.
 * Northbound: the mid and low levels, and the session layer.
 * Southbound: the protocol layer and the DSI layer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "afp_internal.h"
#include "dsi.h"
#include "midlevel.h"
#include "utils.h"

struct dirty_fork_record {
    unsigned short forkid;
    unsigned int session;
    unsigned int resource;
};

/*!
 * @brief Test whether an open fork's record names a file
 *
 * @param[in] p         the open fork's record
 * @param[in] did       directory of the file
 * @param[in] basename  AFP name of the file in did
 * @returns 1 when the record names that file, 0 otherwise
 */
static int names_file(const struct afp_file_info *p, unsigned int did,
                      const char *basename)
{
    return p->did == did && strcmp(p->basename, basename) == 0;
}

/*!
 * @brief Put an open fork on the volume's list, counting it if writable
 *
 * A reconnect into a new session between the open and this call missed the
 * record in forget_open_forks(), so it is left here without its fork.
 *
 * @param[in,out] volume  volume of the fork
 * @param[in,out] fp      the fork's record
 * @returns 0, or -1 for a fork of a session that has ended
 */
int add_opened_fork(struct afp_volume * volume, struct afp_file_info * fp)
{
    pthread_mutex_lock(&volume->open_forks_mutex);

    if (fp->session != afp_server_session_generation(volume->server)) {
        fp->forkid = 0;
        pthread_mutex_unlock(&volume->open_forks_mutex);
        return -1;
    }

    fp->largelist_next = volume->open_forks;
    volume->open_forks = fp;

    if (fp->writable) {
        __atomic_add_fetch(&volume->writable_forks, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
    return 0;
}

/*!
 * @brief Take an open fork off the volume's list and its writable count
 *
 * @param[in,out] volume  volume of the fork
 * @param[in]     fp      the fork's record, if the list holds it
 */
void remove_opened_fork(struct afp_volume * volume, struct afp_file_info * fp)
{
    struct afp_file_info * p, *prev = NULL;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (p = volume->open_forks; p; p = p->largelist_next) {
        if (p == fp) {
            if (prev) {
                prev->largelist_next = p->largelist_next;
            } else {
                volume->open_forks = p->largelist_next;
            }

            if (fp->writable) {
                __atomic_sub_fetch(&volume->writable_forks, 1,
                                   __ATOMIC_RELAXED);
            }

            goto done;
        }

        prev = p;
    }

done:
    pthread_mutex_unlock(&volume->open_forks_mutex);
}

/*!
 * @brief Flush the file's other written forks of fp's kind with FPFlushFork
 *
 * Runs before a read for Apple servers, Time Capsule included, while a
 * writable fork is open; a fork whose flush fails is dirty again.
 *
 * @param[in] volume  volume of the file
 * @param[in] fp      the fork about to be read
 */
void flush_dirty_forks_for_file(struct afp_volume * volume,
                                struct afp_file_info * fp)
{
    struct dirty_fork_record *records = NULL;
    size_t count = 0;
    size_t capacity = 0;

    if (!volume || !fp) {
        return;
    }

    /* Only a writable fork can be dirty. */
    if (__atomic_load_n(&volume->writable_forks, __ATOMIC_RELAXED) == 0) {
        return;
    }

    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks; p; p = p->largelist_next) {
        struct dirty_fork_record *new_records;
        size_t new_capacity;

        if (p == fp || !p->writable || !p->dirty ||
                p->resource != fp->resource ||
                !names_file(p, fp->did, fp->basename)) {
            continue;
        }

        if (count == capacity) {
            new_capacity = capacity ? capacity * 2 : 4;
            new_records = realloc(records,
                                  new_capacity * sizeof(*records));

            if (!new_records) {
                log_for_client(NULL, AFPFSD, LOG_WARNING,
                               "flush_dirty_forks_for_file: out of memory");
                continue;
            }

            records = new_records;
            capacity = new_capacity;
        }

        records[count].forkid = p->forkid;
        records[count].session = p->session;
        records[count].resource = p->resource;
        count++;
        p->dirty = 0;
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);

    for (size_t i = 0; i < count; i++) {
        int rc = afp_flushfork(volume,
                               afpc_dsi_fork(volume->server,
                                             records[i].session,
                                             records[i].forkid),
                               DSI_DEFAULT_TIMEOUT);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "flush_dirty_forks_for_file: flushed dirty fork %u with rc=%d",
                       records[i].forkid, rc);

        if (rc != kFPNoErr) {
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "flush_dirty_forks_for_file: FPFlushFork(%u) returned %d",
                           records[i].forkid, rc);
            pthread_mutex_lock(&volume->open_forks_mutex);

            /* The fork's number and session find it, as a rename may have
             * given it another name since. */
            for (struct afp_file_info *p = volume->open_forks;
                    p; p = p->largelist_next) {
                if (p->forkid == records[i].forkid &&
                        p->session == records[i].session &&
                        p->resource == records[i].resource) {
                    p->dirty = 1;
                    break;
                }
            }

            pthread_mutex_unlock(&volume->open_forks_mutex);
        }
    }

    free(records);
}

/*!
 * @brief Test whether another open fork of the client names fp's file
 *
 * @param[in] volume  volume of the file
 * @param[in] fp      the open fork
 * @param[in] writer  1 to count only a fork of fp's kind open for writing
 * @returns 1 when another record on the volume's open-fork list names the
 *          file, 0 otherwise
 */
static int fork_of_file_open(struct afp_volume * volume,
                             struct afp_file_info * fp, int writer)
{
    int found = 0;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks;
            p; p = p->largelist_next) {
        if (p != fp && names_file(p, fp->did, fp->basename)
                && (!writer || (p->writable && p->resource == fp->resource))) {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
    return found;
}

/*!
 * @brief Test whether another open fork of the client names fp's file
 *
 * @param[in] volume  volume of the file
 * @param[in] fp      the open fork
 * @returns 1 when another record on the volume's open-fork list names the
 *          file, 0 otherwise
 */
int other_fork_open(struct afp_volume * volume, struct afp_file_info * fp)
{
    return fork_of_file_open(volume, fp, 0);
}

/*!
 * @brief Test whether another fork of fp's file and kind is open for writing
 *
 * Such a fork is the one that can hold a deny-write mode.
 *
 * @param[in] volume  volume of the file
 * @param[in] fp      the fork
 * @returns 1 when another record on the volume's open-fork list is such a
 *          fork, 0 otherwise
 */
int other_writer_open(struct afp_volume * volume, struct afp_file_info * fp)
{
    return fork_of_file_open(volume, fp, 1);
}

/*!
 * @brief Forget the volume's forks as a reconnect starts a new session
 *
 * The new session may give their numbers to other files. A record left
 * without its fork fails reads and writes with EIO; its owner still frees it.
 *
 * @param[in,out] volume  the volume
 */
void forget_open_forks(struct afp_volume * volume)
{
    struct afp_file_info *next;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks; p; p = next) {
        next = p->largelist_next;
        p->largelist_next = NULL;
        /* Request threads read the number without the list's lock, and a
         * close that finds none frees the record: the store comes last. */
        __atomic_store_n(&p->forkid, 0, __ATOMIC_RELEASE);
    }

    volume->open_forks = NULL;
    __atomic_store_n(&volume->writable_forks, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&volume->open_forks_mutex);
}

/*!
 * @brief Forget the closes kept for a session a reconnect has replaced
 *
 * @param[in,out] server  the server
 */
void forget_unclosed_forks(struct afp_server *server)
{
    pthread_mutex_lock(&server->unclosed_forks_mutex);
    free(server->unclosed_forks);
    server->unclosed_forks = NULL;
    server->num_unclosed_forks = 0;
    pthread_mutex_unlock(&server->unclosed_forks_mutex);
}

/*!
 * @brief An open fork's number, tying the thread's next request to its session
 *
 * @param[in] volume  volume of the fork
 * @param[in] fp      the fork's record
 * @returns the fork's number
 */
unsigned short fork_number(const struct afp_volume *volume,
                           const struct afp_file_info *fp)
{
    return afpc_dsi_fork(volume->server, fp->session,
                         __atomic_load_n(&fp->forkid, __ATOMIC_RELAXED));
}

/*!
 * @brief Keep a fork whose close never left, unless its session has ended
 *
 * @param[in,out] server   server of the fork's session
 * @param[in]     forkid   the fork's number
 * @param[in]     session  the session generation that gave it
 */
static void keep_unclosed_fork(struct afp_server *server,
                               unsigned short forkid, unsigned int session)
{
    struct afp_unclosed_fork *forks;
    pthread_mutex_lock(&server->unclosed_forks_mutex);

    if (session == afp_server_session_generation(server)) {
        forks = realloc(server->unclosed_forks,
                        (server->num_unclosed_forks + 1) * sizeof(*forks));

        if (forks) {
            forks[server->num_unclosed_forks].forkid = forkid;
            forks[server->num_unclosed_forks].session = session;
            server->unclosed_forks = forks;
            server->num_unclosed_forks++;
        }
    }

    pthread_mutex_unlock(&server->unclosed_forks_mutex);
}

/*!
 * @brief Close the forks whose close never left, each in its session
 *
 * A close that again does not leave keeps its fork and the ones after it,
 * as the session is still away.
 *
 * @param[in,out] server  the server
 */
void close_unclosed_forks(struct afp_server *server)
{
    struct afp_unclosed_fork *forks;
    unsigned int count;
    int away = 0;
    pthread_mutex_lock(&server->unclosed_forks_mutex);
    forks = server->unclosed_forks;
    count = server->num_unclosed_forks;
    server->unclosed_forks = NULL;
    server->num_unclosed_forks = 0;
    pthread_mutex_unlock(&server->unclosed_forks_mutex);

    for (unsigned int i = 0; i < count; i++) {
        if (!away) {
            int rc = afp_closefork(server,
                                   afpc_dsi_fork(server, forks[i].session,
                                                 forks[i].forkid));
            away = rc != kFPNoErr && afpc_dsi_fork_unsent();
        }

        if (away) {
            keep_unclosed_fork(server, forks[i].forkid, forks[i].session);
        }
    }

    free(forks);
}

/*!
 * @brief Close an open fork, keeping it for later if the close never leaves
 *
 * An unsent close leaves the fork open in a session a reconnect may continue;
 * an unanswered one may have closed it and freed its number, so is not kept.
 *
 * @param[in,out] volume  volume of the fork
 * @param[in]     fp      the fork's record
 * @returns afp_closefork()'s result
 */
int close_fork(struct afp_volume *volume, const struct afp_file_info *fp)
{
    struct afp_server *server = volume->server;
    unsigned short forkid = __atomic_load_n(&fp->forkid, __ATOMIC_RELAXED);
    int rc = afp_closefork(server, afpc_dsi_fork(server, fp->session, forkid));

    if (rc != kFPNoErr && afpc_dsi_fork_unsent()) {
        keep_unclosed_fork(server, forkid, fp->session);

        /* A reconnect that ended meanwhile missed this fork. */
        if (!afp_server_reconnect_is_in_progress(server)
                && __atomic_load_n(&server->connect_state, __ATOMIC_ACQUIRE)
                == SERVER_STATE_CONNECTED) {
            close_unclosed_forks(server);
        }
    }

    return rc;
}

/*!
 * @brief Give an open fork's record the directory and names of a resolved path
 *
 * @param[out] fp  the record
 * @param[in]  t   the resolved path
 */
void fork_now_named(struct afp_file_info *fp, const struct ml_target *t)
{
    fp->did = t->did;
    snprintf(fp->name, sizeof(fp->name), "%s", t->name);
    snprintf(fp->basename, sizeof(fp->basename), "%s", t->afp_name);
}

/*!
 * @brief Copy an open fork's directory, names and node ID
 *
 * A rename or an exchange rewrites them under the list's lock, so every
 * reader of a listed fork copies them here.
 *
 * @param[in]  volume  volume of the fork
 * @param[in]  fp      the open fork
 * @param[out] t       its directory and names
 * @returns the node ID its record holds, or 0
 */
unsigned int fork_names(struct afp_volume *volume,
                        const struct afp_file_info *fp, struct ml_target *t)
{
    unsigned int cnid;
    pthread_mutex_lock(&volume->open_forks_mutex);
    t->did = fp->did;
    snprintf(t->name, sizeof(t->name), "%s", fp->name);
    snprintf(t->afp_name, sizeof(t->afp_name), "%s", fp->basename);
    cnid = fp->cnid;
    pthread_mutex_unlock(&volume->open_forks_mutex);
    return cnid;
}

/*!
 * @brief Initialize a volume's open lock, writers ahead of new readers
 *
 * glibc's default lets readers pass a waiting writer, which opens would
 * starve; the preference is safe as no reader holds the lock twice.
 *
 * @param[out] lock  the lock
 */
void afp_open_lock_init(pthread_rwlock_t *lock)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#ifdef __GLIBC__
    pthread_rwlockattr_setkind_np(&attr,
                                  PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(lock, &attr);
    pthread_rwlockattr_destroy(&attr);
}

/*!
 * @brief Carry a renamed file's open forks to its new directory and name
 *
 * The caller holds the volume's open lock for writing from before its
 * request, so every fork opened before the rename is listed.
 *
 * @param[in,out] volume  volume of the file
 * @param[in]     from    the directory and name the forks' records hold
 * @param[in]     to      the directory and name the file has
 */
void rename_opened_forks(struct afp_volume *volume,
                         const struct ml_target *from,
                         const struct ml_target *to)
{
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks; p;
            p = p->largelist_next) {
        if (names_file(p, from->did, from->afp_name)) {
            fork_now_named(p, to);
        }
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
}

/*!
 * @brief Carry an exchange of two files into their open forks
 *
 * The names keep their node IDs over swapped contents, so a fork keeps none,
 * and a done exchange swaps the forks' names. The caller write-locks open_lock.
 *
 * @param[in,out] volume  volume of the files
 * @param[in]     a       one file's directory and name
 * @param[in]     b       the other's
 * @param[in]     done    1 when the server answered that it swapped them
 */
void exchange_opened_forks(struct afp_volume *volume,
                           const struct ml_target *a,
                           const struct ml_target *b, int done)
{
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks; p;
            p = p->largelist_next) {
        if (names_file(p, a->did, a->afp_name)) {
            if (done) {
                fork_now_named(p, b);
            }

            p->cnid = 0;
        } else if (names_file(p, b->did, b->afp_name)) {
            if (done) {
                fork_now_named(p, a);
            }

            p->cnid = 0;
        }
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
}

/*!
 * @brief Close and free every fork still open on the volume, at unmount
 *
 * Taking the list whole first frees every record even if a close reconnects;
 * an unsent close keeps its fork, as the session may outlive the volume.
 *
 * @param[in,out] volume  volume to unmount
 */
void remove_fork_list(struct afp_volume * volume)
{
    struct afp_file_info *p, *next;
    pthread_mutex_lock(&volume->open_forks_mutex);
    p = volume->open_forks;
    volume->open_forks = NULL;
    __atomic_store_n(&volume->writable_forks, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&volume->open_forks_mutex);
    close_unclosed_forks(volume->server);

    for (; p; p = next) {
        next = p->largelist_next;
        /* At unmount, the kernel drops the releases it has not sent. */
        ml_close_flush(volume, p);
        close_fork(volume, p);
        free(p);
    }
}
