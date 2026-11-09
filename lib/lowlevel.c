/*
    lowlevel.c: some functions that abstract common operations; used
	so the same code can be used between meta and normal files

    Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief Fork and directory requests for data forks and AppleDouble metadata
 */

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "compat.h"
#include "did.h"
#include "forklist.h"
#include "lowlevel.h"
#include "midlevel.h"
#include "resource.h"
#include "utils.h"

/*!
 * @brief The bitmap bit of a fork's length
 *
 * AFP 3 carries a length in 64 bits; AirPort Extreme 7.1.1 crashes on an
 * AFP 3 request with the 32-bit bit.
 *
 * @param[in] volume    volume of the fork
 * @param[in] resource  1 for the resource fork, 0 for the data fork
 * @returns kFPExtRsrcForkLenBit or kFPExtDataForkLenBit from AFP 3.0, the
 *          32-bit bits before it
 */
static unsigned int fork_length_bit(const struct afp_volume *volume,
                                    int resource)
{
    if (volume->server->using_version->av_number < 30) {
        return resource ? kFPRsrcForkLenBit : kFPDataForkLenBit;
    }

    return resource ? kFPExtRsrcForkLenBit : kFPExtDataForkLenBit;
}

/*!
 * @brief Unlock a byte range of an open fork
 *
 * @param[in] volume         volume of the fork
 * @param[in] forkid         the open fork
 * @param[in] offset         start of the range
 * @param[in] sizetorequest  bytes of the range
 * @returns 0, or -1 when the server refuses the unlock
 */
int ll_handle_unlocking(struct afp_volume * volume, unsigned short forkid,
                        uint64_t offset, uint64_t sizetorequest)
{
    uint64_t generated_offset;
    int rc;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_NO_LOCKING) {
        return 0;
    }

    if (volume->server->using_version->av_number < 30)
        rc = afp_byterangelock(volume, ByteRangeLock_Unlock,
                               forkid, offset, sizetorequest,
                               (uint32_t *) &generated_offset);
    else
        rc = afp_byterangelockext(volume, ByteRangeLock_Unlock,
                                  forkid, offset, sizetorequest, &generated_offset);

    switch (rc) {
    case kFPNoErr:
        break;

    case kFPMiscErr:
    case kFPParamErr:
    case kFPRangeNotLocked:
    default:
        return -1;
    }

    return 0;
}

/*!
 * @brief Lock a byte range of an open fork
 *
 * @param[in] volume         volume of the fork
 * @param[in] forkid         the open fork
 * @param[in] offset         start of the range
 * @param[in] sizetorequest  bytes of the range
 * @returns 0 once locked or once ten tries found the range held or the
 *          server out of locks, or -1 when the server refuses the lock for
 *          another reason
 */
int ll_handle_locking(struct afp_volume * volume, unsigned short forkid,
                      uint64_t offset, uint64_t sizetorequest)
{
#define MAX_LOCKTRYCOUNT 10
    int rc = 0;

    int try = 0;

    uint64_t generated_offset;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_NO_LOCKING) {
        return 0;
    }

    while (try < MAX_LOCKTRYCOUNT) {
            try++;

            if (volume->server->using_version->av_number < 30)
                rc = afp_byterangelock(volume, ByteRangeLock_Lock,
                                       forkid, offset, sizetorequest,
                                       (uint32_t *) &generated_offset);
            else
                rc = afp_byterangelockext(volume, ByteRangeLock_Lock,
                                          forkid, offset, sizetorequest, &generated_offset);

            switch (rc) {
            case kFPNoErr:
                goto done;

            case kFPNoMoreLocks: /* Max num of locks on server */
            case kFPLockErr:  /*Some or all of the requested range is locked
				    by another user. */
                sleep(1);
                break;

            default:
                return -1;
            }
        }

done:
    return 0;
}




/*!
 * @brief Forget the cached attributes of an open fork's file
 *
 * The file's node ID finds it under names another session's rename gave it;
 * without one, only the name its record holds is forgotten.
 *
 * @param[in,out] volume  volume of the fork
 * @param[in]     fp      the open fork
 */
