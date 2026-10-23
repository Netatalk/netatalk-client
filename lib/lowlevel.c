/*
    lowlevel.c: some functions that abstract common operations; used
	so the same code can be used between meta and normal files

    Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>

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
#include "midlevel.h"
#include "users.h"
#include "utils.h"

static void set_nonunix_perms(unsigned int *mode, struct afp_file_info *fp)
{
    if (fp->isdir) {
        *mode = 0700 | S_IFDIR;
    } else {
        *mode = 0600 | S_IFREG;
    }
}

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




/* zero_file()
 *
 * This function will truncate the fork given to zero bytes in length.
 * This has been abstracted because there is some differences in the
 * expectation of Ext or not Ext. */

int ll_zero_file(struct afp_volume * volume, unsigned short forkid,
                 unsigned int resource)
{
    unsigned int bitmap;
    int ret;

    /* The Airport Extreme 7.1.1 will crash if you send it
     * DataForkLenBit. */

    if (volume->server->using_version->av_number < 30) {
        bitmap = (resource ?
                  kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        bitmap = (resource ?
                  kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    ret = afp_setforkparms(volume, forkid, bitmap, 0);

    switch (ret) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPVolLocked:
    case kFPLockErr:
        ret = EBUSY;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
        ret = EIO;
        break;

    default:
        ret = 0;
    }

    return ret;
}

int ll_setfork_size(struct afp_volume * volume, unsigned short forkid,
                    unsigned int resource, uint64_t size)
{
    unsigned int bitmap;
    int ret;

    /* The Airport Extreme 7.1.1 will crash if you send it
     * DataForkLenBit. */

    if (volume->server->using_version->av_number < 30) {
        bitmap = (resource ?
                  kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        bitmap = (resource ?
                  kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    ret = afp_setforkparms(volume, forkid, bitmap, size);

    switch (ret) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPVolLocked:
    case kFPLockErr:
        ret = EBUSY;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
        ret = EIO;
        break;

    default:
        ret = 0;
    }

    return ret;
}


/* get_directory_entry is used to abstract afp_getfiledirparms
 *  * because in AFP<3.0 there is only afp_getfileparms and afp_getdirparms.
 *   */

int ll_get_directory_entry(struct afp_volume * volume,
                           char *basename,
                           unsigned int dirid,
                           unsigned int filebitmap, unsigned int dirbitmap,
                           struct afp_file_info *p)
{
    int ret;
    char tmpname[AFPC_MAX_NAME_BYTES];
    memcpy(tmpname, p->basename, sizeof(tmpname));
    ret = afp_getfiledirparms(volume, dirid,
                              filebitmap, dirbitmap, basename, p);
    memcpy(p->basename, tmpname, sizeof(p->basename));
    return ret;
}



/*!
 * @brief Open the data or resource fork that fp names
 *
 * O_TRUNC empties the file only when the flags lack O_CREAT.
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
        ret = EPERM;
        goto error;
    }

    /*
       O_NOBLOCK - todo: it is unclear how to this in fuse.
    */
    /* The following flags don't need any attention here:
       O_ASYNC - not relevant for files
       O_APPEND
       O_NOATIME - we have no way to handle this anyway
    */
    /* Handle file creation properly when O_CREAT is set */
    if ((flags & O_CREAT) && (aflags & AFP_OPENFORK_ALLOWWRITE)) {
        if (flags & O_EXCL) {
            /* With O_EXCL, file must not exist. Use kFPHardCreate
             * (hard create - fails if file exists) instead of kFPSoftCreate */
            rc = afp_createfile(volume, kFPHardCreate, fp->did, fp->basename);

            if (rc == kFPObjectExists) {
                ret = EEXIST;
                goto error;
            } else if (rc != 0) {
                ret = EIO;
                goto error;
            }
        } else {
            /* Without O_EXCL, use kFPSoftCreate (soft create - succeeds if exists) */
            rc = afp_createfile(volume, kFPSoftCreate, fp->did, fp->basename);

            if (rc && rc != kFPObjectExists) {
                ret = EIO;
                goto error;
            }

            /* If rc == kFPObjectExists, file already exists, which is fine */
        }
    }

    if (volume->server->using_version->av_number < 30) {
        switch (ll_get_directory_entry(volume, fp->basename, fp->did,
                                       kFPParentDirIDBit | kFPNodeIDBit |
                                       (resource ? kFPRsrcForkLenBit : kFPDataForkLenBit),
                                       0, fp)) {
        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPObjectNotFound:
            ret = ENOENT;
            goto error;

        case kFPNoErr:
            break;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPParamErr:
        default:
            ret = EIO;
            goto error;
        }

        if (resource ? (fp->resourcesize >= (AFP_MAX_AFP2_FILESIZE - 1)) :
                (fp->size >= AFP_MAX_AFP2_FILESIZE - 1)) {
            /* According to p.30, if the server doesn't support >4GB files
               and the file being opened is >4GB, then resourcesize or size
               will return 4GB.  How can it return 4GB in 32 bits?  I
               suspect it actually returns 4GB-1.
            */
            ret = EOVERFLOW;
            goto error;
        }

        /* parse_reply_block() clears the complete result structure.  Keep
         * the caller's fork type: resource forks need it for both the AFP
         * open and appledouble_close(), which unregisters the open fork. */
        fp->resource = resource;
    }

    /* These are client-side open state, not AFP reply parameters.  Set them
     * after the AFP 2.x parameter query, whose reply parser clears fp. */
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

    switch (dsi_ret) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPObjectNotFound:
        /* File not found and O_CREAT wasn't set (or wasn't honored above) */
        ret = ENOENT;
        goto error;

    case kFPObjectLocked:
        ret = EROFS;
        goto error;

    case kFPObjectTypeErr:
        ret = EISDIR;
        goto error;

    case kFPParamErr:
        ret = EACCES;
        goto error;

    case kFPTooManyFilesOpen:
        ret = EMFILE;
        goto error;

    case kFPVolLocked:
    case kFPDenyConflict:
    case kFPMiscErr:
    case kFPBitmapErr:
    case -1:
        ret = EFAULT;
        goto error;

    case 0:
        ret = 0;
        break;

    default:
        ret = EFAULT;
        goto error;
    }

    /* Handle O_TRUNC flag: truncate existing files (but not newly created ones) */
    /* Skip truncation if O_CREAT was set (file just created, already empty) */
    if ((flags & O_TRUNC) && !(flags & O_CREAT)) {
        ret = ll_zero_file(volume, fp->forkid, resource);

        if (ret != 0) {
            goto error;
        }

        /* A truncation changes the fork as a write does. */
        fp->size = 0;
        fp->dirty = 1;
    }

    return 0;
error:
    return -ret;
}


/*!
 * @brief Read from an open fork, a quantum per request
 *
 * On an Apple server, first flushes the file's other dirty forks of the same
 * kind. Only kFPEOFErr marks the fork's end: the read continues where a
 * short reply ends.
 *
 * @param[in]     volume  volume of the fork
 * @param[out]    buf     receives the bytes
 * @param[in]     size    capacity of buf, of which the read uses up to
 *                        INT_MAX
 * @param[in]     offset  byte offset to read from
 * @param[in,out] fp      the open fork
 * @param[out]    eof     set to 1 when the read reaches the end of the fork
 * @returns bytes read, or a negative errno, -EIO for a handle without a fork
 *          or a request that fails otherwise
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

    /* Lock the range */
    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        /* There was an irrecoverable error when locking */
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
            if (read_offset > UINT32_MAX) {
                ret = EFBIG;
                goto error;
            }

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
        /* Somehow, we couldn't unlock the range. */
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
 * From AFP 3.1, FPEnumerateExt2 asks for more entries than a reply holds, so
 * the reply budget sizes the page. Older versions use FPEnumerateExt or
 * FPEnumerate. The listing returns names in precomposed UTF-8.
 *
 * @param[in]  volume    volume of the directory
 * @param[in]  path      AFP path of the directory
 * @param[out] fb        the entries, a list the caller frees
 * @param[in]  resource  1 for the files' resource fork lengths, 0 for their
 *                       data fork lengths
 * @returns 0, or a negative errno
 */
int ll_readdir(struct afp_volume * volume, const char *path,
               struct afp_file_info **fb, int resource)
{
    struct afp_file_info * p, *filebase = NULL, *base, *last = NULL;
    unsigned short reqcount = 256;
    unsigned long startindex = 1;
    int rc = 0, ret = 0, exit = 0;
    unsigned int filebitmap, dirbitmap;
    char basename[AFPC_MAX_NAME_BYTES];
    char converted_name[AFPC_MAX_NAME_BYTES];
    unsigned int dirid;

    if (invalid_filename(volume, path)) {
        return -ENAMETOOLONG;
    }

    if (get_dirid(volume, path, basename, &dirid) < 0) {
        return -ENOENT;
    }

    /* We need to handle length bits differently for AFP < 3.0 */
    filebitmap = kFPAttributeBit | kFPParentDirIDBit |
                 kFPCreateDateBit | kFPModDateBit |
                 kFPBackupDateBit |
                 kFPNodeIDBit;
    dirbitmap = kFPAttributeBit | kFPParentDirIDBit |
                kFPCreateDateBit | kFPModDateBit |
                kFPBackupDateBit |
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

    if (volume->server->using_version->av_number < 30) {
        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    while (!exit) {
        unsigned int entries_returned = 0;
        base = NULL;

        /* Select appropriate enumerate command based on AFP version:
         * AFP < 3.0: afp_enumerate (16-bit fields, 4GB volume limit)
         * AFP 3.0: afp_enumerateext (16-bit indexes, handles >4GB volumes)
         * AFP >= 3.1: afp_enumerateext2 (32-bit indexes for large directories) */
        if (volume->server->using_version->av_number < 30) {
            rc = afp_enumerate(volume, dirid,
                               filebitmap, dirbitmap, reqcount,
                               startindex, basename, &base);
        } else if (volume->server->using_version->av_number == 30) {
            rc = afp_enumerateext(volume, dirid,
                                  filebitmap, dirbitmap, reqcount,
                                  startindex, basename, &base);
        } else {
            /* The count asks for more entries than any reply holds, within
             * the signed range Apple servers read, so the reply budget sizes
             * the page. */
            rc = afp_enumerateext2(volume, dirid,
                                   filebitmap, dirbitmap, INT16_MAX,
                                   startindex, basename, &base);
        }

        switch (rc) {
        case -1:
            ret = EIO;
            goto error;

        case 0:
        case kFPObjectNotFound:
            if (filebase == NULL) {
                filebase = base;
            } else {
                last->next = base;
            }

            for (p = base; p; p = p->next) {
                startindex++;
                entries_returned++;
                last = p;
            }

            /* Some Apple AFP servers report end-of-directory as a successful
             * enumeration with zero entries instead of kFPObjectNotFound. */
            if (entries_returned == 0) {
                exit = 1;
            }

            if (rc == kFPObjectNotFound) {
                exit = 1;
            }

            break;

        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPDirNotFound:
            ret = ENOENT;
            exit++;
            break;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        case kFPCallNotSupported:
            ret = EIO;
            goto error;
        }
    }

    for (p = filebase; p; p = p->next) {
        /* Convert all the names back to precomposed */
        convert_path_to_unix(
            volume->path_encoding,
            converted_name, p->name, sizeof(converted_name));
        snprintf(p->name, sizeof(p->name), "%s", converted_name);
        p->name[sizeof(p->name) - 1U] = '\0';
        startindex++;
    }

    if (volume->server->using_version->av_number < 30) {
        for (p = filebase; p; p = p->next) {
            unsigned int temp_permissions = p->unixprivs.permissions;
            set_nonunix_perms(&temp_permissions, p);
            p->unixprivs.permissions = temp_permissions;

            /* Some AFP 2.x servers return the unset-date sentinel for
             * directory times.  Preserve every real server timestamp; only
             * use the connection time where the protocol says no date exists.
             */
            if (p->isdir) {
                if (p->creation_date_is_unset) {
                    p->creation_date = volume->server->connect_time;
                }

                if (p->modification_date_is_unset) {
                    p->modification_date = volume->server->connect_time;
                }
            }
        }
    }

    *fb = filebase;
    return 0;
error:
    return -ret;
}


int ll_getattr(struct afp_volume * volume, const char *path, struct stat *stbuf,
               int resource)
{
    struct afp_file_info fp;
    unsigned int dirid;
    int rc;
    unsigned int filebitmap, dirbitmap;
    char basename[AFPC_MAX_NAME_BYTES];
    time_t creation_date;
    time_t modification_date;
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

    if (volume->server->using_version->av_number < 30) {
        if (path[0] == '/' && path[1] == '\0') {
            /* This will sound odd, but when referring to /, AFP 2.x
               clients check on a 'file' with the volume name. */
            snprintf(basename, sizeof(basename), "%s",
                     volume->volume_name);
            dirid = 1;
        }

        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        filebitmap |= (resource ? kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        dirbitmap |= kFPUnixPrivsBit;
        filebitmap |= kFPUnixPrivsBit;
    } else {
        dirbitmap |= kFPOwnerIDBit | kFPGroupIDBit;
    }

    rc = afp_getfiledirparms(volume, dirid, filebitmap, dirbitmap,
                             (char *) basename, &fp);

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

    if (volume->server->using_version->av_number >= 30
            && fp.unixprivs.permissions != 0) {
        stbuf->st_mode |= fp.unixprivs.permissions;
    } else {
        set_nonunix_perms((unsigned int *)&stbuf->st_mode, &fp);
    }

    stbuf->st_ino = fp.fileid;
    stbuf->st_uid = fp.unixprivs.uid;
    stbuf->st_gid = fp.unixprivs.gid;

    if (translate_uidgid_to_client(volume,
                                   &stbuf->st_uid, &stbuf->st_gid)) {
        return -EIO;
    }

    if (stbuf->st_mode & S_IFDIR) {
        stbuf->st_nlink = fp.offspring + 2;
        stbuf->st_size = resource ? 0 : (fp.offspring * 34) + 24;
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = (stbuf->st_size + 511) / 512;
        /* This slight voodoo was taken from Mac OS X 10.2 */
    } else {
        stbuf->st_nlink = 1;
        stbuf->st_size = (resource ? fp.resourcesize : fp.size);
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = (stbuf->st_size + 511) / 512;
    }

    if ((volume->server->using_version->av_number < 30) &&
            (stbuf->st_mode & S_IFDIR)) {
        /* AFP 2.x doesn't give ctime and mtime for directories*/
        creation_date = volume->server->connect_time;
        modification_date = volume->server->connect_time;
    } else if ((path[0] == '/' && path[1] == '\0') && (stbuf->st_mode & S_IFDIR)) {
        /* For the root directory, use the mount time as creation/modification date */
        creation_date = volume->mount_time;
        modification_date = volume->mount_time;
    } else {
        creation_date = fp.creation_date;
        modification_date = fp.modification_date;
    }

#ifdef __linux__
    stbuf->st_ctim.tv_sec = creation_date;
    stbuf->st_mtim.tv_sec = modification_date;
#else
    stbuf->st_ctime = creation_date;
    stbuf->st_mtime = modification_date;
#endif
#ifdef __APPLE__
    /* On macOS, set birthtimespec for Finder display (handled by stat_to_darwin_attr) */
    stbuf->st_birthtimespec.tv_sec = creation_date;
    stbuf->st_birthtimespec.tv_nsec = 0;
#endif
    return 0;
}


/*!
 * @brief Write to an open fork, a quantum per request
 *
 * Refuses a write that would reach 4 GiB on AFP 2, because FPWrite's offsets
 * are 32 bits. afpd reads them signed and refuses one past 2 GiB itself. The
 * fork is dirty after each chunk the server acknowledges.
 *
 * @param[in]     volume        volume of the fork
 * @param[in]     data          bytes to write
 * @param[in]     size          length of data
 * @param[in]     offset        byte offset to write at
 * @param[in,out] fp            the open fork
 * @param[out]    totalwritten  bytes written, also when a chunk fails
 * @returns 0, or a negative errno, -EFBIG on AFP 2 at 4 GiB and -EIO for a
 *          handle without a fork
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

    /* FPWrite's offset and its reply's are 32 bits, so a write that reaches
     * 4 GiB can neither be addressed nor acknowledged. */
    if (volume->server->using_version->av_number < 30
            && (uint64_t) offset + size >= AFP_MAX_AFP2_FILESIZE) {
        return -EFBIG;
    }

    /* Get a lock */
    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        /* There was an irrecoverable error when locking */
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

        /* Defensive check: never send a zero-byte write */
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
        case kFPMiscErr:
        case kFPParamErr:
            err = EINVAL;
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
        /* Somehow, we couldn't unlock the range. */
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
