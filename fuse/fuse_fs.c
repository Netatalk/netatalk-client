/*
 *  fuse_fs.c, FUSE interfaces for Netatalk Client
 *  Heavily modifed from the example code by Miklos Szeredi
 *
 *  Copyright (C) 2001-2005 Miklos Szeredi <miklos@szeredi.hu>
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The FUSE operations of a mounted AFP volume
 */

#define HAVE_ARCH_STRUCT_FLOCK

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <syslog.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <pwd.h>

/* Extended attribute headers - platform dependent */
#if defined(HAVE_ATTR_XATTR_H)
#include <attr/xattr.h>
#elif defined(HAVE_SYS_XATTR_H)
#include <sys/xattr.h>
#elif defined(HAVE_SYS_EXTATTR_H)
#include <sys/extattr.h>
#endif

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/codepage.h"
#include "lib/compat.h"
#include "lib/dsi.h"
#include "lib/midlevel.h"
#include "lib/xattr.h"

#include "fuse_error.h"
#include "fuse_fs.h"

/* Define xattr constants if not provided by system headers */
#ifndef XATTR_CREATE
#define XATTR_CREATE 0x1
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE 0x2
#endif

#ifdef __APPLE__
#define AFP_FINDERINFO_FLAG_INVISIBLE 0x4000
#define AFP_ACCESS_UNKNOWN_BITS 0x000fc001
#define AFP_ACCESS_READ_BITS    ((1U << 1) | (1U << 7) | (1U << 9) | (1U << 11))
#define AFP_ACCESS_WRITE_BITS   ((1U << 2) | (1U << 4) | (1U << 5) | \
                                 (1U << 6) | (1U << 8) | (1U << 10))
#define AFP_ACCESS_OWNER_BITS   ((1U << 12) | (1U << 13))
#define AFP_ACCESS_EXEC_BITS    (1U << 3)
#ifndef UF_HIDDEN
#define UF_HIDDEN 0x00008000
#endif
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004
#endif
#endif

/* If not defined by the build system, default to 0 (old API) */
#ifndef FUSE_NEW_API
#define FUSE_NEW_API 0
#endif

#if defined(__APPLE__) && FUSE_USE_VERSION >= 30
static uint16_t finderinfo_get_flags(const char finderinfo[32])
{
    return ((uint16_t)(unsigned char)finderinfo[8] << 8)
           | (uint16_t)(unsigned char)finderinfo[9];
}

static void finderinfo_set_flags(char finderinfo[32], uint16_t flags)
{
    finderinfo[8] = (char)((flags >> 8) & 0xff);
    finderinfo[9] = (char)(flags & 0xff);
}
#endif

/*
 * Work around malformed paths that macFUSE occasionally supplies as
 * <dir1>/<dir2>/(null) by truncating the path before "(null)".
 */
static int strip_null_path_component(const char **path, char **storage)
{
    const char *null_component = strstr(*path, "(null)");
    *storage = NULL;

    if (null_component && null_component > *path) {
        *storage = strndup(*path, (size_t)(null_component - *path - 1));

        if (!*storage) {
            return -ENOMEM;
        }

        *path = *storage;
    }

    return 0;
}


#if defined(__APPLE__) && FUSE_USE_VERSION >= 30
/* Helper function to convert struct stat to struct fuse_darwin_attr on macOS */
static void stat_to_darwin_attr(const struct stat *st,
                                struct fuse_darwin_attr *attr)
{
    memset(attr, 0, sizeof(struct fuse_darwin_attr));
    attr->ino = st->st_ino;
    attr->mode = st->st_mode;
    attr->nlink = st->st_nlink;
    attr->uid = st->st_uid;
    attr->gid = st->st_gid;
    attr->rdev = st->st_rdev;
    attr->atimespec = st->st_atimespec;
    attr->mtimespec = st->st_mtimespec;
    attr->ctimespec = st->st_ctimespec;
    /* Set birth time (btimespec) to the creation time for macOS Finder display */
    attr->btimespec = st->st_birthtimespec;
    attr->size = st->st_size;
    attr->blocks = st->st_blocks;
    attr->blksize = st->st_blksize;
    attr->flags = st->st_flags;
}

#endif

static int fuse_readlink(const char *path, char *buf, size_t size)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** readlink of %s", path);
    ret = ml_readlink(volume, path, buf, size);

    if (ret == -EFAULT) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Got some sort of internal error in afp_open for readlink");
    }

    return ret;
}

static int fuse_rmdir(const char *path)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** rmdir of %s", path);
    ret = ml_rmdir(volume, path);
    return ret;
}

static int fuse_unlink(const char *path)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** unlink of %s", path);
    ret = ml_unlink(volume, path);
    return ret;
}

/* Extended attributes (AFP 3.2+) */
#ifdef __APPLE__
static int fuse_getxattr(const char *path, const char *name, char *value,
                         size_t size, uint32_t position)
#else
static int fuse_getxattr(const char *path, const char *name, char *value,
                         size_t size)
#endif
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
#ifdef __APPLE__

    if (strcmp(name, AFP_XATTR_RESOURCEFORK) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** getxattr ResourceFork %s (size=%zu position=%u)",
                       path, size, position);
        ret = ml_getresourcefork(volume, path, value, size, position);
        return ret;
    }

    if (strcmp(name, AFP_XATTR_FINDERINFO) == 0) {
        if (position != 0) {
            return -EOPNOTSUPP;
        }

        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** getxattr FinderInfo %s (size=%zu)", path, size);
        ret = ml_getfinderinfo(volume, path, value, size);
        return ret;
    }

    /* AFP does not support positioned xattrs; ignore non-zero positions */
    if (position != 0) {
        return -EOPNOTSUPP;
    }