void ll_fork_changed(struct afp_volume *volume,
                     const struct afp_file_info *fp)
{
    struct ml_target t;
    unsigned int cnid = fork_names(volume, fp, &t);
    dir_cache_invalidate(volume, t.did, t.name, cnid);
}

/*!
 * @brief Set an open fork's length with FPSetForkParms
 *
 * @param[in] volume  volume of the fork
 * @param[in] fp      the open fork
 * @param[in] size    the new length
 * @returns 0, or a positive errno: EFBIG for an AFP 2 length past
 *          AFP_MAX_AFP2_FILESIZE, EROFS for a read-only filesystem, EIO for
 *          an unexpected answer or a request without one, or the server's
 *          EACCES, EBUSY or ENOSPC
 */
int ll_setfork_size(struct afp_volume * volume, struct afp_file_info *fp,
                    uint64_t size)
{
    int resource = fp->resource == AFP_META_RESOURCE;
    unsigned int bitmap;
    int ret;

    if (volume->server->using_version->av_number < 30
            && size > AFP_MAX_AFP2_FILESIZE) {
        return EFBIG;
    }

    bitmap = fork_length_bit(volume, resource);
    ret = afp_setforkparms(volume, fp->forkid, (unsigned short) bitmap, size);
    /* Forget whatever the answer, as afpd may refuse after cutting the fork. */
    ll_fork_changed(volume, fp);

    switch (ret) {
    case kFPNoErr:
        break;

    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPVolLocked:
        ret = EROFS;
        break;

    case kFPLockErr:
        ret = EBUSY;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    default:
        ret = EIO;
    }

    return ret;
}


/*!
 * @brief Create a file with a soft FPCreateFile
 *
 * A soft create leaves a file at the name as it is, where a hard create
 * would empty it.
 *
 * @param[in] volume    volume of the file
 * @param[in] did       directory of the file
 * @param[in] basename  AFP name of the file in did
 * @returns 0 for a file this call made, -EEXIST for a name a file or a
 *          directory holds, or another negative errno
 */
int ll_createfile(struct afp_volume * volume, unsigned int did,
                  char *basename)
{
    int rc = afp_createfile(volume, kFPSoftCreate, did, basename);

    switch (rc) {
    /* afpd answers a directory at the name with kFPObjectTypeErr. */
    case kFPObjectExists:
    case kFPObjectTypeErr:
        return -EEXIST;

    case kFPDiskFull:
        return -ENOSPC;

    case kFPFileBusy:
        return -EBUSY;

    default:
        return -ll_answer_errno(rc);
    }
}

/*!
 * @brief Map the answer of an FPOpenFork to an errno
 *
 * @param[in] rc  the answer, or the request's failure
 * @returns 0 for kFPNoErr, or a positive errno: EACCES for a refused open,
 *          ENOENT, EPERM for a locked file, EROFS, EISDIR for a directory,
 *          EMFILE, EBUSY for a deny conflict, ETIMEDOUT for a request whose
 *          wait expired, or EIO for another answer or a failed request
 */
int ll_openfork_errno(int rc)
{
    switch (rc) {
    case kFPNoErr:
        return 0;

    /* afpd answers a write open that EACCES refuses with kFPLockErr. */
    case kFPAccessDenied:
    case kFPLockErr:
        return EACCES;

    case kFPObjectNotFound:
        return ENOENT;

    case kFPObjectLocked:
        return EPERM;

    case kFPVolLocked:
        return EROFS;

    case kFPObjectTypeErr:
        return EISDIR;

    case kFPTooManyFilesOpen:
        return EMFILE;

    case kFPDenyConflict:
        return EBUSY;

    /* A request whose wait expired may still be carried out. */
    case ETIMEDOUT:
        return ETIMEDOUT;

    default:
        return EIO;
    }
}

/*!
 * @brief Map an answer about a file or a directory to an errno
 *
 * @param[in] rc  the answer, or the request's failure
 * @returns 0 for kFPNoErr, or a positive errno: EACCES, ENOENT, EROFS, or
 *          EIO for another answer or a request without one
 */
