/*
 *  uams_dhx.c
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

/* The initialization vectors are universally fixed. These are the values
 * documented by Apple.
 */
const unsigned char dhx_c2siv[] = { 'L', 'W', 'a', 'l', 'l', 'a', 'c', 'e' };
const unsigned char dhx_s2civ[] = { 'C', 'J', 'a', 'l', 'b', 'e', 'r', 't' };

/* The values of p and g are fixed for DHCAST128. */
static const unsigned char p_binary[] = { 0xba, 0x28, 0x73, 0xdf, 0xb0, 0x60,
                                          0x57, 0xd4, 0x3f, 0x20, 0x24, 0x74, 0x4c, 0xee, 0xe7, 0x5b
                                        };
static const unsigned char g_binary[] = { 0x07 };

/*
 * Transaction sequence for DHCAST128 UAM:
 *
 * +---------------+  +----------------+  +-----------------+
 * |    FPLogin    |  +       ID       +  |   FPLoginCont   |
 * +---------------+  +----------------+  +-----------------+
 * |       0       |  | Random number  |  |        0        |
 * +---------------+  |   (16 bytes)   |  +-----------------+
 * /  AFPVersion   /  +----------------+  +       ID        +
 * +---------------+  | Nonce followed |  +-----------------+
 * |  'DHCAST128'  |  | by 16 bytes of |  |   Nonce + 1,    |
 * +---------------+  | zero encrypted |  | followed by the |
 * /   Username    /  | by session key |  |  password, all  |
 * + - - - - - - - +  |   (32 bytes)   |  |  encrypted by   |
 * |       0       |  +----------------+  |   session key   |
 * + - - - - - - - +                      +-----------------|
 * | Random number |
 * |  (16 bytes)   |
 * +---------------+
 */