#endif
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** getxattr %s:%s (size=%zu)", path, name, size);
    ret = ml_getxattr(volume, path, name, value, size);
    return ret;
}

#ifdef __APPLE__
static int fuse_setxattr(const char *path, const char *name,
                         const char *value, size_t size, int flags,
                         uint32_t position)
#else
static int fuse_setxattr(const char *path, const char *name,
                         const char *value, size_t size, int flags)
#endif
{
    int ml_flags = 0;
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
#ifdef XATTR_CREATE

    if (flags & XATTR_CREATE) {
        ml_flags |= kXAttrCreate;
    }

#endif
#ifdef XATTR_REPLACE

    if (flags & XATTR_REPLACE) {
        ml_flags |= kXAttrReplace;
    }

#endif
#ifdef __APPLE__

    if (strcmp(name, AFP_XATTR_RESOURCEFORK) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** setxattr ResourceFork %s (size=%zu position=%u)",
                       path, size, position);
        ret = ml_setresourcefork_flags(volume, path, value, size, position,
                                       ml_flags);
        return ret;
    }

    if (strcmp(name, AFP_XATTR_FINDERINFO) == 0) {
        if (position != 0) {
            return -EOPNOTSUPP;
        }

        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** setxattr FinderInfo %s (size=%zu)", path, size);
        ret = ml_setfinderinfo_flags(volume, path, value, size, ml_flags);
        return ret;
    }

    if (position != 0) {
        return -EOPNOTSUPP;
    }

#endif
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** setxattr %s:%s (size=%zu)", path, name, size);
    ret = ml_setxattr(volume, path, name, value, size, ml_flags);
    return ret;
}

static int fuse_listxattr(const char *path, char *list, size_t size)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** listxattr %s (size=%zu)", path, size);
    ret = ml_listxattr(volume, path, list, size);
    return ret;
}

static int fuse_removexattr(const char *path, const char *name)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** removexattr %s:%s", path, name);
#ifdef __APPLE__

    if (strcmp(name, AFP_XATTR_RESOURCEFORK) == 0) {
        ret = ml_removeresourcefork(volume, path);
        return ret;
    }

    if (strcmp(name, AFP_XATTR_FINDERINFO) == 0) {
        ret = ml_removefinderinfo(volume, path);
        return ret;
    }

#endif
    ret = ml_removexattr(volume, path, name);
    return ret;
}


/* Function signature differs by platform and FUSE version */
#if defined(__APPLE__) && FUSE_USE_VERSION >= 30
static int fuse_readdir_darwin(const char *path, void *buf,
                               fuse_darwin_fill_dir_t filler,
                               off_t offset _U_,
                               struct fuse_file_info *fi _U_,
                               enum fuse_readdir_flags flags _U_)
#elif FUSE_USE_VERSION >= 30 && FUSE_NEW_API
/* Linux FUSE 3.10+ with fuse_readdir_flags */
static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset _U_, struct fuse_file_info *fi _U_,
                        enum fuse_readdir_flags flags _U_)
#else
/* BSD FUSE 3.x and FUSE 2.x - older API without fuse_readdir_flags */
static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset _U_, struct fuse_file_info *fi _U_)
#endif
{
    struct afp_file_info * filebase = NULL;
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** readdir of %s", path);
#if defined(__APPLE__) && FUSE_USE_VERSION >= 30 || (FUSE_USE_VERSION >= 30 && FUSE_NEW_API)
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
#else
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
#endif
    ret = ml_readdir(volume, path, &filebase);

    if (ret) {
        goto error;
    }

    for (struct afp_file_info *p = filebase; p; p = p->next) {
#if defined(__APPLE__) && FUSE_USE_VERSION >= 30 || (FUSE_USE_VERSION >= 30 && FUSE_NEW_API)
        filler(buf, p->name, NULL, 0, 0);
#else
        filler(buf, p->name, NULL, 0);
#endif
    }

    afp_ml_filebase_free(&filebase);
    return 0;
error:
    return ret;
}

/*!
 * @brief Refuse mknod(2) of a FIFO, a socket or a device node
 *
 * AFP has none of them; libfuse sends a regular file's mknod(2) to
 * fuse_create().
 *
 * @param[in] path  UTF-8 path of the node, for the log
 * @param[in] mode  unused
 * @param[in] dev   unused
 * @returns -EPERM
 */
static int fuse_mknod(const char *path, mode_t mode _U_, dev_t dev _U_)
{
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** mknod of %s", path);
    return -EPERM;
}

/*!
 * @brief Create and open a file for open(2) with O_CREAT
 *
 * The handle records whether this create made the file, which the offload
 * requires of its destination, and an open the server refuses removes it.
 *
 * @param[in]     path  UTF-8 path of the file
 * @param[in]     mode  its mode
 * @param[in,out] fi    the kernel's file info, which receives the handle
 * @returns 0, or a negative errno
 */
