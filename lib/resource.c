/*
 *  resource.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The .AppleDouble pseudo-files that expose a file's metadata
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "compat.h"
#include "did.h"
#include "dsi.h"
#include "forklist.h"
#include "lowlevel.h"
#include "midlevel.h"
#include "resource.h"

#define appledouble ".AppleDouble"
#define finderinfo_string ".finderinfo"
#define comment_string ".comment"
/* The longest Finder comment AFP stores */
#define AFP_COMMENT_MAX 199
#define servericon_string "/.servericon"

#define min(a,b) (((a)<(b)) ? (a) : (b))

/*!
 * @brief Find the pseudo-file of the .AppleDouble view a path names
 *
 * Only a volume that shows AppleDouble files has them. A path through a
 * .AppleDouble directory names the file or the directory beside it, whose
 * path the caller receives and frees.
 *
 * @param[in]  volume     volume of the path
 * @param[in]  path       UTF-8 path
 * @param[out] newpath_p  receives the path the pseudo-file stands for, or
 *                        NULL
 * @returns an AFP_META_* type, 0 for a path that names no pseudo-file, or
 *          -ENOMEM
 */
static int extra_translate(
    struct afp_volume * volume, const char *path,
    char **newpath_p)
{
    const char *p;
    char *newpath;
    *newpath_p = NULL;

    /* No path, as for a stream whose folder is gone, is no pseudo-file. */
    if (path == NULL
            || (~volume->extra_flags & VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE)) {
        return 0;
    }

    if (strcmp(path, servericon_string) == 0) {
        return AFP_META_SERVER_ICON;
    }

    if (strlen(path) < (strlen(appledouble) +1)) {
        return 0;
    }

    if ((p = strrchr(path, '/')) == NULL) {
        return 0;
    }

    p += 1;

    if (strcmp(p, appledouble) == 0) {
        /* Okay, all we have is /foo/.AppleDouble */
        /* Make a copy, but not the last .AppleDouble */
        newpath = malloc(strlen(path));

        if (newpath == NULL) {
            return -ENOMEM;
        }

        memset(newpath, 0, strlen(path));

        if (strlen(path) == strlen(appledouble) +1) {
            newpath[0] = '/';
        } else {
            memcpy(newpath, path, strlen(path) - (strlen(appledouble) +1));
        }

        *newpath_p = newpath;
        return AFP_META_APPLEDOUBLE;
    }

    /* The directory is a whole component, "/.AppleDouble/", not a name
     * that ends in it; the first such component counts. */
    const char *component;

    for (component = strstr(path, appledouble); component;
            component = strstr(component + 1, appledouble)) {
        if (component > path && component[-1] == '/'
                && component[strlen(appledouble)] == '/') {
            break;
        }
    }

    if (component) {
        /* So we have /foo/.AppleDouble/blah: the copy is /foo/blah. */
        size_t prefix_len = (size_t)(component - path);
        const char *rest = component + strlen(appledouble) + 1;
        newpath = malloc(prefix_len + strlen(rest) + 1);

        if (newpath == NULL) {
            return -ENOMEM;
        }

        memcpy(newpath, path, prefix_len);
        memcpy(newpath + prefix_len, rest, strlen(rest) + 1);
        *newpath_p = newpath;

        /* If the ending is .finderinfo, tag it */

        if (strlen(newpath) > strlen(finderinfo_string)) {
            char *suffix = newpath + strlen(newpath) - strlen(finderinfo_string);

            if (strcmp(suffix, finderinfo_string) == 0) {
                *suffix = '\0';
                return AFP_META_FINDERINFO;
            }
        }

        if (strlen(newpath) > strlen(comment_string)) {
            char *suffix = newpath + strlen(newpath) - strlen(comment_string);

            if (strcmp(suffix, comment_string) == 0) {
                *suffix = '\0';
                return AFP_META_COMMENT;
            }
        }

        return AFP_META_RESOURCE;
    }

    return 0;
}

/*!
 * @brief Whether the AppleDouble view answers for a name in a directory
 *
 * The view's folder in every directory and the server icon in the root
 * stand in for an object of that name the server lists.
 *
 * @param[in] volume  volume of the directory
 * @param[in] dir     path of the directory
 * @param[in] name    a name listed in it
 * @returns 1 when the view answers for the name, 0 otherwise
 */
