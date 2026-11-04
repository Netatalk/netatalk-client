#ifndef __DID_H_
#define __DID_H_

int free_entire_did_cache(struct afp_volume *volume);
void remove_did_lineage(struct afp_volume *volume, const char *path);
void remove_did_subtree(struct afp_volume *volume, const char *path);
void check_did_entry(struct afp_volume *volume, const char *path,
                     unsigned int did, const struct afp_listing *l);
int get_dirid(struct afp_volume *volume, const char *path,
              char *basename, unsigned int *dirid);

#endif
