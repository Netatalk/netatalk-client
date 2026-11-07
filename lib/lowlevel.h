#ifndef __LOWLEVEL_H_
#define __LOWLEVEL_H_

#include <sys/types.h>
#include <stdint.h>

int ll_wire_name(const struct afp_volume *volume, const char *name,
                 char *afp_name);
int ll_fetch_entry(struct afp_volume *volume, unsigned int parent_did,
                   const char *name, struct afp_dirent *e);
int ll_readdir(struct afp_volume * volume, unsigned int dirid,
               const char *afp_name, struct afp_listing *l);

int ll_setfork_size(struct afp_volume * volume, struct afp_file_info *fp,
                    uint64_t size);
void ll_fork_changed(struct afp_volume *volume,
                     const struct afp_file_info *fp);

int ll_read(struct afp_volume * volume,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof);

int ll_handle_unlocking(struct afp_volume * volume, unsigned short forkid,
                        uint64_t offset, uint64_t sizetorequest);

int ll_handle_locking(struct afp_volume * volume, unsigned short forkid,
                      uint64_t offset, uint64_t sizetorequest);

int ll_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset, int append,
             struct afp_file_info * fp, size_t *totalwritten);

int ll_createfile(struct afp_volume * volume, unsigned int did,
                  char *basename);

int ll_openfork_errno(int rc);

int ll_answer_errno(int rc);
int ll_unanswered(int rc);

int ll_open(struct afp_volume * volume, const char *path, int flags,
            struct afp_file_info *fp);

#endif
