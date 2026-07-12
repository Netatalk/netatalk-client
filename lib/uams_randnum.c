/*
 *  uams_randnum.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

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

/*
 *   Request block when changing the password for Randnum Exchange
 *   (also used by 2-Way Randnum Exchange -- the key rotation used
 *   during login does not apply to password change)
 *
 *   AFP < 3.0:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |'Randnum Exchange'|
 *      +------------------+
 *      /     UserName     /
 *      + - - - - - - - -  +
 *      |        0         |
 *      + - - - - - - - -  +
 *      |   Old Password   |
 *      |  DES-encrypted   |
 *      |  with new passwd |
 *      |    (8 bytes)     |
 *      +------------------+
 *      |   New Password   |
 *      |  DES-encrypted   |
 *      |  with old passwd |
 *      |    (8 bytes)     |
 *      +------------------+
 *
 *   AFP >= 3.0:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |'Randnum Exchange'|
 *      +------------------+
 *      |      0x0000      |
 *      +------------------+
 *      |   Old Password   |
 *      |  DES-encrypted   |
 *      |  with new passwd |
 *      |    (8 bytes)     |
 *      +------------------+
 *      |   New Password   |
 *      |  DES-encrypted   |
 *      |  with old passwd |
 *      |    (8 bytes)     |
 *      +------------------+
 */
int randnum_passwd(struct afp_server *server,
                   char *username, char *passwd, char *newpasswd)
{
    char *p, *ai = NULL;
    int len, ret;
    int afp_version = server->using_version->av_number;
    char old_buf[8], new_buf[8];
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    /* Prepare null-padded 8-byte copies of old and new passwords */
    memset(old_buf, 0, sizeof(old_buf));
    size_t old_len = strnlen(passwd, 8);
    memcpy(old_buf, passwd, old_len);
    memset(new_buf, 0, sizeof(new_buf));
    size_t new_len = strnlen(newpasswd, 8);
    memcpy(new_buf, newpasswd, new_len);

    if (afp_version >= 30) {
        /* AFP 3.0+: username is not sent, just two zero bytes */
        len = 2 + 16;
        ai = calloc(1, len);
        p = ai;

        if (ai == NULL) {
            goto randnum_pw_fail;
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
            goto randnum_pw_fail;
        }

        p += copy_to_pascal(p, username) + 1;

        if ((long)p & 0x1) {
            len--;
        } else {
            p++;
        }
    }

    /* DES-encrypt old password with new password as key */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_DES,
                                GCRY_CIPHER_MODE_ECB, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto randnum_pw_fail;
    }

    ctxerror = gcry_cipher_setkey(ctx, new_buf, 8);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        gcry_cipher_close(ctx);
        goto randnum_pw_fail;
    }

    ctxerror = gcry_cipher_encrypt(ctx, p, 8, old_buf, 8);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        gcry_cipher_close(ctx);
        goto randnum_pw_fail;
    }

    gcry_cipher_close(ctx);
    p += 8;
    /* DES-encrypt new password with old password as key */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_DES,
                                GCRY_CIPHER_MODE_ECB, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto randnum_pw_fail;
    }

    ctxerror = gcry_cipher_setkey(ctx, old_buf, 8);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        gcry_cipher_close(ctx);
        goto randnum_pw_fail;
    }

    ctxerror = gcry_cipher_encrypt(ctx, p, 8, new_buf, 8);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        gcry_cipher_close(ctx);
        goto randnum_pw_fail;
    }

    gcry_cipher_close(ctx);
    /* Send the password change request.
     * Both Randnum Exchange and 2-Way Randnum Exchange use the
     * "Randnum Exchange" UAM name for password changes. */
    ret = afp_changepassword(server, "Randnum Exchange", ai, len, NULL);
    goto randnum_pw_cleanup;
randnum_pw_fail:
    ret = -1;
randnum_pw_cleanup:
    explicit_bzero(old_buf, sizeof(old_buf));
    explicit_bzero(new_buf, sizeof(new_buf));
    free(ai);
    return ret;
}