int ll_answer_errno(int rc)
{
    switch (rc) {
    case kFPNoErr:
        return 0;

    case kFPAccessDenied:
        return EACCES;

    case kFPObjectNotFound:
        return ENOENT;

    case kFPVolLocked:
        return EROFS;

    default:
        return EIO;
    }
}

/*!
 * @brief Whether a request went without the server's answer
 *
 * AFP's answers are kFPNoErr and -5000 down; anything else leaves open
 * whether the server acted.
 *
 * @param[in] rc  the request's result
 * @returns 1 for a result that is no answer, 0 for an answer
 */
int ll_unanswered(int rc)
{
    return rc != kFPNoErr && rc > kFPAccessDenied;
}

/*!
 * @brief Open the data or resource fork that fp names
 *
 * O_CREAT's soft create fails O_EXCL for a name taken; an open the server
 * refuses removes the file it made, and one finding no file forgets the name.
 *
 * @param[in]     volume  volume of the file
 * @param[in]     path    unused
 * @param[in]     flags   open(2) flags
 * @param[in,out] fp      the fork to open with its directory, name, and fork
 *                        type set, and the open fills in the rest
 * @returns 0, or a negative errno
 */
int ll_open(struct afp_volume * volume,
            const char *path _U_, int flags,
            struct afp_file_info *fp)
{
    int ret;
    int dsi_ret;
    int rc;
    int created = 0;
    unsigned int resource = fp->resource;
    unsigned short bitmap = 0;
    unsigned char aflags = 0;
    /* O_RDONLY is 0, so we need to check access mode bits properly */
    int access_mode = flags & O_ACCMODE;

    if (access_mode == O_RDONLY) {
        aflags |= AFP_OPENFORK_ALLOWREAD;
    } else if (access_mode == O_WRONLY) {
        /* Always request read+write even for O_WRONLY. macOS AFP always opens
         * forks with AccessMode=0x0003 (Read|Write) when writing; Time Capsule
         * returns kFPMiscErr on FPWriteExt if the fork was opened write-only. */
        aflags |= (AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE);
    } else if (access_mode == O_RDWR) {
        aflags |= (AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE);
    }

    if ((aflags & AFP_OPENFORK_ALLOWWRITE) &&
            (volume_is_readonly(volume))) {
        ret = EROFS;
        goto error;
    }

    /* O_NONBLOCK, O_ASYNC, O_APPEND and O_NOATIME need nothing of the open:
     * each write says whether it appends, and AFP keeps no access time. */
    if (flags & O_CREAT) {
        rc = ll_createfile(volume, fp->did, fp->basename);
        created = rc == 0;

        /* A taken name only ends a cached absence, as nothing changed; any
         * other answer may have put a new object there. */
        if (rc == -EEXIST) {
            dir_cache_invalidate(volume, fp->did, fp->name, 0);
        } else {
            dir_cache_remove(volume, fp->did, fp->name);
        }

        if (rc != 0 && (rc != -EEXIST || (flags & O_EXCL))) {
            ret = -rc;
            goto error;
        }
    }

    if (volume->server->using_version->av_number < 30) {
        struct afp_dirent entry;
        rc = afp_getfiledirparms(volume, fp->did, kFPNodeIDBit
                                 | fork_length_bit(volume, resource), 0,
                                 fp->basename, &entry);
        ret = ll_answer_errno(rc);

        if (!ret && entry.isdir) {
            ret = EISDIR;
        }

        if (ret) {
            goto error;
        }

        /* The read's AFP 2 bound weighs size, which a truncation keeps for
         * either fork. */
        fp->size = resource ? entry.resourcesize : entry.size;
        fp->cnid = entry.cnid;

        if (!resource && dir_cache_check_open(volume, fp->did, fp->name,
                                              fp->cnid, fp->size)) {
            fp->stale = 1;
        }
    }

    fp->sync = (flags & O_SYNC) != 0;
    fp->writable = (aflags & AFP_OPENFORK_ALLOWWRITE) ? 1 : 0;
    fp->dirty = 0;

