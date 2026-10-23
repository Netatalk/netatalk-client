/*
    forklist.c: some functions which help record which forks were opened.

    Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief A volume's list of open forks, and the flushes and checks it serves
 */

#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "afp_internal.h"
#include "dsi.h"
#include "midlevel.h"
#include "utils.h"

struct dirty_fork_record {
    unsigned short forkid;
    unsigned int did;
    unsigned int resource;
    char basename[AFPC_MAX_NAME_BYTES];
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

void add_opened_fork(struct afp_volume * volume, struct afp_file_info * fp)
{
    pthread_mutex_lock(&volume->open_forks_mutex);
    fp->largelist_next = volume->open_forks;
    volume->open_forks = fp;
    pthread_mutex_unlock(&volume->open_forks_mutex);
}

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
 * Runs before a read, as the compatibility measure for Apple servers, Time
 * Capsule included. Flushes the forks outside the list's mutex. One whose
 * flush fails is dirty again.
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
        records[count].did = p->did;
        records[count].resource = p->resource;
        memcpy(records[count].basename, p->basename,
               sizeof(records[count].basename));
        count++;
        p->dirty = 0;
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);

    for (size_t i = 0; i < count; i++) {
        int rc = afp_flushfork(volume, records[i].forkid, DSI_DEFAULT_TIMEOUT);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "flush_dirty_forks_for_file: flushed dirty fork %u with rc=%d",
                       records[i].forkid, rc);

        if (rc != kFPNoErr) {
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "flush_dirty_forks_for_file: FPFlushFork(%u) returned %d",
                           records[i].forkid, rc);
            pthread_mutex_lock(&volume->open_forks_mutex);

            for (struct afp_file_info *p = volume->open_forks;
                    p; p = p->largelist_next) {
                if (p->forkid == records[i].forkid &&
                        p->resource == records[i].resource &&
                        names_file(p, records[i].did, records[i].basename)) {
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
 * @returns 1 when another record on the volume's open-fork list names the
 *          file, 0 otherwise
 */
int other_fork_open(struct afp_volume * volume, struct afp_file_info * fp)
{
    int found = 0;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (struct afp_file_info *p = volume->open_forks;
            p; p = p->largelist_next) {
        if (p != fp && names_file(p, fp->did, fp->basename)) {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
    return found;
}

/*!
 * @brief Close and free every fork still open on the volume, at unmount
 *
 * A flush_on_close mount flushes each written fork first.
 *
 * @param[in,out] volume  volume to unmount
 */
void remove_fork_list(struct afp_volume * volume)
{
    struct afp_file_info * p, *next;
    pthread_mutex_lock(&volume->open_forks_mutex);

    for (p = volume->open_forks; p; p = next) {
        next = p->largelist_next;
        /* At unmount, the kernel drops the releases it has not sent. */
        ml_close_flush(volume, p);
        afp_closefork(volume, p->forkid);
        volume->open_forks = p->largelist_next;
        free(p);
    }

    pthread_mutex_unlock(&volume->open_forks_mutex);
}
