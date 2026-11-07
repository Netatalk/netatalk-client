/*!
 * @file
 * @brief The directory cache: directory IDs, attributes and absences by
 * directory and name
 */

#ifndef __DID_H_
#define __DID_H_

/* The cache's bounds: entries held, the life of an identity, of an
 * absence, of attributes at the least and the most, and of an extended
 * attribute's value, in the whole seconds of the cache's clock */
#define DIR_CACHE_MAX_ENTRIES 8192U
#define DIR_CACHE_DID_TTL 120
#define DIR_CACHE_NEGATIVE_TTL 2
#define DIR_CACHE_STAT_TTL_MIN 5
#define DIR_CACHE_STAT_TTL_MAX 60
#define DIR_CACHE_XATTR_TTL 2

/* What dir_cache_lookup() found at a name */
#define DIR_CACHE_DID 0x1U
#define DIR_CACHE_STAT 0x2U
#define DIR_CACHE_NEGATIVE 0x4U

void dir_cache_init(struct afp_volume *volume);
void dir_cache_destroy(struct afp_volume *volume);
void dir_cache_purge(struct afp_volume *volume);
uint32_t dir_cache_counts(struct afp_volume *volume,
                          struct dir_cache_counts *counts);

unsigned int dir_cache_lookup(struct afp_volume *volume,
                              unsigned int parent_did, const char *name,
                              struct afp_dirent *e);
int dir_cache_fetch(struct afp_volume *volume, unsigned int parent_did,
                    const char *name, struct afp_dirent *e);
int dir_cache_resolve(struct afp_volume *volume, const char *path,
                      unsigned int *parent_did, char *name);
int dir_cache_resolve_fresh(struct afp_volume *volume, const char *path,
                            unsigned int *parent_did, char *name);

uint64_t dir_cache_changes(struct afp_volume *volume);
void dir_cache_upsert(struct afp_volume *volume, unsigned int parent_did,
                      const char *name, const struct afp_dirent *e,
                      uint64_t sent);
void dir_cache_insert_negative(struct afp_volume *volume,
                               unsigned int parent_did, const char *name,
                               uint64_t sent);
void dir_cache_insert_dir(struct afp_volume *volume, unsigned int parent_did,
                          const char *name, unsigned int did, uint64_t sent);
uint64_t dir_cache_listing(struct afp_volume *volume);
void dir_cache_insert_listing(struct afp_volume *volume,
                              const struct afp_listing *l, uint32_t first,
                              uint64_t sent, uint64_t serial);
void dir_cache_listed(struct afp_volume *volume, unsigned int did,
                      uint64_t serial);
int dir_cache_listed_empty(struct afp_volume *volume,
                           unsigned int parent_did, const char *name);

void dir_cache_remove(struct afp_volume *volume, unsigned int parent_did,
                      const char *name);
void dir_cache_made_dir(struct afp_volume *volume, unsigned int parent_did,
                        const char *name, unsigned int did, uint64_t sent);
void dir_cache_rename(struct afp_volume *volume, unsigned int from_did,
                      const char *from, unsigned int to_did, const char *to,
                      unsigned int cnid);
void dir_cache_invalidate(struct afp_volume *volume, unsigned int parent_did,
                          const char *name, unsigned int cnid);
void dir_cache_forget_dir(struct afp_volume *volume, unsigned int did);
void dir_cache_forget_path(struct afp_volume *volume, const char *path);

int dir_cache_get_xattr(struct afp_volume *volume, unsigned int parent_did,
                        const char *name, const char *xattr, void *value,
                        size_t size);
void dir_cache_set_xattr(struct afp_volume *volume, unsigned int parent_did,
                         const char *name, const char *xattr,
                         const void *value, size_t len, uint64_t sent);
void dir_cache_forget_xattr(struct afp_volume *volume,
                            unsigned int parent_did, const char *name);

#endif