static int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct afp_file_info * fp;
    unsigned char created = 0;
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** create of %s with mode 0%o, flags 0x%x", path, mode, fi->flags);
    ret = ml_creat(volume, path, mode, &created);

    /* Another client can take the name after the kernel's lookup: O_EXCL
     * fails the create, and XNU retries osxfuse's O_EXCL creates as opens. */
    if (ret != 0 && (ret != -EEXIST || (fi->flags & O_EXCL))) {
        return ret;
    }

    if (ret == -EEXIST) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** create: file exists, opening it");
    }

    /* The file exists, so the open sends no create of its own. A file
     * ml_creat made is empty, and one it found takes O_TRUNC. */
    ret = ml_open(volume, path,
                  fi->flags & ~(O_CREAT | O_EXCL | (created ? O_TRUNC : 0)),
                  &fp);

    if (ret != 0) {
        /* An open without an answer may still open the file. */
        if (created && ret != -ETIMEDOUT) {
            ml_unlink(volume, path);
        }

        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** create open failed with ret=%d", ret);
        return ret;
    }

    /* An open fork writes whatever mode the file then takes. */
    if (created) {
        ml_set_created_mode(volume, path, mode);
    }

    fi->fh = (unsigned long) fp;
    fp->created = created;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache create \"%s\" flags=0x%x fh=%" PRIu64
                   " forkid=%d", path, fi->flags, fi->fh, fp->forkid);
    return 0;
}

/*!
 * @brief Flush the handle's fork to the server's disk for fsync(2)
 *
 * @param[in] path      UTF-8 path of the file, for the log
 * @param[in] datasync  unused: fdatasync(2) flushes the same way
 * @param[in] fi        the kernel's file info with the handle
 * @returns 0, -EACCES when the server denies access, -EIO for any other
 *          failure, or -EBADF without a handle
 */
static int fuse_fsync(const char *path, int datasync _U_,
                      struct fuse_file_info *fi)
{
    struct afp_file_info *fp = (struct afp_file_info *) fi->fh;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache fsync \"%s\" forkid=%d writable=%u resource=%u",
                   path, fp ? fp->forkid : -1, fp ? fp->writable : 0,
                   fp ? fp->resource : 0);

    if (!fp) {
        return -EBADF;
    }

    return ml_fsync(volume, fp);
}

/*!
 * @brief Close a file the kernel releases, and free its handle
 *
 * @param[in] path  UTF-8 path of the file, for the log
 * @param[in] fi    the kernel's file info with the handle
 * @returns 0, or a negative errno
 */
static int fuse_release(const char *path, struct fuse_file_info * fi)
{
    struct afp_file_info * fp = (void *) fi->fh;
    int ret = 0;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache release \"%s\" forkid=%d writable=%u resource=%u",
                   path, fp ? fp->forkid : -1, fp ? fp->writable : 0,
                   fp ? fp->resource : 0);
    ret = ml_close(volume, fp);
    free((void *) fi->fh);
    fi->fh = 0;
    return ret;
}

static int fuse_open(const char *path, struct fuse_file_info *fi)
{
    struct afp_file_info * fp;
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    int flags = fi->flags;
    ret = ml_open(volume, path, flags, &fp);

    if (ret == 0) {
        fi->fh = (unsigned long) fp;
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** cache open \"%s\" flags=0x%x fh=%" PRIu64
                       " forkid=%d writable=%u resource=%u",
                       path, flags, fi->fh, fp->forkid, fp->writable,
                       fp->resource);
    }

    return ret;
}


/*!
 * @brief Write to the handle's fork for write(2)
 *
 * @param[in] path    UTF-8 path of the file, for the log
 * @param[in] data    bytes to write
 * @param[in] size    length of data
 * @param[in] offset  byte offset to write at
 * @param[in] fi      the kernel's file info with the handle
 * @returns bytes written, or a negative errno
 */
static int fuse_write(const char *path, const char *data,
                      size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    struct afp_file_info *fp = (struct afp_file_info *) fi->fh;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache write \"%s\" offset=%llu size=%llu",
                   path, (unsigned long long) offset, (unsigned long long) size);
    return ml_write(volume, data, size, offset, fp);
}

/* Linux clamps a copy to the source's end and copies pseudo-files itself on
 * EOPNOTSUPP. Before 3.18, libfuse narrows the length to size_t. */
#if defined(HAVE_FUSE_COPY_FILE_RANGE) && defined(__linux__) \
    && SIZE_MAX > UINT32_MAX
#define AFP_FUSE_COPY_FILE_RANGE 1
#endif

#ifdef AFP_FUSE_COPY_FILE_RANGE
/*!
 * @brief Copy between two handles of the mount for copy_file_range(2)
 *
 * -EBADF means a missing handle, or a destination that is not open for
 * writing or is on a read-only volume.
 *
 * @param[in] path_in     UTF-8 path of the source, for the log
 * @param[in] fi_in       the source's file info with its handle
 * @param[in] offset_in   offset in the source
 * @param[in] path_out    UTF-8 path of the destination, for the log
 * @param[in] fi_out      the destination's file info with its handle
 * @param[in] offset_out  offset in the destination
 * @param[in] size        the cap on bytes to copy
 * @param[in] flags       copy_file_range(2) flags, which must be 0
 * @returns the bytes copied, or a negative errno: -EINVAL for flags, -EBADF,
 *          or -EOPNOTSUPP for a pseudo-file other than a resource fork
 */
