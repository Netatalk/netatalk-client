/*!
 * @file
 * @brief The login methods' entry points and shared vectors, and the
 *        registry's login and password change calls
 *
 * Internal to the session layer's login methods: uams.c and the method files
 * share it.
 * Lane: session.
 */

#ifndef __UAMS_H_
#define __UAMS_H_

struct afp_server;

int afp_dopasswd(struct afp_server *server,
                 unsigned int uam, char *username,
                 char *oldpasswd, char *newpasswd);
int afp_dologin(struct afp_server *server,
                unsigned int uam, char *username, char *passwd);

/* Cleartext UAM (uams_clrtxt.c) */
int cleartxt_login(struct afp_server *server, char *username, char *passwd);
int cleartxt_passwd(struct afp_server *server, char *username,
                    char *passwd, char *newpasswd);

#ifdef HAVE_LIBGCRYPT
/* Randnum UAMs (uams_randnum.c) */
int randnum_login(struct afp_server *server, char *username, char *passwd);
int randnum2_login(struct afp_server *server, char *username, char *passwd);
int randnum_passwd(struct afp_server *server, char *username,
                   char *passwd, char *newpasswd);

/* DHCAST128 UAM (uams_dhx.c) */
int dhx_login(struct afp_server *server, char *username, char *passwd);
int dhx_passwd(struct afp_server *server, char *username,
               char *passwd, char *newpasswd);

/* DHX2 UAM (uams_dhx2.c) */
int dhx2_login(struct afp_server *server, char *username, char *passwd);
int dhx2_passwd(struct afp_server *server, char *username,
                char *passwd, char *newpasswd);

/* SRP UAM (uams_srp.c) */
int srp_login(struct afp_server *server, char *username, char *passwd);

/* Shared DH initialization vectors (used by DHX and DHX2) */
extern const unsigned char dhx_c2siv[8];
extern const unsigned char dhx_s2civ[8];
#endif /* HAVE_LIBGCRYPT */

#endif
