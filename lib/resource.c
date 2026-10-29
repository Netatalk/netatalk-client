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

    if (~volume->extra_flags & VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE) {
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

static int ensure_dt_opened(struct afp_volume * volume)
{
    if (volume->dtrefnum > 0) {
        return 0;
    }

    return (afp_opendt(volume, &volume->dtrefnum));
}

static int get_comment_size(struct afp_volume * volume, const char *basename,
                            unsigned int did)
{
    struct afp_comment comment;
    int ret = 0;
    int size = 1024;

    if ((comment.data = malloc(size)) == NULL) {
        return -1;
    }

    comment.maxsize = size;
    comment.size = 0;

    if (volume->dtrefnum == 0)
        if (afp_opendt(volume, &volume->dtrefnum) < 0) {
            ret = -EIO;
            goto out;
        }

    ret = (afp_getcomment(volume, did, basename, &comment));

    switch (ret) {
    case kFPAccessDenied:
        ret = -EACCES;
        break;

    case kFPMiscErr:
    case kFPParamErr:
        ret = -EIO;
        break;

    case kFPItemNotFound:
    case kFPObjectNotFound:
        ret = -ENOENT;
        break;

    case kFPNoErr:
        ret = comment.size;

    default:
        break;
    }

out:
    free(comment.data);
    return ret;
}

/*!
 * @brief Create the AppleDouble pseudo-file at path
 *
 * A resource fork or Finder information comes with its file, so the call makes
 * no request.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @param[in] mode    unused
 * @returns 1 for a resource fork or Finder information, 0 for another path,
 *          or a negative errno: -EBADF for the .AppleDouble directory, -EPERM
 *          for the server icon
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
        free(newpath);
        return 1;

    case AFP_META_APPLEDOUBLE:
        free(newpath);
        return -EBADF;

    case AFP_META_SERVER_ICON:
        return -EPERM;

    case AFP_META_FINDERINFO:
        free(newpath);
        return 1;
    }

    if (newpath) {
        free(newpath);
    }

    return 0;
}

/*!
 * @brief Refuse a change of mode of an AppleDouble pseudo-file
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @param[in] mode    unused
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_chmod(struct afp_volume * volume, const char *path,
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

    switch (fp->resource) {
    case AFP_META_RESOURCE:
        ret = ml_close_flush(volume, fp);

        if (ret == 0 && fp->dirty && afp_server_is_apple(volume->server)
                && afp_flushfork(volume, fp->forkid,
                                 DSI_DEFAULT_TIMEOUT) != kFPNoErr) {
            ret = -EIO;
        }

        switch (afp_closefork(volume, fp->forkid)) {
        case kFPNoErr:
            break;

        default:
        case kFPParamErr:
        case kFPMiscErr:
            remove_opened_fork(volume, fp);
            return -EIO;
        }

        remove_opened_fork(volume, fp);
        return ret;

    case AFP_META_APPLEDOUBLE:
        return -EBADF;

    case AFP_META_SERVER_ICON:
        return 0;

    case AFP_META_FINDERINFO:
        return 0;
    }

    return 0;
}

int appledouble_write(struct afp_volume * volume, struct afp_file_info *fp,
                      const char *data, size_t size, off_t offset, size_t *totalwritten)
{
    int ret;
    int towrite = size;
    struct afp_file_info fp2;

    switch (fp->resource) {
    case AFP_META_RESOURCE:
        ret = ll_write(volume, data, size, offset, fp, totalwritten);

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

        /* Get the previous finderinfo */
        ret = ll_get_directory_entry(volume, fp->basename, fp->did,
                                     kFPFinderInfoBit, kFPFinderInfoBit, &fp2);

        switch (ret) {
        case kFPNoErr:
            break;

        case kFPAccessDenied:
            return -EACCES;

        case kFPObjectNotFound:
            return -ENOENT;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        default:
            return -EIO;
        }

        /* Copy in only the parts we got */
        memcpy(fp->finderinfo, fp2.finderinfo, sizeof(fp->finderinfo));
        memcpy(fp->finderinfo + offset, data, towrite);
        ret = afp_setfiledirparms(volume,
                                  fp->did, fp->basename,
                                  kFPFinderInfoBit, fp);

        switch (ret) {
        case kFPNoErr:
            break;

        case kFPAccessDenied:
            return -EACCES;

        case kFPObjectNotFound:
            return -ENOENT;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        default:
            return -EIO;
        }

        *totalwritten = towrite;
        return 1;

    case AFP_META_COMMENT:
        switch (afp_addcomment(volume, fp->did, fp->basename,
                               (char *)data, (uint64_t *) totalwritten)) {
        case kFPAccessDenied:
            return -EACCES;

        case kFPObjectNotFound:
            return -ENOENT;

        case kFPNoErr:
            *totalwritten = size;
            return 1;

        case kFPMiscErr:
        default:
            return -EIO;
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
    struct afp_comment comment;
    struct afp_file_info info;
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

        ret = ll_get_directory_entry(volume, fp->basename, fp->did,
                                     kFPFinderInfoBit, kFPFinderInfoBit,
                                     &info);

        switch (ret) {
        case kFPNoErr:
            break;

        case kFPAccessDenied:
            return -EACCES;

        case kFPObjectNotFound:
            return -ENOENT;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        default:
            return -EIO;
        }

        tocopy = min(size, (32 - (unsigned long) offset));
        memcpy(buf, info.finderinfo + offset, tocopy);

        if (offset + tocopy == 32) {
            *eof = 1;
        }

        *amount_read = tocopy;
        return 1;

    case AFP_META_COMMENT:
        comment.data = malloc(size);

        if (size > 0 && comment.data == NULL) {
            return -ENOMEM;
        }

        comment.maxsize = size;

        if (fp->eof) {
            ret = 1;
        }  else
            switch (afp_getcomment(volume, fp->did, fp->basename, &comment)) {
            case kFPAccessDenied:
                ret = -EACCES;
                break;

            case kFPMiscErr:
            case kFPParamErr:
                ret = -EIO;
                break;

            case kFPItemNotFound:
            case kFPObjectNotFound:
                ret = -ENOENT;
                break;

            case kFPNoErr:
                memcpy(buf, comment.data, comment.size);
                *amount_read = comment.size;
                ret = 1;
                *eof = 1;
                fp->eof = 1;
                break;

            default:
                ret = -ENOENT;
                break;
            }

        free(comment.data);
        return ret;

    case AFP_META_SERVER_ICON:
        if (offset < 0 || offset > AFP_SERVER_ICON_LEN) {
            return -EFAULT;
        }

        tocopy = min(size, (AFP_SERVER_ICON_LEN - (unsigned long) offset));
        memcpy(buf, volume->server->icon + offset, tocopy);
        *eof = 1;
        fp->eof = 1;
        *amount_read = tocopy;
        return 1;
    }

    return 0;
}