static ssize_t fuse_copy_file_range(const char *path_in,
                                    struct fuse_file_info *fi_in,
                                    off_t offset_in,
                                    const char *path_out,
                                    struct fuse_file_info *fi_out,
                                    off_t offset_out, size_t size, int flags)
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache copy_file_range \"%s\" offset=%llu to \"%s\" "
                   "offset=%llu size=%zu",
                   path_in, (unsigned long long) offset_in, path_out,
                   (unsigned long long) offset_out, size);

    if (flags != 0) {
        return -EINVAL;
    }

    if (!fi_in || !fi_in->fh || !fi_out || !fi_out->fh) {
        return -EBADF;
    }

#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 18)

    /* The reply's count is 32 bits wide, and an older kernel passes cp's
     * whole length: the callback clamps it as libfuse 3.18 does. */
    if (size > 0xfffff000U) {
        size = 0xfffff000U;
    }

#endif
    return ml_copy_range(volume, (struct afp_file_info *) fi_in->fh,
                         offset_in, (struct afp_file_info *) fi_out->fh,
                         offset_out, size);
}
#endif


static int fuse_mkdir(const char *path, mode_t mode)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** mkdir of %s", path);
    ret = ml_mkdir(volume, path, mode);
    return ret;
}

#ifdef __APPLE__
static int fuse_opendir(const char *path, struct fuse_file_info *fi)
{
    struct stat stbuf;
    int ret;
    struct afp_volume *volume = fuse_get_context()->private_data;
    ret = ml_getattr(volume, path, &stbuf);

    if (ret < 0) {
        return ret;
    }

    if (!S_ISDIR(stbuf.st_mode)) {
        return -ENOTDIR;
    }

    if (fi) {
        fi->fh = 0;
    }

    return 0;
}

static int fuse_releasedir(const char *path _U_,
                           struct fuse_file_info *fi _U_)
{
    return 0;
}

static int fuse_fsyncdir(const char *path _U_,
                         int datasync _U_,
                         struct fuse_file_info *fi _U_)
{
    return 0;
}
#endif

/*!
 * @brief Read from the handle's fork for read(2)
 *
 * Reads until size bytes, the end of the fork, or a read that makes no
 * progress. It keeps reading because the kernel treats a short count as the
 * end of the file.
 *
 * @param[in]  path    UTF-8 path of the file, for the log
 * @param[out] buf     receives the bytes
 * @param[in]  size    capacity of buf
 * @param[in]  offset  byte offset to read from
 * @param[in]  fi      the kernel's file info with the handle
 * @returns bytes read, or a negative errno
 */
static int fuse_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    struct afp_file_info *fp;
    struct afp_volume *volume = fuse_get_context()->private_data;
    int eof = 0;
    size_t amount_read = 0;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache read \"%s\" offset=%llu size=%zu",
                   path, (unsigned long long)offset, size);

    if (!fi || !fi->fh) {
        return -EBADF;
    }

    fp = (void *) fi->fh;

    while (amount_read < size) {
        size_t remaining = size - amount_read;
        off_t read_offset = offset + (off_t)amount_read;
        int read_result;
        read_result = ml_read(volume, buf + amount_read, remaining,
                              read_offset, fp, &eof);

        if (read_result < 0) {
            return read_result;
        }

        if (read_result == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "*** read of %s made no progress at offset %llu, eof=%d",
                           path, (unsigned long long)read_offset, eof);
            break;
        }

        amount_read += (size_t)read_result;

        if (eof) {
            break;
        }
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** cache read \"%s\" offset=%llu returned=%zu eof=%d",
                   path, (unsigned long long)offset, amount_read, eof);
    return (int)amount_read;
}

#ifdef __APPLE__
static int fuse_access(const char *path, int mask)
{
    struct stat stbuf;
    struct fuse_context *context = fuse_get_context();
    struct afp_volume *volume = context->private_data;
    int ret;
    ret = ml_getattr(volume, path, &stbuf);

    if (ret < 0) {
        return ret;
    }

    if (mask & ~(AFP_ACCESS_UNKNOWN_BITS | AFP_ACCESS_READ_BITS |
                 AFP_ACCESS_WRITE_BITS | AFP_ACCESS_OWNER_BITS |
                 AFP_ACCESS_EXEC_BITS)) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** access ignoring unknown Darwin mask bits 0x%x",
                       mask & ~(AFP_ACCESS_UNKNOWN_BITS | AFP_ACCESS_READ_BITS |
                                AFP_ACCESS_WRITE_BITS | AFP_ACCESS_OWNER_BITS |
                                AFP_ACCESS_EXEC_BITS));
    }

    /* macFUSE passes KAUTH-style masks. Authorization is ultimately enforced
     * by the AFP operation because server ACLs cannot be reconstructed here. */
    return 0;
}
#endif

#if FUSE_NEW_API
static int fuse_chown(const char *path, uid_t uid, gid_t gid,
                      struct fuse_file_info *fi _U_)
#else
static int fuse_chown(const char *path, uid_t uid, gid_t gid)
#endif
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** chown %s to uid %d, gid %d",
                   path, (int)uid, (int)gid);
    ret = ml_chown(volume, path, uid, gid);

    if (ret == -ENOSYS) {
        log_for_client(NULL, AFPFSD, LOG_WARNING, "chown unsupported on this server");
    }

    return ret;
}

#if FUSE_NEW_API
/*!
 * @brief Truncate a file for truncate(2) and ftruncate(2)
 *
 * With a handle, the truncation goes through its open fork. A file info
 * without one belongs to a create still in progress, whose open applies
 * O_TRUNC. Without a file info, the truncation goes by path.
 *
 * @param[in] path    UTF-8 path of the file
 * @param[in] offset  the new length
 * @param[in] fi      the kernel's file info, or NULL for truncate(2)
 * @returns 0, or a negative errno
 */
