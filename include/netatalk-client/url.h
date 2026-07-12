#ifndef NETATALK_CLIENT_URL_H
#define NETATALK_CLIENT_URL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void afp_sl_url_init(struct afpc_url *url);
int afp_sl_url_parse(struct afpc_url *url, const char *text);

#ifdef __cplusplus
}
#endif

#endif