int dhx_login(struct afp_server *server, char *username, char *passwd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    char *ai = NULL;
    char *d = NULL;
    unsigned char Ra_binary[32], K_binary[16];
    int ai_len, ret;
    const int Ma_len = 16, Mb_len = 16, nonce_len = 16;
    gcry_mpi_t p, g, Ra, Ma, Mb, K, nonce, new_nonce;
    size_t len;
    struct afp_rx_buffer rbuf;
    unsigned short ID;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    rbuf.data = NULL;
    /* Initialize all gcry_mpi_t variables, so they can all be uninitialized
     * in an orderly manner later. */
    p = gcry_mpi_new(0);
    g = gcry_mpi_new(0);
    Ra = gcry_mpi_new(0);
    Ma = gcry_mpi_new(0);
    Mb = gcry_mpi_new(0);
    K = gcry_mpi_new(0);
    nonce = gcry_mpi_new(0);
    new_nonce = gcry_mpi_new(0);
    /* Get p and g into a form that libgcrypt can use */
    gcry_mpi_scan(&p, GCRYMPI_FMT_USG, p_binary, sizeof(p_binary), NULL);
    gcry_mpi_scan(&g, GCRYMPI_FMT_USG, g_binary, sizeof(g_binary), NULL);
    /* Get random bytes for Ra. */
    gcry_randomize(Ra_binary, sizeof(Ra_binary), GCRY_STRONG_RANDOM);
    /* Translate the binary form of Ra into libgcrypt's preferred form */
    gcry_mpi_scan(&Ra, GCRYMPI_FMT_USG, Ra_binary, sizeof(Ra_binary), NULL);
    /* Ma = g^Ra mod p <- This is our "public" key, which we exchange
     * with the remote server to help make K, the session key. */
    gcry_mpi_powm(Ma, g, Ra, p);
    /* The first authinfo block, containing the username and our Ma value. */
    ai_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN) + 1 + Ma_len;
    ai = calloc(1, ai_len);
    d = ai;

    if (ai == NULL) {
        goto dhx_noctx_fail;
    }

    d += copy_to_pascal(ai, username) + 1;

    if (((long)d) % 2) {
        d++;
    } else {
        ai_len--;
    }

    /* Extract Ma to send to the server for the exchange. */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) d, Ma_len, &len, Ma);

    if (len < (size_t) Ma_len) {
        memmove(d + Ma_len - len, d, len);
        memset(d, 0, Ma_len - len);
    }

    /* 2 bytes for id, 16 bytes for Mb, 32 bytes of crypted message text */
    rbuf.maxsize = 2 + Mb_len + 32;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx_noctx_fail;
    }

    rbuf.size = 0;
    /* Send the first FPLogin request, and see what happens. */
    ret = afp_login(server, "DHCAST128", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx_noctx_cleanup;
    }

    /* The block returned from the server should always be 50 bytes.
     * If it's not, for now, choke and die loudly so we know it. */
    if (rbuf.size != rbuf.maxsize) {
        assert("size of data returned during dhx auth process was wrong size, should be 50 bytes!");
    }

    /* Extract the transaction ID from the server's reply block. */
    ID = ntohs(*(unsigned short *)d);
    d += sizeof(ID);
    /* Now, extract Mb (the server's "public key" part) directly into
     * a gcry_mpi_t. */
    gcry_mpi_scan(&Mb, GCRYMPI_FMT_USG, d, Mb_len, NULL);
    d += Mb_len;
    /* d now points to the ciphertext, which we'll decrypt in a bit. */
    /* K = Mb^Ra mod p <- This nets us the "session key", which we
     * actually use to encrypt and decrypt data. */
    gcry_mpi_powm(K, Mb, Ra, p);
    gcry_mpi_print(GCRYMPI_FMT_USG, K_binary, sizeof(K_binary), &len, K);

    if (len < sizeof(K_binary)) {
        memmove(K_binary + (sizeof(K_binary) - len), K_binary, len);
        memset(K_binary, 0, sizeof(K_binary) - len);
    }

    /* FIXME: To support the Reconnect UAM, we need to stash this key
     * somewhere in the session data. We'll worry about doing that
     * later, but this would be a prime spot to do that. */
    /* Set up our encryption context. */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_CAST5,
                                GCRY_CIPHER_MODE_CBC, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_noctx_fail;
    }

    /* Set the binary form of K as our key for this encryption context. */
    ctxerror = gcry_cipher_setkey(ctx, K_binary, sizeof(K_binary));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_fail;
    }

    /* Set the initialization vector for server->client transfer. */
    ctxerror = gcry_cipher_setiv(ctx, dhx_s2civ, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_fail;
    }

    /* The plaintext will hold the nonce (16 bytes) and the server's
     * signature (16 bytes - we don't actually look at it though). */
    len = nonce_len + 16;
    /* Decrypt the ciphertext from the server. */
    ctxerror = gcry_cipher_decrypt(ctx, d, len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_fail;
    }

    /* Pull the binary form of the nonce into a form that libgcrypt can
     * deal with. */
    gcry_mpi_scan(&nonce, GCRYMPI_FMT_USG, d, nonce_len, NULL);
    /* NOTE: The following 16 bytes of plaintext, which the docs indicate
     * as the server signature, will always contain just 0 values - Apple's
     * docs claim that due to an error in an early implementation, it will
     * always be that way. No point in looking at that. */
    /* d still points into rbuf.data, which is no longer needed. */
    free(rbuf.data);
    rbuf.data = NULL;
    /* Increment the nonce by 1 for sending back to the server. */
    gcry_mpi_add_ui(new_nonce, nonce, 1);
    /* New plaintext is 16 bytes of nonce, and (up to) 64 bytes of
     * password (filled out with NULL values). */
    ai_len = nonce_len + 64;
    ai = calloc(1, ai_len);
    d = ai;

    if (ai == NULL) {
        goto dhx_fail;
    }

    /* Pull the incremented nonce value back out into binary form. */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) d, nonce_len, &len,
                   new_nonce);

    if (len < (size_t) nonce_len) {
        memmove(d + nonce_len - len, d, len);
        memset(d, 0, nonce_len - len);
    }

    d += nonce_len;
    /* Copy the user's password into the plaintext. */
    strlcpy(d, passwd, 64);
    /* Set the initialization vector for client->server transfer. */
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_c2siv));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_fail;
    }

    /* Encrypt the plaintext to create our new authinfo block. */
    ctxerror = gcry_cipher_encrypt(ctx, ai, ai_len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_fail;
    }

    /* Send the FPLoginCont with the new authinfo block, sit back,
     * cross fingers... */
    ret = afp_logincont(server, ID, ai, ai_len, NULL);
    goto dhx_cleanup;
dhx_noctx_fail:
    ret = -1;
    goto dhx_noctx_cleanup;
dhx_fail:
    ret = -1;
dhx_cleanup:
    gcry_cipher_close(ctx);
dhx_noctx_cleanup:
    gcry_mpi_release(p);
    gcry_mpi_release(g);
    gcry_mpi_release(Ra);
    gcry_mpi_release(Ma);
    gcry_mpi_release(Mb);
    gcry_mpi_release(K);
    gcry_mpi_release(nonce);
    gcry_mpi_release(new_nonce);
    free(ai);
    free(rbuf.data);
    return ret;
}

