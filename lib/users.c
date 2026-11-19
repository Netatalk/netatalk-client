/*
 *  users.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief How a volume maps the server's user and group IDs to the client's
 */

#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>

#include "afp_internal.h"
#include "mapping.h"

/* How mapping works
 *
 * There are 3 kinds of user/group mapping:
 *
 * 1. Common user directory
 *
 * This is where the client and server share the same user/group database.
 * An example of this is where NIS is used.
 *
 * 2. Login ids
 *
 * This is where all the files appear as the user that logged in.  This would
 * typically be used where the databases are separate, and one user expects to
 * be able to read/write all the files he sees.  This can get confusing, since
 * any files that aren't his will appear to be owned by him, but writing to
 * them will result in an EACCES.
 *
 * 3. Name mapped
 *
 * All the uid/gid translation is done based on usernames.
 *
 * This is currently unsupported.
 *
 */

int translate_uidgid_to_client(struct afp_volume * volume,
                               unsigned int *newuid, unsigned int *newgid)
{
    switch (volume->mapping) {
    case AFP_MAPPING_COMMON:
        /* The user databases are the same, don't do
         * any translaion. */
        break;

    case AFP_MAPPING_LOGINIDS:
        /* This is the case where we always return the uid/gid
         * of the user who ran afpfsd.
         */
        *newuid = volume->server->passwd.pw_uid;
        *newgid = volume->server->passwd.pw_gid;
        break;

    default:
    case AFP_MAPPING_UNKNOWN:
        return -1;
    }

    return 0;
}

/*!
 * @brief Find how a volume maps the server's user and group IDs
 *
 * A mapping already set stays. Otherwise the IDs are common when the volume
 * has network IDs and UNIX privileges, and the server knows the user by the
 * local ID and name; the login's IDs stand for the server's when not.
 *
 * @param[in,out] volume  the volume, whose mapping is set
 * @returns 0
 */
int afp_detect_mapping(struct afp_volume * volume)
{
    char name[AFPC_MAX_NAME_BYTES];
    struct passwd * passwd_entry;
    unsigned int dummy;
    unsigned int tmpgid;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Detecting UID/GID mapping mode (current: %d)", volume->mapping);

    /* See if it is already set.  This is typically when the client
         * requested a specific mapping. */

    if (volume->mapping != AFP_MAPPING_UNKNOWN) {
        return 0;
    }

    if ((volume->attributes & kNoNetworkUserIDs)) {
        volume->mapping = AFP_MAPPING_LOGINIDS;
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Volume has kNoNetworkUserIDs set, forcing LOGINIDS mapping");
        return 0;
    }

    /* A volume without UNIX privileges gives a file no owner, which the
     * common mapping would show as root's. */
    if (!(volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX)) {
        volume->mapping = AFP_MAPPING_LOGINIDS;
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Volume has no UNIX privileges, keeping LOGINIDS");
        return 0;
    }

    /* Check to see if the user databases appear to be synchronous.
         * This is done based on the algorithm specified on p.20.
         */
    /* By default, we're in AFP_MAPPING_LOGINIDS. */
    volume->mapping = AFP_MAPPING_LOGINIDS;
    volume->server->server_gid_valid = 0;
    /* 1. call getuid */
    passwd_entry = &volume->server->passwd;
    /* 2. Send FPGetUserInfo to get the user's uid */

    if (afp_getuserinfo(volume->server, 1, /* this user */
                        0, /* Irrelevant since we're getting the info for ThisUser */
                        kFPGetUserInfo_USER_ID,
                        &volume->server->server_uid,
                        &dummy)) {
        return 0;
    }

    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Server reports UID %u for current user", volume->server->server_uid);

    /* In the past, this has not worked for some versions of Mac OS, but
     * this hasn't been fully verified */
    if (afp_getuserinfo(volume->server, 1, /* this user */
                        0,
                        kFPGetUserInfo_PRI_GROUPID, &dummy,
                        &tmpgid) == 0) {
        volume->server->server_gid_valid = 1;
        volume->server->server_gid = tmpgid;
    }

    /* 3. If they match, call getpwuid to get the local user name */
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "UID comparison: server=%u, client=%u",
                   volume->server->server_uid, passwd_entry->pw_uid);

    /* A user the passwd database lacks has no name to match. */
    if (volume->server->server_uid != passwd_entry->pw_uid
            || passwd_entry->pw_name[0] == '\0') {
        return 0;
    }

    /* An answer can come without the parser filling the name. */
    name[0] = '\0';

    /* 4. Send an FPMapID for the user's name. 5. The IDs are common when it
     * answers with the local name. */
    if (afp_mapid(volume->server,
                  (volume->attributes & kSupportsUTF8Names)
                  ? kUserIDToUTF8Name : kUserIDToName,
                  passwd_entry->pw_uid, name) == 0
            && strcmp(name, passwd_entry->pw_name) == 0) {
        volume->mapping = AFP_MAPPING_COMMON;
    }

    return 0;
}


