/*
 *  uams_dhx2.c
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

int dhx2_login(struct afp_server *server, char *username, char *passwd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    gcry_mpi_t p, g, Ma, Mb, Ra, K, nonce, new_nonce;
    char *ai = NULL, *d, *Ra_binary = NULL, *K_binary = NULL;
    char *K_hash = NULL, nonce_binary[16];
    int ai_len, hash_len, ret;
    const int g_len = 4;
    size_t len;
    struct afp_rx_buffer rbuf;
    unsigned short ID, bignum_len;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    rbuf.data = NULL;
    p = gcry_mpi_new(0);
    g = gcry_mpi_new(0);
    Ra = gcry_mpi_new(0);
    Ma = gcry_mpi_new(0);
    Mb = gcry_mpi_new(0);
    K = gcry_mpi_new(0);
    nonce = gcry_mpi_new(0);
    new_nonce = gcry_mpi_new(0);
    ai_len = (int)strnlen(username, AFP_MAX_USERNAME_LEN) + 1;
    ai = calloc(1, ai_len);

    if (ai == NULL) {
        goto dhx2_noctx_fail;
    }

    copy_to_pascal(ai, username);
    /* Reply block will contain:
     *   Transaction ID (2 bytes, MSB)
     *   g (4 bytes, MSB)
     *   length of large values in bytes (2 bytes, MSB)
     *   p (minimum 64 bytes, indicated by length value, MSB)
     *   Mb (minimum 64 bytes, indicated by length value, MSB)
     * We'll reserve 256 bytes for each of p and Mb, which covers
     * primes up to 2048 bits. Known servers use 512 or 1024 bits. */
    rbuf.maxsize = 2 + 4 + 2 + 256 * 2;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx2_noctx_fail;
    }

    rbuf.size = 0;
    /* Send the initial request in the login sequence. */
    ret = afp_login(server, "DHX2", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx2_noctx_cleanup;
    }

    /* Pull the transaction ID out of the reply block. */
    ID = ntohs(*(unsigned short *)d);
    d += sizeof(ID);
    /* Pull the value of g out of the reply block and directly into an
     * gcry_mpi_t container for later use with libgcrypt. */
    gcry_mpi_scan(&g, GCRYMPI_FMT_USG, d, g_len, NULL);
    d += g_len;
    bignum_len = ntohs(*(unsigned short *)d);
    d += sizeof(bignum_len);

    if (bignum_len > 256) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "DHX2: server prime length %u bytes exceeds maximum supported (256)",
                       bignum_len);
        goto dhx2_noctx_fail;
    }

    /* Extract p into an gcry_mpi_t. */
    gcry_mpi_scan(&p, GCRYMPI_FMT_USG, d, bignum_len, NULL);
    d += bignum_len;
    /* Extract Mb into an gcry_mpi_t. */
    gcry_mpi_scan(&Mb, GCRYMPI_FMT_USG, d, bignum_len, NULL);
    free(rbuf.data);
    rbuf.data = NULL;
    Ra_binary = calloc(1, bignum_len);

    if (Ra_binary == NULL) {
        goto dhx2_noctx_fail;
    }

    /* Get random bytes for Ra. */
    gcry_randomize(Ra_binary, bignum_len, GCRY_STRONG_RANDOM);
    /* Pull the random value we just read into an gcry_mpi_t so we can do
     * large-value exponentiation, and generate our Ma. */
    gcry_mpi_scan(&Ra, GCRYMPI_FMT_USG, Ra_binary, bignum_len, NULL);
    free(Ra_binary);
    Ra_binary = NULL;
    /* Ma = g^Ra mod p <- This is our "public" key, which we exchange
     * with the remote server to help make K, the session key. */
    gcry_mpi_powm(Ma, g, Ra, p);
    /* K = Mb^Ra mod p <- This nets us the "session key", which we
     * actually use to encrypt and decrypt data. */
    gcry_mpi_powm(K, Mb, Ra, p);
    K_binary = calloc(1, bignum_len);

    if (K_binary == NULL) {
        goto dhx2_noctx_fail;
    }

    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) K_binary, bignum_len, &len,
                   K);

    if (len < bignum_len) {
        memmove(K_binary + bignum_len - len, K_binary, len);
        memset(K_binary, 0, bignum_len - len);
    }

    /* Use a one-shot hash function to generate the MD5 hash of K. */
    hash_len = gcry_md_get_algo_dlen(GCRY_MD_MD5);
    K_hash = calloc(1, hash_len);

    if (K_hash == NULL) {
        goto dhx2_noctx_fail;
    }

    gcry_md_hash_buffer(GCRY_MD_MD5, K_hash, K_binary, bignum_len);
    /* FIXME: To support the Reconnect UAM, we need to stash this key
     * somewhere in the session data. We'll worry about doing that
     * later, but this would be a prime spot to do that. */
    /* Use an internal gcrypt function to create the random number, so
     * we can do things (more) portably... */
    gcry_create_nonce(nonce_binary, sizeof(nonce_binary));
    /* Set up our encryption context. */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_CAST5,
                                GCRY_CIPHER_MODE_CBC, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_noctx_fail;
    }

    /* Set the hashed form of K as our key for this encryption context. */
    ctxerror = gcry_cipher_setkey(ctx, K_hash, hash_len);
    free(K_hash);
    K_hash = NULL;

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* Set the initialization vector for client->server transfer. */
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* The new authinfo block will contain Ma (our "public" key part) and
     * the encrypted form of our nonce. */
    ai_len = bignum_len + sizeof(nonce_binary);
    ai = calloc(1, ai_len);
    d = ai;

    if (ai == NULL) {
        goto dhx2_fail;
    }

    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) d, bignum_len, &len, Ma);

    if (len < bignum_len) {
        memmove(d + bignum_len - len, d, len);
        memset(d, 0, bignum_len - len);
    }

    d += bignum_len;
    /* Encrypt our nonce into the new authinfo block. */
    ctxerror = gcry_cipher_encrypt(ctx, d, sizeof(nonce_binary),
                                   nonce_binary, sizeof(nonce_binary));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* Reply block will contain ID, then the encrypted form of our
     * nonce + 1 and the server's nonce. */
    rbuf.maxsize = sizeof(ID) + sizeof(nonce_binary) * 2;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx2_fail;
    }

    rbuf.size = 0;
    ret = afp_logincont(server, ID, ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx2_cleanup;
    }

    /* Get the new transaction ID for the last portion of the exchange. */
    ID = ntohs(*(unsigned short *)d);
    d += sizeof(ID);
    /* Set the initialization vector for server->client transfer. */
    ctxerror = gcry_cipher_setiv(ctx, dhx_s2civ, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    len = rbuf.maxsize - sizeof(ID);
    /* Decrypt the ciphertext from the server. */
    ctxerror = gcry_cipher_decrypt(ctx, d, len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* Pull our nonce into an gcry_mpi_t so we can operate. */
    gcry_mpi_scan(&nonce, GCRYMPI_FMT_USG, nonce_binary, sizeof(nonce_binary),
                  NULL);
    /* Increment our nonce by one. */
    gcry_mpi_add_ui(new_nonce, nonce, 1);
    /* Pull the incremented nonce back out into binary form. */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) nonce_binary,
                   sizeof(nonce_binary), &len,
                   new_nonce);

    if (len < sizeof(nonce_binary)) {
        memmove(nonce_binary + sizeof(nonce_binary) - len,
                nonce_binary, len);
        memset(nonce_binary, 0, sizeof(nonce_binary) - len);
    }

    /* Compare our incremented nonce to the server's incremented copy
     * of our original nonce value; if they don't match, something
     * terrible has happened. */
    if (memcmp(nonce_binary, d, 16) != 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "DHX2: nonce verification failed");
        goto dhx2_fail;
    }

    d += sizeof(nonce_binary);
    /* Pull the server's nonce value into an gcry_mpi_t. */
    gcry_mpi_scan(&nonce, GCRYMPI_FMT_USG, d, sizeof(nonce_binary), NULL);
    /* d still points into rbuf.data; let's dispose of it safely. */
    free(rbuf.data);
    rbuf.data = NULL;
    /* Increment the server's nonce by one. */
    gcry_mpi_add_ui(new_nonce, nonce, 1);
    /* The new plaintext will need 16 bytes for the server nonce (after
     * incrementing), followed by 256 bytes of null-filled space for the
     * user's password. */
    ai_len = sizeof(nonce_binary) + 256;
    ai = calloc(1, ai_len);
    d = ai;

    if (ai == NULL) {
        goto dhx2_fail;
    }

    /* Extract the binary form of the incremented server nonce into
     * the plaintext buffer. */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *) d, sizeof(nonce_binary), &len,
                   new_nonce);

    if (len < sizeof(nonce_binary)) {
        memmove(d + sizeof(nonce_binary) - len, d, len);
        memset(d, 0, sizeof(nonce_binary) - len);
    }

    d += sizeof(nonce_binary);
    /* Copy the user's password into the plaintext buffer. */
    strlcpy(d, passwd, 256);
    /* Set the initialization vector for client->server transfer. */
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* Encrypt our nonce into the new authinfo block. */
    ctxerror = gcry_cipher_encrypt(ctx, ai, ai_len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_fail;
    }

    /* Send the FPLoginCont with the new authinfo block, sit back,
     * cross fingers... */
    ret = afp_logincont(server, ID, ai, ai_len, NULL);
    goto dhx2_cleanup;