static int fuse_truncate(const char *path, off_t offset,
                         struct fuse_file_info *fi)
{
    int ret = 0;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** truncate of %s to %lld, fi=%p, fh=%lu",
                   path, (long long)offset, (void *)fi, fi ? (unsigned long)fi->fh : 0UL);

    /* If we have an open file handle, use it directly instead of
     * opening/closing a new fork */
    if (fi && fi->fh) {
        struct afp_file_info *fp = (struct afp_file_info *) fi->fh;
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** truncate using open forkid %d",
                       fp->forkid);
        ret = ml_ftruncate(volume, fp, offset);
    } else if (fi) {
        /* fi is provided but fh is not set yet (create in progress).
         * The file is being opened, so ll_open will handle O_TRUNC.
         * Don't call ml_truncate as it would close the fork being opened. */
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** truncate with fi but no fh - skipping (will be handled by open)");
        ret = 0;
    } else {
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** truncate calling ml_truncate");
        ret = ml_truncate(volume, path, offset);
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** truncate returning %d", ret);
    return ret;
}

#else
static int fuse_truncate(const char *path, off_t offset)
{
    int ret = 0;
    struct afp_volume * volume = fuse_get_context()->private_data;
    ret = ml_truncate(volume, path, offset);
    return ret;
}

#endif

#if FUSE_USE_VERSION < 30
/*!
 * @brief Set the length of an open file for ftruncate(2)
 *
 * Without this operation libfuse 2 sends ftruncate(2) to truncate(), which
 * opens the file a second time by path.
 *
 * @param[in] path    unused
 * @param[in] offset  the new length
 * @param[in] fi      the kernel's file info, with the handle
 * @returns 0, or a negative errno
 */
static int fuse_ftruncate(const char *path _U_, off_t offset,
                          struct fuse_file_info *fi)
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    return ml_ftruncate(volume, (struct afp_file_info *) fi->fh, offset);
}

#endif


#if FUSE_NEW_API
static int fuse_chmod(const char *path, mode_t mode,
                      struct fuse_file_info *fi _U_)
#else
static int fuse_chmod(const char *path, mode_t mode)
#endif
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    int ret;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "** chmod %s", path);
    ret = ml_chmod(volume, path, mode);

    switch (ret) {
    case -EPERM:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "You're not the owner of this file, cannot change permissions");
        break;

    case -ENOSYS:
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "chmod unsupported or this mode is not possible with this server");
        break;

    case -EFAULT:
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "We don't support these permission bits on this server");
        ret = 0;
        break;

    default:
        break;
    }

    return ret;
}

#if FUSE_USE_VERSION >= 30
#if FUSE_NEW_API
static int fuse_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi _U_)
#else
static int fuse_utimens(const char *path, const struct timespec tv[2])
#endif
{
    int ret = 0;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** utimens \"%s\"", path);
    struct utimbuf timebuf;

    if (!tv) {
        /* NULL tv means set both timestamps to current time */
        time_t now = time(NULL);
        timebuf.actime = now;
        timebuf.modtime = now;
    } else {
        /*
         * AFP only supports modification time, so we only need to check
         * tv[1] (mtime).  If mtime is UTIME_OMIT, there is nothing to do.
         */
        if (tv[1].tv_nsec == UTIME_OMIT) {
            return 0;
        }

        if (tv[1].tv_nsec == UTIME_NOW) {
            timebuf.modtime = time(NULL);
        } else {
            timebuf.modtime = tv[1].tv_sec;
        }

        /* unused by AFP, just initialize */
        timebuf.actime = timebuf.modtime;
    }

    ret = ml_utime(volume, path, &timebuf);

    /* Timestamp failures are non-fatal: same reasoning as fuse_setattr */
    if (ret == -ENOSYS || ret == -EPERM || ret == -ENOENT || ret == -EACCES) {
        if (ret != 0)
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "*** utimens \"%s\" ignored: %d", path, ret);

        return 0;
    } else if (ret < 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "*** utimens \"%s\" failed: %d", path, ret);
        return ret;
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** utimens \"%s\" ok", path);
    return 0;
}

#else
static int fuse_utime(const char *path, struct utimbuf *timebuf)
{
    int ret = 0;
    struct afp_volume *volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "** utime");
    ret = ml_utime(volume, path, timebuf);
    return ret;
}

#endif

static void afp_destroy(void *ignore _U_)
{
    struct afp_volume *volume = fuse_get_context()->private_data;

    if (!volume || !volume->server) {
        return;
    }

    if (volume->mounted == AFP_VOLUME_UNMOUNTED) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Volume %s already unmounted", volume->volume_name_printable);
        return;
    }

    /* If volume is currently unmounting, afp_destroy() was called from within
     * fuse_unmount() (during the FUSE shutdown sequence). In this case, we
     * must NOT call afp_unmount_volume() again as it would create a nested
     * unmount. */
    if (volume->mounted == AFP_VOLUME_UNMOUNTING) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Volume %s is already unmounting - skipping destroy callback",
                       volume->volume_name_printable);
        return;
    }

    /* afp_destroy() is being called for an externally-initiated unmount
     * (e.g., via umount command). The kernel has unmounted the filesystem,
     * so we need to do AFP-side cleanup only. */
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "FUSE destroy callback - external unmount of %s",
                   volume->volume_name_printable);
    volume->priv = NULL;
    afp_unmount_volume(volume);
}

