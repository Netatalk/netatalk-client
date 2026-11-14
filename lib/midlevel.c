/*
    midlevel.c: some funtions to abstract some of the common functions

    Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

/*!
 * @file
 * @brief POSIX file operations on a volume, carried out with AFP requests
 */

#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>

#include "afp_internal.h"
#include "did.h"
#include "dsi.h"
#include "codepage.h"
#include "compat.h"
#include "lowlevel.h"
#include "mapping.h"
#include "midlevel.h"
#include "forklist.h"
#include "resource.h"
#include "uams.h"
#include "users.h"
#include "utils.h"
#include "xattr.h"


#define min(a,b) (((a)<(b)) ? (a) : (b))

/*!
 * @brief Convert a path to the volume's AFP encoding
 *
 * @param[in]  encoding        the volume's path encoding
 * @param[in]  path            UTF-8 path
 * @param[out] converted_path  the converted path, in a buffer sized for
 *                             its decomposition, which the caller frees
 * @returns 0, -EINVAL for a path that does not convert, -ENAMETOOLONG, or
 *          -ENOMEM
 */
static int convert_path_to_afp_alloc(char encoding, const char *path,
                                     char **converted_path)
{
    char *converted;
    size_t len;
    size_t converted_len;
    int ret;

    if (!path || !converted_path) {
        return -EINVAL;
    }

    ret = afpc_path_validate(path, AFPC_MAX_UTF8_PATH_BYTES, &len);

    if (ret) {
        return ret;
    }

    /* No decomposition grows past three times its UTF-8 length (U+0390,
     * two bytes to six): four times the source and a terminator hold it. */
    converted_len = len * 4U + 1U;
    converted = malloc(converted_len);

    if (!converted) {
        return -ENOMEM;
    }

    if (convert_path_to_afp(encoding, converted, path,
                            (int) converted_len)) {
        free(converted);
        return -EINVAL;
    }

    /* The wire carries a 16-bit path length, which a decomposition can
     * grow past. */
    if (strnlen(converted, converted_len) > AFPC_MAX_UTF8_PATH_BYTES) {
        free(converted);
        return -ENAMETOOLONG;
    }

    *converted_path = converted;
    return 0;
}

/*!
 * @brief The name a request sends for an attribute, past any user. prefix
 *
 * @param[in]  name         the attribute's name
 * @param[out] afp_name     receives the name past the prefix
 * @param[out] afp_namelen  receives its length
 * @returns 0, or -ENAMETOOLONG for a name past AFP_XATTR_NAME_MAX
 */
static int normalize_xattr_name(const char *name, const char **afp_name,
                                unsigned short *afp_namelen)
{
    size_t length = strnlen(name, AFP_XATTR_NAME_MAX + 1U);

    if (length > AFP_XATTR_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    const char *stripped = afp_xattr_strip_user_prefix(name);

    if (stripped != name) {
        length -= AFP_XATTR_USER_PREFIX_LEN;
    }

    *afp_name = stripped;
    *afp_namelen = (unsigned short)length;
    return 0;
}

/*!
 * @brief An attribute's name past any user. prefix
 *
 * @param[in] name  the attribute's name
 * @returns the name past the prefix
 */
static const char *normalized_xattr_name_for_compare(const char *name)
{
    return afp_xattr_strip_user_prefix(name);
}

/*!
 * @brief Whether an attribute's name is the resource fork's
 *
 * @param[in] name  the attribute's name, with or without user.
 * @returns 1 when it is
 */
static int is_resourcefork_xattr(const char *name)
{
    return strcmp(normalized_xattr_name_for_compare(name),
                  AFP_XATTR_RESOURCEFORK) == 0;
}

/*!
 * @brief Whether an attribute's name is the FinderInfo's
 *
 * @param[in] name  the attribute's name, with or without user.
 * @returns 1 when it is
 */
static int is_finderinfo_xattr(const char *name)
{
    return strcmp(normalized_xattr_name_for_compare(name),
                  AFP_XATTR_FINDERINFO) == 0;
}

/*!
 * @brief Whether a name begins with Netatalk's metadata attribute's name
 *
 * A user. prefix is dropped first, as for the name the request would send;
 * Netatalk's server guards the same prefix.
 *
 * @param[in] name  the attribute's name
 * @returns nonzero for such a name
 */
static int is_netatalk_meta_xattr(const char *name)
{
    return IS_INTERNAL_SERVER_XATTR(normalized_xattr_name_for_compare(name));
}

/*!
 * @brief Read parameters of a file or a directory with FPGetFileDirParms
 *
 * @param[in]  vol       volume of the object
 * @param[in]  dirid     its directory
 * @param[in]  basename  its AFP name in dirid
 * @param[in]  bitmap    the parameters, bits the file and directory bitmaps
 *                       share
 * @param[out] e         receives them
 * @returns 0, or a negative errno, as ll_answer_errno() gives it
 */
static int get_parameters(struct afp_volume * vol, unsigned int dirid,
                          char *basename, unsigned short bitmap,
                          struct afp_dirent *e)
{
    int ret = afp_getfiledirparms(vol, dirid, bitmap, bitmap, basename, e);
    return -ll_answer_errno(ret);
}

/*!
 * @brief Set parameters of a file or a directory
 *
 * Owner and group are directory parameters, which the AFP Reference leaves to
 * FPSetDirParms.
 *
 * @param[in] vol       volume of the object
 * @param[in] dirid     its directory
 * @param[in] basename  its AFP name in dirid
 * @param[in] bitmap    the parameters
 * @param[in] e         their values
 * @returns 0, or a negative errno: -EPERM for a refused change of the
 *          privileges or the ids, -EACCES for another, -ENOSYS for a
 *          parameter the server does not set, -ENOENT, -EROFS, or -EIO for
 *          another answer or a request without one
 */
static int set_parameters(struct afp_volume * vol, unsigned int dirid,
                          const char *basename, unsigned short bitmap,
                          const struct afp_dirent *e)
{
    int rc = (bitmap & (kFPOwnerIDBit | kFPGroupIDBit))
             ? afp_setdirparms(vol, dirid, basename, bitmap, e)
             : afp_setfiledirparms(vol, dirid, basename, bitmap, e);

    switch (rc) {
    case kFPNoErr:
        return 0;

    case kFPAccessDenied:
        return (bitmap & (kFPUnixPrivsBit | kFPOwnerIDBit | kFPGroupIDBit))
               ? -EPERM : -EACCES;

    case kFPBitmapErr:
        return -ENOSYS;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPVolLocked:
        return -EROFS;

    default:
        return -EIO;
    }
}

/*!
 * @brief Give a file or a directory an owner and a group, read back
 *
 * The ids are read back, as Netatalk answers a change it keeps with success;
 * a set-ID file keeps its ids, as afpd restores the bits chown(2) clears.
 *
 * @param[in]     vol       volume of the object
 * @param[in]     dirid     its directory
 * @param[in]     basename  its AFP name in dirid
 * @param[in,out] fp        its UNIX privileges and modification time, as
 *                          read or set, which a file's request carries
 *                          with the new ids
 * @param[in]     mode      its mode as read, whose set-ID bits a mode the
 *                          request carries cannot clear
 * @param[in]     uid       the server's id of the owner
 * @param[in]     gid       the server's id of the group
 * @returns 0, -EPERM for an owner or a group the server kept or would not
 *          set alone, or another negative errno
 */
static int set_owner(struct afp_volume * vol, unsigned int dirid,
                     char *basename, struct afp_dirent *fp,
                     unsigned int mode, unsigned int uid, unsigned int gid)
{
    struct afp_dirent check;
    struct afp_dirent sent;
    unsigned int was_uid = fp->unixprivs.uid;
    unsigned int was_gid = fp->unixprivs.gid;
    unsigned short bitmap;
    int ret = 0;

    if (was_uid == uid && was_gid == gid) {
        return 0;
    }

    if (!fp->isdir && (mode & (S_ISUID | S_ISGID))) {
        return -EPERM;
    }

    if (fp->isdir && afp_server_gives_directory_ids_to_files(vol->server)) {
        return -EPERM;
    }

    while (ret == 0 && (fp->unixprivs.uid != uid
                        || fp->unixprivs.gid != gid)) {
        if (!fp->isdir) {
            fp->unixprivs.uid = uid;
            fp->unixprivs.gid = gid;
            bitmap = kFPUnixPrivsBit | (fp->valid & kFPModDateBit);
        } else {
            bitmap = (fp->unixprivs.uid != uid ? kFPOwnerIDBit : 0)
                     | (fp->unixprivs.gid != gid ? kFPGroupIDBit : 0);
            fp->unixprivs.uid = uid;
            fp->unixprivs.gid = gid;
        }

        /* Netatalk changes no id of -1: a file's id the call keeps goes as
         * -1, so a change another client made to it since the read stands. */
        sent = *fp;

        if (!fp->isdir && afp_server_keeps_unset_ids(vol->server)) {
            if (uid == was_uid) {
                sent.unixprivs.uid = UINT32_MAX;
            }

            if (gid == was_gid) {
                sent.unixprivs.gid = UINT32_MAX;
            }
        }

        ret = set_parameters(vol, dirid, basename, bitmap, &sent);

        if (ret == 0) {
            memset(&check, 0, sizeof(check));
            ret = get_parameters(vol, dirid, basename, kFPUnixPrivsBit,
                                 &check);
        }

        /* Only an id the call changes is read back. */
        if (ret == 0 && ((fp->unixprivs.uid != was_uid
                          && check.unixprivs.uid != fp->unixprivs.uid)
                         || (fp->unixprivs.gid != was_gid
                             && check.unixprivs.gid != fp->unixprivs.gid))) {
            ret = -EPERM;
        }
    }

    return ret;
}

/*!
 * @brief Resolve a path to its parent's directory ID and its last component
 *
 * The last component comes in the cache's spelling and in the wire form.
 *
 * @param[in]  volume  volume of the path
 * @param[in]  path    UTF-8 path
 * @param[out] t       receives the parent's ID and the component
 * @returns 0, or dir_cache_resolve()'s or ll_wire_name()'s negative errno
 */
int ml_resolve(struct afp_volume *volume, const char *path,
               struct ml_target *t)
{
    int ret = dir_cache_resolve(volume, path, &t->did, t->name);
    return ret ? ret : ll_wire_name(volume, t->name, t->afp_name);
}

/*!
 * @brief Resolve a path into an open fork's directory and names
 *
 * @param[in]  volume  volume of the path
 * @param[in]  path    UTF-8 path of the file
 * @param[out] fp      receives the directory, the name and the wire name
 * @returns ml_resolve()'s result
 */
int ml_resolve_fork(struct afp_volume *volume, const char *path,
                    struct afp_file_info *fp)
{
    struct ml_target t;
    int ret = ml_resolve(volume, path, &t);

    if (ret) {
        return ret;
    }

    fork_now_named(fp, &t);
    return 0;
}

/*!
 * @brief Read a resolved object's record
 *
 * @param[in]  volume  volume of the object
 * @param[in]  t       the resolved path
 * @param[in]  how     ML_ENTRY_CACHED, ML_ENTRY_FRESH or ML_ENTRY_FORK
 * @param[out] e       receives the record
 * @returns 0, -ENOENT for a name known to hold nothing, or
 *          dir_cache_fetch()'s error
 */
static int read_entry(struct afp_volume *volume, const struct ml_target *t,
                      int how, struct afp_dirent *e)
{
    if (how != ML_ENTRY_FRESH) {
        switch (dir_cache_lookup(volume, t->did, t->name, e)
                & (DIR_CACHE_STAT | DIR_CACHE_NEGATIVE)) {
        case DIR_CACHE_STAT:

            /* The server measures a held fork; a stale length cuts a read. */
            if (how != ML_ENTRY_FORK || e->resourcesize == 0) {
                return 0;
            }

            break;

        case DIR_CACHE_NEGATIVE:
            return -ENOENT;

        default:
            break;
        }
    }

    return dir_cache_fetch(volume, t->did, t->name, e);
}

/*!
 * @brief Read an object's record
 *
 * @param[in]  volume  volume of the object
 * @param[in]  path    UTF-8 path of the object
 * @param[in]  how     ML_ENTRY_CACHED for the cache's record while its
 *                     parameters hold, ML_ENTRY_FRESH for the server's,
 *                     ML_ENTRY_FORK for the cache's unless it holds a
 *                     resource fork
 * @param[out] t       receives the resolved path
 * @param[out] e       receives the record
 * @returns 0, or a negative errno
 */
int ml_entry(struct afp_volume *volume, const char *path, int how,
             struct ml_target *t, struct afp_dirent *e)
{
    int ret;
    memset(e, 0, sizeof(*e));
    ret = ml_resolve(volume, path, t);

    if (ret) {
        return ret;
    }

    return read_entry(volume, t, how, e);
}

/*!
 * @brief List the directory at a UNIX path
 *
 * A listing refused under cached IDs is retried once if a fresh walk finds
 * another parent ID; the folder's entry takes the ID its entries carry.
 *
 * @param[in]     volume  volume of the directory
 * @param[in]     path    UTF-8 path of the directory
 * @param[in,out] l       the listing the entries join, zeroed by the
 *                        caller; empty when the listing fails
 * @returns 0, or a negative errno
 */
int ml_list_directory(struct afp_volume *volume, const char *path,
                      struct afp_listing *l)
{
    struct ml_target t;
    struct afp_dirent e;
    unsigned int refused_did;
    uint64_t sent;
    int ret = ml_resolve(volume, path, &t);

    if (ret) {
        return ret;
    }

    sent = dir_cache_changes(volume);
    ret = ll_readdir(volume, t.did, t.afp_name, l);

    if (ret == -ENOENT) {
        refused_did = t.did;
        ret = dir_cache_resolve_fresh(volume, path, &t.did, t.name);

        if (ret) {
            return ret;
        }

        if (t.did == refused_did) {
            ret = -ENOENT;
        } else {
            sent = dir_cache_changes(volume);
            ret = ll_readdir(volume, t.did, t.afp_name, l);
        }
    }

    /* No folder holds the name, or a file does: the name goes. */
    if (ret == -ENOENT || ret == -ENOTDIR) {
        dir_cache_remove(volume, t.did, t.name);
        return ret;
    }

    /* The entries carry the folder's ID; an empty listing carries none, and
     * the cache's may be another folder's. */
    if (ret == 0 && l->count == 0) {
        /* The root has only objects to forget; a folder the cache lacks is
         * read for its ID. */
        if (t.name[0] == '\0') {
            dir_cache_listed(volume, t.did, 0);
        } else if (!dir_cache_listed_empty(volume, t.did, t.name)
                   && dir_cache_fetch(volume, t.did, t.name, &e) == 0
                   && e.isdir) {
            dir_cache_listed(volume, e.cnid, 0);
        }
    } else if (ret == 0 && t.name[0]
               && (l->entries[0].valid & kFPParentDirIDBit)) {
        dir_cache_insert_dir(volume, t.did, t.name, l->entries[0].parent_did,
                             sent);
    }

    return ret;
}

/*!
 * @brief Open a file's data fork, or an AppleDouble pseudo-file, at path
 *
 * @param[in]  volume  volume of the file
 * @param[in]  path    UTF-8 path of the file
 * @param[in]  flags   open(2) flags
 * @param[out] newfp   receives the open record of an open that succeeds,
 *                     which the caller frees after ml_close()
 * @returns 0, or a negative errno: -EROFS for a write open of a read-only
 *          volume, the lookup's errno, or the open's
 */
int ml_open(struct afp_volume * volume, const char *path, int flags,
            struct afp_file_info **newfp)
{
    struct afp_file_info * fp;
    int ret;

    if (volume_is_readonly(volume) &&
            (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND | O_CREAT))) {
        return -EROFS;
    }

    if ((fp = malloc(sizeof(*fp))) == NULL) {
        return -ENOMEM;
    }

    memset(fp, 0, sizeof(*fp));
    ret = appledouble_open(volume, path, flags, fp);

    if (ret == 0) {
        ret = ml_resolve_fork(volume, path, fp);
    }

    if (ret == 0) {
        ret = ll_open(volume, path, flags, fp);
    }

    if (ret < 0) {
        free(fp);
        return ret;
    }

    *newfp = fp;
    return 0;
}