    /* The open reports the node ID a server copy checks the name against,
     * and the data fork's length, which bounds a flush's wait. */
    if (!resource && afp_server_reports_fork_length(volume->server)) {
        bitmap = kFPExtDataForkLenBit
                 | (afp_server_copies_files(volume->server) ? kFPNodeIDBit : 0);
    }

    pthread_rwlock_rdlock(&volume->open_lock);
    dsi_ret = afp_openfork(volume, resource ? 1 : 0, fp->did, aflags, bitmap,
                           fp->basename, fp);

    if (dsi_ret == kFPNoErr) {
        if ((bitmap & kFPExtDataForkLenBit)
                && dir_cache_check_open(volume, fp->did, fp->name, fp->cnid,
                                        fp->size)) {
            fp->stale = 1;
        }

        add_opened_fork(volume, fp);
    }

    pthread_rwlock_unlock(&volume->open_lock);
    ret = ll_openfork_errno(dsi_ret);

    /* The server may still open the fork, so its file stays. */
    if (ret == ETIMEDOUT) {
        created = 0;
    }

    if (ret) {
        goto error;
    }

    /* A file this open created is empty already. */
    if ((flags & O_TRUNC) && !created) {
        ret = ll_setfork_size(volume, fp, 0);

        if (ret != 0) {
            /* The caller frees the record: the fork closes and the record
             * leaves the list first. */
            afp_closefork(volume, fp->forkid);
            remove_opened_fork(volume, fp);
            goto error;
        }

        /* A truncation changes the fork as a write does. */
        fp->size = 0;
        fp->dirty = 1;
    }

    return 0;
error:

    if (created) {
        afp_delete(volume, fp->did, fp->basename);
        dir_cache_remove(volume, fp->did, fp->name);
    } else if (ret == ENOENT || ret == EISDIR) {
        /* No file holds the name, or a folder does: the record goes. */
        dir_cache_remove(volume, fp->did, fp->name);
    }

    return -ret;
}


/*!
 * @brief Whether an AFP 2 fork reaches past an offset
 *
 * A held length that ends by the offset is asked again, as another client can
 * grow the fork; afpd's 0xFFFFFFFF stands for that length or more.
 *
 * @param[in] volume  volume of the fork
 * @param[in] fp      the open fork, whose directory and name the query names
 * @param[in] offset  the offset
 * @returns 1 when the fork reaches past offset, 0 when it ends at or before
 *          it, or a negative errno when the query fails
 */
static int afp2_fork_reaches(struct afp_volume * volume,
                             struct afp_file_info * fp, uint64_t offset)
{
    struct afp_dirent now;
    uint64_t length = fp->size;
    int ret;

    if (length <= offset && length < UINT32_MAX) {
        struct ml_target t;
        (void) fork_names(volume, fp, &t);
        memset(&now, 0, sizeof(now));
        ret = afp_getfiledirparms(volume, t.did,
                                  fork_length_bit(volume, fp->resource), 0,
                                  t.afp_name, &now);

        if (ret != kFPNoErr) {
            return -ll_answer_errno(ret);
        }

        length = fp->resource ? now.resourcesize : now.size;
    }

    return length > offset || length >= UINT32_MAX;
}

/*!
 * @brief Read from an open fork, a quantum per request
 *
 * Flushes the file's other dirty forks of its kind first on an Apple server.
 * Only kFPEOFErr marks the end, or on AFP 2 the fork's length past the bound.
 *
 * @param[in]     volume  volume of the fork
 * @param[out]    buf     receives the bytes
 * @param[in]     size    capacity of buf, of which the read uses up to
 *                        INT_MAX
 * @param[in]     offset  byte offset to read from
 * @param[in,out] fp      the open fork
 * @param[out]    eof     set to 1 when the read reaches the end of the fork
 * @returns bytes read, or a negative errno, -EIO for a handle without a fork
 *          or a request that fails otherwise, -EOVERFLOW for an AFP 2 read
 *          that starts past AFP_MAX_AFP2_FILESIZE in a longer fork
 */