int appledouble_shadows(const struct afp_volume *volume, const char *dir,
                        const char *name)
{
    if (~volume->extra_flags & VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE) {
        return 0;
    }

    return strcmp(name, appledouble) == 0
           || (strcmp(dir, "/") == 0
               && strcmp(name, &servericon_string[1]) == 0);
}

/*!
 * @brief Open the volume's Desktop database once
 *
 * Apple's servers from OS X 10.6 have none; one refusal stands for the volume.
 *
 * @param[in,out] volume  the volume, which keeps the reference
 * @returns 0, -ENOENT for a server without the database, or -EIO
 */
static int ensure_dt_opened(struct afp_volume * volume)
{
    int rc;

    if (volume->dtrefnum > 0) {
        return 0;
    }

    if (volume->no_desktop) {
        return -ENOENT;
    }

    rc = afp_opendt(volume, &volume->dtrefnum);

    if (rc == kFPCallNotSupported) {
        volume->no_desktop = 1;
        return -ENOENT;
    }

    return rc == kFPNoErr ? 0 : -EIO;
}

/*!
 * @brief Read a file's Finder comment
 *
 * @param[in]  volume    volume of the file
 * @param[in]  basename  its AFP name in did
 * @param[in]  did       its directory
 * @param[out] buf       receives the comment, or NULL
 * @param[in]  size      capacity of buf
 * @returns the comment's length, 0 for a file without one, at most size when
 *          buf is given, or a negative errno: -ENOENT for a file that is gone
 *          or a server without a Desktop database
 */
static int read_comment(struct afp_volume * volume, const char *basename,
                        unsigned int did, char *buf, size_t size)
{
    char data[UINT8_MAX];
    struct afp_comment comment;
    int ret = ensure_dt_opened(volume);

    if (ret != 0) {
        return ret;
    }

    comment.data = data;
    comment.maxsize = sizeof(data);
    comment.size = 0;

    switch (afp_getcomment(volume, did, basename, &comment)) {
    /* A file without a comment has an empty one. */
    case kFPNoErr:
    case kFPItemNotFound:
        break;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    default:
        return -EIO;
    }

    if (buf) {
        size = min(size, comment.size);
        memcpy(buf, data, size);
        return (int) size;
    }

    return (int) comment.size;
}

/*!
 * @brief Give a file's Finder comment a length
 *
 * The comment keeps its bytes up to the length and gains NULs past its end,
 * of which Netatalk reads back the first 128 alone.
 *
 * @param[in] volume    volume of the file
 * @param[in] basename  its AFP name in did
 * @param[in] did       its directory
 * @param[in] length    the length, AFP_COMMENT_MAX at most
 * @returns 0, or a negative errno: -EFBIG for a length past AFP_COMMENT_MAX
 */
static int set_comment_length(struct afp_volume * volume, const char *basename,
                              unsigned int did, off_t length)
{
    char comment[AFP_COMMENT_MAX];
    int len = 0;
    int ret;

    if (length < 0) {
        return -EINVAL;
    }

    if (length > AFP_COMMENT_MAX) {
        return -EFBIG;
    }

    ret = ensure_dt_opened(volume);

    if (ret != 0) {
        return ret;
    }

    if (length > 0) {
        len = read_comment(volume, basename, did, comment, sizeof(comment));

        if (len < 0) {
            return len;
        }

        if (len < length) {
            memset(comment + len, 0, (size_t)(length - len));
        }
    }

    ret = afp_addcomment_sized(volume, did, basename, comment, (size_t) length);
    return -ll_answer_errno(ret);
}

/*!
 * @brief Create the AppleDouble pseudo-file at path
 *
 * A resource fork, Finder information or comment comes with its file, so the
 * call makes no request.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @param[in] mode    unused
 * @returns 1 for a resource fork, Finder information or a comment, 0 for
 *          another path, or a negative errno: -EBADF for the .AppleDouble
 *          directory, -EPERM for the server icon
 */