/*!
 * @brief Create a file at path with FPCreateFile, unless one exists
 *
 * A soft create leaves a file it finds; a pseudo-file needs no request.
 *
 * @param[in]  volume   volume of the path
 * @param[in]  path     UTF-8 path of the new file
 * @param[in]  mode     unused; the caller gives it with ml_set_created_mode()
 *                      after the open, so a read-only mode still writes
 * @param[out] created  set to 1 when this call made the file, or NULL
 * @returns 0, -EEXIST for a file or a directory at the path, or another
 *          negative errno
 */
int ml_creat(struct afp_volume * volume, const char *path, mode_t mode,
             unsigned char *created)
{
    struct ml_target t;
    int ret;

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    ret = appledouble_creat(volume, path, mode);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    ret = ml_resolve(volume, path, &t);

    if (ret) {
        return ret;
    }

    ret = ll_createfile(volume, t.did, t.afp_name);

    if (ret == 0 && created) {
        *created = 1;
    }

    /* A taken name only ends a cached absence, as nothing changed; any
     * other answer may have put a new object there. */
    if (ret == -EEXIST) {
        dir_cache_invalidate(volume, t.did, t.name, 0);
    } else {
        dir_cache_remove(volume, t.did, t.name);
    }

    return ret;
}

/*!
 * @brief Whether an object created on the volume takes the mode asked for
 *
 * @param[in] volume  volume of the object
 * @returns 1 on a volume with UNIX privileges, 0 elsewhere
 */
static int takes_created_mode(const struct afp_volume * volume)
{
    return (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) != 0;
}

/*!
 * @brief Give a file or a directory this client created the mode asked for
 *
 * Netatalk keeps ids of -1, so the mode goes without reading them first; a
 * mode the server cannot set stays the server's.
 *
 * @param[in] volume  volume of the object
 * @param[in] t       its resolved path
 * @param[in] mode    the mode asked for
 */
static void set_created_mode(struct afp_volume * volume, struct ml_target *t,
                             mode_t mode)
{
    struct afp_dirent fp;
    int ret;

    if (!takes_created_mode(volume)) {
        return;
    }

    memset(&fp, 0, sizeof(fp));
    fp.unixprivs.uid = UINT32_MAX;
    fp.unixprivs.gid = UINT32_MAX;
    ret = afp_server_keeps_unset_ids(volume->server) ? 0
          : get_parameters(volume, t->did, t->afp_name, kFPUnixPrivsBit, &fp);

    if (ret == 0) {
        fp.unixprivs.permissions = mode & ~S_IFMT;
        ret = set_parameters(volume, t->did, t->afp_name, kFPUnixPrivsBit,
                             &fp);
    }

    if (ret != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "%s keeps the server's mode", t->name);
    }

    dir_cache_invalidate(volume, t->did, t->name, 0);
}

/*!
 * @brief Give a file this client created the mode asked for
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] mode    the mode asked for, as set_created_mode() gives it
 */
void ml_set_created_mode(struct afp_volume * volume, const char *path,
                         mode_t mode)
{
    struct ml_target t;

    if (takes_created_mode(volume) && ml_resolve(volume, path, &t) == 0) {
        set_created_mode(volume, &t, mode);
        /* afpd dates the folder of a file whose privileges a set changes. */
        dir_cache_forget_dir(volume, t.did);
    }
}



/*!
 * @brief List the directory at path
 *
 * @param[in]     volume  volume of the directory
 * @param[in]     path    UTF-8 path of the directory
 * @param[in,out] l       the listing the entries join, empty before, and
 *                        after a listing that fails; the caller frees it
 * @returns 0, or a negative errno
 */
int ml_readdir(struct afp_volume * volume,
               const char *path,
               struct afp_listing *l)
{
    int ret = appledouble_readdir(volume, path, l);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    return ml_list_directory(volume, path, l);
}

/*!
 * @brief Read from an open fork
 *
 * @param[in]     volume  volume of the fork
 * @param[out]    buf     receives the bytes
 * @param[in]     size    capacity of buf
 * @param[in]     offset  byte offset to read from
 * @param[in,out] fp      the open fork, or an AppleDouble pseudo-file
 * @param[out]    eof     set to 1 when the read reaches the end of the fork
 * @returns bytes read, or a negative errno
 */
int ml_read(struct afp_volume * volume,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof)
{
    int ret = 0;
    size_t amount_copied = 0;
    *eof = 0;

    if (fp->resource) {
        ret = appledouble_read(volume, fp, buf, size, offset, &amount_copied, eof);

        if (ret < 0) {
            return ret;
        }

        if (ret == 1) {
            return amount_copied;
        }
    }

    ret = ll_read(volume, buf, size, offset, fp, eof);
    return ret;
}


/*!
 * @brief Whether a change can carry the modification time its read returned
 *
 * afpd stamps its clock on a change without a time, but would set a symlink's
 * time on its target and store one before 1970 as one after 2068.
 *
 * @param[in] e  the object's parameters as read
 * @returns 1 when a change can carry e's modification time, or 0
 */
int ml_carries_time(const struct afp_dirent *e)
{
    return (e->valid & kFPModDateBit) && e->mod_ns >= 0
           && !S_ISLNK(e->unixprivs.permissions)
           && memcmp(e->finderinfo, ML_SYMLINK_FINDERINFO, 8) != 0;
}

/*!
 * @brief Set the mode, owner, group and modification time of an object
 *
 * A mode, owner or group equal to the object's sends no change, and one the
 * server keeps while answering success fails with EPERM.
 *
 * @param[in] vol    volume of the object
 * @param[in] path   UTF-8 path of the object
 * @param[in] which  ML_SET_MODE, ML_SET_UID, ML_SET_GID and ML_SET_MTIME
 * @param[in] mode   the mode, for ML_SET_MODE
 * @param[in] uid    the owner, for ML_SET_UID
 * @param[in] gid    the group, for ML_SET_GID
 * @param[in] mtime  the modification time, for ML_SET_MTIME
 * @returns 0, or a negative errno; a time asked is set even when the
 *          privileges cannot be read, are not the session's to change, or
 *          are not the server's to set
 */
int ml_setattr(struct afp_volume * vol, const char *path, unsigned int which,
               mode_t mode, uid_t uid, gid_t gid, time_t mtime)
{
    unsigned int privs = which & (ML_SET_MODE | ML_SET_UID | ML_SET_GID);
    struct afp_dirent fp;
    struct afp_dirent now;
    struct ml_target t;
    unsigned short bitmap = 0;
    unsigned int was = 0;
    int result = 0;
    int ret;

    if (volume_is_readonly(vol)) {
        return -EROFS;
    }

    ret = appledouble_setattr(vol, path);

    if (ret < 0) {
        return ret;
    }

    /* AFP 2, and Netatalk with "unix priv = no", carry no UNIX privileges. */
    if (privs && !(vol->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX)) {
        if (!(vol->extra_flags & VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS)) {
            result = -ENOSYS;
        } else if (!(which & ML_SET_MTIME)) {
            return ml_entry(vol, path, ML_ENTRY_CACHED, &t, &fp);
        }

        privs = 0;
    }

    if ((privs & (ML_SET_UID | ML_SET_GID))
            && vol->mapping != AFP_MAPPING_COMMON) {
        unsigned int shown_uid = 0;
        unsigned int shown_gid = 0;

        if (translate_uidgid_to_client(vol, &shown_uid, &shown_gid)
                || ((privs & ML_SET_UID) && uid != shown_uid)
                || ((privs & ML_SET_GID) && gid != shown_gid)) {
            result = -EPERM;
        }

        privs &= ~(ML_SET_UID | ML_SET_GID);
    }

    if (!privs && !(which & ML_SET_MTIME)) {
        return result;
    }

    ret = ml_resolve(vol, path, &t);

    if (ret) {
        return ret;
    }

    memset(&fp, 0, sizeof(fp));

    if (privs) {
        ret = get_parameters(vol, t.did, t.afp_name,
                             kFPUnixPrivsBit | kFPModDateBit, &fp);

        /* A time asked goes alone. */
        if (ret) {
            result = ret;
            privs = 0;
        }

        if (ret == -ENOENT) {
            dir_cache_remove(vol, t.did, t.name);
        }

        was = fp.unixprivs.permissions;

        if (!ml_carries_time(&fp)) {
            fp.valid &= ~(uint32_t) kFPModDateBit;
        }
    }

    if ((privs & ML_SET_MODE) && (was & ~S_IFMT) != (mode & ~S_IFMT)) {
        fp.unixprivs.permissions = (was & S_IFMT) | (mode & ~S_IFMT);
        bitmap = kFPUnixPrivsBit;
    }

    if (which & ML_SET_MTIME) {
        fp.mod_ns = afp_ns_from_sec(mtime);
        fp.valid |= kFPModDateBit;
    }

    /* afpd stamps its clock on a change of the privileges that has no time. */
    if ((bitmap || (which & ML_SET_MTIME)) && (fp.valid & kFPModDateBit)) {
        bitmap |= kFPModDateBit;
    }

    if (bitmap) {
        unsigned int uid_read = fp.unixprivs.uid;
        unsigned int gid_read = fp.unixprivs.gid;

        /* Netatalk changes no id of -1; the ids read would undo a change made
         * since, and a directory's would go to every file in it on Netatalk 2. */
        if ((bitmap & kFPUnixPrivsBit)
                && afp_server_keeps_unset_ids(vol->server)) {
            fp.unixprivs.uid = UINT32_MAX;
            fp.unixprivs.gid = UINT32_MAX;
        }

        ret = set_parameters(vol, t.did, t.afp_name, bitmap, &fp);
        fp.unixprivs.uid = uid_read;
        fp.unixprivs.gid = gid_read;

        /* A server can refuse the time with the mode, or the mode alone. */
        if ((ret == -EPERM || ret == -ENOSYS) && (bitmap & kFPUnixPrivsBit)
                && (which & ML_SET_MTIME)) {
            int time_ret = set_parameters(vol, t.did, t.afp_name,
                                          kFPModDateBit, &fp);
            ret = time_ret ? time_ret : ret;
        }

        dir_cache_invalidate(vol, t.did, t.name, 0);

        /* afpd dates the folder of a file whose privileges a set changes. */
        if ((bitmap & kFPUnixPrivsBit) && !fp.isdir) {
            dir_cache_forget_dir(vol, t.did);
        }

        if (ret) {
            return ret;
        }
    }

    /* afpd answers a file's refused chmod(2) with success, and Netatalk 2 a
     * directory's; kUserIsOwner also marks euid 0 and root's objects. */
    if ((bitmap & kFPUnixPrivsBit)
            && (fp.isdir ? afp_server_keeps_directory_modes(vol->server)
                : (!(fp.unixprivs.ua_permissions & kUserIsOwner)
                   || fp.unixprivs.uid == 0))) {
        /* afpd keeps a file's special bits, so their clearing counts as done,
         * and hides a directory's set-group-ID bit. */
        const unsigned int shown = S_IRWXU | S_IRWXG | S_IRWXO
                                   | (fp.isdir ? S_ISUID | S_ISVTX : 0);
        unsigned int added = fp.isdir ? 0
                             : mode & ~was & (S_ISUID | S_ISGID | S_ISVTX);
        unsigned int asked = ((mode ^ was) & shown) | added;

        if (asked) {
            memset(&now, 0, sizeof(now));
            ret = get_parameters(vol, t.did, t.afp_name, kFPUnixPrivsBit,
                                 &now);

            if (ret) {
                return ret;
            }

            if (!(((now.unixprivs.permissions ^ was) & shown)
                    | (now.unixprivs.permissions & added))) {
                result = -EPERM;
            }
        }
    }

    if (privs & (ML_SET_UID | ML_SET_GID)) {
        ret = set_owner(vol, t.did, t.afp_name, &fp, was,
                        (privs & ML_SET_UID) ? uid : fp.unixprivs.uid,
                        (privs & ML_SET_GID) ? gid : fp.unixprivs.gid);
        dir_cache_invalidate(vol, t.did, t.name, 0);

        /* afpd dates the folder of a file whose owner changes too. */
        if (!fp.isdir) {
            dir_cache_forget_dir(vol, t.did);
        }

        if (ret) {
            return ret;
        }
    }

    return result;
}

/*!
 * @brief Set the mode of a file or a directory
 *
 * @param[in] vol   volume of the object
 * @param[in] path  UTF-8 path of the object
 * @param[in] mode  the mode
 * @returns 0, or a negative errno, as ml_setattr()
 */
int ml_chmod(struct afp_volume * vol, const char *path, mode_t mode)
{
    return ml_setattr(vol, path, ML_SET_MODE, mode, 0, 0, 0);
}


/*!
 * @brief Map the answer of an FPDelete to an errno
 *
 * @param[in] rc  the answer, or the request's failure
 * @returns 0 for kFPNoErr, or a positive errno: EPERM for a locked object,
 *          EBUSY for one open elsewhere, EROFS for a read-only filesystem,
 *          ENOTEMPTY for a directory with entries, EIO for an unexpected
 *          answer or a request without one, or the server's EACCES or ENOENT
 */
static int delete_errno(int rc)
{
    switch (rc) {
    case kFPObjectLocked:
        return EPERM;

    case kFPFileBusy:
        return EBUSY;

    case kFPDirNotEmpty:
        return ENOTEMPTY;

    default:
        return ll_answer_errno(rc);
    }
}

/*!
 * @brief Delete a resolved object with FPDelete
 *
 * The cache forgets the name whatever the answer, as a refusal may come
 * from an object other than the one its record describes.
 *
 * @param[in,out] vol  volume of the object
 * @param[in]     t    the resolved path
 * @returns 0, or a negative errno, as delete_errno() gives it
 */
static int delete_target(struct afp_volume *vol, struct ml_target *t)
{
    int ret = delete_errno(afp_delete(vol, t->did, t->afp_name));
    dir_cache_remove(vol, t->did, t->name);
    return -ret;
}

/*!
 * @brief Delete the file at path with FPDelete
 *
 * The type comes from the cache's record, so an empty folder another
 * session put at the name since goes, and a full one refuses.
 *
 * @param[in] vol   volume of the file
 * @param[in] path  UTF-8 path of the file
 * @returns 0, or a negative errno: -EISDIR for a directory, -ENOTEMPTY for
 *          a full one a record took for a file, -ENOTDIR for a path through
 *          a file, the lookups' errno when they fail, -EROFS for a
 *          read-only filesystem, -EPERM for a locked file, -EBUSY for one
 *          open elsewhere, -EIO for an unexpected answer or a request
 *          without one
 */
int ml_unlink(struct afp_volume * vol, const char *path)
{
    struct ml_target t;
    struct afp_dirent e;
    int ret;

    if (volume_is_readonly(vol)) {
        return -EROFS;
    }

    ret = appledouble_unlink(vol, path);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    /* FPDelete removes an empty directory too; one goes by rmdir(2). */
    ret = ml_entry(vol, path, ML_ENTRY_CACHED, &t, &e);

    /* Another session may have put a file in the folder's place. */
    if (ret == 0 && e.isdir) {
        ret = read_entry(vol, &t, ML_ENTRY_FRESH, &e);
    }

    if (ret) {
        return ret;
    }

    if (e.isdir) {
        return -EISDIR;
    }

    return delete_target(vol, &t);
}




/*!
 * @brief Create a directory at path with FPCreateDir
 *
 * The new directory gets the mode asked for, as set_created_mode() gives it.
 *
 * @param[in] vol   volume of the directory
 * @param[in] path  UTF-8 path of the new directory
 * @param[in] mode  the mode asked for
 * @returns 0, or a negative errno: the lookup's when it fails, -EROFS for a
 *          read-only filesystem, -EPERM for a volume without directories,
 *          -EIO for an unexpected answer or a request without one
 */
int ml_mkdir(struct afp_volume * vol, const char *path, mode_t mode)
{
    struct ml_target t;
    unsigned int result_did;
    uint64_t sent;
    int ret, rc;

    if (volume_is_readonly(vol)) {
        return -EROFS;
    }

    ret = appledouble_mkdir(vol, path, mode);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    ret = ml_resolve(vol, path, &t);

    if (ret) {
        return ret;
    }

    sent = dir_cache_changes(vol);
    rc = afp_createdir(vol, t.did, t.afp_name, &result_did);

    switch (rc) {
    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPObjectExists:
        ret = EEXIST;
        break;

    /* mkdir(2)'s errno for a filesystem without directories */
    case kFPFlatVol:
        ret = EPERM;
        break;

    default:
        ret = ll_answer_errno(rc);
    }

    /* A taken name only ends a cached absence, as nothing changed; any
     * other answer may have put a new object there. */
    if (ret == 0 && result_did != 0) {
        dir_cache_made_dir(vol, t.did, t.name, result_did, sent);
    } else if (ret == EEXIST) {
        dir_cache_invalidate(vol, t.did, t.name, 0);
    } else {
        dir_cache_remove(vol, t.did, t.name);
    }

    if (ret == 0) {
        set_created_mode(vol, &t, mode);
    }

    return -ret;
}

/*!
 * @brief Close an open fork and take it off the volume's open-fork list
 *
 * A flush_on_close mount flushes a written fork first. A handle that a
 * failed server-side copy left without a fork closes without a request.
 * The record is the caller's to free afterwards, whatever the result.
 *
 * @param[in]     volume  volume of the fork
 * @param[in,out] fp      the open fork, or NULL
 * @returns the flush's result, 0 without one, or a negative errno
 */
int ml_close(struct afp_volume * volume, struct afp_file_info * fp)
{
    int ret = 0;
    int synced = 0;

    /* The logic here is that if we don't have an fp anymore, then the
       fork must already be closed. */
    if (!fp) {
        return -EBADF;
    }

    if (fp->resource) {
        return appledouble_close(volume, fp);
    }

    /* Fork 0: a failed server-side copy leaves the handle without a fork. */
    if (fp->forkid == 0) {
        return 0;
    }

    synced = ml_close_flush(volume, fp);

    switch (afp_closefork(volume, fp->forkid)) {
    case kFPNoErr:
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        break;
    }

    remove_opened_fork(volume, fp);
    return ret ? -ret : synced;
}

/* The write speed of the slowest disk the server flushes or copies to. */
#define AFP_DISK_BYTES_PER_SECOND (1024U * 1024U)

/*!
 * @brief Flush an open data or resource fork to the server's disk
 *
 * From AFP 3.1 it sends FPSyncFork, and FPFlushFork below that or to a
 * server that refuses FPSyncFork. A pseudo-file other than a resource fork
 * has nothing to flush.
 *
 * @param[in]     volume  volume of the fork
 * @param[in,out] fp      the open fork, dirty again if the flush fails or a
 *                        write lands during it
 * @returns 0, -EACCES when the server denies access, or -EIO for any other
 *          failure, also for a handle without a fork
 */
int ml_fsync(struct afp_volume * volume, struct afp_file_info * fp)
{
    unsigned char was_dirty = fp->dirty;
    int wait;
    int rc;

    if (fp->resource > AFP_META_RESOURCE) {
        return 0;
    }

    if (fp->forkid == 0) {
        return -EIO;
    }

    /* A write that completes during the flush marks the fork again. */
    fp->dirty = 0;
    /* The reply follows the server's fsync(2) of what the fork holds. */
    wait = afpc_dsi_transfer_wait(volume->server, fp->size,
                                  AFP_DISK_BYTES_PER_SECOND);
    /* A server without FPSyncFork answers kFPCallNotSupported. */
    rc = volume->server->using_version->av_number >= 31
         ? afp_syncfork(volume, fp->forkid, wait) : kFPCallNotSupported;

    if (rc == kFPCallNotSupported) {
        rc = afp_flushfork(volume, fp->forkid, wait);
    }

    /* Netatalk dates a written resource fork at its flush. */
    if (was_dirty && fp->resource == AFP_META_RESOURCE) {
        ll_fork_changed(volume, fp);
    }

    if (rc == kFPNoErr) {
        return 0;
    }

    if (was_dirty) {
        fp->dirty = 1;
    }

    return rc == kFPAccessDenied ? -EACCES : -EIO;
}

/*!
 * @brief Flush a written fork as its file closes, on a flush_on_close mount
 *
 * @param[in]     volume  volume of the fork
 * @param[in,out] fp      the open fork
 * @returns 0 with nothing to flush, otherwise the result of ml_fsync()
 */
int ml_close_flush(struct afp_volume * volume, struct afp_file_info * fp)
{
    if (!fp->dirty
            || !(volume->extra_flags & VOLUME_EXTRA_FLAGS_FLUSH_ON_CLOSE)) {
        return 0;
    }

    return ml_fsync(volume, fp);
}

/*!
 * @brief Present a resolved object's record as its attributes
 *
 * The volume's root shows the mount's time.
 *
 * @param[in]     volume  volume of the object
 * @param[in]     t       the object's directory and name
 * @param[in,out] e       its record
 * @param[out]    stbuf   receives the attributes
 * @returns 0, or a negative errno, as afp_dirent_to_stat() gives it
 */
int ml_dirent_to_stat(struct afp_volume *volume, const struct ml_target *t,
                      struct afp_dirent *e, struct stat *stbuf)
{
    if (t->did == AFP_ROOT_DID && t->name[0] == '\0') {
        e->create_ns = afp_ns_from_sec(volume->mount_time);
        e->mod_ns = e->create_ns;
        e->valid |= kFPCreateDateBit | kFPModDateBit;
    }

    return afp_dirent_to_stat(volume, e, stbuf);
}

/*!
 * @brief Read the attributes of the object at path
 *
 * A pseudo-file's come from its file's record as appledouble_getattr()
 * reads it, whatever fresh says, and show no Finder info.
 *
 * @param[in]  volume      volume of the object
 * @param[in]  path        UTF-8 path, of a file, a directory or a
 *                         pseudo-file
 * @param[in]  fresh       1 to ask the server, 0 to take the cache's
 *                         record while its parameters hold
 * @param[out] stbuf       receives the attributes
 * @param[out] finderinfo  receives the 32 bytes of Finder info of the
 *                         record the attributes come from, or NULL
 * @returns 0, or a negative errno
 */
static int read_attributes(struct afp_volume *volume, const char *path,
                           int fresh, struct stat *stbuf, char *finderinfo)
{
    struct ml_target t;
    struct afp_dirent e;
    int ret;
    memset(stbuf, 0, sizeof(struct stat));

    if (finderinfo) {
        memset(finderinfo, 0, sizeof(e.finderinfo));
    }

    ret = appledouble_getattr(volume, path, stbuf);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    ret = ml_entry(volume, path, fresh ? ML_ENTRY_FRESH : ML_ENTRY_CACHED,
                   &t, &e);

    if (ret) {
        return ret;
    }

    if (finderinfo) {
        memcpy(finderinfo, e.finderinfo, sizeof(e.finderinfo));
    }

    return ml_dirent_to_stat(volume, &t, &e, stbuf);
}

/*!
 * @brief Read the attributes of the object at path, from the cache while
 * it holds them
 *
 * @param[in]  volume  volume of the object
 * @param[in]  path    UTF-8 path, of a file, a directory or a pseudo-file
 * @param[out] stbuf   receives the attributes
 * @returns 0, or a negative errno
 */
int ml_getattr(struct afp_volume * volume, const char *path, struct stat *stbuf)
{
    return read_attributes(volume, path, 0, stbuf, NULL);
}

