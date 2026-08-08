#ifndef NETATALK_CLIENT_URL_H
#define NETATALK_CLIENT_URL_H

#include "types.h"
#include "path.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize before use and clear when the URL is no longer needed. */
void afp_sl_url_init(struct afpc_url *url);
void afp_sl_url_clear(struct afpc_url *url);
int afp_sl_url_set_path(struct afpc_url *url, const char *path);
int afp_sl_url_copy(struct afpc_url *dest, const struct afpc_url *src);
int afp_sl_url_parse(struct afpc_url *url, const char *text);

#ifdef __cplusplus
}
#endif

#endif