/*
 *   Password change for DHCAST128 UAM (two-round DH exchange):
 *
 *   Round 1 (key exchange):
 *      Client sends:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |   'DHCAST128'    |
 *      +------------------+
 *      /  UserName/0x0000 /
 *      +------------------+
 *      |  sessid = 0x0000 |
 *      +------------------+
 *      |   Ma (16 bytes)  |
 *      +------------------+
 *
 *      Server responds with kFPAuthContinue:
 *      +------------------+
 *      |  sessid (2 bytes)|
 *      +------------------+
 *      |   Mb (16 bytes)  |
 *      +------------------+
 *      |  Encrypted block |
 *      | [nonce + 16 zero |
 *      |  bytes] (32 bytes|
 *      |  CAST5-CBC, K,   |
 *      |  "CJalbert" IV)  |
 *      +------------------+
 *
 *   Round 2 (password submission):
 *      Client sends:
 *      +------------------+
 *      | kFPChangePassword|
 *      +------------------+
 *      |        0         |
 *      +------------------+
 *      |   'DHCAST128'    |
 *      +------------------+
 *      /  UserName/0x0000 /
 *      +------------------+
 *      |  sessid (2 bytes)|
 *      +------------------+
 *      |  Encrypted block |
 *      | [nonce+1(16) +   |
 *      |  new pw (64) +   |
 *      |  old pw (64)]    |
 *      | (144 bytes,      |
 *      |  CAST5-CBC, K,   |
 *      |  "LWallace" IV)  |
 *      +------------------+
 */