int appledouble_creat(struct afp_volume * volume, const char *path,
                      mode_t mode _U_)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case AFP_META_RESOURCE:
    case AFP_META_FINDERINFO:
    case AFP_META_COMMENT:
        free(newpath);
        return 1;

    case AFP_META_APPLEDOUBLE:
        free(newpath);
        return -EBADF;

    case AFP_META_SERVER_ICON:
        return -EPERM;

    default:
        return 0;
    }
}

/*!
 * @brief Refuse a change of mode, owner or time of an AppleDouble pseudo-file
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_setattr(struct afp_volume * volume, const char *path)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Refuse the deletion of an AppleDouble pseudo-file
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_unlink(struct afp_volume * volume, const char *path)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Close an AppleDouble pseudo-file
 *
 * Flushes a written resource fork first, on a flush_on_close mount or with
 * FPFlushFork on an Apple server. The fork then closes and leaves the
 * volume's open-fork list.
 *
 * @param[in]     volume  volume of the file
 * @param[in,out] fp      the open pseudo-file
 * @returns 0 or the flush's result, -EIO when the close fails, or -EBADF for
 *          the .AppleDouble directory
 */
int appledouble_close(struct afp_volume * volume, struct afp_file_info * fp)
{
    int ret;
    int rc;

    switch (fp->resource) {
    case AFP_META_RESOURCE:
        ret = ml_close_flush(volume, fp);

        if (ret == 0 && fp->dirty && afp_server_is_apple(volume->server)
                && afp_flushfork(volume, fp->forkid,
                                 DSI_DEFAULT_TIMEOUT) != kFPNoErr) {
            ret = -EIO;
        }

        rc = afp_closefork(volume, fp->forkid);
        remove_opened_fork(volume, fp);

        /* Netatalk dates a written resource fork at its close. */
        if (fp->dirty) {
            ll_fork_changed(volume, fp);
        }

        return rc == kFPNoErr ? ret : -EIO;

    case AFP_META_APPLEDOUBLE:
        return -EBADF;

    case AFP_META_SERVER_ICON:
        return 0;

    case AFP_META_FINDERINFO:
        return 0;
    }

    return 0;
}

/*!
 * @brief Write to an AppleDouble pseudo-file
 *
 * A write changes the bytes it covers of Finder information's 32 and a
 * comment's AFP_COMMENT_MAX, past whose first 128 Netatalk reads back none.
 *
 * @param[in]     volume        volume of the file
 * @param[in,out] fp            the pseudo-file's open record
 * @param[in]     data          bytes to write
 * @param[in]     size          length of data
 * @param[in]     offset        byte offset to write at
 * @param[out]    totalwritten  bytes written
 * @returns 1 when fp is a pseudo-file's, 0 for a data fork, or a negative
 *          errno
 */