int ll_read(struct afp_volume * volume,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof)
{
    size_t totalsize = 0;
    int ret = 0;
    int rc;
    unsigned int quantum = volume->server->quantum;
    int locked = 0;
    struct afp_rx_buffer buffer;
    *eof = 0;

    if (!fp) {
        return -EBADF;
    }

    /* Fork 0: a server-side copy keeps the handle's fork closed, or leaves
     * the handle without one. */
    if (fp->forkid == 0) {
        return -EIO;
    }

    if (size == 0) {
        return 0;
    }

    if (size > (size_t)INT_MAX) {
        size = INT_MAX;
    }

    if (offset < 0) {
        return -EINVAL;
    }

    /* As a compatibility measure for Apple servers, Time Capsule included, a
     * read first flushes the file's other dirty forks of this kind. */
    if (afp_server_is_apple(volume->server)) {
        flush_dirty_forks_for_file(volume, fp);
    }

    if (quantum == 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "ll_read: quantum is 0, cannot read");
        return -EIO;
    }

    /* No AFP 2 request, a lock's included, passes the bound: a read stops
     * there, and one past it fails in a fork that reaches further. */
    if (volume->server->using_version->av_number < 30) {
        if ((uint64_t) offset > AFP_MAX_AFP2_FILESIZE) {
            ret = afp2_fork_reaches(volume, fp, (uint64_t) offset);

            if (ret < 0) {
                return ret;
            }

            *eof = !ret;
            return ret ? -EOVERFLOW : 0;
        }

        if (size > AFP_MAX_AFP2_FILESIZE - (uint64_t) offset + 1) {
            size = (size_t)(AFP_MAX_AFP2_FILESIZE - (uint64_t) offset + 1);
        }
    }

    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        ret = EBUSY;
        goto error;
    }

    locked = 1;

    while (totalsize < size) {
        size_t remaining = size - totalsize;
        size_t chunksize = min((size_t)quantum, remaining);
        uint64_t read_offset = (uint64_t)offset + totalsize;
        buffer.data = buf + totalsize;
        buffer.maxsize = (unsigned int)chunksize;
        buffer.size = 0;
        buffer.errorcode = 0;

        if (volume->server->using_version->av_number < 30) {
            rc = afp_read(volume, fp->forkid, (uint32_t)read_offset,
                          (uint32_t)chunksize, &buffer);
        } else {
            rc = afp_readext(volume, fp->forkid, read_offset,
                             (uint64_t)chunksize, &buffer);
        }

        switch (rc) {
        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPLockErr:
            ret = EBUSY;
            goto error;

        case kFPEOFErr:
            *eof = 1;
            totalsize += buffer.size;
            goto done;

        case kFPNoErr:
            break;

        default:
            /* Any other error, or a request that timed out or never left,
             * reads nothing. A short count would read as the file's end. */
            ret = EIO;
            goto error;
        }

        if (buffer.size == 0) {
            break;
        }

        /* A short successful AFP read is not EOF.  In particular, Classic
         * Mac OS AFP servers may cap a kFPNoErr reply below the requested
         * count. Continue from the returned offset; only an explicit
         * kFPEOFErr above establishes EOF. */
        totalsize += buffer.size;
    }

done:

    if (ll_handle_unlocking(volume, fp->forkid, offset, size)) {
        ret = EIO;
        goto error;
    }

    return (int)totalsize;
error:

    if (locked) {
        ll_handle_unlocking(volume, fp->forkid, offset, size);
    }

    return -ret;
}




/*!
 * @brief The parameters a lookup and a listing read for each object
 *
 * The same bits for both, so a listing's entry fills the cache as a
 * lookup's reply does.
 *
 * @param[in]  volume      volume of the objects
 * @param[out] filebitmap  receives the file bitmap
 * @param[out] dirbitmap   receives the directory bitmap
 */