/*!
 * @brief Truncate the AppleDouble pseudo-file at path
 *
 * Empties a resource fork, whatever offset asks for. Finder information and
 * comments accept the call without a request.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the pseudo-file
 * @param[in] offset  unused
 * @returns 1 when path is a pseudo-file's, 0 for another file, or a negative
 *          errno
 */
int appledouble_truncate(struct afp_volume * volume, const char *path,
                         int offset _U_)
{
    char *newpath;
    int resource = extra_translate(volume, path, &newpath);
    struct afp_file_info fp;
    int ret;
    unsigned int dirid;
    char basename[AFPC_MAX_NAME_BYTES];
    memset(&fp, 0, sizeof(fp));

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case AFP_META_RESOURCE:
        ret = get_dirid(volume, newpath, basename, &dirid);

        if (ret < 0) {
            free(newpath);
            return ret;
        }

        ret = afp_openfork(volume, 1, dirid,
                           AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE, 0,
                           basename, &fp);

        if (ret != kFPNoErr) {
            free(newpath);

            switch (ret) {
            case kFPAccessDenied:
                return -EACCES;

            case kFPObjectNotFound:
                return -ENOENT;

            case kFPObjectLocked:
            case kFPVolLocked:
            case kFPDenyConflict:
                return -EBUSY;

            default:
                return -EIO;
            }
        }

        add_opened_fork(volume, &fp);
        ret = ll_zero_file(volume, fp.forkid, 1);

        if (ret != 0) {
            afp_closefork(volume, fp.forkid);
            remove_opened_fork(volume, &fp);
            free(newpath);
            return -ret;
        }

        free(newpath);
        afp_closefork(volume, fp.forkid);
        remove_opened_fork(volume, &fp);
        return 1;

    case AFP_META_APPLEDOUBLE:
        free(newpath);
        return -EISDIR;

    case AFP_META_FINDERINFO:
        free(newpath);
        return 1;

    case AFP_META_COMMENT:
        free(newpath);
        return 1;

    case AFP_META_SERVER_ICON:
        free(newpath);
        return -EPERM;
    }

    return 0;
}