dhx2_noctx_fail:
    ret = -1;
    goto dhx2_noctx_cleanup;
dhx2_fail:
    ret = -1;
dhx2_cleanup:
    gcry_cipher_close(ctx);
dhx2_noctx_cleanup:
    gcry_mpi_release(p);
    gcry_mpi_release(g);
    gcry_mpi_release(Ra);
    gcry_mpi_release(Ma);
    gcry_mpi_release(Mb);
    gcry_mpi_release(K);
    gcry_mpi_release(nonce);
    gcry_mpi_release(new_nonce);
    free(Ra_binary);
    free(K_binary);
    free(K_hash);
    free(ai);
    free(rbuf.data);
    return ret;
}

/*
 *   Password change for DHX2 UAM (three-round DH exchange):
 *
 *   Round 1 (DH parameter exchange):
 *      Client sends FPChangePassword with username only (no payload).
 *      Server responds with kFPAuthContinue:
 *        ID(2) + g(4) + bignum_len(2) + p(bignum_len) + Ma(bignum_len)
 *
 *   Round 2 (key exchange + nonce):
 *      Client computes Mb = g^Rb mod p, K = Ma^Rb mod p, K_hash = MD5(K)
 *      Client sends:  username + ID(2) + Mb(bignum_len) + enc_nonce(16)
 *      Server responds with kFPAuthContinue:
 *        ID+1(2) + encrypted[clientNonce+1(16) + serverNonce(16)]
 *
 *   Round 3 (password submission):
 *      Client sends:  username + ID+1(2) +
 *        encrypted[serverNonce+1(16) + new_pw(256) + old_pw(256)]
 *      Server responds with success or error.
 */
