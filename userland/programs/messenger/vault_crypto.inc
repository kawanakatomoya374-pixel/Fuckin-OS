/* Crypto for the app lock: scrypt (RFC 7914) to turn the password into a key, AES-256-GCM for the data.
 * Pure C on top of BearSSL - no operating-system calls - so tools/messenger_vault_test.sh can check it on the host
 * against Python's hashlib.scrypt and the `cryptography` package's AES-GCM. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "bearssl.h"

static void pbkdf2_sha256(const uint8_t *pw, size_t pl, const uint8_t *salt, size_t sl, uint32_t iters, uint8_t *out, size_t dk) {
    br_hmac_key_context kc; br_hmac_key_init(&kc, &br_sha256_vtable, pw, pl);
    for (uint32_t blk = 1; dk > 0; blk++) {
        uint8_t u[32], t[32], be[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
        br_hmac_context hc;
        br_hmac_init(&hc, &kc, 0); br_hmac_update(&hc, salt, sl); br_hmac_update(&hc, be, 4); br_hmac_out(&hc, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iters; i++) {
            br_hmac_init(&hc, &kc, 0); br_hmac_update(&hc, u, 32); br_hmac_out(&hc, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = dk < 32 ? dk : 32; memcpy(out, t, n); out += n; dk -= n;
    }
}

#define ROTL32(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
static void salsa20_8(uint32_t b[16]) {
    uint32_t x[16]; memcpy(x, b, 64);
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= ROTL32(x[0] + x[12], 7);   x[8] ^= ROTL32(x[4] + x[0], 9);    x[12] ^= ROTL32(x[8] + x[4], 13);  x[0] ^= ROTL32(x[12] + x[8], 18);
        x[9] ^= ROTL32(x[5] + x[1], 7);    x[13] ^= ROTL32(x[9] + x[5], 9);   x[1] ^= ROTL32(x[13] + x[9], 13);   x[5] ^= ROTL32(x[1] + x[13], 18);
        x[14] ^= ROTL32(x[10] + x[6], 7);  x[2] ^= ROTL32(x[14] + x[10], 9);  x[6] ^= ROTL32(x[2] + x[14], 13);   x[10] ^= ROTL32(x[6] + x[2], 18);
        x[3] ^= ROTL32(x[15] + x[11], 7);  x[7] ^= ROTL32(x[3] + x[15], 9);   x[11] ^= ROTL32(x[7] + x[3], 13);   x[15] ^= ROTL32(x[11] + x[7], 18);
        x[1] ^= ROTL32(x[0] + x[3], 7);    x[2] ^= ROTL32(x[1] + x[0], 9);    x[3] ^= ROTL32(x[2] + x[1], 13);    x[0] ^= ROTL32(x[3] + x[2], 18);
        x[6] ^= ROTL32(x[5] + x[4], 7);    x[7] ^= ROTL32(x[6] + x[5], 9);    x[4] ^= ROTL32(x[7] + x[6], 13);    x[5] ^= ROTL32(x[4] + x[7], 18);
        x[11] ^= ROTL32(x[10] + x[9], 7);  x[8] ^= ROTL32(x[11] + x[10], 9);  x[9] ^= ROTL32(x[8] + x[11], 13);   x[10] ^= ROTL32(x[9] + x[8], 18);
        x[12] ^= ROTL32(x[15] + x[14], 7); x[13] ^= ROTL32(x[12] + x[15], 9); x[14] ^= ROTL32(x[13] + x[12], 13); x[15] ^= ROTL32(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) b[i] += x[i];
}

static void scrypt_blockmix(uint32_t *b, uint32_t *y, uint32_t r) {
    uint32_t x[16]; memcpy(x, &b[(2 * r - 1) * 16], 64);
    for (uint32_t i = 0; i < 2 * r; i++) {
        for (int j = 0; j < 16; j++) x[j] ^= b[i * 16 + j];
        salsa20_8(x);
        memcpy(&y[i * 16], x, 64);
    }
    for (uint32_t i = 0; i < r; i++) memcpy(&b[i * 16], &y[(2 * i) * 16], 64);
    for (uint32_t i = 0; i < r; i++) memcpy(&b[(i + r) * 16], &y[(2 * i + 1) * 16], 64);
}

/* Returns 0, or -1 when the memory (128*r*N bytes) cannot be had. */
static int scrypt_kdf(const uint8_t *pw, size_t pl, const uint8_t *salt, size_t sl, uint32_t n, uint32_t r, uint32_t p, uint8_t *out, size_t dk) {
    size_t blen = (size_t)128 * r;
    uint8_t *buf = (uint8_t *)malloc(blen * p + blen * 2 + blen * n);
    if (!buf) return -1;
    uint8_t *b = buf, *tmp = buf + blen * p; uint32_t *v = (uint32_t *)(tmp + blen * 2); uint32_t *y = (uint32_t *)(tmp + blen);
    pbkdf2_sha256(pw, pl, salt, sl, 1, b, blen * p);
    for (uint32_t k = 0; k < p; k++) {
        uint32_t *x = (uint32_t *)tmp; memcpy(x, b + k * blen, blen);
        for (uint32_t i = 0; i < n; i++) { memcpy(&v[(size_t)i * 32 * r], x, blen); scrypt_blockmix(x, y, r); }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = x[(2 * r - 1) * 16] & (n - 1);
            for (uint32_t w = 0; w < 32 * r; w++) x[w] ^= v[(size_t)j * 32 * r + w];
            scrypt_blockmix(x, y, r);
        }
        memcpy(b + k * blen, x, blen);
    }
    pbkdf2_sha256(pw, pl, b, blen * p, 1, out, dk);
    memset(buf, 0, blen * p + blen * 2 + blen * n); free(buf);
    return 0;
}

/* AES-256-GCM, in place. aad is the file name, so a ciphertext cannot be moved to another file undetected. */
static void gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const char *aad, uint8_t *data, size_t n, uint8_t tag[16]) {
    br_aes_ct_ctr_keys bc; br_gcm_context gc;
    br_aes_ct_ctr_init(&bc, key, 32); br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
    br_gcm_reset(&gc, nonce, 12); br_gcm_aad_inject(&gc, aad, strlen(aad)); br_gcm_flip(&gc);
    br_gcm_run(&gc, 1, data, n); br_gcm_get_tag(&gc, tag);
}
static int gcm_open(const uint8_t key[32], const uint8_t nonce[12], const char *aad, uint8_t *data, size_t n, const uint8_t tag[16]) {
    br_aes_ct_ctr_keys bc; br_gcm_context gc;
    br_aes_ct_ctr_init(&bc, key, 32); br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
    br_gcm_reset(&gc, nonce, 12); br_gcm_aad_inject(&gc, aad, strlen(aad)); br_gcm_flip(&gc);
    br_gcm_run(&gc, 0, data, n);
    return br_gcm_check_tag(&gc, tag) == 1;
}