/*!
 * @brief Read the attributes of the object at path and the Finder info of
 * the record they come from, from the cache while it holds them
 *
 * A stat shows the invisible flag of the record it presents; the bytes a
 * write sets back come from ml_getfinderinfo().
 *
 * @param[in]  volume      volume of the object
 * @param[in]  path        UTF-8 path, of a file, a directory or a
 *                         pseudo-file
 * @param[out] stbuf       receives the attributes
 * @param[out] finderinfo  receives the 32 bytes, all zero for a
 *                         pseudo-file
 * @returns 0, or a negative errno
 */
int ml_getattr_finderinfo(struct afp_volume *volume, const char *path,
                          struct stat *stbuf, char *finderinfo)
{
    return read_attributes(volume, path, 0, stbuf, finderinfo);
}

/*!
 * @brief Read the attributes of a file or a directory from the server
 *
 * A pseudo-file's come from its file's record as ml_getattr() takes it.
 *
 * @param[in]  volume  volume of the object
 * @param[in]  path    UTF-8 path, of a file, a directory or a pseudo-file
 * @param[out] stbuf   receives the attributes
 * @returns 0, or a negative errno
 */
int ml_getattr_fresh(struct afp_volume *volume, const char *path,
                     struct stat *stbuf)
{
    return read_attributes(volume, path, 1, stbuf, NULL);
}

/*!
 * @brief Write to an open fork
 *
 * @param[in]     volume  volume of the fork
 * @param[in]     data    bytes to write
 * @param[in]     size    length of data
 * @param[in]     offset  byte offset to write at
 * @param[in]     append  1 for an append, which goes to the fork's end on
 *                        the server; the Finder info's and the comment's
 *                        pseudo-files write at offset
 * @param[in,out] fp      the open fork, or an AppleDouble pseudo-file
 * @returns bytes written, or a negative errno
 */
int ml_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset, int append,
             struct afp_file_info * fp)
{
    int ret;
    size_t totalwritten = 0;

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    /* The view's resource fork is the fork itself, so an append goes to
     * the fork's end, as the view's length may come from the cache. */
    if (fp->resource == AFP_META_RESOURCE) {
        ret = ll_write(volume, data, size, offset, append, fp, &totalwritten);
        return ret < 0 ? ret : (int) totalwritten;
    }

    ret = appledouble_write(volume, fp, data, size, offset, &totalwritten);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return totalwritten;
    }

    ret = ll_write(volume, data, size, offset, append, fp, &totalwritten);
    return ret < 0 ? ret : (int) totalwritten;
}

/* The offload sends seven requests where a copy through the client sends
 * two. Below about 2.5 times a link's bandwidth-delay product, the five
 * extra round trips cost more than the data's two crossings. That is about
 * 400 KiB on loopback, and 500 KB on 400 Mbit/s Wi-Fi with 4 ms round trips. */
#define AFP_COPY_OFFLOAD_MIN_BYTES (512U * 1024U)
/* The cap counts both forks and applies where a long command drops the
 * session. On a server that copies faster than about 9 MB/s, 1 GiB stays
 * inside afpd's idle limit, 2 minutes by default. */
#define AFP_COPY_OFFLOAD_MAX_BYTES (1024ULL * 1024 * 1024)
/* A copy through the client returns after this count of quanta, so one call
 * is a bounded run of requests. The caller continues from there. */
#define AFP_COPY_QUANTA_PER_CALL 16U
/* A copy inherits these attributes, which would refuse the reopen for
 * writing, a rename, or a delete of it. */
#define AFP_COPY_INHIBIT_ATTRIBUTES \
    (kFPWriteInhibitBit | kFPRenameInhibitBit | kFPDeleteInhibitBit)

/*!
 * @brief Read the file at a fork's name and test whether it is the fork's
 *
 * FPCopyFile and FPDelete go by name, so the file there must have the node
 * ID the fork's open reported; the copy's requests use the names read here.
 *
 * @param[in]  volume  volume of the fork
 * @param[in]  fp      the open fork
 * @param[in]  bitmap  file parameters to read, with the node ID added
 * @param[out] params  the parameters of the file at the name
 * @param[out] t       the fork's directory and names
 * @returns 1 when the name holds the file fp opened, 0 otherwise
 */
static int fork_still_named(struct afp_volume *volume,
                            const struct afp_file_info *fp,
                            unsigned int bitmap, struct afp_dirent *params,
                            struct ml_target *t)
{
    unsigned int cnid = fork_names(volume, fp, t);
    return cnid != 0
           && afp_getfiledirparms(volume, t->did, bitmap | kFPNodeIDBit, 0,
                                  t->afp_name, params) == kFPNoErr
           && params->cnid == cnid;
}

/*!
 * @brief Leave a handle without a fork
 *
 * The handle leaves the volume's open-fork list and keeps fork number 0, for
 * which the client sends no request. Its name may hold another file now.
 *
 * @param[in]     volume  volume of the fork
 * @param[in,out] fp      the handle
 * @returns -EIO
 */
static ssize_t copy_lost_fork(struct afp_volume *volume,
                              struct afp_file_info *fp)
{
    remove_opened_fork(volume, fp);
    fp->forkid = 0;
    dir_cache_remove(volume, fp->did, fp->name);
    return -EIO;
}

/*!
 * @brief Replace the empty destination with FPCopyFile's copy and reopen it
 *
 * When the server makes no copy, the handle ends up open on an empty file at
 * the name, its own or a new one, for the copy through the client.
 *
 * @param[in]     volume  volume of both forks
 * @param[in,out] fp_out  the destination's open data fork, reopened here
 * @param[in]     in      the source's directory and names
 * @param[in]     out     the destination's
 * @param[in]     src     the source's parameters
 * @param[in,out] dst     the destination's parameters, whose mode and time
 *                        the new file takes
 * @returns the bytes copied, 0 when the server made no copy, or -EIO with
 *          the handle left without a fork
 */
static ssize_t copy_into_destination(struct afp_volume *volume,
                                     struct afp_file_info *fp_out,
                                     struct ml_target *in,
                                     struct ml_target *out,
                                     const struct afp_dirent *src,
                                     struct afp_dirent *dst)
{
    unsigned short bitmap;
    int deleted;
    int exists = 0;
    int rc;

    if (afp_closefork(volume, fp_out->forkid) != kFPNoErr) {
        return copy_lost_fork(volume, fp_out);
    }

    fp_out->forkid = 0;
    rc = afp_delete(volume, out->did, out->afp_name);
    deleted = rc == kFPNoErr;

    if (deleted) {
        rc = afp_copyfile(volume, in->did, in->afp_name, out->did,
                          out->afp_name,
                          afpc_dsi_transfer_wait(volume->server,
                                                 src->size + src->resourcesize,
                                                 AFP_DISK_BYTES_PER_SECOND));
    }

    /* The name holds a file, never truncated. It is a copy whose reply timed
     * out, the file a refused delete left, or a new empty one for a copy
     * through the client. */
    if (rc != kFPNoErr) {
        switch (afp_createfile(volume, kFPSoftCreate, out->did,
                               out->afp_name)) {
        case kFPNoErr:
            break;

        case kFPObjectExists:
            exists = 1;
            break;

        default:
            return copy_lost_fork(volume, fp_out);
        }
    }

    /* A reply without the node ID leaves 0, never the deleted file's. */
    fp_out->cnid = 0;

    /* The fork opens read-write, as every writable fork does. A mode without
     * the owner's write permission would refuse that open. */
    if (afp_openfork(volume, 0, out->did, ll_write_access(volume),
                     kFPNodeIDBit | kFPExtDataForkLenBit, out->afp_name,
                     fp_out) != kFPNoErr) {
        return copy_lost_fork(volume, fp_out);
    }

    /* After the delete, a file found is a late copy or another client's.
     * Without the delete, the name must still hold this handle's file. */
    if (exists) {
        if (deleted && rc == ETIMEDOUT && fp_out->size == src->size) {
            rc = kFPNoErr;
        } else if (deleted || fp_out->cnid != dst->cnid) {
            afp_closefork(volume, fp_out->forkid);
            return copy_lost_fork(volume, fp_out);
        }
    }

    /* A new file at the name keeps the destination's mode and takes the time
     * of the copy, as a newly written file would. */
    if (deleted || !exists) {
        bitmap = dst->unixprivs.permissions ? kFPUnixPrivsBit : 0;

        if (rc == kFPNoErr) {
            bitmap |= kFPModDateBit;
            dst->mod_ns = afp_ns_from_sec(time(NULL));
        }

        if (bitmap && afp_setfiledirparms(volume, out->did, out->afp_name,
                                          bitmap, dst) != kFPNoErr) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "copy to %s kept the server's mode or date",
                           out->afp_name);
        }
    }

    /* The name holds a new file, or the destination as it was. */
    dir_cache_remove(volume, out->did, out->name);

    if (rc != kFPNoErr) {
        return 0;
    }

    fp_out->dirty = 1;
    return (ssize_t) src->size;
}

/*!
 * @brief Copy a whole data fork on the server with FPCopyFile
 *
 * Holds the volume's open_lock for writing from the check of the names until
 * the destination's fork is reopened or lost.
 *
 * @param[in]     volume   volume of both forks
 * @param[in]     fp_in    the source's open fork
 * @param[in]     off_in   offset in the source, 0 to qualify
 * @param[in,out] fp_out   the destination's open fork
 * @param[in]     off_out  offset in the destination, 0 to qualify
 * @param[in]     len      bytes to copy, the source's whole data fork
 * @returns the bytes copied, 0 with the fork open when the server made no
 *          copy, or -EIO with the handle left without a fork
 */
static ssize_t copy_range_on_server(struct afp_volume *volume,
                                    struct afp_file_info *fp_in,
                                    off_t off_in,
                                    struct afp_file_info *fp_out,
                                    off_t off_out, size_t len)
{
    unsigned int unix_privs = volume->extra_flags
                              & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
    uint64_t cap = afp_server_drops_long_commands(volume->server)
                   ? AFP_COPY_OFFLOAD_MAX_BYTES : UINT64_MAX;
    struct afp_dirent src;
    struct afp_dirent dst;
    struct ml_target in;
    struct ml_target out;
    ssize_t copied;

    if (fp_in->resource != AFP_META_NONE || fp_out->resource != AFP_META_NONE
            || fp_in->forkid == 0 || fp_out->forkid == 0 || !fp_out->created
            || fp_out->size != 0 || off_in != 0 || off_out != 0
            || len < AFP_COPY_OFFLOAD_MIN_BYTES || len > cap
            || !afp_server_copies_files(volume->server)) {
        return 0;
    }

    /* Every ll_open() in flight has listed its record once this returns. */
    pthread_rwlock_wrlock(&volume->open_lock);

    /* A source whose length differs from len goes through the client, as it
     * may be another file or a changed fork, and so does a destination the
     * copy's reopen may not read and write. The cap counts both forks. */
    if (other_fork_open(volume, fp_out)
            || !fork_still_named(volume, fp_in,
                                 kFPAttributeBit | kFPExtDataForkLenBit
                                 | kFPExtRsrcForkLenBit, &src, &in)
            || src.size != len
            || src.resourcesize > cap - src.size
            || (src.attributes & AFP_COPY_INHIBIT_ATTRIBUTES)
            || !fork_still_named(volume, fp_out,
                                 kFPExtDataForkLenBit | kFPExtRsrcForkLenBit
                                 | (unix_privs ? kFPUnixPrivsBit : 0),
                                 &dst, &out)
            || dst.size != 0 || dst.resourcesize != 0
            || (unix_privs && (~dst.unixprivs.ua_permissions
                               & (kUserCanRead | kUserCanWrite)))) {
        copied = 0;
    } else {
        copied = copy_into_destination(volume, fp_out, &in, &out, &src, &dst);
    }

    pthread_rwlock_unlock(&volume->open_lock);
    return copied;
}

/*!
 * @brief Copy through the client, a read and a write per quantum
 *
 * One call copies up to sixteen quanta. The caller continues from there.
 *
 * @param[in]     volume   volume of both forks
 * @param[in]     fp_in    the source's open fork
 * @param[in]     off_in   offset in the source
 * @param[in,out] fp_out   the destination's open fork
 * @param[in]     off_out  offset in the destination
 * @param[in]     len      the cap on bytes to copy
 * @returns the bytes copied if any, else the read's or the write's error, or
 *          0 at the source's end
 */
static ssize_t copy_range_through_client(struct afp_volume *volume,
                                         struct afp_file_info *fp_in,
                                         off_t off_in,
                                         struct afp_file_info *fp_out,
                                         off_t off_out, size_t len)
{
    unsigned long long quantum = volume->server->quantum;
    size_t copied = 0;
    char *buf;
    int eof = 0;
    int ret = 0;
    /* The length stays within INT_MAX, inside the 32-bit count of the
     * caller's reply. */
    len = min(len, min((unsigned long long) INT_MAX,
                       AFP_COPY_QUANTA_PER_CALL * quantum));

    if ((buf = malloc(min(len, quantum))) == NULL) {
        return -ENOMEM;
    }

    while (copied < len && !eof) {
        ret = ml_read(volume, buf, min(len - copied, quantum),
                      off_in + (off_t) copied, fp_in, &eof);

        if (ret <= 0) {
            break;
        }

        ret = ml_write(volume, buf, (size_t) ret, off_out + (off_t) copied,
                       0, fp_out);

        if (ret <= 0) {
            break;
        }

        copied += (size_t) ret;
    }

    free(buf);
    return copied ? (ssize_t) copied : ret;
}

