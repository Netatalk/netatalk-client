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
#include "compat.h"
#include "did.h"
#include "forklist.h"
#include "lowlevel.h"
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
 * @brief Set an open fork's length with FPSetForkParms
 *
 * @param[in] volume    volume of the fork
 * @param[in] forkid    the open fork
 * @param[in] resource  1 for a resource fork, 0 for a data fork
 * @param[in] size      the new length
 * @returns 0, or a positive errno: EFBIG for an AFP 2 length past
 *          AFP_MAX_AFP2_FILESIZE, EROFS for a read-only filesystem, EIO for
 *          an unexpected answer or a request without one, or the server's
 *          EACCES, EBUSY or ENOSPC
 */
int ll_setfork_size(struct afp_volume * volume, unsigned short forkid,
                    unsigned int resource, uint64_t size)
{
    unsigned int bitmap;
    int ret;

    if (volume->server->using_version->av_number < 30
            && size > AFP_MAX_AFP2_FILESIZE) {
        return EFBIG;
    }

    bitmap = fork_length_bit(volume, resource);
    ret = afp_setforkparms(volume, forkid, bitmap, size);

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
 * @brief Open the data or resource fork that fp names
 *
 * O_CREAT's soft create fails O_EXCL for a name taken and O_TRUNC empties
 * a file found; an open the server refuses removes the file it made.
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
     * each write carries an append's offset, and AFP keeps no access time. */
    if (flags & O_CREAT) {
        rc = ll_createfile(volume, fp->did, fp->basename);

        if (rc == 0) {
            created = 1;
        } else if (rc != -EEXIST || (flags & O_EXCL)) {
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
        ret = ll_setfork_size(volume, fp->forkid, resource, 0);

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
        memset(&now, 0, sizeof(now));
        ret = afp_getfiledirparms(volume, fp->did,
                                  fork_length_bit(volume, fp->resource), 0,
                                  fp->basename, &now);

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
 * @brief List the directory at path, a page per request
 *
 * Names come in the UNIX form, never empty. A folder a cached ID misses is
 * resolved and listed again; the cache forgets IDs the entries contradict.
 *
 * @param[in]     volume    volume of the directory
 * @param[in]     path      AFP path of the directory
 * @param[in,out] l         the listing the entries join, empty before and
 *                          after a listing that fails
 * @param[in]     resource  1 for the files' resource fork lengths, 0 for
 *                          their data fork lengths
 * @returns 0, or a negative errno: -EOVERFLOW for an AFP 2 or 3.0 folder
 *          of 65 535 entries or more
 */
int ll_readdir(struct afp_volume * volume, const char *path,
               struct afp_listing *l, int resource)
{
    unsigned short reqcount = 256;
    unsigned long startindex = 1;
    int rc = 0, ret = 0, exit = 0, resolved = 0;
    unsigned int filebitmap, dirbitmap;
    char basename[AFPC_MAX_NAME_BYTES];
    unsigned int dirid, newdid;

    if (invalid_filename(volume, path)) {
        ret = ENAMETOOLONG;
        goto error;
    }

    if (get_dirid(volume, path, basename, &dirid) < 0) {
        ret = ENOENT;
        goto error;
    }

    filebitmap = kFPAttributeBit | kFPParentDirIDBit |
                 kFPCreateDateBit | kFPModDateBit |
                 kFPNodeIDBit;
    dirbitmap = kFPAttributeBit | kFPParentDirIDBit |
                kFPCreateDateBit | kFPModDateBit |
                kFPNodeIDBit | kFPOffspringCountBit |
                kFPOwnerIDBit | kFPGroupIDBit;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        dirbitmap |= kFPUnixPrivsBit;
        filebitmap |= kFPUnixPrivsBit;
    }

    if (volume->attributes & kSupportsUTF8Names) {
        dirbitmap |= kFPUTF8NameBit;
        filebitmap |= kFPUTF8NameBit;
    } else {
        dirbitmap |= kFPLongNameBit | kFPShortNameBit;
        filebitmap |= kFPLongNameBit | kFPShortNameBit;
    }

    filebitmap |= fork_length_bit(volume, resource);

    while (!exit) {
        uint32_t before = l->count;

        /* Select appropriate enumerate command based on AFP version:
         * AFP < 3.0: afp_enumerate (16-bit fields, 4GB volume limit)
         * AFP 3.0: afp_enumerateext (16-bit indexes, handles >4GB volumes)
         * AFP >= 3.1: afp_enumerateext2 (32-bit indexes for large directories) */
        if (volume->server->using_version->av_number < 30) {
            rc = afp_enumerate(volume, dirid,
                               filebitmap, dirbitmap, reqcount,
                               (unsigned short) startindex, basename, l);
        } else if (volume->server->using_version->av_number == 30) {
            rc = afp_enumerateext(volume, dirid,
                                  filebitmap, dirbitmap, reqcount,
                                  (unsigned short) startindex, basename, l);
        } else {
            /* The count asks for more entries than any reply holds, within
             * the signed range Apple servers read, so the reply budget sizes
             * the page. */
            rc = afp_enumerateext2(volume, dirid,
                                   filebitmap, dirbitmap, INT16_MAX,
                                   startindex, basename, l);
        }

        switch (rc) {
        case 0:
        case kFPObjectNotFound:
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

            /* The folder is gone, or a cached ID on its path names no
             * folder since another session replaced one. */
            if (!resolved) {
                resolved = 1;
                remove_did_lineage(volume, path);

                if (get_dirid(volume, path, basename, &newdid) == 0
                        && newdid != dirid) {
                    dirid = newdid;
                    startindex = 1;
                    l->count = 0;
                    l->pool_used = 0;
                    break;
                }
            }

            ret = ENOENT;
            goto error;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        case kFPCallNotSupported:
        default:
            /* A request not sent or not answered is never a page. */
            ret = EIO;
            goto error;
        }
    }

    afp_listing_drop_unnamed(l);

    /* The entries carry the folder's ID and its folders', which the cache
     * checks; an empty folder's is in none, and nothing stands beneath it. */
    if (l->count == 0) {
        remove_did_subtree(volume, path);
    } else {
        check_did_entry(volume, path,
                        l->entries[0].valid & kFPParentDirIDBit
                        ? l->entries[0].parent_did : 0, l);
    }

    return 0;
error:
    afp_listing_free(l);
    return -ret;
}


/*!
 * @brief Read a path's parameters for stat(2)
 *
 * The root shows the time of the mount, or of afpsld's attach.
 *
 * @param[in]  volume    volume of the path
 * @param[in]  path      AFP path
 * @param[out] stbuf     receives the attributes
 * @param[in]  resource  1 for the resource view, whose size is the resource
 *                       fork's
 * @returns 0, or a negative errno
 */
int ll_getattr(struct afp_volume * volume, const char *path, struct stat *stbuf,
               int resource)
{
    struct afp_dirent entry;
    unsigned int dirid;
    int rc;
    unsigned int filebitmap, dirbitmap;
    char basename[AFPC_MAX_NAME_BYTES];
    memset(stbuf, 0, sizeof(struct stat));

    if ((volume->server) &&
            (invalid_filename(volume, path))) {
        return -ENAMETOOLONG;
    }

    if (get_dirid(volume, path, basename, &dirid) < 0) {
        return -ENOENT;
    }

    dirbitmap = kFPAttributeBit
                | kFPCreateDateBit | kFPModDateBit |
                kFPNodeIDBit |
                kFPParentDirIDBit | kFPOffspringCountBit;
    filebitmap = kFPAttributeBit |
                 kFPCreateDateBit | kFPModDateBit |
                 kFPNodeIDBit |
                 kFPFinderInfoBit |
                 kFPParentDirIDBit;

    if (volume->server->using_version->av_number < 30
            && path[0] == '/' && path[1] == '\0') {
        /* AFP 2 reads the root as the volume's name in its parent, DID 1. */
        snprintf(basename, sizeof(basename), "%s",
                 volume->volume_name);
        dirid = 1;
    }

    filebitmap |= fork_length_bit(volume, resource);

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        dirbitmap |= kFPUnixPrivsBit;
        filebitmap |= kFPUnixPrivsBit;
    } else {
        dirbitmap |= kFPOwnerIDBit | kFPGroupIDBit;
    }

    rc = afp_getfiledirparms(volume, dirid, filebitmap, dirbitmap,
                             (char *) basename, &entry);

    switch (rc) {
    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPNoErr:
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
    default:
        return -EIO;
    }

    /* A folder another session replaced has another ID: the cache forgets
     * the old one and those beneath it. */
    if (entry.isdir && (entry.valid & kFPNodeIDBit)) {
        check_did_entry(volume, path, entry.cnid, NULL);
    }

    entry.kind = resource ? AFP_META_RESOURCE : AFP_META_NONE;

    if (path[0] == '/' && path[1] == '\0' && entry.isdir) {
        entry.create_ns = afp_ns_from_sec(volume->mount_time);
        entry.mod_ns = entry.create_ns;
        entry.valid |= kFPCreateDateBit | kFPModDateBit;
    }

    return afp_dirent_to_stat(volume, &entry, stbuf);
}


/*!
 * @brief Write to an open fork, a quantum per request
 *
 * Refuses an AFP 2 write that would end past AFP_MAX_AFP2_FILESIZE. The
 * fork is dirty after each chunk the server acknowledges.
 *
 * @param[in]     volume        volume of the fork
 * @param[in]     data          bytes to write
 * @param[in]     size          length of data
 * @param[in]     offset        byte offset to write at
 * @param[in,out] fp            the open fork
 * @param[out]    totalwritten  bytes written, also when a chunk fails
 * @returns 0, or a negative errno, -EFBIG for that AFP 2 write and -EIO for
 *          a handle without a fork
 */
int ll_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset,
             struct afp_file_info * fp, size_t *totalwritten)
{
    int ret, err = 0;
    uint64_t sizetowrite;
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

        if (volume->server->using_version->av_number < 30) {
            uint32_t written32 = 0;
            ret = afp_write(volume, fp->forkid,
                            offset + o, sizetowrite,
                            (char *) data + o, &written32);
            reply_offset = written32;
        } else {
            ret = afp_writeext(volume, fp->forkid,
                               offset + o, sizetowrite,
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

        /* FPWrite/FPWriteExt replies carry the resulting fork offset, not a
         * byte count.  At offset 0 those are the same; later chunks must be
         * converted back into bytes written. */
        write_offset = (uint64_t)offset + (uint64_t)o;

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

    return 0;
error:

    if (locked) {
        ll_handle_unlocking(volume, fp->forkid, offset, size);
    }

    return -err;
}