int appledouble_write(struct afp_volume * volume, struct afp_file_info *fp,
                      const char *data, size_t size, off_t offset, size_t *totalwritten)
{
    int ret;
    int towrite = size;
    int keeps_time;
    unsigned short bitmap;
    struct afp_dirent fp2;

    switch (fp->resource) {
    case AFP_META_RESOURCE:
        ret = ll_write(volume, data, size, offset, 0, fp, totalwritten);

        if (ret < 0) {
            return ret;
        }

        return 1;

    case AFP_META_APPLEDOUBLE:
        return -EBADF;

    case AFP_META_FINDERINFO:
        if (offset < 0 || offset >= 32) {
            return -EINVAL;
        }

        if (towrite > (32 - offset)) {
            towrite = 32 - offset;
        }

        /* The Finder info so far, and the time, which afpd stamps its clock
         * over in a set without one. */
        bitmap = kFPFinderInfoBit | kFPModDateBit
                 | ((volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX)
                    ? kFPUnixPrivsBit : 0);
        memset(&fp2, 0, sizeof(fp2));
        ret = afp_getfiledirparms(volume, fp->did, bitmap, bitmap,
                                  fp->basename, &fp2);

        if (ret != kFPNoErr) {
            return -ll_answer_errno(ret);
        }

        /* afpd makes a file a symlink when its Finder information names one,
         * and then sets the time on the target. */
        keeps_time = ml_carries_time(&fp2);
        memcpy(fp2.finderinfo + offset, data, towrite);
        keeps_time = keeps_time
                     && memcmp(fp2.finderinfo, ML_SYMLINK_FINDERINFO, 8) != 0;
        ret = afp_setfiledirparms(volume, fp->did, fp->basename,
                                  kFPFinderInfoBit
                                  | (keeps_time ? kFPModDateBit : 0), &fp2);
        ll_fork_changed(volume, fp);

        /* The link afpd puts in the file's place dates the folder. */
        if (memcmp(fp2.finderinfo, ML_SYMLINK_FINDERINFO, 8) == 0) {
            dir_cache_forget_dir(volume, fp->did);
        }

        if (ret != kFPNoErr) {
            return -ll_answer_errno(ret);
        }

        *totalwritten = towrite;
        return 1;

    case AFP_META_COMMENT: {
        char comment[AFP_COMMENT_MAX];
        int len;

        if (offset < 0) {
            return -EINVAL;
        }

        if (offset >= AFP_COMMENT_MAX) {
            return -EFBIG;
        }

        if (size == 0) {
            *totalwritten = 0;
            return 1;
        }

        len = read_comment(volume, fp->basename, fp->did, comment,
                           sizeof(comment));

        if (len < 0) {
            return len;
        }

        towrite = (int) min(size, (size_t)(AFP_COMMENT_MAX - offset));

        if (offset > len) {
            memset(comment + len, 0, (size_t)(offset - len));
        }

        memcpy(comment + offset, data, (size_t) towrite);

        if (offset + towrite > len) {
            len = (int) offset + towrite;
        }

        ret = afp_addcomment_sized(volume, fp->did, fp->basename, comment,
                                   (size_t) len);

        if (ret != kFPNoErr) {
            return -ll_answer_errno(ret);
        }

        *totalwritten = (size_t) towrite;
        return 1;
    }

    case AFP_META_SERVER_ICON:
        return -EPERM;
    }

    return 0;
}

/*!
 * @brief Read an AppleDouble pseudo-file
 *
 * @param[in]     volume       volume of the file
 * @param[in,out] fp           the open pseudo-file
 * @param[out]    buf          receives the bytes
 * @param[in]     size         capacity of buf
 * @param[in]     offset       byte offset to read from
 * @param[out]    amount_read  bytes read
 * @param[out]    eof          set to 1 when the read reaches the end
 * @returns 1 when the read is a pseudo-file's, 0 for another file, or a
 *          negative errno
 */
int appledouble_read(struct afp_volume * volume, struct afp_file_info *fp,
                     char *buf, size_t size, off_t offset, size_t *amount_read,
                     int *eof)
{
    int tocopy;
    int ret;
    struct afp_dirent info;
    *amount_read = 0;
    *eof = 0;

    switch (fp->resource) {
    case AFP_META_RESOURCE:
        ret = ll_read(volume, buf, size, offset, fp, eof);

        if (ret < 0) {
            return ret;
        }

        *amount_read = ret;
        return 1;

    case AFP_META_APPLEDOUBLE:
        return -EBADF;

    case AFP_META_FINDERINFO:
        if (offset < 0 || offset > 32) {
            return -EFAULT;
        }

        ret = afp_getfiledirparms(volume, fp->did, kFPFinderInfoBit,
                                  kFPFinderInfoBit, fp->basename, &info);

        if (ret != kFPNoErr) {
            return -ll_answer_errno(ret);
        }

        tocopy = min(size, (32 - (unsigned long) offset));
        memcpy(buf, info.finderinfo + offset, tocopy);

        if (offset + tocopy == 32) {
            *eof = 1;
        }

        *amount_read = tocopy;
        return 1;

    case AFP_META_COMMENT: {
        char comment[AFP_COMMENT_MAX];
        int len = read_comment(volume, fp->basename, fp->did, comment,
                               sizeof(comment));

        if (len < 0) {
            return len;
        }

        if (offset < 0) {
            return -EINVAL;
        }

        tocopy = 0;

        if (offset < len) {
            tocopy = (int) min(size, (size_t)(len - offset));
            memcpy(buf, comment + offset, (size_t) tocopy);
        }

        *amount_read = (size_t) tocopy;
        *eof = offset + tocopy >= len;
        return 1;
    }

    case AFP_META_SERVER_ICON:
        if (offset < 0 || offset > AFP_SERVER_ICON_LEN) {
            return -EFAULT;
        }

        tocopy = min(size, (AFP_SERVER_ICON_LEN - (unsigned long) offset));
        memcpy(buf, volume->server->icon + offset, tocopy);
        *eof = 1;
        *amount_read = tocopy;
        return 1;
    }

    return 0;
}

