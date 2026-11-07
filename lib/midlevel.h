/*!
 * @file
 * @brief POSIX file operations on a volume, for the FUSE and afpsld layers
 */

#ifndef __MIDLEVEL_H_
#define __MIDLEVEL_H_

#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>
#include <utime.h>

#include "afp_internal.h"

/* A path resolved: its parent's directory ID and its last component, in
 * the UNIX form and in the volume's wire form */
struct ml_target {
    unsigned int did;
    char name[AFPC_MAX_NAME_BYTES];
    char afp_name[AFPC_MAX_NAME_BYTES];
};

int ml_resolve(struct afp_volume *volume, const char *path,
               struct ml_target *t);
int ml_resolve_fork(struct afp_volume *volume, const char *path,
                    struct afp_file_info *fp);
/* How ml_entry() reads a record */
#define ML_ENTRY_CACHED 0
#define ML_ENTRY_FRESH 1
#define ML_ENTRY_FORK 2

int ml_entry(struct afp_volume *volume, const char *path, int how,
             struct ml_target *t, struct afp_dirent *e);
int ml_dirent_to_stat(struct afp_volume *volume, const struct ml_target *t,
                      struct afp_dirent *e, struct stat *stbuf);
int ml_list_directory(struct afp_volume *volume, const char *path,
                      struct afp_listing *l);

int ml_open(struct afp_volume * volume, const char *path, int flags,
            struct afp_file_info **newfp);

int ml_creat(struct afp_volume * volume, const char *path, mode_t mode,
             unsigned char *created);

void ml_set_created_mode(struct afp_volume * volume, const char *path,
                         mode_t mode);

int ml_readdir(struct afp_volume * volume,
               const char *path,
               struct afp_listing *l);

int ml_read(struct afp_volume * volume,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof);

int ml_chmod(struct afp_volume * vol, const char *path, mode_t mode);

int ml_unlink(struct afp_volume * vol, const char *path);

int ml_mkdir(struct afp_volume * vol, const char *path, mode_t mode);

int ml_close(struct afp_volume * volume, struct afp_file_info * fp);

int ml_fsync(struct afp_volume * volume, struct afp_file_info * fp);

int ml_close_flush(struct afp_volume * volume, struct afp_file_info * fp);

int ml_getattr(struct afp_volume * volume, const char *path,
               struct stat *stbuf);
int ml_getattr_finderinfo(struct afp_volume *volume, const char *path,
                          struct stat *stbuf, char *finderinfo);
int ml_getattr_fresh(struct afp_volume *volume, const char *path,
                     struct stat *stbuf);

int ml_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset, int append,
             struct afp_file_info * fp);

ssize_t ml_copy_range(struct afp_volume * volume,
                      struct afp_file_info *fp_in, off_t off_in,
                      struct afp_file_info *fp_out, off_t off_out,
                      size_t len);

int ml_readlink(struct afp_volume * vol, const char *path,
                char *buf, size_t size);

int ml_rmdir(struct afp_volume * vol, const char *path);

int ml_chown(struct afp_volume * vol, const char *path,
             uid_t uid, gid_t gid);

int ml_truncate(struct afp_volume * vol, const char *path, off_t offset);

int ml_ftruncate(struct afp_volume * vol, struct afp_file_info * fp,
                 off_t offset);

int ml_utime(struct afp_volume * vol, const char *path,
             struct utimbuf * timebuf);

/* The members ml_setattr() sets */
#define ML_SET_MODE  0x1U
#define ML_SET_UID   0x2U
#define ML_SET_GID   0x4U
#define ML_SET_MTIME 0x8U

int ml_setattr(struct afp_volume * vol, const char *path, unsigned int which,
               mode_t mode, uid_t uid, gid_t gid, time_t mtime);

int ml_carries_time(const struct afp_dirent *e);

/* The type and creator of a symlink's Finder information */
#define ML_SYMLINK_FINDERINFO "slnkrhap"

int ml_symlink(struct afp_volume *vol, const char *path1, const char *path2);

int ml_rename(struct afp_volume * vol,
              const char *path_from, const char *path_to);
int ml_rename_excl(struct afp_volume *vol,
                   const char *path_from, const char *path_to);

int ml_exchange(struct afp_volume * vol,
                const char *path_from, const char *path_to);

int ml_statfs(struct afp_volume * vol, const char *path, struct statvfs *stat);

int ml_passwd(struct afp_server *server,
              char *username, char *oldpasswd, char *newpasswd);

/* Extended Attributes (AFP 3.2+) */

int ml_getxattr(struct afp_volume * volume, const char *path,
                const char *name, void *value, size_t size);

int ml_setxattr(struct afp_volume * volume, const char *path,
                const char *name, const void *value, size_t size, int flags);

int ml_listxattr(struct afp_volume * volume, const char *path,
                 char *list, size_t size);

int ml_removexattr(struct afp_volume * volume, const char *path,
                   const char *name);

int ml_getresourcefork(struct afp_volume * volume, const char *path,
                       void *value, size_t size, off_t position);

int ml_setresourcefork(struct afp_volume * volume, const char *path,
                       const void *value, size_t size, off_t position);

int ml_setresourcefork_flags(struct afp_volume * volume, const char *path,
                             const void *value, size_t size, off_t position,
                             int flags);

int ml_truncateresourcefork(struct afp_volume * volume, const char *path,
                            uint64_t size);

int ml_removeresourcefork(struct afp_volume * volume, const char *path);

int ml_getfinderinfo(struct afp_volume * volume, const char *path,
                     void *value, size_t size);

int ml_setfinderinfo(struct afp_volume * volume, const char *path,
                     const void *value, size_t size);

int ml_setfinderinfo_flags(struct afp_volume * volume, const char *path,
                           const void *value, size_t size, int flags);

int ml_removefinderinfo(struct afp_volume * volume, const char *path);


#endif
