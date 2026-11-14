/*
 *  uams.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief The client's UAMs: their registry, names, logins and password changes
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBGCRYPT
#include <gcrypt.h>
#include <assert.h>
#endif /* HAVE_LIBGCRYPT */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp_internal.h"
#include "compat.h"
#include "dsi.h"
#include "uam_registry.h"
#include "uams.h"
#include "utils.h"

struct afp_uam {
    unsigned int bitmap;
    char name[AFP_UAM_LENGTH];
    int (*do_server_login)(struct afp_server *server, char *username,
                           char *password);
    int (*do_server_passwd)(struct afp_server * server, char *username,
                            char *oldpasswd, char *newpasswd);
    struct afp_uam *next;
};

static struct afp_uam *uam_base = NULL;

static int noauth_login(struct afp_server *server, char *username,
                        char *passwd);

static struct afp_uam uam_noauth =
{UAM_NOUSERAUTHENT, "No User Authent", &noauth_login, NULL, NULL};
static struct afp_uam uam_cleartxt = {
    UAM_CLEARTXTPASSWRD, "Cleartxt Passwrd", &cleartxt_login,
    &cleartxt_passwd, NULL
};
#ifdef HAVE_LIBGCRYPT
static struct afp_uam uam_randnum =
{UAM_RANDNUMEXCHANGE, "Randnum Exchange", &randnum_login, &randnum_passwd, NULL};
static struct afp_uam uam_randnum2 =
{UAM_2WAYRANDNUM, "2-Way Randnum Exchange", &randnum2_login, &randnum_passwd, NULL};
static struct afp_uam uam_dhx =
{UAM_DHCAST128, "DHCAST128", &dhx_login, &dhx_passwd, NULL};
static struct afp_uam uam_dhx2 =
{UAM_DHX2, "DHX2", &dhx2_login, &dhx2_passwd, NULL};
static struct afp_uam uam_srp =
{UAM_SRP, "SRP", &srp_login, NULL, NULL};

#endif /* HAVE_LIBGCRYPT */

unsigned int default_uams_mask(void)
{
    unsigned int uam_mask = UAM_CLEARTXTPASSWRD;
#ifdef HAVE_LIBGCRYPT
    uam_mask |= UAM_RANDNUMEXCHANGE | UAM_2WAYRANDNUM;
    uam_mask |= UAM_DHCAST128 | UAM_DHX2 | UAM_SRP;
#endif
    return uam_mask;
}

/*!
 * @brief Write the names of the registered UAMs, separated by commas
 *
 * @param[out] names  receives the names, cut to fit
 * @param[in]  size   bytes of names, above 0
 * @returns names
 */
char *get_uam_names_list(char *names, size_t size)
{
    size_t pos = 0;
    names[0] = '\0';

    for (struct afp_uam *u = uam_base; u && pos < size; u = u->next) {
        int n = snprintf(names + pos, size - pos, "%s%.*s",
                         pos ? ", " : "", AFP_UAM_LENGTH, u->name);

        if (n < 0) {
            break;
        }

        pos += (size_t) n;
    }

    return names;
}

static int register_uam(struct afp_uam * uam)
{
    if ((uam->bitmap = uam_string_to_bitmap(uam->name)) == 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Could not register UAM: %s", uam->name);
        return -1;
    }

    if (!uam_base) {
        uam_base = uam;
    } else {
        struct afp_uam *u = uam_base;

        while (u->next) {
            u = u->next;
        }

        u->next = uam;
    }

    uam->next = NULL;
    return 0;
}

static struct afp_uam *find_uam_by_bitmap(unsigned int i)
{
    struct afp_uam * u = uam_base;

    for (; u; u = u->next)
        if (u->bitmap == i) {
            return u;
        }

    return NULL;
}

unsigned int find_uam_by_name(const char *name)
{
    struct afp_uam * u = uam_base;
    const char *resolved_name = resolve_uam_shorthand(name);

    for (; u; u = u->next)
        if (strcmp(u->name, resolved_name) == 0) {
            return u->bitmap;
        }

    return 0;
}

/*!
 * @brief Register the client's UAMs
 *
 * @returns 0, or -1 when one does not register
 */
int init_uams(void)
{
    uam_base = NULL;
    int ret = 0;
#ifdef HAVE_LIBGCRYPT
    /* libgcrypt's first call sets it up, which two logins at once would
     * race to do. */
    (void) gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION);
#endif
    ret |= register_uam(&uam_cleartxt);
    ret |= register_uam(&uam_noauth);
#ifdef HAVE_LIBGCRYPT
    ret |= register_uam(&uam_randnum);
    ret |= register_uam(&uam_randnum2);
    ret |= register_uam(&uam_dhx);
    ret |= register_uam(&uam_dhx2);
    ret |= register_uam(&uam_srp);
#endif /* HAVE_LIBGCRYPT */
    return ret;
}

static int noauth_login(
    struct afp_server *server,
    char *username _U_,
    char *passwd _U_
)
{
    return afp_login(server, "No User Authent", NULL, 0, NULL);
}

int afp_dologin(struct afp_server *server,
                unsigned int uam, char *username, char *passwd)
{
    struct afp_uam * u;

    if ((u = find_uam_by_bitmap(uam)) == NULL) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "afp_dologin -- Unknown UAM");
        return -1;
    }

    return u->do_server_login(server, username, passwd);
}

int afp_dopasswd(struct afp_server *server,
                 unsigned int uam, char *username,
                 char *oldpasswd, char *newpasswd)
{
    struct afp_uam * u;

    if ((u = find_uam_by_bitmap(uam)) == NULL) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "afp_dopasswd -- Unknown UAM");
        return -1;
    }

    if (u->do_server_passwd == NULL) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "afp_dopasswd -- UAM %s does not support password change",
                       u->name);
        return kFPCallNotSupported;
    }

    return u->do_server_passwd(server, username, oldpasswd, newpasswd);
}