/*!
 * @brief Truncate the AppleDouble pseudo-file at path
 *
 * Sets a resource fork's length. Finder information accepts the call
 * without a request, and a comment's length goes through its open handle.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the pseudo-file
 * @param[in] offset  the new length
 * @returns 1 for a resource fork or Finder information, 0 for another file
 *          or a comment, or a negative errno
 */
int appledouble_truncate(struct afp_volume * volume, const char *path,
                         off_t offset)
{
    char *newpath;
    int resource = extra_translate(volume, path, &newpath);
    int ret;

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case AFP_META_RESOURCE:
        ret = ml_truncateresourcefork(volume, newpath, (uint64_t) offset);
        free(newpath);
        return ret < 0 ? ret : 1;

    case AFP_META_APPLEDOUBLE:
        free(newpath);
        return -EISDIR;

    case AFP_META_FINDERINFO:
        free(newpath);
        return 1;

    case AFP_META_SERVER_ICON:
        free(newpath);
        return -EPERM;
    }

    free(newpath);
    return 0;
}

/*!
 * @brief Set the length of an open AppleDouble pseudo-file
 *
 * Finder information keeps its 32 bytes without a request, and a resource
 * fork's length is the caller's to set.
 *
 * @param[in] volume  volume of the file
 * @param[in] fp      the open pseudo-file
 * @param[in] offset  the new length
 * @returns 1 for a pseudo-file other than a resource fork, 0 for a fork, or
 *          a negative errno: -EPERM for the server icon
 */
int appledouble_ftruncate(struct afp_volume * volume,
                          struct afp_file_info * fp, off_t offset)
{
    int ret;

    switch (fp->resource) {
    case AFP_META_FINDERINFO:
        return 1;

    case AFP_META_COMMENT:
        ret = set_comment_length(volume, fp->basename, fp->did, offset);
        return ret < 0 ? ret : 1;

    case AFP_META_SERVER_ICON:
        return -EPERM;

    default:
        return 0;
    }
}

/*!
 * @brief Open the AppleDouble pseudo-file at path
 *
 * A resource fork opens as its file's fork and O_TRUNC empties a comment;
 * Finder information and the server icon open without a request of their own.
 *
 * @param[in]     volume  volume of the file
 * @param[in]     path    UTF-8 path
 * @param[in]     flags   open(2) flags
 * @param[in,out] fp      the record, which receives the pseudo-file's type
 * @returns 1 when path is a pseudo-file's and it opened, 0 for another
 *          path, or a negative errno: -EISDIR for the .AppleDouble directory,
 *          -EPERM for a write or O_TRUNC open of the server icon
 */
int appledouble_open(struct afp_volume * volume, const char *path, int flags,
                     struct afp_file_info *fp)
{
    char *newpath;
    int ret = 0;
    int resource = extra_translate(volume, path, &newpath);

    if (resource < 0) {
        return resource;
    }

    switch ((fp->resource = resource)) {
    case AFP_META_RESOURCE:
        ret = ml_resolve_fork(volume, newpath, fp);

        if (ret < 0) {
            goto out;
        }

        ret = ll_open(volume, newpath, flags, fp);

        if (ret < 0) {
            goto out;
        }

        ret = 1;
        goto out;

    case AFP_META_APPLEDOUBLE:
        ret = -EISDIR;
        goto out;

    case AFP_META_FINDERINFO:
        ret = ml_resolve_fork(volume, newpath, fp);

        if (ret < 0) {
            goto out;
        }

        ret = 1;
        goto out;

    case AFP_META_COMMENT:
        ret = ml_resolve_fork(volume, newpath, fp);

        if (ret < 0) {
            goto out;
        }

        ret = ensure_dt_opened(volume);

        if (ret != 0) {
            goto out;
        }

        if (flags & O_TRUNC) {
            ret = set_comment_length(volume, fp->basename, fp->did, 0);

            if (ret < 0) {
                goto out;
            }
        }

        ret = 1;
        goto out;

    case AFP_META_SERVER_ICON:
        ret = (flags & (O_ACCMODE | O_TRUNC)) == O_RDONLY ? 1 : -EPERM;
        goto out;

    default:
        break;
    }

out:
    free(newpath);
    return ret;
}

