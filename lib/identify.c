/*
 *  identify.c
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief Server type, version, and behavior predicates from the machine type
 */

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "dsi.h"

/* These are the model prefixes of the Macs that serve AFP, VMware's virtual
 * one included. */
static const char *const mac_models[] = {
    "Mac", "iMac", "eMac", "iBook", "PowerBook", "PowerMac", "RackMac",
    "Xserve", "VMware",
};

/*!
 * @brief Test whether machine_type starts with the model prefix of a Mac
 *
 * @param[in] machine_type  the server's machine type
 * @returns 1 for a Mac model, 0 otherwise
 */
static int is_mac_model(const char *machine_type)
{
    for (size_t i = 0; i < sizeof(mac_models) / sizeof(mac_models[0]); i++) {
        if (strncmp(machine_type, mac_models[i], strlen(mac_models[i])) == 0) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Map the machine type FPGetSrvrInfo reports to a server type
 *
 * @param[in] machine_type  the machine type, or NULL
 * @returns the server type, AFPC_SERVER_TYPE_UNKNOWN for NULL or a name it
 *          does not know
 */
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

/*!
 * @brief Parse a version number such as "4.6.1" or "4.7.0dev"
 *
 * Takes up to three numbers between dots and ignores any suffix. A number
 * above UINT_MAX counts as UINT_MAX.
 *
 * @param[in]  text     the version text
 * @param[out] version  the version, all zero when text has none
 * @returns 0, or -1 when text starts with no digit
 */
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

/*!
 * @brief Test whether version is major.minor.patch or a later version
 *
 * @param[in] version  the version to test
 * @param[in] major    major number of the oldest version that passes
 * @param[in] minor    minor number of that version
 * @param[in] patch    patch number of that version
 * @returns 1 for that version or a later one, 0 otherwise
 */
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

/*!
 * @brief Identify a server from the machine type FPGetSrvrInfo reported
 *
 * Sets the server type and the version a Netatalk server appends to its
 * machine type, all zero when it appends none. Also sets the reply timeout,
 * which is longer for a Time Capsule.
 *
 * @param[in,out] s  server with its machine_type set
 */
void afp_server_identify(struct afp_server * s)
{
    s->dsi_default_timeout = DSI_DEFAULT_TIMEOUT;
    s->server_type = afp_identify_machine_type(s->machine_type);
    memset(&s->server_version, 0, sizeof(s->server_version));

    switch (s->server_type) {
    case AFPC_SERVER_TYPE_NETATALK:
        /* afpd sends "Netatalk" and its version, "Netatalk4.2.3". */
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

/*!
 * @brief Test whether the server gets the Apple compatibility flushes
 *
 * @param[in] s  identified server
 * @returns 1 for a Time Capsule, an AirPort, or a Mac, 0 otherwise
 */
int afp_server_is_apple(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_TIMECAPSULE
           || s->server_type == AFPC_SERVER_TYPE_AIRPORT
           || s->server_type == AFPC_SERVER_TYPE_MACINTOSH;
}

/*!
 * @brief Test whether a data fork's open asks the server for its length
 *
 * The open asks only Netatalk on AFP 3, the one server known to report it.
 *
 * @param[in] s  identified server with a negotiated AFP version
 * @returns 1 when the open asks for the length, 0 otherwise
 */
int afp_server_reports_fork_length(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && s->using_version->av_number >= 30;
}

/*!
 * @brief Test whether the offload may copy files with the server's FPCopyFile
 *
 * Netatalk qualifies from 3.1.1, its first release on AFP 3.4. Netatalk 2.x
 * gives a copy the source's exact mode, which can refuse the reopen for
 * writing.
 *
 * @param[in] s  identified server with a negotiated AFP version
 * @returns 1 for Netatalk on AFP 3.4, 0 otherwise
 */
int afp_server_copies_files(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && s->using_version->av_number >= 34;
}

/*!
 * @brief Test whether a command past afpd's idle limit drops the session
 *
 * afpd before 4.5 runs the idle limit in a signal handler.
 *
 * @param[in] s  identified server
 * @returns 1 for Netatalk before 4.5 or of an unknown version, 0 otherwise
 */
int afp_server_drops_long_commands(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && !server_version_at_least(&s->server_version, 4, 5, 0);
}

/*!
 * @brief Test whether the server sets a directory's owner and group alone
 *
 * Netatalk 2 changes those of every file in the directory with them, and
 * the AFP specification leaves a directory's owner to FPSetDirParms.
 *
 * @param[in] s  identified server
 * @returns 1 for Netatalk 3 and later, 0 otherwise
 */
int afp_server_sets_directory_owner(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && server_version_at_least(&s->server_version, 3, 0, 0);
}

/*!
 * @brief Test whether the server keeps an owner or a group of -1 as it is
 *
 * @param[in] s  identified server
 * @returns 1 for Netatalk, 0 otherwise
 */
int afp_server_keeps_unset_ids(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK;
}

/*!
 * @brief Test whether the server may keep a directory's mode and answer
 *        success
 *
 * Netatalk 2 drops chmod(2)'s EPERM for a directory, and from 2.3 sets no
 * directory's own mode.
 *
 * @param[in] s  identified server
 * @returns 1 for Netatalk before 3, 0 otherwise
 */
int afp_server_keeps_directory_modes(const struct afp_server *s)
{
    return s->server_type == AFPC_SERVER_TYPE_NETATALK
           && !server_version_at_least(&s->server_version, 3, 0, 0);
}
