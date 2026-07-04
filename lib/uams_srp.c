/*
 *  uams_srp.c
 *
 *  SRP (Secure Remote Password) User Authentication Method for AFP.
 *  Protocol: SRP-6a with SHA-1, MGF1 KDF, RFC 5054 group #2 (1536-bit).
 *
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#ifdef HAVE_LIBGCRYPT
#include <gcrypt.h>
#endif

#include "dsi.h"
#include "afp.h"
#include "utils.h"
#include "uams_def.h"
#include "uams.h"

#define SRP_INIT_MARKER   0x0001
#define SRP_CLIENT_PROOF  0x0003
#define SRP_SERVER_PROOF  0x0004
#define SRP_SHA1_LEN      20
#define SRP_MAX_NBYTES    256

/* observed on Apple AFP servers with SRP UAM, not a standard AFP error code */
#define SRP_AUTH_FAILURE  (-6754)

/* Read a 2-byte big-endian unsigned integer and advance the pointer. */
static uint16_t read_uint16_be(unsigned char **p)
{
    uint16_t val = (uint16_t)((*p)[0] << 8 | (*p)[1]);
    *p += 2;
    return val;
}

/*
 * Strip leading zero bytes from a big-endian integer buffer.
 * Returns pointer to the first non-zero byte (or last byte if all zeros).
 * Sets *out_len to the stripped length.
 */
static const unsigned char *strip_leading_zeros(const unsigned char *buf,
        size_t len, size_t *out_len)
{
    while (len > 1 && *buf == 0) {
        buf++;
        len--;
    }

    *out_len = len;
    return buf;
}

/*
 * MGF1 mask generation function (PKCS#1 v2.1) with SHA-1.
 * Produces out_len bytes of output from seed.
 */
static int mgf1_sha1(const unsigned char *seed, size_t seed_len,
                     unsigned char *out, size_t out_len)
{
    unsigned char counter_be[4];
    unsigned char hash[SRP_SHA1_LEN];
    size_t pos = 0;
    uint32_t counter = 0;

    while (pos < out_len) {
        counter_be[0] = (counter >> 24) & 0xFF;
        counter_be[1] = (counter >> 16) & 0xFF;
        counter_be[2] = (counter >> 8) & 0xFF;
        counter_be[3] = counter & 0xFF;
        gcry_md_hd_t hd;

        if (gcry_md_open(&hd, GCRY_MD_SHA1, 0) != 0) {
            return -1;
        }

        gcry_md_write(hd, seed, seed_len);
        gcry_md_write(hd, counter_be, 4);
        memcpy(hash, gcry_md_read(hd, GCRY_MD_SHA1), SRP_SHA1_LEN);
        gcry_md_close(hd);
        size_t to_copy = out_len - pos;

        if (to_copy > SRP_SHA1_LEN) {
            to_copy = SRP_SHA1_LEN;
        }

        memcpy(out + pos, hash, to_copy);
        pos += to_copy;
        counter++;
    }

    return 0;
}

/*
 * Compute SHA-1 incrementally from multiple buffers.
 * The va_list contains pairs of (const unsigned char *data, size_t len),
 * terminated by a NULL data pointer.
 */
static void sha1_multi(unsigned char *out, ...)
{
    gcry_md_hd_t hd;
    va_list ap;
    gcry_md_open(&hd, GCRY_MD_SHA1, 0);
    va_start(ap, out);

    for (;;) {
        const unsigned char *data = va_arg(ap, const unsigned char *);

        if (data == NULL) {
            break;
        }

        size_t len = va_arg(ap, size_t);
        gcry_md_write(hd, data, len);
    }

    va_end(ap);
    memcpy(out, gcry_md_read(hd, GCRY_MD_SHA1), SRP_SHA1_LEN);
    gcry_md_close(hd);
}

