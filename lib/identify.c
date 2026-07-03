/*
 *  identify.c
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <string.h>
#include "afp.h"
#include "dsi.h"

enum afp_server_type afp_identify_machine_type(const char *machine_type)
{
    if (machine_type == NULL) {
        return AFP_SERVER_TYPE_UNKNOWN;
    }

    if (strncmp(machine_type, "Netatalk", 8) == 0) {
        return AFP_SERVER_TYPE_NETATALK;
    } else if (strncmp(machine_type, "AirPort", 7) == 0) {
        return AFP_SERVER_TYPE_AIRPORT;
    } else if (strncmp(machine_type, "Mac", 3) == 0
               || strncmp(machine_type, "iMac", 4) == 0
               || strncmp(machine_type, "Xserve", 6) == 0) {
        return AFP_SERVER_TYPE_MACINTOSH;
    } else if (strncmp(machine_type, "TimeCapsule", 11) == 0) {
        return AFP_SERVER_TYPE_TIMECAPSULE;
    } else if (strncmp(machine_type, "Windows", 7) == 0) {
        return AFP_SERVER_TYPE_WINDOWS;
    }

    return AFP_SERVER_TYPE_UNKNOWN;
}

/*
 * afp_server_identify()
 *
 * Identifies a server
 *
 * Right now, this only does identification using the machine_type
 * given in getsrvrinfo, but this could later use mDNS to get
 * more details.
 */

void afp_server_identify(struct afp_server * s)
{
    s->dsi_default_timeout = DSI_DEFAULT_TIMEOUT;
    s->server_type = afp_identify_machine_type(s->machine_type);

    switch (s->server_type) {
    case AFP_SERVER_TYPE_NETATALK:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Netatalk",
                       s->server_name_printable);
        break;

    case AFP_SERVER_TYPE_AIRPORT:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as AirPort",
                       s->server_name_printable);
        break;

    case AFP_SERVER_TYPE_MACINTOSH:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Macintosh",
                       s->server_name_printable);
        break;

    case AFP_SERVER_TYPE_TIMECAPSULE:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Time Capsule",
                       s->server_name_printable);
        s->dsi_default_timeout = DSI_TIMECAPSULE_DEFAULT_TIMEOUT;
        break;

    case AFP_SERVER_TYPE_WINDOWS:
        /* PCMacLan returns "Windows based PC"; Windows SFM returns "Windows NT"*/
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Windows",
                       s->server_name_printable);
        break;

    case AFP_SERVER_TYPE_UNKNOWN:
    default:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Could not identify server %s (machine type %s)",
                       s->server_name_printable,
                       s->machine_type);
        break;
    }
}