int dhx_passwd(struct afp_server *server,
               char *username, char *passwd, char *newpasswd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    char *ai = NULL;
    char *d = NULL;
    unsigned char Ra_binary[32], K_binary[16];
    int ai_len, ret;
    const int Ma_len = 16, Mb_len = 16, nonce_len = 16;
    const int changepw_plaintext_len = nonce_len + 64 + 64; /* 144 bytes */
    gcry_mpi_t p, g, Ra, Ma, Mb, K, nonce, new_nonce;
    size_t len;
    struct afp_rx_buffer rbuf;
    unsigned short ID;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    int afp_version = server->using_version->av_number;
    int username_overhead;
    rbuf.data = NULL;
    /* Initialize all gcry_mpi_t variables */
    p = gcry_mpi_new(0);
    g = gcry_mpi_new(0);
    Ra = gcry_mpi_new(0);
    Ma = gcry_mpi_new(0);
    Mb = gcry_mpi_new(0);
    K = gcry_mpi_new(0);
    nonce = gcry_mpi_new(0);
    new_nonce = gcry_mpi_new(0);
    /* Get p and g into a form that libgcrypt can use */
    gcry_mpi_scan(&p, GCRYMPI_FMT_USG, p_binary, sizeof(p_binary), NULL);
    gcry_mpi_scan(&g, GCRYMPI_FMT_USG, g_binary, sizeof(g_binary), NULL);
    /* Get random bytes for Ra */
    gcry_randomize(Ra_binary, sizeof(Ra_binary), GCRY_STRONG_RANDOM);
    gcry_mpi_scan(&Ra, GCRYMPI_FMT_USG, Ra_binary, sizeof(Ra_binary), NULL);
    /* Ma = g^Ra mod p -- our "public" key */
    gcry_mpi_powm(Ma, g, Ra, p);

    /*
     * Round 1: Send sessid=0 + Ma, receive sessid + Mb + encrypted nonce
     */
    if (afp_version >= 30) {
        /* AFP 3.0+: two zero bytes instead of username */
        username_overhead = 2;
        ai_len = username_overhead + 2 + Ma_len; /* 0x0000 + sessid + Ma */
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx_pw_noctx_fail;
        }

        *d++ = 0;
        *d++ = 0;
    } else {
        /* AFP < 3.0: username as pascal string */
        int pascal_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
        int pad = (pascal_len & 1) ? 0 : 1;
        username_overhead = pascal_len + pad;
        ai_len = username_overhead + 2 + Ma_len;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx_pw_noctx_fail;
        }

        d += copy_to_pascal(d, username) + 1;

        if ((long)d & 0x1) {
            ai_len--;
        } else {
            d++;
        }
    }

    /* sessid = 0 for initialization */
    *d++ = 0;
    *d++ = 0;
    /* Extract Ma to send to the server */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *)d, Ma_len, &len, Ma);

    if (len < (size_t)Ma_len) {
        memmove(d + Ma_len - len, d, len);
        memset(d, 0, Ma_len - len);
    }

    /* Receive: 2 bytes ID + 16 bytes Mb + 32 bytes encrypted block = 50 */
    rbuf.maxsize = 2 + Mb_len + 32;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx_pw_noctx_fail;
    }

    rbuf.size = 0;
    /* Send round 1 */
    ret = afp_changepassword(server, "DHCAST128", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx_pw_noctx_cleanup;
    }

    if (rbuf.size != rbuf.maxsize) {
        goto dhx_pw_noctx_fail;
    }

    /* Extract transaction ID */
    ID = ntohs(*(unsigned short *)d);
    d += sizeof(ID);
    /* Extract Mb (server's "public key") */
    gcry_mpi_scan(&Mb, GCRYMPI_FMT_USG, d, Mb_len, NULL);
    d += Mb_len;
    /* d now points to the 32-byte encrypted block */
    /* K = Mb^Ra mod p -- the session key */
    gcry_mpi_powm(K, Mb, Ra, p);
    gcry_mpi_print(GCRYMPI_FMT_USG, K_binary, sizeof(K_binary), &len, K);

    if (len < sizeof(K_binary)) {
        memmove(K_binary + (sizeof(K_binary) - len), K_binary, len);
        memset(K_binary, 0, sizeof(K_binary) - len);
    }

    /* Set up encryption context to decrypt the nonce */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_CAST5,
                                GCRY_CIPHER_MODE_CBC, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_noctx_fail;
    }

    ctxerror = gcry_cipher_setkey(ctx, K_binary, sizeof(K_binary));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_fail;
    }

    /* Decrypt with server->client IV */
    ctxerror = gcry_cipher_setiv(ctx, dhx_s2civ, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_fail;
    }

    /* Decrypt the 32-byte block: nonce (16) + signature (16 zeros) */
    ctxerror = gcry_cipher_decrypt(ctx, d, nonce_len + 16, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_fail;
    }

    /* Extract nonce and increment by 1 */
    gcry_mpi_scan(&nonce, GCRYMPI_FMT_USG, d, nonce_len, NULL);
    gcry_mpi_add_ui(new_nonce, nonce, 1);
    free(rbuf.data);
    rbuf.data = NULL;

    /*
     * Round 2: Send sessid + encrypted [nonce+1 + new_pw + old_pw]
     */
    if (afp_version >= 30) {
        ai_len = 2 + 2 + changepw_plaintext_len;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx_pw_fail;
        }

        *d++ = 0;
        *d++ = 0;
    } else {
        int pascal_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
        int pad = (pascal_len & 1) ? 0 : 1;
        username_overhead = pascal_len + pad;
        ai_len = username_overhead + 2 + changepw_plaintext_len;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx_pw_fail;
        }

        d += copy_to_pascal(d, username) + 1;

        if ((long)d & 0x1) {
            ai_len--;
        } else {
            d++;
        }
    }

    /* Session ID from server */
    *(unsigned short *)d = htons(ID);
    d += sizeof(ID);
    /* Build plaintext: nonce+1 (16) + new password (64) + old password (64) */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *)d, nonce_len, &len,
                   new_nonce);

    if (len < (size_t)nonce_len) {
        memmove(d + nonce_len - len, d, len);
        memset(d, 0, nonce_len - len);
    }

    d += nonce_len;
    /* New password (64 bytes, null-padded) */
    memset(d, 0, 64);
    strlcpy(d, newpasswd, 64);
    d += 64;
    /* Old password (64 bytes, null-padded) */
    memset(d, 0, 64);
    strlcpy(d, passwd, 64);
    /* Encrypt the plaintext with client->server IV */
    /* d currently points into ai; reset to start of encrypted portion */
    d = ai + (ai_len - changepw_plaintext_len);
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_c2siv));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_fail;
    }

    ctxerror = gcry_cipher_encrypt(ctx, d, changepw_plaintext_len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx_pw_fail;
    }

    /* Send round 2 */
    ret = afp_changepassword(server, "DHCAST128", ai, ai_len, NULL);
    goto dhx_pw_cleanup;
dhx_pw_noctx_fail:
    ret = -1;
    goto dhx_pw_noctx_cleanup;
dhx_pw_fail:
    ret = -1;
dhx_pw_cleanup:
    gcry_cipher_close(ctx);
dhx_pw_noctx_cleanup:
    gcry_mpi_release(p);
    gcry_mpi_release(g);
    gcry_mpi_release(Ra);
    gcry_mpi_release(Ma);
    gcry_mpi_release(Mb);
    gcry_mpi_release(K);
    gcry_mpi_release(nonce);
    gcry_mpi_release(new_nonce);
    free(ai);
    free(rbuf.data);
    return ret;
}