/*
 * Transaction sequence for Random Number Exchange UAM:
 *
 * +------------------+  +---------------+  +---------------+
 * |     FPLogin      |  |      ID       |  |  FPLoginCont  |
 * +------------------+  +---------------+  +---------------+
 * |        0         |  | Random number |  |       0       |
 * +------------------+  |   (8 bytes)   |  +---------------+
 * |'Randnum Exchange'|  +---------------+  |      ID       |
 * +------------------+                     +---------------+
 * /     UserName     /                     | Random number |
 * +------------------+                     |   encrypted   |
 *                                          | with password |
 *                                          |   (8 bytes)   |
 *                                          +---------------+
 */
int randnum_login(struct afp_server *server, char *username,
                  char *passwd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    char *ai = NULL, *p;
    unsigned char key_buffer[8];
    int ai_len, ret;
    const int randnum_len = 8;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    struct afp_rx_buffer rbuf;
    unsigned short ID;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Starting Randnum Exchange authentication for user '%s'", username);
    rbuf.maxsize = sizeof(ID) + randnum_len;
    rbuf.data = calloc(1, rbuf.maxsize);
    p = rbuf.data;

    if (rbuf.data == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Failed to allocate receive buffer");
        goto randnum_noctx_fail;
    }

    rbuf.size = 0;
    ai_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
    ai = calloc(1, ai_len);

    if (ai == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Failed to allocate authinfo buffer");
        goto randnum_noctx_fail;
    }

    copy_to_pascal(ai, username);
    /* Send the initial FPLogin request to the server. */
    ret = afp_login(server, "Randnum Exchange", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Randnum: FPLogin returned %d (expected %d for kFPAuthContinue), rbuf.size=%u",
                   ret, kFPAuthContinue, rbuf.size);

    if (ret != kFPAuthContinue) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Randnum: Server did not return kFPAuthContinue (got %d)", ret);
        goto randnum_noctx_cleanup;
    }

    /* For now, if the response block from the server isn't *exactly*
     * 10 bytes long (if we got kFPAuthContinue with this UAM, it
     * should never be any other size), die a horrible death. */
    if (rbuf.size != rbuf.maxsize) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Response size mismatch! Got %u bytes, expected %u bytes",
                       rbuf.size, rbuf.maxsize);
        assert("size of data returned during randnum auth process was wrong size, should be 10 bytes!");
    }

    /* Copy the relevant values out of the response block the server
     * sent to us. */
    ID = ntohs(*(unsigned short *)p);
    p += sizeof(ID);
    /* Establish encryption context for doing password encryption work. */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_DES,
                                GCRY_CIPHER_MODE_ECB, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Failed to create DES cipher context: %s",
                       gcry_strerror(ctxerror));
        goto randnum_noctx_fail;
    }

    /* Copy (up to 8 bytes of) the password into key_buffer. */
    memset(key_buffer, 0, sizeof(key_buffer));
    size_t passwd_len = strnlen(passwd, sizeof(key_buffer));
    memcpy(key_buffer, passwd, passwd_len);
    /* Set the provided password (now in key_buffer) as the encryption
     * key in our established context, for subsequent use to encrypt
     * the random number that the server sends us. */
    ctxerror = gcry_cipher_setkey(ctx, key_buffer, sizeof(key_buffer));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Failed to set DES key: %s",
                       gcry_strerror(ctxerror));
        goto randnum_fail;
    }

    /* Encrypt the random number data into the authinfo block for sending
     * to the server. */
    ctxerror = gcry_cipher_encrypt(ctx, p, randnum_len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum: Failed to encrypt random number: %s",
                       gcry_strerror(ctxerror));
        goto randnum_fail;
    }

    /* Send the FPLoginCont to the server, containing the server's
     * random number encrypted with the password. */
    ret = afp_logincont(server, ID, p, randnum_len, NULL);
    goto randnum_cleanup;
randnum_noctx_fail:
    ret = -1;
    goto randnum_noctx_cleanup;