/*!
 * @brief Open the AppleDouble pseudo-file at path
 *
 * A resource fork opens as its file's fork; Finder information, a comment and
 * the server icon open without a request of their own.
 *
 * @param[in]     volume  volume of the file
 * @param[in]     path    UTF-8 path
 * @param[in]     flags   open(2) flags
 * @param[in,out] fp      the record, which receives the pseudo-file's type
 * @returns 1 when path is a pseudo-file's and it opened, 0 for another
 *          path, or a negative errno: -EISDIR for the .AppleDouble directory
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
        if (get_dirid(volume, newpath, fp->basename, &fp->did) < 0) {
            ret = -ENOENT;
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
        if (get_dirid(volume, newpath, fp->basename, &fp->did) < 0) {
            ret = -ENOENT;
            goto out;
        }

        ret = 1;
        goto out;

    case AFP_META_COMMENT:
        if (get_dirid(volume, newpath, fp->basename, &fp->did) < 0) {
            ret = -ENOENT;
            goto out;
        }

        if (volume->dtrefnum == 0) {
            switch (afp_opendt(volume, &volume->dtrefnum)) {
            case kFPParamErr:
            case kFPMiscErr:
                ret = -EIO;
                goto out;

            case kFPNoErr:
            default:
                break;
            }
        }

        ret = 1;
        goto out;

    case AFP_META_SERVER_ICON:
        ret = 1;
        goto out;

    default:
        break;
    }

out:
    free(newpath);
    return ret;
}

int appledouble_open_meta(struct afp_volume * volume, const char *path,
                          unsigned int resource, int flags,
                          struct afp_file_info *fp)
{
    if (!volume || !path || !fp) {
        return -EINVAL;
    }

    memset(fp, 0, sizeof(*fp));
    fp->resource = resource;

    switch (resource) {
    case AFP_META_RESOURCE:
        if (get_dirid(volume, path, fp->basename, &fp->did) < 0) {
            return -ENOENT;
        }

        return ll_open(volume, path, flags, fp);

    case AFP_META_FINDERINFO:
        if (get_dirid(volume, path, fp->basename, &fp->did) < 0) {
            return -ENOENT;
        }

        return 0;

    default:
        return -EINVAL;
    }
}

/*!
 * @brief Describe the AppleDouble pseudo-file at path for stat(2)
 *
 * @param[in]  volume  volume of the file
 * @param[in]  path    UTF-8 path
 * @param[out] stbuf   receives the attributes
 * @returns 1 when path is a pseudo-file's, 0 for another path, or a negative
 *          errno
 */
