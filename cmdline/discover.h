#ifndef NETATALK_CLIENT_CMDLINE_DISCOVER_H
#define NETATALK_CLIENT_CMDLINE_DISCOVER_H

#include <stddef.h>

/* Returns zero after selecting a service, one when the user quits, or a
 * negative errno value when discovery cannot continue. */
int cmdline_discover_url(char *url, size_t url_size);

#endif