/*!
 * @brief Copy up to len bytes from one open fork to another
 *
 * A qualifying whole data fork copies on the server with FPCopyFile, the rest
 * through the client; an O_SYNC or O_DSYNC destination ends with a flush.
 *
 * @param[in]     volume   volume of both forks
 * @param[in]     fp_in    the source's open fork
 * @param[in]     off_in   offset in the source
 * @param[in,out] fp_out   the destination's open fork, opened for writing
 * @param[in]     off_out  offset in the destination
 * @param[in]     len      the cap on bytes to copy
 * @returns the bytes copied, 0 at the source's end, -EOPNOTSUPP for a
 *          pseudo-file other than a resource fork, or another negative errno
 */
ssize_t ml_copy_range(struct afp_volume * volume,
                      struct afp_file_info *fp_in, off_t off_in,
                      struct afp_file_info *fp_out, off_t off_out,
                      size_t len)
{
    ssize_t copied;

    /* Pseudo-files other than a resource fork have no fork to copy. Linux
     * copies them itself. */
    if (fp_in->resource > AFP_META_RESOURCE
            || fp_out->resource > AFP_META_RESOURCE) {
        return -EOPNOTSUPP;
    }

    if (!fp_out->writable || volume_is_readonly(volume)) {
        return -EBADF;
    }

    if (len == 0) {
        return 0;
    }

    copied = copy_range_on_server(volume, fp_in, off_in, fp_out, off_out,
                                  len);

    if (copied == 0) {
        copied = copy_range_through_client(volume, fp_in, off_in, fp_out,
                                           off_out, len);
    }

    /* A copy into an O_SYNC or O_DSYNC file ends with a flush, as the
     * kernel's writes to one do. */
    if (copied > 0 && fp_out->sync) {
        int rc = ml_fsync(volume, fp_out);

        if (rc < 0) {
            return rc;
        }
    }

    return copied;
}

/*!
 * @brief Give a converted link target back the server's separators
 *
 * convert_path_to_unix() converts one name and turns a '/' in it into ':'.
 * No composition takes a ':' or a '/', so the conversion keeps a valid
 * target's ':' and '/' in their order, and the k-th ':' of dest, which
 * stands for the k-th ':' or '/' of src, takes back the '/' it stood for.
 * A malformed sequence can take a separator into its replacement; dest
 * then has fewer ':' than src has separators and keeps them all.
 *
 * @param[in,out] dest  the converted target
 * @param[in]     src   the target as the server holds it
 */
static void restore_link_separators(char *dest, const char *src)
{
    size_t colons = 0;
    size_t separators = 0;

    for (const char *d = dest; *d; d++) {
        colons += *d == ':';
    }

    for (const char *s = src; *s; s++) {
        separators += *s == ':' || *s == '/';
    }

    if (colons != separators) {
        return;
    }

    for (char *d = dest; *d; d++) {
        if (*d != ':') {
            continue;
        }

        while (*src && *src != ':' && *src != '/') {
            src++;
        }

        if (*src == '/') {
            *d = '/';
        }

        if (*src) {
            src++;
        }
    }
}

/*!
 * @brief Read the target of the symbolic link at path
 *
 * afpd reads a link's target whole, so one longer than buf fails; one whose
 * precomposed UTF-8 form fills buf gives its last byte to the terminator.
 *
 * @param[in]  vol   volume of the link
 * @param[in]  path  UTF-8 path of the link
 * @param[out] buf   receives the target with its terminator
 * @param[in]  size  capacity of buf, at least 1
 * @returns 0, or a negative errno, -ENAMETOOLONG for a size of 0 or above
 *          INT_MAX
 */
int ml_readlink(struct afp_volume * vol, const char *path,
                char *buf, size_t size)
{
    int rc, ret;
    struct afp_file_info fp;
    struct afp_rx_buffer buffer;
    struct ml_target t;
    char *link_path;

    if (size == 0 || size == SIZE_MAX || size > INT_MAX) {
        return -ENAMETOOLONG;
    }

    link_path = calloc(size + 1U, 1U);

    if (!link_path) {
        return -ENOMEM;
    }

    memset(buf, 0, size);
    memset(&fp, 0, sizeof(fp));
    buffer.data = link_path;
    buffer.maxsize = size;
    buffer.size = 0;
    ret = ml_resolve(vol, path, &t);

    if (ret) {
        free(link_path);
        return ret;
    }

    /* Open the fork to read it: a read-only volume refuses a write open. */
    rc = afp_openfork(vol, 0, t.did, AFP_OPENFORK_ALLOWREAD, 0, t.afp_name,
                      &fp);
    ret = ll_openfork_errno(rc);

    /* No link holds the name, or a folder does: the record goes. */
    if (ret == ENOENT || ret == EISDIR) {
        dir_cache_remove(vol, t.did, t.name);
    }

    if (ret) {
        goto error;
    }

    add_opened_fork(vol, &fp);

    if (vol->server->using_version->av_number < 30) {
        rc = afp_read(vol, fp.forkid, 0, size, &buffer);
    } else {
        rc = afp_readext(vol, fp.forkid, 0, size, &buffer);
    }

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        goto close_error;

    case kFPLockErr:
        ret = EBUSY;
        goto close_error;

    case kFPEOFErr:
    case kFPNoErr:
        break;

    case kFPMiscErr:
    case kFPParamErr:
    default:
        /* A request not sent or not answered is never a success either. */
        ret = EIO;
        goto close_error;
    }

    switch (afp_closefork(vol, fp.forkid)) {
    case kFPNoErr:
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        remove_opened_fork(vol, &fp);
        goto error;
    }

    remove_opened_fork(vol, &fp);
    convert_path_to_unix(vol->path_encoding,
                         buf, link_path, (int)size);

    if (!buf[0] && link_path[0]) {
        /* A target whose converted form fills buf gives its last byte to
         * the terminator, and a UTF-8 target the character the cut would
         * split. */
        size_t cut = size - 1;

        while (vol->path_encoding == kFPUTF8Name && cut > 0
                && ((unsigned char) link_path[cut] & 0xC0) == 0x80) {
            cut--;
        }

        link_path[cut] = '\0';
        convert_path_to_unix(vol->path_encoding,
                             buf, link_path, (int)size);
    }

    restore_link_separators(buf, link_path);
    free(link_path);
    return 0;
close_error:
    afp_closefork(vol, fp.forkid);
    remove_opened_fork(vol, &fp);
error:
    free(link_path);
    return -ret;
}

/*!
 * @brief Delete the empty directory at path with FPDelete
 *
 * @param[in] vol   volume of the directory
 * @param[in] path  UTF-8 path of the directory
 * @returns 0, or a negative errno: -ENOTDIR for a name that is no directory,
 *          the lookups' errno when they fail, -ENOTEMPTY for a directory
 *          with entries, -EROFS for a read-only filesystem, -EPERM for a
 *          locked directory, -EBUSY for one in use elsewhere, -EIO for an
 *          unexpected answer or a request without one
 */
int ml_rmdir(struct afp_volume * vol, const char *path)
{
    struct ml_target t;
    struct afp_dirent e;
    int ret;

    if (volume_is_readonly(vol)) {
        return -EROFS;
    }

    ret = appledouble_rmdir(vol, path);

    if (ret) {
        return ret < 0 ? ret : 0;
    }

    /* Fresh: FPDelete would delete a file put where a cached folder stood. */
    ret = ml_entry(vol, path, ML_ENTRY_FRESH, &t, &e);

    if (ret) {
        return ret;
    }

    if (!e.isdir) {
        return -ENOTDIR;
    }

    return delete_target(vol, &t);
}

/*!
 * @brief Set the owner and group of a file or a directory
 *
 * @param[in] vol   volume of the object
 * @param[in] path  UTF-8 path of the object
 * @param[in] uid   the owner, or -1 to keep it
 * @param[in] gid   the group, or -1 to keep it
 * @returns 0, or a negative errno, as ml_setattr()
 */
int ml_chown(struct afp_volume * vol, const char *path,
             uid_t uid, gid_t gid)
{
    return ml_setattr(vol, path,
                      (uid != (uid_t) -1 ? ML_SET_UID : 0)
                      | (gid != (gid_t) -1 ? ML_SET_GID : 0),
                      0, uid, gid, 0);
}

/*!
 * @brief Set the length of the file at path
 *
 * The data fork opens for writing, takes the length and closes, flushed as
 * a write would be. The path of a resource fork sets that fork's length.
 *
 * @param[in] vol     volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] offset  the new length
 * @returns 0, or a negative errno, the open's when it fails, -EINVAL for a
 *          negative length
 */
int ml_truncate(struct afp_volume * vol, const char *path, off_t offset)
{
    struct afp_file_info *fp;
    int close_ret;
    int ret;

    if (offset < 0) {
        return -EINVAL;
    }

    if (volume_is_readonly(vol)) {
        return -EROFS;
    }

    ret = appledouble_truncate(vol, path, offset);

    if (ret != 0) {
        return ret < 0 ? ret : 0;
    }

    ret = ml_open(vol, path, O_WRONLY, &fp);

    if (ret == 0) {
        ret = ml_ftruncate(vol, fp, offset);
        close_ret = ml_close(vol, fp);
        free(fp);

        if (ret == 0) {
            ret = close_ret;
        }
    }

    return ret;
}

/*!
 * @brief Set the length of an open handle's fork or AppleDouble pseudo-file
 *
 * A fork is then dirty, as after a write; appledouble_ftruncate() sets a
 * pseudo-file's.
 *
 * @param[in]     vol     volume of the fork
 * @param[in,out] fp      the open fork
 * @param[in]     offset  the new length
 * @returns 0, or a negative errno, -EIO for a handle without a fork, -EPERM
 *          for the server icon, -EINVAL for a negative length
 */
int ml_ftruncate(struct afp_volume * vol, struct afp_file_info * fp,
                 off_t offset)
{
    int ret;

    if (offset < 0) {
        return -EINVAL;
    }

    ret = appledouble_ftruncate(vol, fp, offset);

    if (ret != 0) {
        return ret < 0 ? ret : 0;
    }

    if (fp->forkid == 0) {
        return -EIO;
    }

    ret = -ll_setfork_size(vol, fp, offset);

    if (ret == 0) {
        fp->size = offset;
        fp->dirty = 1;
    }

    return ret;
}


/*!
 * @brief Set the modification time of a file or a directory
 *
 * @param[in] vol      volume of the object
 * @param[in] path     UTF-8 path of the object
 * @param[in] timebuf  the times, of which the modification time is set
 * @returns 0, or a negative errno, as ml_setattr()
 */
int ml_utime(struct afp_volume * vol, const char *path,
             struct utimbuf * timebuf)
{
    return ml_setattr(vol, path, ML_SET_MTIME, 0, 0, 0, timebuf->modtime);
}


/*!
 * @brief Create a symbolic link at path2 that points to path1
 *
 * The link is a file whose data fork holds the target and whose Finder
 * information carries type slnk and creator rhap. AFP 2 has no links.
 *
 * @param[in] vol    volume of the link
 * @param[in] path1  the link's target
 * @param[in] path2  UTF-8 path of the new link
 * @returns 0, or a negative errno, -ENOSYS below AFP 3
 */
int ml_symlink(struct afp_volume *vol, const char *path1, const char *path2)
{
    int ret;
    struct afp_file_info fp;
    struct afp_dirent marker;
    uint64_t written;
    size_t converted_path1_len;
    int rc;
    struct ml_target t;
    int created = 0;
    char *converted_path1 = NULL;

    if (vol->server->using_version->av_number < 30) {
        /* No symlinks for AFP 2.x. */
        return -ENOSYS;
    }

    memset(&fp, 0, sizeof(fp));
    /* The target is stored as the server reads a path. */
    ret = convert_path_to_afp_alloc(vol->path_encoding, path1,
                                    &converted_path1);

    if (ret) {
        return ret;
    }

    converted_path1_len = strlen(converted_path1);

    if (volume_is_readonly(vol)) {
        free(converted_path1);
        return -EROFS;
    }

    ret = appledouble_symlink(vol, path1, path2);

    if (ret) {
        free(converted_path1);
        return ret < 0 ? ret : 0;
    }

    ret = ml_resolve(vol, path2, &t);

    if (ret) {
        free(converted_path1);
        return ret;
    }

    ret = -ll_createfile(vol, t.did, t.afp_name);

    if (ret) {
        goto error;
    }

    created = 1;
    rc = afp_openfork(vol, 0, t.did, ll_write_access(vol), 0, t.afp_name,
                      &fp);
    ret = ll_openfork_errno(rc);

    /* The server may still open the fork, and a deny conflict means another
     * client holds the file open, so the link's file stays. */
    if (ret == ETIMEDOUT || ret == EBUSY) {
        created = 0;
    }

    if (ret) {
        goto error;
    }

    add_opened_fork(vol, &fp);
    rc = afp_writeext(vol, fp.forkid, 0, 0, converted_path1_len,
                      converted_path1, &written);

    switch (rc) {
    case kFPNoErr:
        if (written != converted_path1_len) {
            ret = EIO;
            goto close_error;
        }

        break;

    case kFPAccessDenied:
        ret = EACCES;
        goto close_error;

    case kFPDiskFull:
        ret = ENOSPC;
        goto close_error;

    case kFPObjectNotFound:
        ret = ENOENT;
        goto close_error;

    default:
        ret = EIO;
        goto close_error;
    }

    rc = afp_closefork(vol, fp.forkid);
    remove_opened_fork(vol, &fp);

    if (rc != kFPNoErr) {
        ret = EIO;
        goto error;
    }

    memset(&marker, 0, sizeof(marker));
    memcpy(marker.finderinfo, ML_SYMLINK_FINDERINFO, 8);
    ret = -set_parameters(vol, t.did, t.afp_name, kFPFinderInfoBit, &marker);
error:

    /* A link the steps after the create could not finish leaves no file
     * in its name, unless its open went unanswered or another client holds
     * the file open. */
    if (ret && created) {
        afp_delete(vol, t.did, t.afp_name);
    }

    /* The name holds the link, another object, or nothing again. */
    dir_cache_remove(vol, t.did, t.name);
    free(converted_path1);
    return -ret;
close_error:
    afp_closefork(vol, fp.forkid);
    remove_opened_fork(vol, &fp);
    afp_delete(vol, t.did, t.afp_name);
    dir_cache_remove(vol, t.did, t.name);
    free(converted_path1);
    return -ret;
}