/*!
 * @brief Make a symbolic link at path2 that points at path1
 *
 * @param[in] path1  the link's target
 * @param[in] path2  UTF-8 path of the link
 * @returns 0, or a negative errno
 */
static int fuse_symlink(const char *path1, const char *path2)
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    int ret;
    ret = ml_symlink(volume, path1, path2);

    if (ret == -ENOSYS) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "The server makes no symlinks");
    }

    return ret;
}

#if FUSE_NEW_API
static int fuse_rename(const char *path_from, const char *path_to,
                       unsigned int flags)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** rename %s -> %s flags=0x%x", path_from, path_to, flags);
#ifdef __APPLE__
    struct stat stbuf;

    if (flags & RENAME_SWAP) {
        ret = ml_exchange(volume, path_from, path_to);
        return ret;
    }

    if (flags & RENAME_EXCL) {
        ret = ml_getattr(volume, path_to, &stbuf);

        if (ret == 0) {
            return -EEXIST;
        }

        if (ret != -ENOENT) {
            return ret;
        }
    }

#else

    if (flags != 0) {
        return -EINVAL;
    }

#endif
    ret = ml_rename(volume, path_from, path_to);
    return ret;
}

#else
static int fuse_rename(const char *path_from, const char *path_to)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** rename %s -> %s", path_from, path_to);
    ret = ml_rename(volume, path_from, path_to);
    return ret;
}

#endif

#if defined(__APPLE__) && FUSE_USE_VERSION >= 30 && !FUSE_NEW_API
static int fuse_renamex(const char *path_from, const char *path_to,
                        unsigned int flags)
{
    int ret;
    struct stat stbuf;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** renamex %s -> %s flags=0x%x", path_from, path_to, flags);

    if (flags & RENAME_SWAP) {
        ret = ml_exchange(volume, path_from, path_to);
        return ret;
    }

    if (flags & RENAME_EXCL) {
        ret = ml_getattr(volume, path_to, &stbuf);

        if (ret == 0) {
            return -EEXIST;
        }

        if (ret != -ENOENT) {
            return ret;
        }
    }

    ret = ml_rename(volume, path_from, path_to);
    return ret;
}

static int fuse_exchange(const char *path_from, const char *path_to,
                         unsigned long options)
{
    int ret;
    struct afp_volume * volume = fuse_get_context()->private_data;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** exchange %s <-> %s options=0x%lx",
                   path_from, path_to, options);
    ret = ml_exchange(volume, path_from, path_to);
    return ret;
}
#endif

#ifdef __APPLE__
#if FUSE_USE_VERSION >= 30
static int fuse_statfs(const char *path, struct statfs *stat)
#else
static int fuse_statfs(const char *path, struct statvfs *stat)
#endif
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    int ret;
    struct statvfs vfsstat;
    ret = ml_statfs(volume, path, &vfsstat);

    if (ret == 0) {
        /* Convert statvfs to statfs for macOS */
        stat->f_bsize = vfsstat.f_bsize;
        stat->f_blocks = vfsstat.f_blocks;
        stat->f_bfree = vfsstat.f_bfree;
        stat->f_bavail = vfsstat.f_bavail;
        /* AFP has no inode counts, but macOS copy preflight treats zero free
         * files as a hard destination limit. Keep this workaround local. */
        stat->f_files = vfsstat.f_files ? vfsstat.f_files
                        : (vfsstat.f_blocks ? vfsstat.f_blocks : 1);
        stat->f_ffree = vfsstat.f_ffree ? vfsstat.f_ffree
                        : (vfsstat.f_bfree < stat->f_files
                           ? vfsstat.f_bfree : stat->f_files);
    }

    return ret;
}

#else
static int fuse_statfs(const char *path, struct statvfs *stat)
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    int ret;
    ret = ml_statfs(volume, path, stat);
    return ret;
}

#endif


#if defined(__APPLE__) && FUSE_USE_VERSION >= 30

/* FUSE_SET_ATTR_* bitmask constants (from fuse3/fuse_lowlevel.h) */
#ifndef FUSE_SET_ATTR_MODE
#define FUSE_SET_ATTR_MODE      (1 << 0)
#define FUSE_SET_ATTR_UID       (1 << 1)
#define FUSE_SET_ATTR_GID       (1 << 2)
#define FUSE_SET_ATTR_SIZE      (1 << 3)
#define FUSE_SET_ATTR_ATIME     (1 << 4)
#define FUSE_SET_ATTR_MTIME     (1 << 5)
#define FUSE_SET_ATTR_ATIME_NOW (1 << 7)
#define FUSE_SET_ATTR_MTIME_NOW (1 << 8)
#define FUSE_SET_ATTR_BTIME     (1 << 28)
#endif
#ifndef FUSE_SET_ATTR_FLAGS
#define FUSE_SET_ATTR_FLAGS     (1U << 31)
#endif

static int fuse_apply_chflags(struct afp_volume *volume, const char *path,
                              unsigned int flags)
{
    char finderinfo[32] = {0};
    uint16_t finder_flags;
    int ret;
    ret = ml_getfinderinfo(volume, path, finderinfo, sizeof(finderinfo));

    if (ret < 0 && ret != -ENOATTR && ret != -ENOENT) {
        return ret;
    }

    finder_flags = finderinfo_get_flags(finderinfo);

    if (flags & UF_HIDDEN) {
        finder_flags |= AFP_FINDERINFO_FLAG_INVISIBLE;
    } else {
        finder_flags &= ~AFP_FINDERINFO_FLAG_INVISIBLE;
    }

    finderinfo_set_flags(finderinfo, finder_flags);
    return ml_setfinderinfo(volume, path, finderinfo, sizeof(finderinfo));
}