/*!
 * @brief Open a file's resource fork or Finder information by its own path
 *
 * Finder information opens without a request of its own.
 *
 * @param[in]  volume    volume of the file
 * @param[in]  path      UTF-8 path of the file
 * @param[in]  resource  AFP_META_RESOURCE or AFP_META_FINDERINFO
 * @param[in]  flags     open(2) flags, for a resource fork
 * @param[out] fp        receives the open record
 * @returns 0, or a negative errno: the lookup's, the open's, or -EINVAL
 */
int appledouble_open_meta(struct afp_volume * volume, const char *path,
                          unsigned int resource, int flags,
                          struct afp_file_info *fp)
{
    int ret;

    if (!volume || !path || !fp) {
        return -EINVAL;
    }

    memset(fp, 0, sizeof(*fp));
    fp->resource = resource;

    switch (resource) {
    case AFP_META_RESOURCE:
        ret = ml_resolve_fork(volume, path, fp);
        return ret < 0 ? ret : ll_open(volume, path, flags, fp);

    case AFP_META_FINDERINFO:
        return ml_resolve_fork(volume, path, fp);

    default:
        return -EINVAL;
    }
}

/*!
 * @brief Read the attributes of the pseudo-file at path
 *
 * A pseudo-file takes its file's record, the cache's while it holds, with
 * its own kind and size; a held resource fork is measured on the server.
 *
 * @param[in]  volume  volume of the path
 * @param[in]  path    UTF-8 path
 * @param[out] stbuf   receives the attributes
 * @returns 1 when path is a pseudo-file's, 0 for another path, or a
 *          negative errno
 */
int appledouble_getattr(struct afp_volume * volume,
                        const char *path, struct stat *stbuf)
{
    int resource;
    char *newpath;
    struct ml_target t;
    struct afp_dirent e;
    int ret;
    resource = extra_translate(volume, path, &newpath);

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case AFP_META_APPLEDOUBLE:
        stbuf->st_mode = 0700 | S_IFDIR;
        goto okay;

    case AFP_META_SERVER_ICON:
        stbuf->st_mode = 0444;
        afp_pseudo_file_stat(stbuf, AFP_SERVER_ICON_LEN);
        goto okay;

    case AFP_META_RESOURCE:
    case AFP_META_FINDERINFO:
    case AFP_META_COMMENT:
        ret = ml_entry(volume, newpath, resource == AFP_META_RESOURCE
                       ? ML_ENTRY_FORK : ML_ENTRY_CACHED, &t, &e);

        if (ret < 0) {
            goto error;
        }

        /* Only a regular file has a resource fork. */
        if (resource == AFP_META_RESOURCE && !S_ISREG(afp_dirent_mode(&e))) {
            ret = -ENOENT;
            goto error;
        }

        if (resource == AFP_META_COMMENT) {
            ret = read_comment(volume, t.afp_name, t.did, NULL, 0);

            if (ret < 0) {
                goto error;
            }

            e.size = (uint64_t) ret;
        } else if (resource == AFP_META_FINDERINFO) {
            e.size = 32;
        }

        e.kind = (uint8_t) resource;
        ret = ml_dirent_to_stat(volume, &t, &e, stbuf);

        if (ret < 0) {
            goto error;
        }

        goto okay;
    }

    return 0;
okay:
    free(newpath);
    return 1;
error:
    free(newpath);
    return ret;
}

/*!
 * @brief Whether a name in the view reads as a pseudo-file
 *
 * @param[in] name  a name in the .AppleDouble directory
 * @returns 1 when it ends in the Finder information's or the comment's
 *          suffix, 0 otherwise
 */
static int names_pseudo_file(const char *name)
{
    size_t len = strlen(name);
    size_t info = strlen(finderinfo_string);
    size_t note = strlen(comment_string);
    return (len >= info && strcmp(name + len - info, finderinfo_string) == 0)
           || (len >= note && strcmp(name + len - note, comment_string) == 0);
}

