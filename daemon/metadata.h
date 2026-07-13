#ifndef NETATALK_CLIENT_DAEMON_METADATA_H
#define NETATALK_CLIENT_DAEMON_METADATA_H

#include <stddef.h>
#include <sys/types.h>

#include "netatalk-client/afpsl.h"

int metadata_name_filtered(const char *name);

int local_metadata_list(const char *path, enum afp_metadata_mode mode,
                        char **list, size_t *size);
int local_metadata_get(const char *path, enum afp_metadata_mode mode,
                       const char *name, void **value, size_t *size);
int local_metadata_set(const char *path, enum afp_metadata_mode mode,
                       const char *name, const void *value, size_t size);
int local_metadata_remove(const char *path, enum afp_metadata_mode mode,
                          const char *name);

int local_finderinfo_get(const char *path, enum afp_metadata_mode mode,
                         unsigned char finderinfo[32]);
int local_finderinfo_set(const char *path, enum afp_metadata_mode mode,
                         const unsigned char finderinfo[32]);
int local_finderinfo_remove(const char *path, enum afp_metadata_mode mode);

off_t local_resourcefork_size(const char *path, enum afp_metadata_mode mode);
ssize_t local_resourcefork_read(const char *path, enum afp_metadata_mode mode,
                                void *buf, size_t size, off_t offset);
int local_resourcefork_write(const char *path, enum afp_metadata_mode mode,
                             const void *buf, size_t size, off_t offset);
int local_resourcefork_remove(const char *path, enum afp_metadata_mode mode);

#endif
