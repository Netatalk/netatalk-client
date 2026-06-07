#ifndef __MIDLEVEL_H_
#define __MIDLEVEL_H_

#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>
#include <utime.h>
#include "afp.h"

int ml_open(struct afp_volume * volume, const char *path, int flags,
            struct afp_file_info **newfp);

int ml_creat(struct afp_volume * volume, const char *path, mode_t mode);

int ml_readdir(struct afp_volume * volume,
               const char *path,
               struct afp_file_info **base);

int ml_read(struct afp_volume * volume, const char *path,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof);

int ml_chmod(struct afp_volume * vol, const char * path, mode_t mode);

int ml_unlink(struct afp_volume * vol, const char *path);

int ml_mkdir(struct afp_volume * vol, const char * path, mode_t mode);

int ml_close(struct afp_volume * volume, const char * path,
             struct afp_file_info * fp);

int ml_getattr(struct afp_volume * volume, const char *path,
               struct stat *stbuf);

int ml_write(struct afp_volume * volume, const char * path,
             const char *data, size_t size, off_t offset,
             struct afp_file_info * fp, uid_t uid,
             gid_t gid);

int ml_readlink(struct afp_volume * vol, const char * path,
                char *buf, size_t size);

int ml_rmdir(struct afp_volume * vol, const char *path);

int ml_chown(struct afp_volume * vol, const char * path,
             uid_t uid, gid_t gid);

int ml_truncate(struct afp_volume * vol, const char * path, off_t offset);

int ml_setfork_size(struct afp_volume * vol, unsigned short forkid,
                    unsigned int resource, uint64_t size);

int ml_utime(struct afp_volume * vol, const char * path,
             struct utimbuf * timebuf);

int ml_symlink(struct afp_volume *vol, const char * path1, const char * path2);

int ml_rename(struct afp_volume * vol,
              const char *path_from, const char *path_to);

int ml_exchange(struct afp_volume * vol,
                const char *path_from, const char *path_to);

int ml_statfs(struct afp_volume * vol, const char *path, struct statvfs *stat);

void afp_ml_filebase_free(struct afp_file_info **filebase);

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

int ml_removeresourcefork(struct afp_volume * volume, const char *path);

int ml_getfinderinfo(struct afp_volume * volume, const char *path,
                     void *value, size_t size);

int ml_setfinderinfo(struct afp_volume * volume, const char *path,
                     const void *value, size_t size);

int ml_removefinderinfo(struct afp_volume * volume, const char *path);


#endif