/*!
 * @brief Set a file's attributes for macFUSE's combined setattr
 *
 * From FUSE_USE_VERSION 30, macFUSE sends every setattrlist() here. Without
 * it, setattrlist() fails with ENOSYS and copyfile() aborts before writing.
 * The creation and backup times stay the server's.
 *
 * @param[in] path    UTF-8 path of the file
 * @param[in] attr    the attributes to set
 * @param[in] to_set  which attributes of attr to set
 * @param[in] fi      the kernel's file info, or NULL
 * @returns 0, or a negative errno
 */
static int fuse_setattr(const char *path, struct fuse_darwin_attr *attr,
                        int to_set, struct fuse_file_info *fi)
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    unsigned int which;
    int ret = 0;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** setattr \"%s\" to_set=0x%x", path, to_set);

    if (to_set & FUSE_SET_ATTR_FLAGS) {
        ret = fuse_apply_chflags(volume, path, attr->flags);

        if (ret < 0) {
            return ret;
        }
    }

    /* A truncation moves the server's time, and a mode without the owner's
     * write permission would refuse the open a truncation by path makes. */
    if (to_set & FUSE_SET_ATTR_SIZE) {
        if (fi && fi->fh) {
            ret = ml_ftruncate(volume, (struct afp_file_info *) fi->fh,
                               attr->size);
        } else {
            ret = ml_truncate(volume, path, attr->size);
        }

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "*** setattr truncate \"%s\" failed: %d", path, ret);
            return ret;
        }
    }

    which = ((to_set & FUSE_SET_ATTR_MODE) ? ML_SET_MODE : 0)
            | ((to_set & FUSE_SET_ATTR_UID) ? ML_SET_UID : 0)
            | ((to_set & FUSE_SET_ATTR_GID) ? ML_SET_GID : 0)
            | ((to_set & (FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_MTIME_NOW))
               ? ML_SET_MTIME : 0);

    if (which) {
        time_t mtime = (to_set & FUSE_SET_ATTR_MTIME_NOW)
                       ? time(NULL) : (time_t) attr->mtimespec.tv_sec;
        ret = ml_setattr(volume, path, which, attr->mode, attr->uid,
                         attr->gid, mtime);

        /* A safe-save goes on past a change refused or unsupported, and past
         * kFPObjectNotFound, which a catalog update gives for a moment. */
        if (ret == -ENOSYS || ret == -EPERM || ret == -EACCES
                || ret == -ENOENT) {
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "*** setattr \"%s\" ignored: %d", path, ret);
        } else if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "*** setattr \"%s\" failed: %d", path, ret);
            return ret;
        }
    }

    return 0;
}

static int fuse_getattr_darwin(const char *path, struct fuse_darwin_attr *attr,
                               struct fuse_file_info *fi _U_)
{
    struct stat stbuf;
    char *normalized_path;
    struct afp_volume * volume = fuse_get_context()->private_data;
    int ret;

    if (!path) {
        return -EIO;
    }

    ret = strip_null_path_component(&path, &normalized_path);

    if (ret < 0) {
        return ret;
    }

    ret = ml_getattr(volume, path, &stbuf);

    if (ret == 0) {
        char finderinfo[32];
        stat_to_darwin_attr(&stbuf, attr);

        if (ml_getfinderinfo(volume, path, finderinfo, sizeof(finderinfo)) == 32
                && (finderinfo_get_flags(finderinfo)
                    & AFP_FINDERINFO_FLAG_INVISIBLE)) {
            attr->flags |= UF_HIDDEN;
        }

        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** getattr \"%s\" -> mode=0%o uid=%d gid=%d size=%lld ino=%llu mtime=%lld.%09ld",
                       path, stbuf.st_mode, stbuf.st_uid, stbuf.st_gid,
                       (long long)stbuf.st_size,
                       (unsigned long long)stbuf.st_ino,
                       (long long)stbuf.st_mtimespec.tv_sec,
                       stbuf.st_mtimespec.tv_nsec);
    } else {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** getattr \"%s\" -> error %d", path, ret);
    }

    free(normalized_path);
    return ret;
}
#else
#if FUSE_NEW_API
static int fuse_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi _U_)
#else
static int fuse_getattr(const char *path, struct stat *stbuf)
#endif
{
    struct afp_volume * volume = fuse_get_context()->private_data;
    char *normalized_path;
    int ret;
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** getattr of \"%s\"", path);

    if (!path) {
        return -EIO;
    }

    ret = strip_null_path_component(&path, &normalized_path);

    if (ret < 0) {
        return ret;
    }

    ret = ml_getattr(volume, path, stbuf);
    free(normalized_path);
    return ret;
}

#endif


#if FUSE_NEW_API
/*!
 * @brief Set up the mount at FUSE init
 *
 * Keeps FUSE writes within the server's quantum, asks for macFUSE's
 * capabilities on macOS, and tells the mount's daemon the mount started.
 *
 * @param[in,out] conn  the connection's capabilities and limits
 * @param[in]     cfg   the mount's configuration, for the log
 * @returns the volume, which FUSE keeps as the mount's private data
 */