static void ll_entry_bitmaps(const struct afp_volume *volume,
                             unsigned int *filebitmap,
                             unsigned int *dirbitmap)
{
    *filebitmap = kFPAttributeBit | kFPParentDirIDBit | kFPCreateDateBit
                  | kFPModDateBit | kFPFinderInfoBit | kFPNodeIDBit;
    *dirbitmap = *filebitmap | kFPOffspringCountBit;
    *filebitmap |= fork_length_bit(volume, 0) | fork_length_bit(volume, 1);

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        *filebitmap |= kFPUnixPrivsBit;
        *dirbitmap |= kFPUnixPrivsBit;
    } else {
        *dirbitmap |= kFPOwnerIDBit | kFPGroupIDBit;
    }
}

/*!
 * @brief Convert a name to the volume's wire form, within its limits
 *
 * @param[in]  volume    volume of the name
 * @param[in]  name      the name, in the UNIX form
 * @param[out] afp_name  receives the wire form, AFPC_MAX_NAME_BYTES long
 * @returns 0, -EINVAL for a name the volume's encoding does not hold, a
 *          Mac Roman one past 255 bytes included,
 *          -ENAMETOOLONG for one the volume refuses or whose wire form
 *          outgrows afp_name, or -ENOMEM
 */
int ll_wire_name(const struct afp_volume *volume, const char *name,
                 char *afp_name)
{
    if (convert_path_to_afp(volume->path_encoding, afp_name, name,
                            AFPC_MAX_NAME_BYTES) < 0) {
        /* A name that converts in the room of four bytes a byte is too
         * long for the record, not malformed. */
        size_t size = strlen(name) * 4U + 1U;
        char *wide = malloc(size);
        int ret;

        if (wide == NULL) {
            return -ENOMEM;
        }

        ret = convert_path_to_afp(volume->path_encoding, wide, name,
                                  (int) size) < 0
              ? -EINVAL : -ENAMETOOLONG;
        free(wide);
        return ret;
    }

    return invalid_filename(volume, afp_name) ? -ENAMETOOLONG : 0;
}

/*!
 * @brief Read one object's record with FPGetFileDirParms
 *
 * AFP 2 addresses the volume's root as a file of the volume's name in
 * directory 1.
 *
 * @param[in]  volume      volume of the object
 * @param[in]  parent_did  its directory
 * @param[in]  name        its name, in the UNIX form
 * @param[out] e           receives the record
 * @returns 0, ll_wire_name()'s error, -ENOENT for a name that holds
 *          nothing or a directory that is gone, or a negative errno, as
 *          ll_answer_errno() gives it
 */
int ll_fetch_entry(struct afp_volume *volume, unsigned int parent_did,
                   const char *name, struct afp_dirent *e)
{
    char afp_name[AFPC_MAX_NAME_BYTES];
    unsigned int filebitmap, dirbitmap;
    int rc = ll_wire_name(volume, name, afp_name);

    if (rc) {
        return rc;
    }

    ll_entry_bitmaps(volume, &filebitmap, &dirbitmap);

    if (volume->server->using_version->av_number < 30
            && parent_did == AFP_ROOT_DID && name[0] == '\0') {
        parent_did = 1;
        snprintf(afp_name, sizeof(afp_name), "%s", volume->volume_name);
    }

    rc = afp_getfiledirparms(volume, parent_did, filebitmap, dirbitmap,
                             afp_name, e);
    return rc == kFPDirNotFound ? -ENOENT : -ll_answer_errno(rc);
}

/*!
 * @brief List a directory, a page per request
 *
 * From AFP 3.1 the reply budget sizes a page, as FPEnumerateExt2 asks more
 * than a reply holds. Pages join the cache; the last forgets what none showed.
 *
 * @param[in]     volume    volume of the directory
 * @param[in]     dirid     the directory's parent
 * @param[in]     afp_name  the directory's AFP name in dirid
 * @param[in,out] l         the listing the entries join, with a lookup's
 *                          parameters and UNIX names, zeroed by the caller;
 *                          empty when the listing fails
 * @returns 0, or a negative errno: -ENOENT when dirid and afp_name name no
 *          directory the server finds, -EOVERFLOW for an AFP 2 or 3.0
 *          folder of 65 535 entries or more, whose next page's index does
 *          not fit 16 bits, -ENOTDIR for a name that holds a file
 */