int dhx2_passwd(struct afp_server *server,
                char *username, char *passwd, char *newpasswd)
{
    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        assert("libgcrypt initialization failed");
    }

    gcry_mpi_t p, g, Ma, Mb, Rb, K, clientNonce, new_clientNonce;
    gcry_mpi_t serverNonce, new_serverNonce;
    char *ai = NULL, *d, *Rb_binary = NULL, *K_binary = NULL;
    char *K_hash = NULL, nonce_binary[16];
    int ai_len, hash_len, ret;
    const int g_len = 4;
    size_t len;
    struct afp_rx_buffer rbuf;
    unsigned short ID;
    unsigned short bignum_len;
    gcry_cipher_hd_t ctx;
    gcry_error_t ctxerror;
    int afp_version = server->using_version->av_number;
    int have_ctx = 0;
    rbuf.data = NULL;
    p = gcry_mpi_new(0);
    g = gcry_mpi_new(0);
    Rb = gcry_mpi_new(0);
    Ma = gcry_mpi_new(0);
    Mb = gcry_mpi_new(0);
    K = gcry_mpi_new(0);
    clientNonce = gcry_mpi_new(0);
    new_clientNonce = gcry_mpi_new(0);
    serverNonce = gcry_mpi_new(0);
    new_serverNonce = gcry_mpi_new(0);

    /*
     * Round 1: Send username only, receive DH parameters
     */
    if (afp_version >= 30) {
        ai_len = 2;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx2_pw_noctx_fail;
        }

        *d++ = 0;
        *d++ = 0;
    } else {
        ai_len = (int)strnlen(username, AFP_MAX_USERNAME_LEN) + 1;
        ai = calloc(1, ai_len);

        if (ai == NULL) {
            goto dhx2_pw_noctx_fail;
        }

        copy_to_pascal(ai, username);
    }

    /* Receive: ID(2) + g(4) + bignum_len(2) + p(bignum_len) + Ma(bignum_len)
     * Reserve 256 bytes each for p and Ma */
    rbuf.maxsize = 2 + 4 + 2 + 256 * 2;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx2_pw_noctx_fail;
    }

    rbuf.size = 0;
    ret = afp_changepassword(server, "DHX2", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx2_pw_noctx_cleanup;
    }

    /* Parse server's DH parameters */
    ID = ntohs(*(unsigned short *)d);
    d += sizeof(ID);
    gcry_mpi_scan(&g, GCRYMPI_FMT_USG, d, g_len, NULL);
    d += g_len;
    bignum_len = ntohs(*(unsigned short *)d);
    d += sizeof(bignum_len);

    if (bignum_len > 256) {
        goto dhx2_pw_noctx_fail;
    }

    /* Extract p and Ma (server's public key) */
    gcry_mpi_scan(&p, GCRYMPI_FMT_USG, d, bignum_len, NULL);
    d += bignum_len;
    gcry_mpi_scan(&Ma, GCRYMPI_FMT_USG, d, bignum_len, NULL);
    free(rbuf.data);
    rbuf.data = NULL;
    /* Generate our random Rb and compute Mb = g^Rb mod p */
    Rb_binary = calloc(1, bignum_len);

    if (Rb_binary == NULL) {
        goto dhx2_pw_noctx_fail;
    }

    gcry_randomize(Rb_binary, bignum_len, GCRY_STRONG_RANDOM);
    gcry_mpi_scan(&Rb, GCRYMPI_FMT_USG, Rb_binary, bignum_len, NULL);
    free(Rb_binary);
    Rb_binary = NULL;
    gcry_mpi_powm(Mb, g, Rb, p);
    gcry_mpi_powm(K, Ma, Rb, p);
    K_binary = calloc(1, bignum_len);

    if (K_binary == NULL) {
        goto dhx2_pw_noctx_fail;
    }

    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *)K_binary, bignum_len,
                   &len, K);

    if (len < bignum_len) {
        memmove(K_binary + bignum_len - len, K_binary, len);
        memset(K_binary, 0, bignum_len - len);
    }

    hash_len = gcry_md_get_algo_dlen(GCRY_MD_MD5);
    K_hash = calloc(1, hash_len);

    if (K_hash == NULL) {
        goto dhx2_pw_noctx_fail;
    }

    gcry_md_hash_buffer(GCRY_MD_MD5, K_hash, K_binary, bignum_len);
    free(K_binary);
    K_binary = NULL;
    /* Generate client nonce */
    gcry_create_nonce(nonce_binary, sizeof(nonce_binary));
    /* Set up encryption context */
    ctxerror = gcry_cipher_open(&ctx, GCRY_CIPHER_CAST5,
                                GCRY_CIPHER_MODE_CBC, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_noctx_fail;
    }

    have_ctx = 1;
    ctxerror = gcry_cipher_setkey(ctx, K_hash, hash_len);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    /* Encrypt client nonce with C2S IV */
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_c2siv));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    /*
     * Round 2: Send Mb + encrypted nonce, receive encrypted nonces
     */
    if (afp_version >= 30) {
        ai_len = 2 + 2 + bignum_len + sizeof(nonce_binary);
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx2_pw_fail;
        }

        *d++ = 0;
        *d++ = 0;
    } else {
        int pascal_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
        int pad = (pascal_len & 1) ? 0 : 1;
        ai_len = pascal_len + pad + 2 + bignum_len + sizeof(nonce_binary);
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx2_pw_fail;
        }

        d += copy_to_pascal(d, username) + 1;

        if ((long)d & 0x1) {
            ai_len--;
        } else {
            d++;
        }
    }

    /* Session ID */
    *(unsigned short *)d = htons(ID);
    d += 2;
    /* Mb (client's public key) */
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *)d, bignum_len, &len, Mb);

    if (len < bignum_len) {
        memmove(d + bignum_len - len, d, len);
        memset(d, 0, bignum_len - len);
    }

    d += bignum_len;
    /* Encrypted client nonce */
    ctxerror = gcry_cipher_encrypt(ctx, d, sizeof(nonce_binary),
                                   nonce_binary, sizeof(nonce_binary));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    /* Receive 34 bytes: 2-byte ID followed by a 32-byte encrypted block
     * containing the incremented client nonce (16 bytes) and server nonce (16 bytes) */
    rbuf.maxsize = 2 + 32;
    rbuf.data = calloc(1, rbuf.maxsize);
    d = rbuf.data;

    if (rbuf.data == NULL) {
        goto dhx2_pw_fail;
    }

    rbuf.size = 0;
    ret = afp_changepassword(server, "DHX2", ai, ai_len, &rbuf);
    free(ai);
    ai = NULL;

    if (ret != kFPAuthContinue) {
        goto dhx2_pw_cleanup;
    }

    /* Get the new transaction ID */
    ID = ntohs(*(unsigned short *)d);
    d += 2;
    /* Decrypt server's response with S2C IV */
    ctxerror = gcry_cipher_setiv(ctx, dhx_s2civ, sizeof(dhx_s2civ));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    ctxerror = gcry_cipher_decrypt(ctx, d, 32, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    /* Verify clientNonce + 1 */
    gcry_mpi_scan(&clientNonce, GCRYMPI_FMT_USG, nonce_binary,
                  sizeof(nonce_binary), NULL);
    gcry_mpi_add_ui(new_clientNonce, clientNonce, 1);
    gcry_mpi_t retNonce;
    retNonce = gcry_mpi_new(0);
    gcry_mpi_scan(&retNonce, GCRYMPI_FMT_USG, d, 16, NULL);

    if (gcry_mpi_cmp(new_clientNonce, retNonce) != 0) {
        gcry_mpi_release(retNonce);
        goto dhx2_pw_fail;
    }

    gcry_mpi_release(retNonce);
    /* Extract server nonce */
    d += 16;
    gcry_mpi_scan(&serverNonce, GCRYMPI_FMT_USG, d, 16, NULL);
    free(rbuf.data);
    rbuf.data = NULL;
    /*
     * Round 3: Send an encrypted block containing the incremented server nonce,
     * the new password (256 bytes), and the old password (256 bytes)
     */
    gcry_mpi_add_ui(new_serverNonce, serverNonce, 1);
    int payload_len = 16 + 256 + 256; /* 528 bytes */

    if (afp_version >= 30) {
        ai_len = 2 + 2 + payload_len;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx2_pw_fail;
        }

        *d++ = 0;
        *d++ = 0;
    } else {
        int pascal_len = 1 + (int)strnlen(username, AFP_MAX_USERNAME_LEN);
        int pad = (pascal_len & 1) ? 0 : 1;
        ai_len = pascal_len + pad + 2 + payload_len;
        ai = calloc(1, ai_len);
        d = ai;

        if (ai == NULL) {
            goto dhx2_pw_fail;
        }

        d += copy_to_pascal(d, username) + 1;

        if ((long)d & 0x1) {
            ai_len--;
        } else {
            d++;
        }
    }

    /* Session ID (ID+1 from round 2 response) */
    *(unsigned short *)d = htons(ID);
    d += 2;
    /* Build plaintext: serverNonce+1(16) + new_pw(256) + old_pw(256) */
    char *plaintext_start = d;
    gcry_mpi_print(GCRYMPI_FMT_USG, (unsigned char *)d, 16, &len,
                   new_serverNonce);

    if (len < 16) {
        memmove(d + 16 - len, d, len);
        memset(d, 0, 16 - len);
    }

    d += 16;
    /* New password (256 bytes, null-padded) */
    memset(d, 0, 256);
    strlcpy(d, newpasswd, 256);
    d += 256;
    /* Old password (256 bytes, null-padded) */
    memset(d, 0, 256);
    strlcpy(d, passwd, 256);
    /* Encrypt with C2S IV */
    ctxerror = gcry_cipher_setiv(ctx, dhx_c2siv, sizeof(dhx_c2siv));

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    ctxerror = gcry_cipher_encrypt(ctx, plaintext_start, payload_len, NULL, 0);

    if (gcry_err_code(ctxerror) != GPG_ERR_NO_ERROR) {
        goto dhx2_pw_fail;
    }

    /* Send round 3 */
    ret = afp_changepassword(server, "DHX2", ai, ai_len, NULL);
    goto dhx2_pw_cleanup;
dhx2_pw_noctx_fail:
    ret = -1;
    goto dhx2_pw_noctx_cleanup;
dhx2_pw_fail:
    ret = -1;
dhx2_pw_cleanup:

    if (have_ctx) {
        gcry_cipher_close(ctx);
    }

dhx2_pw_noctx_cleanup:
    gcry_mpi_release(p);
    gcry_mpi_release(g);
    gcry_mpi_release(Rb);
    gcry_mpi_release(Ma);
    gcry_mpi_release(Mb);
    gcry_mpi_release(K);
    gcry_mpi_release(clientNonce);
    gcry_mpi_release(new_clientNonce);
    gcry_mpi_release(serverNonce);
    gcry_mpi_release(new_serverNonce);
    free(Rb_binary);
    free(K_binary);
    free(K_hash);
    free(ai);
    free(rbuf.data);
    return ret;
}