static void *afp_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    struct afp_volume * vol = fuse_get_context()->private_data;
    struct fuse_context *ctx = fuse_get_context();

    /* Keep each FUSE write within the server's negotiated DSI request
     * quantum.  This gives FUSE 3 a useful large-write size while avoiding
     * an avoidable split and extra AFP write round trip in ll_write(). */
    if (conn && vol->server->quantum
            && conn->max_write > vol->server->quantum) {
        conn->max_write = vol->server->quantum;
    }

#ifdef __APPLE__

    if (conn) {
#ifdef FUSE_CAP_RENAME_SWAP
        conn->want |= FUSE_CAP_RENAME_SWAP;
#endif
#ifdef FUSE_CAP_RENAME_EXCL
        conn->want |= FUSE_CAP_RENAME_EXCL;
#endif
#ifdef FUSE_CAP_EXCHANGE_DATA
        conn->want |= FUSE_CAP_EXCHANGE_DATA;
#endif
#ifdef FUSE_CAP_ACCESS_EXTENDED
        conn->want |= FUSE_CAP_ACCESS_EXTENDED;
#endif
#ifdef FUSE_DARWIN_CAP_ACCESS_EXT
        fuse_darwin_set_feature_flag(conn, FUSE_DARWIN_CAP_ACCESS_EXT);
#endif
    }

#else
    (void) conn;
#endif
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** fuse init entry_timeout=%.3f negative_timeout=%.3f attr_timeout=%.3f capable=0x%x want=0x%x",
                   cfg ? cfg->entry_timeout : -1.0,
                   cfg ? cfg->negative_timeout : -1.0,
                   cfg ? cfg->attr_timeout : -1.0,
                   conn ? conn->capable : 0,
                   conn ? conn->want : 0);

    if (ctx && ctx->fuse) {
        vol->priv = ctx->fuse;
    } else {
        vol->priv = NULL;
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "FUSE handle not available in context for %s",
                       vol->volume_name_printable);
    }

    /* Trigger the daemon that we've started */
    vol->mounted = 1;
    pthread_cond_signal(&vol->startup_condition_cond);
    return (void *) vol;
}

#else
static void *afp_init(struct fuse_conn_info * o _U_)
{
    struct afp_volume * vol = fuse_get_context()->private_data;
    vol->priv = fuse_get_context()->fuse;

    /* Trigger the daemon that we've started */
    if (vol->priv) {
        vol->mounted = 1;
    }

    pthread_cond_signal(&vol->startup_condition_cond);
    return (void *) vol;
}

#endif

#if defined(__APPLE__) && FUSE_USE_VERSION >= 30
static int fuse_chflags(const char *path,
                        struct fuse_file_info *fi _U_,
                        unsigned int flags)
{
    struct afp_volume *volume = fuse_get_context()->private_data;
    return fuse_apply_chflags(volume, path, flags);
}

#endif

static struct fuse_operations afp_oper = {
#if defined(__APPLE__) && FUSE_USE_VERSION >= 30
    .getattr    = fuse_getattr_darwin,
    .setattr    = fuse_setattr,
    .readdir    = fuse_readdir_darwin,
    .chflags    = fuse_chflags,
#if !FUSE_NEW_API
    .renamex    = fuse_renamex,
    .exchange   = fuse_exchange,
#endif
#else
    .getattr    = fuse_getattr,
    .readdir    = fuse_readdir,
#endif
    .open       = fuse_open,
    .read       = fuse_read,
    .mkdir      = fuse_mkdir,
#ifdef __APPLE__
    .opendir    = fuse_opendir,
    .releasedir = fuse_releasedir,
    .fsyncdir   = fuse_fsyncdir,
#endif
    .readlink   = fuse_readlink,
    .rmdir      = fuse_rmdir,
    .unlink     = fuse_unlink,
    .mknod      = fuse_mknod,
    .create     = fuse_create,
    .write      = fuse_write,
#ifdef AFP_FUSE_COPY_FILE_RANGE
    .copy_file_range = fuse_copy_file_range,
#endif
    .fsync      = fuse_fsync,
    .release    = fuse_release,
    .getxattr   = fuse_getxattr,
    .setxattr   = fuse_setxattr,
    .listxattr  = fuse_listxattr,
    .removexattr = fuse_removexattr,
    .chmod      = fuse_chmod,
    .symlink    = fuse_symlink,
    .chown      = fuse_chown,
#ifdef __APPLE__
    .access     = fuse_access,
#endif
    .truncate   = fuse_truncate,
#if FUSE_USE_VERSION < 30
    .ftruncate  = fuse_ftruncate,
#endif
    .rename     = fuse_rename,
#if FUSE_USE_VERSION >= 30
    .utimens    = fuse_utimens,
#else
    .utime      = fuse_utime,
#endif
    .destroy    = afp_destroy,
    .init       = afp_init,
    .statfs     = fuse_statfs,
};


int afp_fuse_main(int argc, char *argv[], struct afp_volume *volume)
{
    int ret;
    struct fuse_operations oper = afp_oper;
    /* FinderInfo and ResourceFork use the xattr callbacks on macOS even
     * when the AFP volume does not support generic extended attributes. */
#ifndef __APPLE__

    if (!(volume->attributes & kSupportsExtAttrs)) {
        oper.getxattr    = NULL;
        oper.setxattr    = NULL;
        oper.listxattr   = NULL;
        oper.removexattr = NULL;
    }

#endif
    fuse_capture_stderr_start();
    ret = fuse_main(argc, argv, &oper, (void *) volume);
    return ret;
}
