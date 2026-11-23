/*!
 * @file
 * @brief The DNS-SD service types of an AFP server's advertisements
 *
 * Constants of the discovery library, which discovery.h includes.
 * Lane: control.
 */

#ifndef NETATALK_CLIENT_DISCOVERY_SERVICE_TYPES_H
#define NETATALK_CLIENT_DISCOVERY_SERVICE_TYPES_H

/* Keep these protocol constants aligned with Netatalk's
 * etc/netatalk/afp_zeroconf.h. */
#define AFPC_DISCOVERY_AFP_SERVICE_TYPE "_afpovertcp._tcp"
#define AFPC_DISCOVERY_ADISK_SERVICE_TYPE "_adisk._tcp"
#define AFPC_DISCOVERY_DEVICE_INFO_SERVICE_TYPE "_device-info._tcp"

#endif
