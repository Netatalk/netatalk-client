#ifndef NETATALK_CLIENT_PROTO_LOGIN_H
#define NETATALK_CLIENT_PROTO_LOGIN_H

#include <stddef.h>
#include <stdint.h>

/*
 * Build the payload for the first login request.  AFP 3.x selects
 * FPLoginExt; older versions select FPLogin.  username is omitted from a
 * legacy FPLogin when it is NULL (the guest UAM), and is encoded as the
 * empty FPLoginExt UserName in that case.
 *
 * pad_before_authinfo requests the even-byte boundary required by UAMs
 * whose opaque data follows their auth name.  It also preserves the padded
 * username-only form used by DHX2.
 */
int afp_login_initial_build_payload(int afp_version_number,
                                    const char *afp_version,
                                    const char *uam_name,
                                    const char *username,
                                    int pad_before_authinfo,
                                    const void *userauthinfo,
                                    size_t userauthinfo_len,
                                    uint8_t **payload_out,
                                    size_t *payload_len_out);

/*
 * Build an AFP FPLoginExt payload without sending it.  The caller owns the
 * returned buffer and must release it with afp_loginext_payload_free(), since
 * it contains a copy of UserAuthInfo.
 *
 * UserName is type 3 with a 16-bit big-endian byte length and UTF-8 bytes.
 * The type-3 directory_domain uses the same length-prefixed format in
 * Netatalk's FPLoginExt parser.  Pass "" for the default directory domain.
 */
int afp_loginext_build_payload(const char *afp_version,
                               const char *uam_name,
                               const char *username,
                               const char *directory_domain,
                               const void *userauthinfo,
                               size_t userauthinfo_len,
                               uint8_t **payload_out,
                               size_t *payload_len_out);

void afp_loginext_payload_free(uint8_t *payload, size_t payload_len);

#endif