/*!
 * @brief Map the answer of an FPMoveAndRename to an errno
 *
 * @param[in] rc  the answer, or the request's failure
 * @returns 0 for kFPNoErr, or a positive errno: EACCES for a locked
 *          object, EROFS for a name the server cannot change, or
 *          ll_answer_errno()'s
 */
static int move_errno(int rc)
{
    switch (rc) {
    case kFPObjectLocked:
        return EACCES;

    case kFPCantRename:
        return EROFS;

    default:
        return ll_answer_errno(rc);
    }
}

/*!
 * @brief Carry a rename into the open forks and the cache
 *
 * The forks go first, so a write answered meanwhile forgets the new name.
 *
 * @param[in,out] vol   volume of the object
 * @param[in]     from  the directory and name the object had
 * @param[in]     to    the directory and name it has
 * @param[in]     cnid  its node ID the cache held before the move, or 0
 * @param[in]     sent  dir_cache_changes() before the move was sent
 */
static void rename_done(struct afp_volume *vol, const struct ml_target *from,
                        const struct ml_target *to, unsigned int cnid,
                        uint64_t sent)
{
    rename_opened_forks(vol, from, to);
    dir_cache_rename(vol, from->did, from->name, to->did, to->name, cnid,
                     sent);
}

/*!
 * @brief Carry an exchange's answer into the open forks and the cache
 *
 * The names keep their node IDs over swapped contents, so every answer
 * forgets both. The forks lose their node IDs, and swap names when done.
 *
 * @param[in,out] vol  volume of the files
 * @param[in]     rc   the exchange's answer, or its request's failure
 * @param[in]     a    one file's directory and name
 * @param[in]     b    the other's
 */
static void exchange_outcome(struct afp_volume *vol, int rc,
                             const struct ml_target *a,
                             const struct ml_target *b)
{
    if (rc == kFPNoErr || ll_unanswered(rc)) {
        exchange_opened_forks(vol, a, b, rc == kFPNoErr);
    }

    dir_cache_remove(vol, a->did, a->name);
    dir_cache_remove(vol, b->did, b->name);
}

/*!
 * @brief Rename or move a file or folder with FPMoveAndRename
 *
 * A folder destination takes the object inside it, and a file one is replaced;
 * a rename done moves the entry and forks, any other answer forgets the names.
 *
 * @param[in] vol        volume of both paths
 * @param[in] path_from  UTF-8 path of the object
 * @param[in] path_to    UTF-8 path it takes
 * @param[in] excl       1 for an exclusive rename, which sends the move alone
 * @returns 0, or a negative errno
 */
static int rename_object(struct afp_volume *vol, const char *path_from,
                         const char *path_to, int excl)
{
    int ret, rc, del_rc;
    int dst_ret;
    /* The folder a move into a directory takes the object to, or 0 */
    unsigned int into = 0;
    /* The node ID the cache holds at the source's name, or 0 */
    unsigned int moved = 0;
    /* The count of changes before the move was sent */
    uint64_t sent;
    struct afp_dirent dst_info;
    struct ml_target from;
    struct ml_target to;

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = ml_resolve(vol, path_from, &from);

    if (ret < 0) {
        return ret;
    }

    ret = ml_resolve(vol, path_to, &to);

    if (ret < 0) {
        return ret;
    }

    if (dir_cache_lookup(vol, from.did, from.name, &dst_info)
            & DIR_CACHE_DID) {
        moved = dst_info.cnid;
    }

    memset(&dst_info, 0, sizeof(dst_info));
    /* The destination from the cache; an exclusive rename's move finds out
     * itself. */
    dst_ret = excl ? -ENOENT : read_entry(vol, &to, ML_ENTRY_CACHED,
                                          &dst_info);

    /* Another session may have taken the folder away. */
    if (dst_ret == 0 && dst_info.isdir) {
        dst_ret = read_entry(vol, &to, ML_ENTRY_FRESH, &dst_info);
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "ml_rename: from dirid=%u name=%s  to dirid=%u name=%s "
                   "dst=%d dst_isdir=%u", from.did, from.afp_name, to.did,
                   to.afp_name, dst_ret, dst_info.isdir);
    /* Held to the forks' walk: an open before it has listed its fork, and
     * one after sees the new names. */
    pthread_rwlock_wrlock(&vol->open_lock);
    sent = dir_cache_changes(vol);

    if (dst_ret == 0 && dst_info.isdir) {
        /* Into the directory, under the source's own name */
        into = dst_info.cnid;
        rc = afp_moveandrename(vol, from.did, to.did, from.afp_name,
                               to.afp_name, from.afp_name);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "ml_rename: afp_moveandrename rc=%d", rc);
    } else {
        /* When the destination is an existing file, use FPExchangeFiles if the
         * server supports it. This preserves the destination file's FileID
         * (which becomes the inode), preventing macOS from seeing a stale vnode
         * and reporting "file does not exist" after a safe-save rename.
         * After exchange, the source path holds the old content; delete it. */
        if (dst_ret == 0 && !(vol->attributes & kNoExchangeFiles)) {
            rc = afp_exchangefiles(vol, from.did, to.did, from.afp_name,
                                   to.afp_name);
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "ml_rename: afp_exchangefiles rc=%d", rc);

            if (rc == kFPNoErr) {
                del_rc = afp_delete(vol, from.did, from.afp_name);
                log_for_client(NULL, AFPFSD, LOG_DEBUG,
                               "ml_rename: afp_delete(src) rc=%d", del_rc);
                exchange_outcome(vol, rc, &from, &to);
                ret = 0;
                goto done;
            }

            /* An unanswered exchange may have been done; a move undoes it. */
            if (ll_unanswered(rc)) {
                exchange_outcome(vol, rc, &from, &to);
                ret = EIO;
                goto done;
            }

            /* Exchange failed (dst doesn't exist, or not supported); fall
             * through to regular moveandrename. */
        }

        rc = afp_moveandrename(vol, from.did, to.did, from.afp_name, NULL,
                               to.afp_name);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "ml_rename: afp_moveandrename rc=%d", rc);
    }

    ret = move_errno(rc);

    if (rc == kFPObjectExists && excl) {
        ret = EEXIST;
    } else if (rc == kFPObjectExists) {
        struct afp_dirent src;
        struct afp_dirent dst;
        int src_ret;
        int to_ret;

        /* The name inside the folder refused the move into it, so that name
         * goes and the move may go over the folder. */
        if (into) {
            dir_cache_remove(vol, into, from.name);
            into = 0;
            sent = dir_cache_changes(vol);
        }

        /* The probe may have read stale records, so both kinds come from
         * the server before any delete. */
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src_ret = dir_cache_fetch(vol, from.did, from.name, &src);
        to_ret = src_ret ? src_ret
                 : dir_cache_fetch(vol, to.did, to.name, &dst);

        if (src_ret || (to_ret && to_ret != -ENOENT)) {
            ret = -to_ret;
        } else if (to_ret == 0 && !src.isdir != !dst.isdir) {
            ret = src.isdir ? ENOTDIR : EISDIR;
        } else {
            if (to_ret == 0) {
                (void) afp_delete(vol, to.did, to.afp_name);
            }

            rc = afp_moveandrename(vol, from.did, to.did, from.afp_name,
                                   NULL, to.afp_name);
            ret = rc == kFPObjectExists ? ENOENT : move_errno(rc);
        }
    }

    if (ret == 0 && !into) {
        rename_done(vol, &from, &to, moved, sent);
        goto done;
    }

    /* The folder's ID is the cache's and may be stale, so the cache forgets
     * the names; the forks take the ID, which the copy offload checks. */
    if (ret == 0) {
        struct ml_target inside = from;
        inside.did = into;
        rename_opened_forks(vol, &from, &inside);
    }

    dir_cache_remove(vol, from.did, from.name);
    dir_cache_remove(vol, to.did, to.name);

    if (into) {
        dir_cache_remove(vol, into, from.name);
    }

done:
    pthread_rwlock_unlock(&vol->open_lock);
    return -ret;
}

/*!
 * @brief Rename or move a file or folder with FPMoveAndRename
 *
 * @param[in] vol        volume of both paths
 * @param[in] path_from  UTF-8 path of the object
 * @param[in] path_to    UTF-8 path it takes
 * @returns 0, or a negative errno
 */
int ml_rename(struct afp_volume * vol,
              const char *path_from, const char *path_to)
{
    return rename_object(vol, path_from, path_to, 0);
}

/*!
 * @brief Rename a file or folder to a name nothing holds
 *
 * @param[in] vol        volume of both paths
 * @param[in] path_from  UTF-8 path of the object
 * @param[in] path_to    UTF-8 path it takes
 * @returns 0, or a negative errno: -EEXIST for a name taken
 */
int ml_rename_excl(struct afp_volume *vol,
                   const char *path_from, const char *path_to)
{
    return rename_object(vol, path_from, path_to, 1);
}

/*!
 * @brief Swap two files' contents and parameters with FPExchangeFiles
 *
 * @param[in] vol        volume of both files
 * @param[in] path_from  UTF-8 path of one file
 * @param[in] path_to    UTF-8 path of the other
 * @returns 0, or a negative errno: -ENOSYS where the volume or the server
 *          has no exchange, -EACCES for a read-only volume or a refusal,
 *          -EISDIR for a folder, -ENOENT for a name that holds nothing or
 *          a stale node ID, -EIO for an unexpected answer or a request
 *          without one, or a resolve's error
 */
