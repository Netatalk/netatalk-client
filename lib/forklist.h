/*!
 * @file
 * @brief Recording and searching a volume's open forks
 */

#ifndef __FORKLIST_H_
#define __FORKLIST_H_

#include <pthread.h>

struct afp_file_info;
struct afp_volume;
struct ml_target;

void add_opened_fork(struct afp_volume * volume, struct afp_file_info * fp);
void remove_opened_fork(struct afp_volume * volume, struct afp_file_info * fp);
void flush_dirty_forks_for_file(struct afp_volume * volume,
                                struct afp_file_info * fp);
int other_fork_open(struct afp_volume * volume, struct afp_file_info * fp);
void remove_fork_list(struct afp_volume * volume);
void afp_open_lock_init(pthread_rwlock_t *lock);
void fork_now_named(struct afp_file_info *fp, const struct ml_target *t);
unsigned int fork_names(struct afp_volume *volume,
                        const struct afp_file_info *fp, struct ml_target *t);
void rename_opened_forks(struct afp_volume *volume,
                         const struct ml_target *from,
                         const struct ml_target *to);
void exchange_opened_forks(struct afp_volume *volume,
                           const struct ml_target *a,
                           const struct ml_target *b, int done);
#endif
