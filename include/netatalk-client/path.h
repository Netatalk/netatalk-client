#ifndef NETATALK_CLIENT_PATH_H
#define NETATALK_CLIENT_PATH_H

#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pass AFPC_PATH_UNBOUNDED for local filesystem paths. */
#define AFPC_PATH_UNBOUNDED SIZE_MAX

/* Validate a NUL-terminated string and optionally return its byte length. */
int afpc_path_validate(const char *data, size_t max_len, size_t *len);
void afpc_path_clear(struct afpc_path *path);
int afpc_path_set(struct afpc_path *path, const char *data, size_t max_len);
int afpc_path_copy(struct afpc_path *dest, const struct afpc_path *src,
                   size_t max_len);
int afpc_path_join(struct afpc_path *dest, const char *parent,
                   const char *name, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif
