/*
 *  identify.c
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "dsi.h"

/* The model prefixes of the Macs that serve AFP, VMware's virtual one
 * included. */
static const char *const mac_models[] = {
    "Mac", "iMac", "eMac", "iBook", "PowerBook", "PowerMac", "RackMac",
    "Xserve", "VMware",
};

/* Whether machine_type starts with the model prefix of a Mac. */
static int is_mac_model(const char *machine_type)
{
    for (size_t i = 0; i < sizeof(mac_models) / sizeof(mac_models[0]); i++) {
        if (strncmp(machine_type, mac_models[i], strlen(mac_models[i])) == 0) {
            return 1;
        }
    }

    return 0;
}

enum afpc_server_type afp_identify_machine_type(const char *machine_type)
{
    if (machine_type == NULL) {
        return AFPC_SERVER_TYPE_UNKNOWN;
    }

    if (strncmp(machine_type, "Netatalk", 8) == 0) {
        return AFPC_SERVER_TYPE_NETATALK;
    } else if (strncmp(machine_type, "AirPort", 7) == 0) {
        return AFPC_SERVER_TYPE_AIRPORT;
    } else if (is_mac_model(machine_type)) {
        return AFPC_SERVER_TYPE_MACINTOSH;
    } else if (strncmp(machine_type, "TimeCapsule", 11) == 0) {
        return AFPC_SERVER_TYPE_TIMECAPSULE;
    } else if (strncmp(machine_type, "Windows", 7) == 0) {
        return AFPC_SERVER_TYPE_WINDOWS;
    }

    return AFPC_SERVER_TYPE_UNKNOWN;
}

/* Parses a release number such as "4.7.0dev" into version: up to three
 * numbers between dots, any suffix ignored.  Returns 0, or -1 with version
 * zeroed when text starts with no number. */
int server_version_parse(const char *text, struct afpc_server_version *version)
{
    unsigned int *parts[] = {
        &version->major, &version->minor, &version->patch,
    };
    const char *p = text;
    memset(version, 0, sizeof(*version));

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        char *end;
        unsigned long number;

        if (!isdigit((unsigned char) *p)) {
            return i == 0 ? -1 : 0;
        }

        number = strtoul(p, &end, 10);
        *parts[i] = number > UINT_MAX ? UINT_MAX : (unsigned int) number;

        if (*end != '.') {
            break;
        }

        p = end + 1;
    }

    return 0;
}

/* Whether version is major.minor.patch or a later release. */
int server_version_at_least(const struct afpc_server_version *version,
                            unsigned int major, unsigned int minor,
                            unsigned int patch)
{
    if (version->major != major) {
        return version->major > major;
    }

    if (version->minor != minor) {
        return version->minor > minor;
    }

    return version->patch >= patch;
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
    memset(&s->server_version, 0, sizeof(s->server_version));

    switch (s->server_type) {
    case AFPC_SERVER_TYPE_NETATALK:
        /* afpd sends "Netatalk" and its release, "Netatalk4.2.3". */
        server_version_parse(s->machine_type + strlen("Netatalk"),
                             &s->server_version);
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Netatalk %u.%u.%u",
                       s->server_name_printable, s->server_version.major,
                       s->server_version.minor, s->server_version.patch);
        break;

    case AFPC_SERVER_TYPE_AIRPORT:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as AirPort",
                       s->server_name_printable);
        break;

    case AFPC_SERVER_TYPE_MACINTOSH:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Macintosh",
                       s->server_name_printable);
        break;

    case AFPC_SERVER_TYPE_TIMECAPSULE:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Time Capsule",
                       s->server_name_printable);
        s->dsi_default_timeout = DSI_TIMECAPSULE_DEFAULT_TIMEOUT;
        break;

    case AFPC_SERVER_TYPE_WINDOWS:
        /* PCMacLan returns "Windows based PC"; Windows SFM returns "Windows NT"*/
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Windows",
                       s->server_name_printable);
        break;

    case AFPC_SERVER_TYPE_UNKNOWN:
    default:
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Could not identify server %s (machine type %s)",
                       s->server_name_printable,
                       s->machine_type);
        break;
    }
}

/* The servers the client's Apple compatibility flushes are for. */
int afp_server_is_apple(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_TIMECAPSULE
           || s->server_type == AFPC_SERVER_TYPE_AIRPORT
           || s->server_type == AFPC_SERVER_TYPE_MACINTOSH;
}

/* Whether a data fork's open asks for its length: Netatalk on AFP 3, the
 * server it is tested with. */
int afp_server_reports_fork_length(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && s->using_version->av_number >= 30;
}

/* Netatalk's FPCopyFile from 3.1.1, the first release on AFP 3.4: 2.x gives
 * a copy the source's exact mode, which can refuse the reopen for writing. */
int afp_server_copies_files(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && s->using_version->av_number >= 34;
}

/* Whether a command running past afpd's idle limit drops the session: before
 * 4.5 the limit ran in a signal handler.  An unknown release counts as old. */
int afp_server_drops_long_commands(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && !server_version_at_least(&s->server_version, 4, 5, 0);
}