int ml_exchange(struct afp_volume * vol,
                const char *path_from, const char *path_to)
{
    int ret;
    int rc;
    struct ml_target from;
    struct ml_target to;

    if (vol->attributes & kNoExchangeFiles) {
        return -ENOSYS;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = ml_resolve(vol, path_from, &from);

    if (ret) {
        return ret;
    }

    ret = ml_resolve(vol, path_to, &to);

    if (ret) {
        return ret;
    }

    /* No ll_open() runs on the server between the exchange and its walk. */
    pthread_rwlock_wrlock(&vol->open_lock);
    rc = afp_exchangefiles(vol, from.did, to.did, from.afp_name,
                           to.afp_name);
    exchange_outcome(vol, rc, &from, &to);
    pthread_rwlock_unlock(&vol->open_lock);

    switch (rc) {
    case kFPNoErr:
        ret = 0;
        break;

    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPObjectTypeErr:
        ret = EISDIR;
        break;

    case kFPObjectNotFound:
    case kFPIDNotFound:
        ret = ENOENT;
        break;

    case kFPCallNotSupported:
        ret = ENOSYS;
        break;

    default:
        ret = EIO;
        break;
    }

    return -ret;
}

/*!
 * @brief Read a volume's size and free space for statfs(2)
 *
 * The file counts read 0, and a block size the server leaves 0 reads 4096.
 *
 * @param[in,out] vol   the volume, whose stat takes the server's figures
 * @param[in]     path  unused
 * @param[out]    stat  receives the figures
 * @returns 0, or -EIO for an error answer
 */
int ml_statfs(struct afp_volume * vol, const char *path _U_,
              struct statvfs *stat)
{
    unsigned short flags;
    int ret;
    memset(stat, 0, sizeof(*stat));

    if (vol->server->using_version->av_number < 30) {
        flags = kFPVolBytesFreeBit | kFPVolBytesTotalBit;
    } else {
        flags = kFPVolExtBytesFreeBit | kFPVolExtBytesTotalBit | kFPVolBlockSizeBit;
    }

    ret = afp_getvolparms(vol, flags);

    switch (ret) {
    case kFPNoErr:
        break;

    case kFPParamErr:
    case kFPMiscErr:
    default:
        return -EIO;
    }

    if (vol->stat.f_bsize == 0) {
        vol->stat.f_bsize = 4096;
    }

    stat->f_blocks = vol->stat.f_blocks / vol->stat.f_bsize;
    stat->f_bfree = vol->stat.f_bfree / vol->stat.f_bsize;
    stat->f_bsize = vol->stat.f_bsize;
    stat->f_frsize = vol->stat.f_bsize;
    stat->f_bavail = stat->f_bfree;
    stat->f_files = 0;
    stat->f_ffree = 0;
    stat->f_favail = 0;
    stat->f_fsid = 0;
    stat->f_flag = 0;
    stat->f_namemax = 255;
    return 0;
}

/*!
 * @brief Change a user's password with the session's UAM
 *
 * @param[in] server     the server
 * @param[in] username   the user
 * @param[in] oldpasswd  the current password
 * @param[in] newpasswd  the new one
 * @returns afp_dopasswd()'s result
 */
int ml_passwd(struct afp_server *server,
              char *username, char *oldpasswd, char *newpasswd)
{
    return afp_dopasswd(server, server->using_uam, username, oldpasswd, newpasswd);
}

/* Extended Attributes (AFP 3.2+) */

/*!
 * @brief Check a set's create or replace flag against the attribute's state
 *
 * @param[in] flags       the set's flags
 * @param[in] exists_ret  the attribute's read: its length, -EFBIG for one
 *                        past INT_MAX bytes, -ENOATTR for none, or another
 *                        error
 * @returns 0, -EINVAL for both flags, -EEXIST or -ENOATTR where the flags
 *          refuse the set, or the read's other error
 */
static int validate_special_xattr_flags(int flags, int exists_ret)
{
    int exists;

    if ((flags & kXAttrCreate) && (flags & kXAttrReplace)) {
        return -EINVAL;
    }

    if (exists_ret < 0 && exists_ret != -ENOATTR && exists_ret != -EFBIG) {
        return exists_ret;
    }

    exists = exists_ret >= 0 || exists_ret == -EFBIG;

    if ((flags & kXAttrCreate) && exists) {
        return -EEXIST;
    }

    if ((flags & kXAttrReplace) && !exists) {
        return -ENOATTR;
    }

    return 0;
}

/*!
 * @brief Check that a set's flags are kXAttrCreate, kXAttrReplace or neither
 *
 * @param[in] flags  the flags
 * @returns 0, or -EINVAL
 */
static int validate_xattr_flags(int flags)
{
    const int supported = kXAttrCreate | kXAttrReplace;

    if ((flags & ~supported) != 0
            || (flags & kXAttrCreate && flags & kXAttrReplace)) {
        return -EINVAL;
    }

    return 0;
}

/*!
 * @brief Add a name to a listxattr(2) list, under user. off macOS
 *
 * Without a list or a size, the name is only counted.
 *
 * @param[out]    list  the list
 * @param[in]     size  capacity of list
 * @param[in,out] used  the list's length, grown by the name's
 * @param[in]     name  the AFP name
 * @returns 0, -ENAMETOOLONG for a name past AFP_XATTR_NAME_MAX, -EOVERFLOW
 *          for a length past SIZE_MAX, or -ERANGE for a list too small
 */
static int append_listed_xattr_name(char *list, size_t size, size_t *used,
                                    const char *name)
{
    size_t namelen = strnlen(name, AFP_XATTR_NAME_MAX + 1U);
#ifdef __APPLE__
    size_t output_namelen = namelen;
#else
    size_t output_namelen = namelen + sizeof("user.") - 1U;
#endif

    if (namelen > AFP_XATTR_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    if (output_namelen > SIZE_MAX - 1U
            || *used > SIZE_MAX - output_namelen - 1U) {
        return -EOVERFLOW;
    }

    if (list && size > 0) {
        if (*used + output_namelen + 1U > size) {
            return -ERANGE;
        }

#ifdef __APPLE__
        memcpy(list + *used, name, namelen + 1U);
#else
        memcpy(list + *used, "user.", sizeof("user.") - 1U);
        memcpy(list + *used + sizeof("user.") - 1U, name, namelen + 1U);
#endif
    }

    *used += output_namelen + 1U;
    return 0;
}

/*!
 * @brief Read the record of a file's resource fork
 *
 * The cache answers a length question for a file it shows without a fork;
 * a held fork, a read, and a check before a change ask the server.
 *
 * @param[in]  volume  volume of the file
 * @param[in]  path    UTF-8 path of the file
 * @param[in]  fresh   1 to ask the server
 * @param[out] t       receives the resolved path
 * @param[out] e       receives the record
 * @returns 0, -ENOATTR for an empty fork, or a negative errno
 */
static int resource_fork_entry(struct afp_volume *volume, const char *path,
                               int fresh, struct ml_target *t,
                               struct afp_dirent *e)
{
    int ret = ml_entry(volume, path, fresh ? ML_ENTRY_FRESH : ML_ENTRY_FORK,
                       t, e);
    return ret == 0 && e->resourcesize == 0 ? -ENOATTR : ret;
}

/*!
 * @brief Read the resource fork of path, or its length
 *
 * @param[in]  volume    volume of the file
 * @param[in]  path      UTF-8 path of the file
 * @param[out] value     receives the bytes, or NULL
 * @param[in]  size      capacity of value; 0 asks for the length alone
 * @param[in]  position  offset in the fork of the first byte read
 * @returns the length or the count of bytes read, -ENOATTR for an empty
 *          fork, -EFBIG for a count past INT_MAX, -EINVAL for a NULL
 *          value with a size, or another negative errno
 */
int ml_getresourcefork(struct afp_volume * volume, const char *path,
                       void *value, size_t size, off_t position)
{
    struct afp_dirent e;
    struct ml_target t;
    struct afp_file_info fp;
    size_t total = 0;
    uint64_t remaining;
    int ret;

    if (!volume || !path || (!value && size > 0)) {
        return -EINVAL;
    }

    ret = resource_fork_entry(volume, path, size > 0, &t, &e);

    if (ret < 0) {
        return ret;
    }

    if (size == 0) {
        return e.resourcesize > INT_MAX ? -EFBIG : (int) e.resourcesize;
    }

    if (position < 0 || (uint64_t) position >= e.resourcesize) {
        return 0;
    }

    memset(&fp, 0, sizeof(fp));
    fp.resource = AFP_META_RESOURCE;
    fork_now_named(&fp, &t);
    ret = ll_open(volume, path, O_RDONLY, &fp);

    if (ret < 0) {
        return ret;
    }

    remaining = e.resourcesize - (uint64_t) position;

    while (total < size && remaining > 0) {
        int eof = 0;
        size_t amount_read = 0;
        size_t chunk = min(size - total, remaining);
        ret = appledouble_read(volume, &fp, (char *)value + total, chunk,
                               position + total, &amount_read, &eof);

        if (ret < 0) {
            appledouble_close(volume, &fp);
            return ret;
        }

        if (ret != 1) {
            appledouble_close(volume, &fp);
            return -EIO;
        }

        if (amount_read == 0) {
            break;
        }

        total += amount_read;
        remaining -= amount_read;

        if (eof) {
            break;
        }
    }

    appledouble_close(volume, &fp);
    return total > INT_MAX ? -EFBIG : (int) total;
}

/*!
 * @brief Write the resource fork of path
 *
 * At position 0, a write of nothing or of the whole fork leaves no tail of
 * the old fork; a flush_on_close mount flushes the fork before the return.
 *
 * @param[in] volume    volume of the file
 * @param[in] path      UTF-8 path of the file
 * @param[in] value     bytes to write, or NULL when size is 0
 * @param[in] size      length of value
 * @param[in] position  offset in the fork to write at
 * @param[in] whole     1 when a value at position 0 is the whole fork
 * @returns 0, or a negative errno
 */
static int write_resource_fork(struct afp_volume * volume, const char *path,
                               const void *value, size_t size,
                               off_t position, int whole)
{
    struct afp_file_info fp = { 0 };
    size_t written = 0;
    int ret;

    if (!volume || !path || (!value && size > 0) || position < 0) {
        return -EINVAL;
    }

    if ((uint64_t)position > INT_MAX
            || size > (size_t)(INT_MAX - (uint64_t)position)) {
        return -EFBIG;
    }

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    ret = appledouble_open_meta(volume, path, AFP_META_RESOURCE, O_RDWR, &fp);

    if (ret < 0) {
        return ret;
    }

    if (size > 0) {
        ret = appledouble_write(volume, &fp, value, size, position, &written);

        if (ret < 0) {
            appledouble_close(volume, &fp);
            return ret;
        }

        if (ret != 1 || written != size) {
            appledouble_close(volume, &fp);
            return -EIO;
        }
    }

    if (position == 0 && (size == 0 || whole)) {
        ret = ml_ftruncate(volume, &fp, (off_t)size);

        if (ret < 0) {
            appledouble_close(volume, &fp);
            return ret;
        }
    }

    ret = appledouble_close(volume, &fp);

    if (ret < 0) {
        return ret;
    }

    return 0;
}

/*!
 * @brief Write the resource fork of path at a position
 *
 * A write of nothing at position 0 empties the fork.
 *
 * @param[in] volume    volume of the file
 * @param[in] path      UTF-8 path of the file
 * @param[in] value     bytes to write, or NULL when size is 0
 * @param[in] size      length of value
 * @param[in] position  offset in the fork to write at
 * @returns 0, or a negative errno
 */
int ml_setresourcefork(struct afp_volume * volume, const char *path,
                       const void *value, size_t size, off_t position)
{
    return write_resource_fork(volume, path, value, size, position, 0);
}

/*!
 * @brief Set the resource fork of path as its extended attribute
 *
 * A value written at position 0 is the whole fork.
 *
 * @param[in] volume    volume of the file
 * @param[in] path      UTF-8 path of the file
 * @param[in] value     bytes to write, or NULL when size is 0
 * @param[in] size      length of value
 * @param[in] position  offset in the fork to write at
 * @param[in] flags     kXAttrCreate, kXAttrReplace or 0
 * @returns 0, or a negative errno
 */
int ml_setresourcefork_flags(struct afp_volume * volume, const char *path,
                             const void *value, size_t size, off_t position,
                             int flags)
{
    int ret = validate_xattr_flags(flags);

    if (ret < 0) {
        return ret;
    }

    if (flags & (kXAttrCreate | kXAttrReplace)) {
        struct ml_target t;
        struct afp_dirent e;
        ret = validate_special_xattr_flags(flags,
                                           resource_fork_entry(volume, path,
                                                               1, &t, &e));

        if (ret < 0) {
            return ret;
        }
    }

    return write_resource_fork(volume, path, value, size, position, 1);
}

/*!
 * @brief Set the length of the resource fork of path
 *
 * The fork is dirty afterwards, so a flush_on_close mount flushes it before
 * the call returns.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] size    the new length
 * @returns 0, or a negative errno
 */
int ml_truncateresourcefork(struct afp_volume * volume, const char *path,
                            uint64_t size)
{
    struct afp_file_info fp = { 0 };
    int ret;
    int close_ret;

    if (!volume || !path) {
        return -EINVAL;
    }

    if (size > INT64_MAX) {
        return -EFBIG;
    }

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    ret = appledouble_open_meta(volume, path, AFP_META_RESOURCE, O_RDWR, &fp);

    if (ret < 0) {
        return ret;
    }

    ret = ml_ftruncate(volume, &fp, (off_t) size);
    close_ret = appledouble_close(volume, &fp);

    if (ret != 0) {
        return ret;
    }

    return close_ret < 0 ? close_ret : 0;
}

/*!
 * @brief Remove the resource fork of path, which leaves it empty
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @returns 0, -ENOATTR for a fork already empty, or a negative errno
 */
int ml_removeresourcefork(struct afp_volume * volume, const char *path)
{
    struct ml_target t;
    struct afp_dirent e;
    int ret = resource_fork_entry(volume, path, 1, &t, &e);
    return ret < 0 ? ret : ml_setresourcefork(volume, path, NULL, 0, 0);
}

/*!
 * @brief Read the record of an object's Finder info
 *
 * @param[in]  volume  volume of the object
 * @param[in]  path    UTF-8 path of the object
 * @param[in]  fresh   1 to ask the server, 0 to take the cache's record
 *                     while its parameters hold
 * @param[out] e       receives the record
 * @returns 0, -ENOATTR when the Finder info is all zero, or a negative
 *          errno
 */
static int finderinfo_entry(struct afp_volume *volume, const char *path,
                            int fresh, struct afp_dirent *e)
{
    static const char empty_finderinfo[32] = {0};
    struct ml_target t;
    int ret = ml_entry(volume, path, fresh ? ML_ENTRY_FRESH : ML_ENTRY_CACHED,
                       &t, e);
    return ret == 0 && memcmp(e->finderinfo, empty_finderinfo,
                              sizeof(e->finderinfo)) == 0 ? -ENOATTR : ret;
}

/*!
 * @brief Read the Finder info of the object at path
 *
 * The cache answers the length; a read asks the server, as a set reads the
 * bytes and writes them back.
 *
 * @param[in]  volume  volume of the object
 * @param[in]  path    UTF-8 path of the object
 * @param[out] value   receives the Finder info, or NULL
 * @param[in]  size    capacity of value; 0 asks for the length alone
 * @returns 32, -ENOATTR when the Finder info is all zero, -ERANGE when
 *          value is too small, or another negative errno
 */
int ml_getfinderinfo(struct afp_volume * volume, const char *path,
                     void *value, size_t size)
{
    struct afp_dirent e;
    int ret;

    if (!volume || !path) {
        return -EINVAL;
    }

    ret = finderinfo_entry(volume, path, size != 0, &e);

    if (ret < 0) {
        return ret;
    }

    if (size == 0) {
        return sizeof(e.finderinfo);
    }

    if (!value || size < sizeof(e.finderinfo)) {
        return -ERANGE;
    }

    memcpy(value, e.finderinfo, sizeof(e.finderinfo));
    return sizeof(e.finderinfo);
}

/*!
 * @brief Set a file's Finder information as its extended attribute
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] value   the 32 bytes
 * @param[in] size    length of value, 32
 * @returns 0, or a negative errno: -EINVAL for another length, -EROFS on a
 *          read-only volume
 */
int ml_setfinderinfo(struct afp_volume * volume, const char *path,
                     const void *value, size_t size)
{
    struct afp_file_info fp;
    size_t written = 0;
    int ret;

    if (!volume || !path || (!value && size > 0) || size != 32) {
        return -EINVAL;
    }

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    ret = appledouble_open_meta(volume, path, AFP_META_FINDERINFO, O_RDWR,
                                &fp);

    if (ret < 0) {
        return ret;
    }

    ret = appledouble_write(volume, &fp, value, size, 0, &written);
    appledouble_close(volume, &fp);

    if (ret < 0) {
        return ret;
    }

    if (ret != 1 || written != size) {
        return -EIO;
    }

    return 0;
}

/*!
 * @brief Set the Finder info of path as setxattr(2) asks
 *
 * @param[in] volume  volume of the object
 * @param[in] path    UTF-8 path of the object
 * @param[in] value   the 32 bytes
 * @param[in] size    length of value
 * @param[in] flags   kXAttrCreate, kXAttrReplace or 0
 * @returns 0, -EEXIST or -ENOATTR where the flags refuse the set, -EINVAL
 *          for flags it does not take, or another negative errno
 */
int ml_setfinderinfo_flags(struct afp_volume * volume, const char *path,
                           const void *value, size_t size, int flags)
{
    int ret = validate_xattr_flags(flags);

    if (ret < 0) {
        return ret;
    }

    if (flags & (kXAttrCreate | kXAttrReplace)) {
        struct afp_dirent e;
        ret = validate_special_xattr_flags(flags,
                                           finderinfo_entry(volume, path, 1,
                                                            &e));

        if (ret < 0) {
            return ret;
        }
    }

    return ml_setfinderinfo(volume, path, value, size);
}

/*!
 * @brief Clear a file's FinderInfo to 32 zero bytes
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @returns ml_setfinderinfo()'s result
 */
int ml_removefinderinfo(struct afp_volume * volume, const char *path)
{
    char finderinfo[32];
    memset(finderinfo, 0, sizeof(finderinfo));
    return ml_setfinderinfo(volume, path, finderinfo, sizeof(finderinfo));
}

/*!
 * @brief Read an extended attribute of a file
 *
 * Netatalk's metadata attribute reads as absent.
 *
 * @param[in]  volume  volume of the file
 * @param[in]  path    UTF-8 path of the file
 * @param[in]  name    the attribute's name, with or without user.
 * @param[out] value   receives the value
 * @param[in]  size    capacity of value, or 0 to ask the value's length
 * @returns the value's length, the bytes read of the resource fork, or a
 *          negative errno
 */
int ml_getxattr(struct afp_volume * volume, const char *path,
                const char *name, void *value, size_t size)
{
    struct afp_extattr_info info;
    struct ml_target t;
    const char *afp_name;
    unsigned short afp_namelen;
    uint64_t sent;
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    if (is_resourcefork_xattr(name)) {
        return ml_getresourcefork(volume, path, value, size, 0);
    }

    if (is_finderinfo_xattr(name)) {
        return ml_getfinderinfo(volume, path, value, size);
    }

    /* Ignore EAs if server doesn't support them */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return -ENOATTR;
    }

    /* Filter internal server EAs - pretend they don't exist */
    if (is_netatalk_meta_xattr(name)) {
        return -ENOATTR;
    }

    /* Special case: root directory - EAs not supported on volume root */
    if (strcmp(path, "/") == 0) {
        return -ENOATTR;
    }

    ret = normalize_xattr_name(name, &afp_name, &afp_namelen);

    if (ret < 0) {
        return ret;
    }

    ret = ml_resolve(volume, path, &t);

    if (ret != 0) {
        return ret;
    }

    /* The kernel asks the length, then the value. */
    ret = dir_cache_get_xattr(volume, t.did, t.name, afp_name, value, size);

    if (ret >= 0 || ret == -ERANGE) {
        return ret;
    }

    sent = dir_cache_changes(volume);
    /* Time Capsule rejects small MaxReplySize values.  The reply buffer is
     * fixed-size, so cap the request at the storage we actually have. */
    info.maxsize = AFP_EXTATTR_DATA_MAX;
    info.size = 0;
    info.copied = 0;
    /* Get the extended attribute */
    ret = afp_getextattr(volume, t.did, 0, info.maxsize,
                         t.afp_name, afp_namelen, afp_name, &info);

    switch (ret) {
    case kFPNoErr:

        /* Kept for the value question that follows. */
        if (size == 0 && info.copied == info.size) {
            dir_cache_set_xattr(volume, t.did, t.name, afp_name, info.data,
                                info.size, sent);
        }

        break;

    case kFPItemNotFound:
    case kFPMiscErr:
    case kFPParamErr:
        /* AFP 3.2/3.3 returns kFPMiscErr for "attribute not found".
         * AFP 3.4 returns kFPItemNotFound instead.
         * Apple AFP servers may return kFPParamErr when an xattr doesn't exist. */
        return -ENOATTR;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        dir_cache_remove(volume, t.did, t.name);
        return -ENOENT;

    default:
        return -EIO;
    }

    /* If size is 0, just return the size needed */
    if (size == 0) {
        return info.size;
    }

    /* Check if buffer is too small */
    if (size < info.size) {
        return -ERANGE;
    }

    if (info.copied < info.size) {
        return -ERANGE;
    }

    /* Copy data to user buffer */
    if (value && info.size > 0) {
        memcpy(value, info.data, info.size);
    }

    return info.size;
}

/*!
 * @brief Set an extended attribute of a file
 *
 * Apart from the resource fork and the Finder info, a set at the root, on a
 * volume without extended attributes or of Netatalk's metadata stores nothing.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] name    the attribute's name, with or without user.
 * @param[in] value   the value
 * @param[in] size    length of value
 * @param[in] flags   kXAttrCreate or kXAttrReplace, or 0
 * @returns 0, or a negative errno
 */
int ml_setxattr(struct afp_volume * volume, const char *path,
                const char *name, const void *value, size_t size, int flags)
{
    struct ml_target t;
    const char *afp_name;
    unsigned short afp_namelen;
    unsigned short bitmap = 0;
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    ret = validate_xattr_flags(flags);

    if (ret < 0) {
        return ret;
    }

    if (is_resourcefork_xattr(name)) {
        return ml_setresourcefork_flags(volume, path, value, size, 0, flags);
    }

    if (is_finderinfo_xattr(name)) {
        return ml_setfinderinfo_flags(volume, path, value, size, flags);
    }

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    /* Silently succeed when server doesn't support EAs to allow file copies */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return 0;
    }

    /* Filter internal server EAs - silently succeed to allow file copies */
    if (is_netatalk_meta_xattr(name)) {
        return 0;
    }

    /* Special case: root directory - silently succeed to allow file copies */
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    ret = normalize_xattr_name(name, &afp_name, &afp_namelen);

    if (ret < 0) {
        return ret;
    }

    ret = ml_resolve(volume, path, &t);

    if (ret != 0) {
        return ret;
    }

    /* Handle flags (create/replace semantics) */
    if (flags & kXAttrCreate) {
        bitmap |= kXAttrCreate;
    }

    if (flags & kXAttrReplace) {
        bitmap |= kXAttrReplace;
    }

    /* Set the extended attribute */
    ret = afp_setextattr(volume, t.did, bitmap, 0, t.afp_name,
                         afp_namelen, afp_name, size, value);
    dir_cache_forget_xattr(volume, t.did, t.name);

    switch (ret) {
    case kFPNoErr:
        return 0;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        dir_cache_remove(volume, t.did, t.name);
        return -ENOENT;

    case kFPObjectExists:
        return -EEXIST;  /* CREATE flag set but attr already exists */

    case kFPItemNotFound:
        return -ENOATTR; /* REPLACE flag set but attr does not exist */

    case kFPVolLocked:
        return -EROFS;

    case kFPMiscErr:
        return -EIO;

    default:
        return -EIO;
    }
}