int appledouble_getattr(struct afp_volume * volume,
                        const char *path, struct stat *stbuf)
{
    int resource;
    char *newpath;
    int ret;
    resource = extra_translate(volume, path, &newpath);

    if (resource < 0) {
        return resource;
    }

    switch (resource) {
    case AFP_META_RESOURCE:
        ll_getattr(volume, newpath, stbuf, 1);
        goto okay;

    case AFP_META_APPLEDOUBLE:
        stbuf->st_mode = 0700 | S_IFDIR;
        goto okay;

    case AFP_META_FINDERINFO:
        ll_getattr(volume, newpath, stbuf, 0);
        stbuf->st_mode |= S_IFREG;
        stbuf->st_size = 32;
        goto okay;

    case AFP_META_COMMENT: {
        unsigned int did;
        char basename[AFPC_MAX_NAME_BYTES];
        ret = ll_getattr(volume, newpath, stbuf, 0);

        if (ret < 0) {
            goto error;
        }

        ret = get_dirid(volume, newpath, basename, &did);

        if (ret < 0) {
            goto error;
        }

        ret = get_comment_size(volume, basename, did);

        if (ret < 0) {
            goto error;
        }

        stbuf->st_mode |= S_IFREG;
        stbuf->st_size = ret;
        goto okay;
    }

    case AFP_META_SERVER_ICON:
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = AFP_SERVER_ICON_LEN;
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

static int add_fp(struct afp_file_info **newchain, struct afp_file_info *fp,
                  char *suffix, unsigned int size)
{
    struct afp_file_info * newfp;
    newfp = malloc(sizeof(struct afp_file_info));

    if (!newfp) {
        return 1;
    }

    memcpy(newfp, fp, sizeof(struct afp_file_info));
    strcat(newfp->name, suffix);
    newfp->resourcesize = size;
    newfp->unixprivs.permissions |= S_IFREG;
    newfp->next = *newchain;
    *newchain = newfp;
    return 0;
}

/*!
 * @brief List the .AppleDouble directory at path
 *
 * Each file of the directory beside it lists its Finder information, its
 * comment when it has one, and its resource fork when that is not empty.
 *
 * @param[in]  volume  volume of the directory
 * @param[in]  path    UTF-8 path
 * @param[out] base    receives the entries, which the caller frees
 * @returns 1 for the .AppleDouble directory, 0 for another path, or a
 *          negative errno: -ENOTDIR for a pseudo-file
 */
int appledouble_readdir(struct afp_volume * volume,
                        const char *path, struct afp_file_info **base)
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
        struct afp_file_info *newchain = NULL;
        struct afp_file_info *validNodes = NULL;
        int ret = ll_readdir(volume, newpath, base, 1);

        if (ret < 0) {
            free(newpath);
            return ret;
        }

        /* First pass: build list of valid nodes */
        for (struct afp_file_info *fp = *base; fp;) {
            struct afp_file_info *next = fp->next;
            /* Add metadata entries to newchain */
            add_fp(&newchain, fp, finderinfo_string, 32);

            /* Add comments if it has a size > 0 */
            if (ensure_dt_opened(volume) == 0) {
                int size = get_comment_size(volume,
                                            fp->name, fp->did);

                if (size > 0) {
                    add_fp(&newchain, fp, comment_string, 32);
                }
            }

            /* Check if we keep this node */
            if (fp->unixprivs.permissions & S_IFREG && fp->resourcesize != 0) {
                fp->next = validNodes;
                validNodes = fp;
                fp = next;
                continue;
            }

            /* Node not kept - free it */
            free(fp);
            fp = next;
        }

        /* Clear original list */
        *base = NULL;
        /* Rebuild list in original order */
        struct afp_file_info *prev = NULL;

        while (validNodes != NULL) {
            struct afp_file_info *curr = validNodes;
            validNodes = validNodes->next;

            if (prev == NULL) {
                *base = curr;
            } else {
                prev->next = curr;
            }

            prev = curr;
        }

        /* Connect newchain at the end if we have valid nodes */
        if (prev != NULL) {
            prev->next = newchain;
        } else {
            /* No valid nodes, newchain becomes the list */
            *base = newchain;
        }

        free(newpath);
        return 1;
    }

    case AFP_META_RESOURCE:
    case AFP_META_SERVER_ICON:
    case AFP_META_COMMENT:
        free(newpath);
        return -ENOTDIR;
    }

    if (newpath) {
        free(newpath);
    }

    return 0;
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
 * @brief Refuse a change of owner of an AppleDouble pseudo-file
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path
 * @param[in] uid     unused
 * @param[in] gid     unused
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_chown(struct afp_volume * volume, const char *path,
                      uid_t uid _U_, gid_t gid _U_)
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
 * @brief Refuse a change of times of an AppleDouble pseudo-file
 *
 * @param[in] volume   volume of the file
 * @param[in] path     UTF-8 path
 * @param[in] timebuf  unused
 * @returns 0 for a path that names no pseudo-file, -EPERM for one, or
 *          -ENOMEM
 */
int appledouble_utime(struct afp_volume * volume, const char *path,
                      struct utimbuf * timebuf _U_)
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