int ll_readdir(struct afp_volume * volume, unsigned int dirid,
               const char *afp_name, struct afp_listing *l)
{
    unsigned short reqcount = 256;
    unsigned long startindex = 1;
    uint64_t serial = dir_cache_listing(volume);
    int rc = 0, ret = 0, exit = 0;
    unsigned int filebitmap, dirbitmap;
    ll_entry_bitmaps(volume, &filebitmap, &dirbitmap);

    if (volume->attributes & kSupportsUTF8Names) {
        dirbitmap |= kFPUTF8NameBit;
        filebitmap |= kFPUTF8NameBit;
    } else {
        dirbitmap |= kFPLongNameBit | kFPShortNameBit;
        filebitmap |= kFPLongNameBit | kFPShortNameBit;
    }

    while (!exit) {
        uint32_t before = l->count;
        uint64_t sent = dir_cache_changes(volume);

        /* Select appropriate enumerate command based on AFP version:
         * AFP < 3.0: afp_enumerate (16-bit fields, 4GB volume limit)
         * AFP 3.0: afp_enumerateext (16-bit indexes, handles >4GB volumes)
         * AFP >= 3.1: afp_enumerateext2 (32-bit indexes for large directories) */
        if (volume->server->using_version->av_number < 30) {
            rc = afp_enumerate(volume, dirid,
                               filebitmap, dirbitmap, reqcount,
                               (unsigned short) startindex,
                               afp_name, l);
        } else if (volume->server->using_version->av_number == 30) {
            rc = afp_enumerateext(volume, dirid,
                                  filebitmap, dirbitmap, reqcount,
                                  (unsigned short) startindex,
                                  afp_name, l);
        } else {
            /* The count asks for more entries than any reply holds, within
             * the signed range Apple servers read, so the reply budget sizes
             * the page. */
            rc = afp_enumerateext2(volume, dirid,
                                   filebitmap, dirbitmap, INT16_MAX,
                                   startindex, afp_name, l);
        }

        switch (rc) {
        case 0:
        case kFPObjectNotFound:
            dir_cache_insert_listing(volume, l, before, sent, serial);
            startindex += l->count - before;

            /* Some Apple AFP servers report end-of-directory as a successful
             * enumeration with zero entries instead of kFPObjectNotFound. */
            if (l->count == before || rc == kFPObjectNotFound) {
                exit = 1;
            } else if (startindex > UINT16_MAX
                       && volume->server->using_version->av_number <= 30) {
                /* FPEnumerate's and FPEnumerateExt's index is 16 bits
                 * wide: the next page's would wrap to the folder's start. */
                ret = EOVERFLOW;
                goto error;
            }

            break;

        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPDirNotFound:
            ret = ENOENT;
            goto error;

        case kFPObjectTypeErr:
            ret = ENOTDIR;
            goto error;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPParamErr:
        case kFPCallNotSupported:
        default:
            /* A request not sent or not answered is never a page. */
            ret = EIO;
            goto error;
        }
    }

    afp_listing_drop_unnamed(l);

    if (l->count > 0 && (l->entries[0].valid & kFPParentDirIDBit)) {
        dir_cache_listed(volume, l->entries[0].parent_did, serial);
    }

    return 0;
error:
    afp_listing_free(l);
    return -ret;
}


/*!
 * @brief Write to an open fork, a quantum per request
 *
 * The fork is dirty after each chunk sent, as a refused or unanswered one may
 * have landed; an append's chunks go to the server's end of the fork.
 *
 * @param[in]     volume        volume of the fork
 * @param[in]     data          bytes to write
 * @param[in]     size          length of data
 * @param[in]     offset        byte offset to write at, but for an append
 * @param[in]     append        1 for an append
 * @param[in,out] fp            the open fork, whose recorded length grows to
 *                              the write's end, an append's to the server's end
 * @param[out]    totalwritten  bytes written, also when a chunk fails
 * @returns 0, or a negative errno, -EFBIG for an AFP 2 write that would end
 *          past AFP_MAX_AFP2_FILESIZE, -EIO for a handle without a fork or
 *          for a later chunk of an append the server cut short
 */
