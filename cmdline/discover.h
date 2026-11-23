/*!
 * @file
 * @brief afpcmd's choice of a discovered server
 *
 * The interface of cmdline/discover.c.
 * Lane: control.
 * Northbound: afpcmd's entry point.
 */

#ifndef NETATALK_CLIENT_CMDLINE_DISCOVER_H
#define NETATALK_CLIENT_CMDLINE_DISCOVER_H

#include <stddef.h>

/* Returns zero after selecting a service, one when the user quits, or a
 * negative errno value when discovery cannot continue.  On success, *url
 * receives an allocated URL owned by the caller. */
int cmdline_discover_url(char **url);

#endif
