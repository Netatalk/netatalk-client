/*
 *  uams_clrtxt.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp_internal.h"
#include "uam_registry.h"
#include "compat.h"
#include "dsi.h"
#include "uams.h"
#include "utils.h"

/*
 *   Request block when using the Cleartext Password UAM:
 *
 *      +------------------+
 *      |     kFPLogin     |
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |'Cleartxt Passwrd'|
 *      +------------------+
 *      /     UserName     /
 *      + - - - - - - - -  +
 *      |        0         |
 *      + - - - - - - - -  +
 *      |     Password     |
 *      |   in cleartext   |
 *      |    (8 bytes)     |
 *      +------------------+
 */
int cleartxt_login(struct afp_server *server, char *username,
                   char *passwd)
{
    char *p, *ai = NULL;
    int len, ret;
    /* Pack the username and password into the authinfo struct. */
    len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN) + 1 + 8;
    ai = calloc(1, len);
    p = ai;

    if (ai == NULL) {
        goto cleartxt_fail;
    }

    p += copy_to_pascal(p, username) + 1;

    if ((long)p & 0x1) {
        len--;
    } else {
        p++;
    }

    size_t passwd_len = strnlen(passwd, 8);

    if (passwd_len > 8) {
        passwd_len = 8;
    }

    memcpy(p, passwd, passwd_len);

    if (passwd_len < 8) {
        memset(p + passwd_len, 0, 8 - passwd_len);
    }

    /* Send the login request on to the server. */
    ret = afp_login(server, "Cleartxt Passwrd", ai, len, NULL);
    goto cleartxt_cleanup;
cleartxt_fail:
    ret = -1;
cleartxt_cleanup:
    free(ai);
    return ret;
}

/*
 *   Request block when changing the password for cleartext
 *
 *   AFP < 3.0:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |'Cleartxt Passwrd'|
 *      +------------------+
 *      /     UserName     /
 *      + - - - - - - - -  +
 *      |        0         |
 *      + - - - - - - - -  +
 *      |   Old Password   |
 *      |   in cleartext   |
 *      |    (8 bytes)     |
 *      +------------------+
 *      |   New Password   |
 *      |   in cleartext   |
 *      |    (8 bytes)     |
 *      +------------------+
 *
 *   AFP >= 3.0:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |'Cleartxt Passwrd'|
 *      +------------------+
 *      |      0x0000      |
 *      +------------------+
 *      |   Old Password   |
 *      |   in cleartext   |
 *      |    (8 bytes)     |
 *      +------------------+
 *      |   New Password   |
 *      |   in cleartext   |
 *      |    (8 bytes)     |
 *      +------------------+
 */
int cleartxt_passwd(struct afp_server *server,
                    char *username, char *passwd, char *newpasswd)
{
    char *p, *ai = NULL;
    int len, ret;
    int afp_version = server->using_version->av_number;

    if (afp_version >= 30) {
        /* AFP 3.0+: username is not sent, just two zero bytes */
        len = 2 + 16;
        ai = calloc(1, len);
        p = ai;

        if (ai == NULL) {
            goto cleartxt_fail;
        }

        /* Two zero bytes (username placeholder) */
        *p++ = 0;
        *p++ = 0;
    } else {
        /* AFP < 3.0: username as pascal string */
        len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN) + 1 + 16;
        ai = calloc(1, len);
        p = ai;

        if (ai == NULL) {
            goto cleartxt_fail;
        }

        p += copy_to_pascal(p, username) + 1;

        if ((long)p & 0x1) {
            len--;
        } else {
            p++;
        }
    }

    /* Copy old password (8 bytes, null-padded) */
    size_t old_passwd_len = strnlen(passwd, 8);

    if (old_passwd_len > 8) {
        old_passwd_len = 8;
    }

    memcpy(p, passwd, old_passwd_len);

    if (old_passwd_len < 8) {
        memset(p + old_passwd_len, 0, 8 - old_passwd_len);
    }

    p += 8;
    /* Copy new password (8 bytes, null-padded) */
    size_t new_passwd_len = strnlen(newpasswd, 8);

    if (new_passwd_len > 8) {
        new_passwd_len = 8;
    }

    memcpy(p, newpasswd, new_passwd_len);

    if (new_passwd_len < 8) {
        memset(p + new_passwd_len, 0, 8 - new_passwd_len);
    }

    /* Send the password change request to the server. */
    ret = afp_changepassword(server, "Cleartxt Passwrd", ai, len, NULL);
    goto cleartxt_cleanup;
cleartxt_fail:
    ret = -1;
cleartxt_cleanup:
    free(ai);
    return ret;
}