int ll_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset, int append,
             struct afp_file_info * fp, size_t *totalwritten)
{
    int ret, err = 0;
    uint64_t sizetowrite;
    uint64_t last_end = 0;
    unsigned int max_packet_size = volume->server->quantum;
    off_t o = 0;
    int locked = 0;
    *totalwritten = 0;

    if (!fp) {
        return -EBADF;
    }

    /* Fork 0: a server-side copy keeps the handle's fork closed, or leaves
     * the handle without one. */
    if (fp->forkid == 0) {
        return -EIO;
    }

    /* The session has no quantum before DSIOpenSession. */
    if (max_packet_size == 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "ll_write: quantum is 0, cannot write");
        return -EIO;
    }

    if (volume->server->using_version->av_number < 30
            && (uint64_t) offset + size > AFP_MAX_AFP2_FILESIZE) {
        return -EFBIG;
    }

    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        err = EBUSY;
        goto error;
    }

    locked = 1;
    ret = 0;

    while (*totalwritten < size) {
        uint64_t reply_offset = 0;
        uint64_t write_offset;
        uint64_t written;
        sizetowrite = max_packet_size;

        if ((size - *totalwritten) < max_packet_size) {
            sizetowrite = size - *totalwritten;
        }

        if (sizetowrite == 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "ll_write: sizetowrite is 0, aborting");
            err = EIO;
            goto error;
        }

        /* An append's offset counts from the fork's end. */
        write_offset = append ? 0 : (uint64_t) offset + (uint64_t) o;

        if (volume->server->using_version->av_number < 30) {
            uint32_t written32 = 0;
            ret = afp_write(volume, fp->forkid,
                            append ? AFP_WRITE_FROM_END : 0,
                            (uint32_t) write_offset,
                            (uint32_t) sizetowrite,
                            (char *) data + o, &written32);
            reply_offset = written32;
        } else {
            ret = afp_writeext(volume, fp->forkid,
                               append ? AFP_WRITE_FROM_END : 0,
                               write_offset, sizetowrite,
                               (char *) data + o, &reply_offset);
        }

        switch (ret) {
        case kFPNoErr:
            break;

        case kFPAccessDenied:
            err = EACCES;
            goto error;

        case kFPDiskFull:
            err = ENOSPC;
            goto error;

        case kFPLockErr:
            err = EBUSY;
            goto error;

        default:
            err = EIO;
            goto error;
        }

        /* A reply carries the fork offset past its chunk, not a byte count;
         * a later append chunk ending short of last_end + its size was cut. */
        if (append) {
            if (o > 0 && reply_offset < last_end + sizetowrite) {
                err = EIO;
                goto error;
            }

            write_offset = reply_offset - sizetowrite;
            last_end = reply_offset;
        }

        if (reply_offset < write_offset) {
            err = EIO;
            goto error;
        }

        written = reply_offset - write_offset;

        if (written == 0 || written > sizetowrite) {
            err = EIO;
            goto error;
        }

        *totalwritten += written;
        o += written;
        fp->dirty = 1;
    }

    if (ll_handle_unlocking(volume, fp->forkid, offset, size)) {
        err = EIO;
        goto error;
    }

    /* An append ends where the server's last reply says. */
    if (append && *totalwritten > 0) {
        fp->size = last_end;
    } else if ((uint64_t) offset + *totalwritten > fp->size) {
        fp->size = (uint64_t) offset + *totalwritten;
    }

    ll_fork_changed(volume, fp);
    return 0;
error:

    /* A refused or unanswered chunk may have landed; a refused lock sent
     * none. */
    if (locked) {
        ll_handle_unlocking(volume, fp->forkid, offset, size);
        fp->dirty = 1;
        ll_fork_changed(volume, fp);
    }

    return -err;
}