int srp_login(struct afp_server *server, char *username, char *passwd)
{
    int ret = -1;
    unsigned char *rbuf_data = NULL;
    unsigned char *ai = NULL;
    unsigned char *a_binary = NULL;
    unsigned char *S_binary = NULL;
    gcry_mpi_t N = NULL, g_mpi = NULL, B = NULL;
    gcry_mpi_t a = NULL, A = NULL;
    gcry_mpi_t x = NULL, v = NULL, k = NULL, u = NULL;
    gcry_mpi_t S = NULL, kv = NULL, tmp = NULL;

    if (!gcry_check_version(UAM_NEED_LIBGCRYPT_VERSION)) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: libgcrypt version check failed");
        return -1;
    }

    /*
     * Round 1: FPLoginExt — send username + init marker, receive SRP params
     */
    unsigned char init_marker[2];
    init_marker[0] = (SRP_INIT_MARKER >> 8) & 0xFF;
    init_marker[1] = SRP_INIT_MARKER & 0xFF;
    /* Server response: field1(2) + field2(2) + N_len(2) + N + g_len(2) + g +
     *                  salt_len(2) + salt + B_len(2) + B
     * For 1536-bit group: 2+2+2+192+2+1+2+16+2+192 = 413 bytes */
    struct afp_rx_buffer rbuf;
    rbuf.maxsize = 2 + 2 + 2 + SRP_MAX_NBYTES + 2 + SRP_MAX_NBYTES +
                   2 + 64 + 2 + SRP_MAX_NBYTES;
    rbuf_data = calloc(1, rbuf.maxsize);
    rbuf.data = (char *)rbuf_data;

    if (rbuf.data == NULL) {
        goto srp_fail;
    }

    rbuf.size = 0;
    ret = afp_loginext(server, "SRP", username,
                       (char *)init_marker, sizeof(init_marker), &rbuf);

    if (ret != kFPAuthContinue) {
        if (ret == SRP_AUTH_FAILURE) {
            ret = kFPUserNotAuth;
        }

        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: FPLoginExt failed (ret=%d, expected kFPAuthContinue)",
                       ret);
        goto srp_cleanup;
    }

    /*
     * Parse server response
     */
    unsigned char *d = rbuf_data;
    const unsigned char *end = rbuf_data + rbuf.size;

    if (rbuf.size < 8) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: server response too short (%u bytes)", rbuf.size);
        goto srp_fail;
    }

    /* field1 (transaction context), field2 (group index) — skip */
    d += 2; /* field1 */
    d += 2; /* field2 (group index, e.g. 0x0002 for RFC 5054 group #2) */

    /* N */
    if (d + 2 > end) {
        goto srp_parse_fail;
    }

    uint16_t N_len = read_uint16_be(&d);

    if (N_len > SRP_MAX_NBYTES || d + N_len > end) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: invalid N length %u", N_len);
        goto srp_fail;
    }

    unsigned char *N_raw = d;
    gcry_mpi_scan(&N, GCRYMPI_FMT_USG, d, N_len, NULL);
    d += N_len;

    /* g */
    if (d + 2 > end) {
        goto srp_parse_fail;
    }

    uint16_t g_len = read_uint16_be(&d);

    if (g_len > SRP_MAX_NBYTES || d + g_len > end) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: invalid g length %u", g_len);
        goto srp_fail;
    }

    gcry_mpi_scan(&g_mpi, GCRYMPI_FMT_USG, d, g_len, NULL);
    unsigned int g_val = 0;

    for (uint16_t i = 0; i < g_len; i++) {
        g_val = (g_val << 8) | d[i];
    }

    d += g_len;

    /* salt */
    if (d + 2 > end) {
        goto srp_parse_fail;
    }

    uint16_t salt_len = read_uint16_be(&d);

    if (d + salt_len > end) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: invalid salt length %u", salt_len);
        goto srp_fail;
    }

    unsigned char *salt = d;
    d += salt_len;

    /* B */
    if (d + 2 > end) {
        goto srp_parse_fail;
    }

    uint16_t B_len = read_uint16_be(&d);

    if (B_len > SRP_MAX_NBYTES || d + B_len > end) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: invalid B length %u", B_len);
        goto srp_fail;
    }

    const unsigned char *B_raw = d;
    gcry_mpi_scan(&B, GCRYMPI_FMT_USG, d, B_len, NULL);
    /* Validate B != 0 mod N */
    tmp = gcry_mpi_new(0);
    gcry_mpi_mod(tmp, B, N);

    if (gcry_mpi_cmp_ui(tmp, 0) == 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR, "SRP: B mod N == 0");
        goto srp_fail;
    }

    /*
     * Compute SRP client values
     */
    size_t nbytes = N_len;
    size_t len;
    /* x = SHA1(salt | SHA1(username | ":" | password)) */
    unsigned char inner_hash[SRP_SHA1_LEN];
    unsigned char x_hash[SRP_SHA1_LEN];
    size_t user_len = strnlen(username, AFP_MAX_USERNAME_LEN);
    size_t pass_len = strnlen(passwd, AFP_MAX_PASSWORD_LEN);
    sha1_multi(inner_hash,
               (const unsigned char *)username, user_len,
               (const unsigned char *)":", (size_t)1,
               (const unsigned char *)passwd, pass_len,
               NULL);
    sha1_multi(x_hash,
               salt, (size_t)salt_len,
               inner_hash, (size_t)SRP_SHA1_LEN,
               NULL);
    gcry_mpi_scan(&x, GCRYMPI_FMT_USG, x_hash, SRP_SHA1_LEN, NULL);
    /* v = g^x mod N */
    v = gcry_mpi_new(0);
    gcry_mpi_powm(v, g_mpi, x, N);
    /* Generate client ephemeral: a random, A = g^a mod N */
    a_binary = calloc(1, nbytes);

    if (a_binary == NULL) {
        goto srp_fail;
    }

    a = gcry_mpi_new(0);
    A = gcry_mpi_new(0);

    do {
        gcry_randomize(a_binary, nbytes, GCRY_STRONG_RANDOM);
        gcry_mpi_scan(&a, GCRYMPI_FMT_USG, a_binary, nbytes, NULL);
        gcry_mpi_mod(a, a, N);
        gcry_mpi_powm(A, g_mpi, a, N);
    } while (gcry_mpi_cmp_ui(A, 0) == 0);

    /* A as nbytes big-endian (zero-padded) */
    unsigned char A_buf[SRP_MAX_NBYTES];
    memset(A_buf, 0, sizeof(A_buf));
    gcry_mpi_print(GCRYMPI_FMT_USG, A_buf, nbytes, &len, A);

    if (len < nbytes) {
        memmove(A_buf + nbytes - len, A_buf, len);
        memset(A_buf, 0, nbytes - len);
    }

    /* B as nbytes big-endian (zero-padded) for u computation */
    unsigned char B_buf[SRP_MAX_NBYTES];
    memset(B_buf, 0, sizeof(B_buf));
    memcpy(B_buf + nbytes - B_len, B_raw, B_len);
    /* u = SHA1(PAD(A) | PAD(B)) — SRP-6a padded */
    unsigned char u_hash[SRP_SHA1_LEN];
    sha1_multi(u_hash,
               A_buf, nbytes,
               B_buf, nbytes,
               NULL);
    gcry_mpi_scan(&u, GCRYMPI_FMT_USG, u_hash, SRP_SHA1_LEN, NULL);

    if (gcry_mpi_cmp_ui(u, 0) == 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR, "SRP: u == 0, aborting");
        goto srp_fail;
    }

    /* k = SHA1(N | PAD(g)) — SRP-6a padded */
    unsigned char g_padded[SRP_MAX_NBYTES];
    memset(g_padded, 0, sizeof(g_padded));
    unsigned char g_byte = (unsigned char)g_val;
    g_padded[nbytes - 1] = g_byte;
    unsigned char k_hash[SRP_SHA1_LEN];
    sha1_multi(k_hash,
               N_raw, (size_t)N_len,
               g_padded, nbytes,
               NULL);
    gcry_mpi_scan(&k, GCRYMPI_FMT_USG, k_hash, SRP_SHA1_LEN, NULL);
    /* S = (B - k*v) ^ (a + u*x) mod N */
    kv = gcry_mpi_new(0);
    S = gcry_mpi_new(0);
    gcry_mpi_mulm(kv, k, v, N);
    /* diff = (B - k*v) mod N — use mpi_subm */
    gcry_mpi_t diff = gcry_mpi_new(0);
    gcry_mpi_subm(diff, B, kv, N);
    /* exp = a + u*x */
    gcry_mpi_t ux = gcry_mpi_new(0);
    gcry_mpi_t exp = gcry_mpi_new(0);
    gcry_mpi_mul(ux, u, x);
    gcry_mpi_add(exp, a, ux);
    gcry_mpi_powm(S, diff, exp, N);
    gcry_mpi_release(diff);
    gcry_mpi_release(ux);
    gcry_mpi_release(exp);
    /* K = MGF1-SHA1(strip(S), 40) */
    S_binary = calloc(1, nbytes);

    if (S_binary == NULL) {
        goto srp_fail;
    }

    gcry_mpi_print(GCRYMPI_FMT_USG, S_binary, nbytes, &len, S);

    if (len < nbytes) {
        memmove(S_binary + nbytes - len, S_binary, len);
        memset(S_binary, 0, nbytes - len);
    }

    /* Strip leading zeros from S before MGF1 */
    size_t S_stripped_len;
    const unsigned char *S_stripped = strip_leading_zeros(S_binary, nbytes,
                                      &S_stripped_len);
    unsigned char K[40];

    if (mgf1_sha1(S_stripped, S_stripped_len, K, sizeof(K)) != 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR, "SRP: MGF1 failed");
        goto srp_fail;
    }

    /*
     * Compute M1 = SHA1(H(N)^H(g) | H(username) | salt | strip(A) | strip(B) | K)
     */
    /* H(N) — hash of N with leading zeros stripped */
    size_t N_stripped_len;
    const unsigned char *N_stripped = strip_leading_zeros(N_raw, N_len,
                                      &N_stripped_len);
    unsigned char H_N[SRP_SHA1_LEN];
    sha1_multi(H_N, N_stripped, N_stripped_len, NULL);
    /* H(g) — hash of g as minimal bytes */
    unsigned char g_min = (unsigned char)g_val;
    unsigned char H_g[SRP_SHA1_LEN];
    sha1_multi(H_g, &g_min, (size_t)1, NULL);
    /* H(N) XOR H(g) */
    unsigned char xor_ng[SRP_SHA1_LEN];

    for (int i = 0; i < SRP_SHA1_LEN; i++) {
        xor_ng[i] = H_N[i] ^ H_g[i];
    }

    /* H(username) */
    unsigned char H_user[SRP_SHA1_LEN];
    sha1_multi(H_user,
               (const unsigned char *)username, user_len,
               NULL);
    /* strip(A) and strip(B) for M1 */
    size_t A_stripped_len, B_stripped_len;
    const unsigned char *A_stripped = strip_leading_zeros(A_buf, nbytes,
                                      &A_stripped_len);
    const unsigned char *B_stripped = strip_leading_zeros(B_raw, B_len,
                                      &B_stripped_len);
    unsigned char M1[SRP_SHA1_LEN];
    sha1_multi(M1,
               xor_ng, (size_t)SRP_SHA1_LEN,
               H_user, (size_t)SRP_SHA1_LEN,
               salt, (size_t)salt_len,
               A_stripped, A_stripped_len,
               B_stripped, B_stripped_len,
               K, sizeof(K),
               NULL);
    /*
     * Round 2: FPLoginCont — send A + M1, receive M2
     */
    /* Build authinfo: step(2) + A_len(2) + A(nbytes) + M1_len(2) + M1(20) */
    unsigned int ai_len = 2 + 2 + (unsigned int)nbytes + 2 + SRP_SHA1_LEN;
    ai = calloc(1, ai_len);

    if (ai == NULL) {
        goto srp_fail;
    }

    unsigned char *p = ai;
    /* SRP step marker */
    *p++ = (SRP_CLIENT_PROOF >> 8) & 0xFF;
    *p++ = SRP_CLIENT_PROOF & 0xFF;
    /* A_len + A */
    *p++ = (nbytes >> 8) & 0xFF;
    *p++ = nbytes & 0xFF;
    memcpy(p, A_buf, nbytes);
    p += nbytes;
    /* M1_len + M1 */
    *p++ = 0;
    *p++ = SRP_SHA1_LEN;
    memcpy(p, M1, SRP_SHA1_LEN);
    /* Expect: step(2) + M2_len(2) + M2(20) = 24 bytes */
    free(rbuf_data);
    rbuf_data = NULL;
    rbuf.maxsize = 2 + 2 + SRP_SHA1_LEN;
    rbuf_data = calloc(1, rbuf.maxsize);
    rbuf.data = (char *)rbuf_data;

    if (rbuf.data == NULL) {
        goto srp_fail;
    }

    rbuf.size = 0;
    ret = afp_logincont(server, 0, (char *)ai, ai_len, &rbuf);

    if (ret != 0) {
        if (ret == SRP_AUTH_FAILURE) {
            ret = kFPUserNotAuth;
            goto srp_cleanup;
        }

        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "SRP: authentication failed (ret=%d)", ret);
        goto srp_cleanup;
    }

    /*
     * Verify M2 (server proof)
     */
    if (rbuf.size >= 24) {
        d = rbuf_data;
        /* Skip step marker (2 bytes) and M2_len (2 bytes) */
        d += 4;
        /* M2 = SHA1(strip(A) | M1 | K) */
        unsigned char M2_expected[SRP_SHA1_LEN];
        sha1_multi(M2_expected,
                   A_stripped, A_stripped_len,
                   M1, (size_t)SRP_SHA1_LEN,
                   K, sizeof(K),
                   NULL);

        if (memcmp(d, M2_expected, SRP_SHA1_LEN) != 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "SRP: server proof (M2) verification failed");
        }
    }

    ret = 0;
    goto srp_cleanup;
srp_parse_fail:
    log_for_client(NULL, AFPFSD, LOG_ERR,
                   "SRP: truncated server response");
srp_fail:
    ret = -1;
srp_cleanup:
    gcry_mpi_release(N);
    gcry_mpi_release(g_mpi);
    gcry_mpi_release(B);
    gcry_mpi_release(a);
    gcry_mpi_release(A);
    gcry_mpi_release(x);
    gcry_mpi_release(v);
    gcry_mpi_release(k);
    gcry_mpi_release(u);
    gcry_mpi_release(S);
    gcry_mpi_release(kv);
    gcry_mpi_release(tmp);
    free(a_binary);
    free(S_binary);
    free(rbuf_data);
    free(ai);
    return ret;
}