/*!
 * @brief List the .AppleDouble directory at path
 *
 * Every entry beside it lists its Finder information and any comment, each
 * file a non-empty resource fork unless its name reads as a pseudo-file.
 *
 * @param[in]     volume  volume of the directory
 * @param[in]     path    UTF-8 path
 * @param[in,out] l       an empty listing, which receives the entries
 * @returns 1 for the .AppleDouble directory, 0 for another path, or a
 *          negative errno: -ENOTDIR for a pseudo-file
 */
int appledouble_readdir(struct afp_volume * volume,
                        const char *path, struct afp_listing *l)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case 0:
        return 0;

    case AFP_META_APPLEDOUBLE: {
        uint32_t real, kept = 0;
        int ret = ml_list_directory(volume, newpath, l);

        if (ret < 0) {
            free(newpath);
            return ret;
        }

        real = l->count;

        for (uint32_t i = 0; i < real && ret == 0; i++) {
            /* A copy: the appends may move the array. A listed name is in
             * the client's form, a request's in the volume's. */
            struct afp_dirent e = l->entries[i];
            char afp_name[AFPC_MAX_NAME_BYTES];
            ret = afp_listing_append(l, &e, finderinfo_string,
                                     AFP_META_FINDERINFO, 32);

            if (ret == 0
                    && convert_path_to_afp(volume->path_encoding, afp_name,
                                           afp_dirent_name(l, &e),
                                           (int) sizeof(afp_name)) == 0) {
                int size = read_comment(volume, afp_name, e.parent_did, NULL,
                                        0);

                if (size > 0) {
                    ret = afp_listing_append(l, &e, comment_string,
                                             AFP_META_COMMENT, (uint64_t) size);
                }
            }

            /* A file's resource fork stays listed, but not under a name the
             * view's lookup reads as another file's pseudo-file. */
            if (S_ISREG(afp_dirent_mode(&e)) && e.resourcesize != 0
                    && !names_pseudo_file(afp_dirent_name(l, &e))) {
                e.kind = AFP_META_RESOURCE;
                l->entries[kept++] = e;
            }
        }

        if (ret < 0) {
            afp_listing_free(l);
            free(newpath);
            return ret;
        }

        /* The pseudo-files close up behind the entries kept; an empty
         * listing has no array. */
        if (l->count > real) {
            memmove(&l->entries[kept], &l->entries[real],
                    (l->count - real) * sizeof(*l->entries));
        }

        l->count -= real - kept;
        free(newpath);
        return 1;
    }

    default:
        free(newpath);
        return -ENOTDIR;
    }
}

/*!
 * @brief Refuse a directory at a path of the .AppleDouble view
 *
 * @param[in] volume  volume of the directory
 * @param[in] path    UTF-8 path
 * @param[in] mode    unused
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_mkdir(struct afp_volume * volume, const char *path,
                      mode_t mode _U_)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Refuse to read an AppleDouble pseudo-file as a link
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @param[in] buf     unused
 * @param[in] size    unused
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_readlink(struct afp_volume * volume, const char *path,
                         char *buf _U_, size_t size _U_)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Refuse the removal of a path of the .AppleDouble view
 *
 * @param[in] volume  volume of the directory
 * @param[in] path    UTF-8 path
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_rmdir(struct afp_volume * volume, const char *path)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Refuse a link at a path of the .AppleDouble view
 *
 * @param[in] vol    volume of the link
 * @param[in] path1  unused: a link's target is any text
 * @param[in] path2  UTF-8 path of the new link
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_symlink(struct afp_volume *vol, const char *path1 _U_,
                        const char *path2)
{
    int resource;
    char *newpath;
    resource = extra_translate(vol, path2, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}

/*!
 * @brief Refuse a rename onto a path of the .AppleDouble view
 *
 * @param[in] volume     volume of the file
 * @param[in] path_from  unused
 * @param[in] path_to    UTF-8 path the file is to take
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_rename(struct afp_volume * volume,
                       const char *path_from _U_,
                       const char *path_to)
{
    int resource;
    char *newpath;
    resource = extra_translate(volume, path_to, &newpath);
    free(newpath);

    if (resource <= 0) {
        return resource;
    }

    return -EPERM;
}