/*!
 * @brief List the extended attributes of a file
 *
 * Off macOS every name is listed under user.; Netatalk's internal
 * attributes are left out.
 *
 * @param[in]  volume  volume of the file
 * @param[in]  path    UTF-8 path of the file
 * @param[out] list    receives the names, each NUL-terminated
 * @param[in]  size    capacity of list, or 0 to ask the list's length
 * @returns the list's length, or a negative errno
 */
int ml_listxattr(struct afp_volume * volume, const char *path,
                 char *list, size_t size)
{
    struct afp_extattr_info info;
    struct ml_target t;
    size_t filtered_size = 0;
    int ret = 0;

    if (!volume || !path) {
        return -EINVAL;
    }

    if ((volume->attributes & kSupportsExtAttrs) && strcmp(path, "/") != 0) {
        ret = ml_resolve(volume, path, &t);

        if (ret != 0) {
            return ret;
        }

        /* Fetch the complete raw AFP list. Namespace prefixes added below can
         * make the caller-visible list substantially larger. */
        info.maxsize = AFP_EXTATTR_LIST_MAX;
        info.size = 0;
        info.copied = 0;
        /* List extended attributes */
        ret = afp_listextattr(volume, t.did, 0, t.afp_name, &info);

        switch (ret) {
        case kFPNoErr:
            break;

        case kFPAccessDenied:
            ret = -EACCES;
            goto out;

        case kFPObjectNotFound:
            dir_cache_remove(volume, t.did, t.name);
            ret = -ENOENT;
            goto out;

        case kFPParamErr:
            /* TC returns kFPParamErr when MaxReplySize is too small; treat as
             * empty list so callers see no attributes rather than a hard error. */
            info.size = 0;
            break;

        default:
            ret = -EIO;
            goto out;
        }
    } else {
        info.size = 0;
        info.copied = 0;
    }

    if (info.copied < info.size) {
        ret = -E2BIG;
        goto out;
    }

    /* Filter out internal server EAs from the list.
     * On Linux, prepend "user." namespace prefix (required by the kernel EA
     * interface).  On macOS, AFP xattr names are passed as-is; no prefix. */
    const char *src = info.data;
    size_t remaining = info.copied;

    while (remaining > 0) {
        size_t namelen = strnlen(src, remaining);

        if (namelen == 0) {
            break;  /* End of list */
        }

        /* Ensure we have a null terminator within bounds */
        if (namelen == remaining) {
            /* No null terminator found - malformed data from server */
            ret = -EIO;
            goto out;
        }

        /* Only include this EA if it's not filtered */
        if (!IS_INTERNAL_SERVER_XATTR(src)) {
            ret = append_listed_xattr_name(list, size, &filtered_size, src);

            if (ret < 0) {
                goto out;
            }
        }

        src += namelen + 1;
        remaining -= namelen + 1;
    }

    ret = filtered_size > INT_MAX ? -E2BIG : (int)filtered_size;
out:
    return ret;
}

/*!
 * @brief Remove an extended attribute of a file
 *
 * The resource fork and the Finder info are cleared on a volume without
 * extended attributes too.
 *
 * @param[in] volume  volume of the file
 * @param[in] path    UTF-8 path of the file
 * @param[in] name    the attribute's name, with or without user.
 * @returns 0, or a negative errno: -EROFS on a read-only volume, -ENOATTR
 *          on one without extended attributes, -EACCES at the volume root
 *          and for Netatalk's metadata attribute
 */
int ml_removexattr(struct afp_volume * volume, const char *path,
                   const char *name)
{
    struct ml_target t;
    const char *afp_name;
    unsigned short afp_namelen;
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    if (is_resourcefork_xattr(name)) {
        return ml_removeresourcefork(volume, path);
    }

    if (is_finderinfo_xattr(name)) {
        return ml_removefinderinfo(volume, path);
    }

    if (volume_is_readonly(volume)) {
        return -EROFS;
    }

    /* Ignore EAs if server doesn't support them to allow file deletions */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return -ENOATTR;
    }

    /* Filter internal server EAs - deny removal */
    if (is_netatalk_meta_xattr(name)) {
        return -EACCES;
    }

    /* Special case: root directory - deny removal */
    if (strcmp(path, "/") == 0) {
        return -EACCES;
    }

    ret = normalize_xattr_name(name, &afp_name, &afp_namelen);

    if (ret < 0) {
        return ret;
    }

    ret = ml_resolve(volume, path, &t);

    if (ret != 0) {
        return ret;
    }

    ret = afp_removeextattr(volume, t.did, 0, t.afp_name,
                            afp_namelen, afp_name);
    dir_cache_forget_xattr(volume, t.did, t.name);

    switch (ret) {
    case kFPNoErr:
        return 0;

    case kFPItemNotFound:
    case kFPMiscErr:
    case kFPParamErr:
        /* AFP 3.2/3.3 returns kFPMiscErr for "attribute not found".
         * AFP 3.4 returns kFPItemNotFound instead.
         * Apple AFP servers may return kFPParamErr when an xattr doesn't exist. */
        return -ENOATTR;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        dir_cache_remove(volume, t.did, t.name);
        return -ENOENT;

    case kFPVolLocked:
        return -EROFS;

    default:
        return -EIO;
    }
}