randnum_fail:
    ret = -1;
randnum_cleanup:
    /* Destroy the encryption context. */
    gcry_cipher_close(ctx);
randnum_noctx_cleanup:

    if (ret == kFPNoErr) {
        log_for_client(NULL, AFPFSD, LOG_INFO,
                       "Randnum Exchange authentication succeeded for user '%s'", username);
    } else {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Randnum Exchange authentication failed for user '%s' with code %d",
                       username, ret);
    }

    free(rbuf.data);
    free(ai);
    return ret;
}

/*
 * First transaction of Two-Way Random Number Exchange UAM:
 *
 * +------------------------+  +---------------+
 * |        FPLogin         |  |      ID       |
 * +------------------------+  +---------------+
 * |           0            |  | Random number |
 * +------------------------+  |   (8 bytes)   |
 * |'2-Way Randnum Exchange'|  +---------------+
 * +------------------------+
 * /        UserName        /
 * +------------------------+
 *
 * Second transaction of Two-Way Random Number Exchange UAM:
 *
 * +---------------+  +---------------+
 * |  FPLoginCont  |  |    Client     |
 * +---------------+  | random number |
 * |       0       |  |   encrypted   |
 * +---------------+  | with password |
 * |      ID       |  |   (8 bytes)   |
 * +---------------+  +---------------+
 * | Random number |
 * |   encrypted   |
 * | with password |
 * |   (8 bytes)   |
 * +---------------+
 * |    Client     |
 * | random number |
 * |   (8 bytes)   |
 * +---------------+
 */
int randnum2_login(struct afp_server *server, char *username,
                   char *passwd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    char *ai = NULL, *p = NULL, crypted[8];
    unsigned char key_buffer[8];
    int ai_len, ret, carry;
    unsigned int i;
    size_t passwd_len;
    const int randnum_len = 8, crypted_len = 8;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    struct afp_rx_buffer rbuf;
    unsigned short ID;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Starting 2-Way Randnum Exchange authentication for user '%s'", username);
    rbuf.maxsize = sizeof(ID) + 8;
    rbuf.data = calloc(1, rbuf.maxsize);
    p = rbuf.data;

    if (rbuf.data == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to allocate receive buffer");
        return -1;
    }

    rbuf.size = 0;
    ai_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
    ai = calloc(1, ai_len);

    if (ai == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to allocate authinfo buffer");
        goto randnum2_noctx_fail;
    }

    copy_to_pascal(ai, username);
    /* Send the initial FPLogin request to the server. */
    ret = afp_login(server, "2-Way Randnum Exchange", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Randnum2: FPLogin returned %d (expected %d for kFPAuthContinue), rbuf.size=%u",
                   ret, kFPAuthContinue, rbuf.size);

    if (ret != kFPAuthContinue) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Randnum2: Server did not return kFPAuthContinue (got %d)", ret);
        goto randnum2_noctx_cleanup;
    }

    /* For now, if the response block from the server isn't *exactly*
     * 10 bytes long (if we got kFPAuthContinue with this UAM, it
     * should never be any other size), die a horrible death. */
    if (rbuf.size != rbuf.maxsize) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Response size mismatch! Got %u bytes, expected %u bytes",
                       rbuf.size, rbuf.maxsize);
        assert("size of data returned during randnum2 auth process was wrong size, should be 10 bytes!");
    }

    /* Copy the relevant values out of the response block the server
     * sent to us. */
    ID = ntohs(*(unsigned short *)p);
    p += sizeof(ID);
    /* Establish encryption context for doing password encryption work. */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_DES,
                                GCRY_CIPHER_MODE_ECB, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to create DES cipher context: %s",
                       gcry_strerror(ctxerror));
        goto randnum2_noctx_fail;
    }

    /* Copy (up to 8 bytes of) the password into key_buffer, after
     * zeroing it out first. */
    memset(key_buffer, 0, sizeof(key_buffer));
    passwd_len = strnlen(passwd, sizeof(key_buffer));
    memcpy(key_buffer, passwd, passwd_len);
    /* Rotate each byte left one bit, carrying the high bit to the next. */
    carry = key_buffer[0] >> 7;

    for (i = 0; i < sizeof(key_buffer) - 1; i++) {
        key_buffer[i] = (unsigned char)(key_buffer[i] << 1 | key_buffer[i + 1] >> 7);
    }

    /* Wrap the high bit we copied right away to the end of the array. */
    key_buffer[i] = (unsigned char)(key_buffer[i] << 1 | carry);
    /* Set the provided password (now in key_buffer) as the encryption
     * key in our established context, for subsequent use to encrypt
     * the random number that the server sends us. */
    ctxerror = gcry_cipher_setkey(ctx, key_buffer, 8);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to set DES key: %s",
                       gcry_strerror(ctxerror));
        goto randnum2_fail;
    }

    /* Setup a new authinfo block for the FPLoginCont invocation. It will
     * contain the DES hashed password, followed by our chosen random
     * number, which the server will use to hash the password and then
     * send back to us for comparison. */
    ai_len = crypted_len + randnum_len;
    ai = calloc(1, ai_len);

    if (ai == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to allocate second authinfo buffer");
        goto randnum2_fail;
    }

    /* Encrypt the random number data into the new authinfo block. */
    ctxerror = gcry_cipher_encrypt(ctx, ai, crypted_len, p, randnum_len);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to encrypt server's random: %s",
                       gcry_strerror(ctxerror));
        free(rbuf.data);
        rbuf.data = NULL;
        goto randnum2_fail;
    }

    free(rbuf.data);
    rbuf.data = NULL;
    p = ai + crypted_len;
    /* Use an internal gcrypt function to create the random number, so
     * we can do things (more) portably... */
    gcry_create_nonce(p, randnum_len);
    /* Make a place for the server's hashing of our password. */
    rbuf.maxsize = 8;
    rbuf.data = calloc(1, rbuf.maxsize);

    if (rbuf.data == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to allocate verification buffer");
        goto randnum2_fail;
    }

    rbuf.size = 0;
    /* Send the FPLoginCont to the server, containing the server's
     * random number encrypted with the password, and our random number.
     */
    ret = afp_logincont(server, ID, ai, ai_len, &rbuf);

    if (ret != kFPNoErr) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Randnum2: FPLoginCont failed with code %d", ret);
        goto randnum2_cleanup;
    }

    if (rbuf.size != rbuf.maxsize) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Verification response size mismatch! Got %u bytes, expected %u bytes",
                       rbuf.size, rbuf.maxsize);
        assert("size of data returned during randnum2 auth process was wrong size, should be 8 bytes!");
    }

    /* Encrypt our random number data into crypted[]. */
    ctxerror = gcry_cipher_encrypt(ctx, crypted, sizeof(crypted),
                                   p, randnum_len);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Failed to encrypt client random for verification: %s",
                       gcry_strerror(ctxerror));
        goto randnum2_fail;
    }

    /* If they didn't match, tell the caller that the user wasn't
     * authenticated, so it'll junk the connection. */
    if (memcmp(crypted, rbuf.data, sizeof(crypted)) != 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Randnum2: Verification FAILED - encrypted client random does not match server's response");
        ret = kFPUserNotAuth;
    }

    goto randnum2_cleanup;
randnum2_noctx_fail:
    ret = -1;
    goto randnum2_noctx_cleanup;
randnum2_fail:
    ret = -1;
randnum2_cleanup:
    /* Destroy the encryption context. */
    gcry_cipher_close(ctx);
randnum2_noctx_cleanup:

    if (ret == kFPNoErr) {
        log_for_client(NULL, AFPFSD, LOG_INFO,
                       "2-Way Randnum Exchange authentication succeeded for user '%s'", username);
    } else {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "2-Way Randnum Exchange authentication failed for user '%s' with code %d",
                       username, ret);
    }

    free(rbuf.data);
    free(ai);
    return ret;
}
