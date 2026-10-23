/* e2e.inc - end-to-end encryption for C-OS Message (pure C on BearSSL: no operating-system calls, so tools/messenger_e2e_test.sh can
 * check it on the host against Python's `cryptography` package).
 *
 * WHAT IT PROTECTS: the relay server (and anybody who owns it or listens to it) sees who talks to whom and when, and how long a message
 * is - never what it says. Only the two people's own computers hold the keys.
 *
 * KEYS (per account, made on the device and never sent anywhere):
 *   identity  IK  : X25519 key (agreement) + ECDSA P-256 key (signatures). Its fingerprint is what two people compare ("safety number").
 *   prekey    SPK : an X25519 key that is replaced every week, signed with the identity's ECDSA key. The public bundle
 *                   (IK public keys + SPK + signature, 197 bytes) is what the server hands out; the server cannot forge it
 *                   because clients pin the fingerprint on first use and warn when it changes.
 *
 * ONE MESSAGE (X3DH-style, no handshake needed, works while the other side is offline):
 *   the sender makes a fresh ephemeral X25519 key EK and computes
 *       dh1 = X25519(IK_s, SPK_r)   dh2 = X25519(EK, IK_r)   dh3 = X25519(EK, SPK_r)
 *   key||nonce = HKDF-SHA256(salt "C-OS-E2E-v1", ikm dh1||dh2||dh3, info IK_s||IK_r||spk_id||EK)
 *   ciphertext = ChaCha20-Poly1305(key, nonce, padded text, aad = "sender|recipient|msgid|kind|spk_id|EK").
 *   Every message has its own key. dh1 authenticates the sender (only the owner of IK_s can compute it); the prekey gives forward
 *   secrecy: once the receiver has deleted an old SPK (after 30 days) nobody can decrypt the messages sent to it any more.
 *   Envelope = 0x01 | spk_id (4, big endian) | EK (32) | ciphertext | tag (16), written as base64url after the marker "\x01E".
 * FILES: a random 32-byte key per file, ChaCha20-Poly1305 per 4 KiB segment with a counter nonce and a "last segment" flag
 *   (STREAM construction: no reordering, dropping or truncating). The key travels inside an end-to-end message. */
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "bearssl.h"

#define E2E_BUNDLE_LEN 197
#define E2E_SEG 4096
typedef struct { uint8_t dh_priv[32], dh_pub[32], sg_priv[32], sg_pub[65]; } e2e_id_t;
typedef struct { uint32_t id, created; uint8_t priv[32], pub[32]; } e2e_spk_t;
typedef struct { uint8_t ik_dh[32], ik_sg[65]; uint32_t spk_id; uint8_t spk[32], sig[64]; } e2e_bundle_t;

/* ---- randomness: HMAC-DRBG (SHA-256) seeded from a platform entropy source and reseeded now and then ---- */
static void (*e2e_entropy)(uint8_t *buf, size_t n);
static br_hmac_drbg_context e2e_drbg; static bool e2e_seeded; static uint32_t e2e_gen_count;
static bool e2e_rng_ready(void) { return e2e_seeded; }
static void e2e_rng_init(void (*entropy)(uint8_t *, size_t)) {
    uint8_t s[48]; e2e_entropy = entropy; entropy(s, sizeof s);
    br_hmac_drbg_init(&e2e_drbg, &br_sha256_vtable, s, sizeof s); e2e_seeded = true; e2e_gen_count = 0; memset(s, 0, sizeof s);
}
static void e2e_rand(void *buf, size_t n) {
    if (!e2e_seeded) { memset(buf, 0, n); return; }
    if (e2e_gen_count++ > 512) { uint8_t s[32]; e2e_entropy(s, sizeof s); br_hmac_drbg_update(&e2e_drbg, s, sizeof s); e2e_gen_count = 0; memset(s, 0, sizeof s); }
    br_hmac_drbg_generate(&e2e_drbg, buf, n);
}
static void e2e_wipe(void *p, size_t n) { volatile uint8_t *v = (volatile uint8_t *)p; while (n--) *v++ = 0; }

/* ---- base64url without padding ---- */
static const char E2E_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static size_t e2e_b64enc(const uint8_t *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) { uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2]; out[o++] = E2E_B64[v >> 18]; out[o++] = E2E_B64[(v >> 12) & 63]; out[o++] = E2E_B64[(v >> 6) & 63]; out[o++] = E2E_B64[v & 63]; }
    if (n - i == 1) { uint32_t v = (uint32_t)in[i] << 16; out[o++] = E2E_B64[v >> 18]; out[o++] = E2E_B64[(v >> 12) & 63]; }
    else if (n - i == 2) { uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8; out[o++] = E2E_B64[v >> 18]; out[o++] = E2E_B64[(v >> 12) & 63]; out[o++] = E2E_B64[(v >> 6) & 63]; }
    out[o] = 0; return o;
}
static long e2e_b64dec(const char *in, size_t inlen, uint8_t *out, size_t cap) {
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        const char *p = strchr(E2E_B64, in[i]); if (!p || !in[i]) return -1;
        acc = (acc << 6) | (uint32_t)(p - E2E_B64); bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (bits >= 6) return -1;                                    /* a lone leftover character is not valid */
    return (long)o;
}

/* ---- primitives ---- */
static bool e2e_x25519(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32]) {
    memcpy(out, pub, 32);
    if (!br_ec_c25519_i31.mul(out, 32, priv, 32, BR_EC_curve25519)) return false;
    uint8_t z = 0; for (int i = 0; i < 32; i++) z |= out[i]; return z != 0;              /* an all-zero result means a bad (low-order) public key */
}
static void e2e_x25519_pub(uint8_t pub[32], const uint8_t priv[32]) { br_ec_c25519_i31.mulgen(pub, priv, 32, BR_EC_curve25519); }
static void e2e_sha256(const void *a, size_t al, const void *b, size_t bl, const void *c, size_t cl, uint8_t out[32]) {
    br_sha256_context sc; br_sha256_init(&sc); if (al) br_sha256_update(&sc, a, al); if (bl) br_sha256_update(&sc, b, bl); if (cl) br_sha256_update(&sc, c, cl); br_sha256_out(&sc, out);
}
static void e2e_aead(bool enc, const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aadl, uint8_t *data, size_t n, uint8_t tag[16]) {
    br_poly1305_ctmul_run(key, nonce, data, n, aad, aadl, tag, br_chacha20_ct_run, enc ? 1 : 0);
}
static bool e2e_ct_eq(const uint8_t *a, const uint8_t *b, size_t n) { uint8_t r = 0; for (size_t i = 0; i < n; i++) r |= a[i] ^ b[i]; return r == 0; }

/* ---- identity, prekey, bundle ---- */
static bool e2e_new_identity(e2e_id_t *id) {
    e2e_rand(id->dh_priv, 32); id->dh_priv[0] &= 248; id->dh_priv[31] &= 127; id->dh_priv[31] |= 64; e2e_x25519_pub(id->dh_pub, id->dh_priv);
    br_ec_private_key sk; br_ec_public_key pk;
    if (br_ec_keygen(&e2e_drbg.vtable, &br_ec_prime_i31, &sk, id->sg_priv, BR_EC_secp256r1) != 32) return false;
    return br_ec_compute_pub(&br_ec_prime_i31, &pk, id->sg_pub, &sk) == 65;
}
static void e2e_new_spk(e2e_spk_t *s, uint32_t id, uint32_t now) {
    s->id = id; s->created = now; e2e_rand(s->priv, 32); s->priv[0] &= 248; s->priv[31] &= 127; s->priv[31] |= 64; e2e_x25519_pub(s->pub, s->priv);
}
static void e2e_spk_digest(uint32_t id, const uint8_t spk[32], const uint8_t ik_dh[32], uint8_t out[32]) {
    uint8_t buf[11 + 4 + 32 + 32]; memcpy(buf, "C-OS-SPK-v1", 11); buf[11] = (uint8_t)(id >> 24); buf[12] = (uint8_t)(id >> 16); buf[13] = (uint8_t)(id >> 8); buf[14] = (uint8_t)id;
    memcpy(buf + 15, spk, 32); memcpy(buf + 47, ik_dh, 32); e2e_sha256(buf, sizeof buf, NULL, 0, NULL, 0, out);
}
static bool e2e_make_bundle(const e2e_id_t *id, const e2e_spk_t *spk, e2e_bundle_t *b) {
    memcpy(b->ik_dh, id->dh_pub, 32); memcpy(b->ik_sg, id->sg_pub, 65); b->spk_id = spk->id; memcpy(b->spk, spk->pub, 32);
    uint8_t d[32]; e2e_spk_digest(spk->id, spk->pub, id->dh_pub, d);
    br_ec_private_key sk = { BR_EC_secp256r1, (unsigned char *)id->sg_priv, 32 };
    return br_ecdsa_i31_sign_raw(&br_ec_prime_i31, &br_sha256_vtable, d, &sk, b->sig) == 64;
}
static bool e2e_bundle_verify(const e2e_bundle_t *b) {
    uint8_t d[32]; e2e_spk_digest(b->spk_id, b->spk, b->ik_dh, d);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)b->ik_sg, 65 };
    return b->ik_sg[0] == 4 && br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, d, 32, &pk, b->sig, 64) == 1;
}
static void e2e_bundle_pack(const e2e_bundle_t *b, uint8_t out[E2E_BUNDLE_LEN]) {
    memcpy(out, b->ik_dh, 32); memcpy(out + 32, b->ik_sg, 65); out[97] = (uint8_t)(b->spk_id >> 24); out[98] = (uint8_t)(b->spk_id >> 16); out[99] = (uint8_t)(b->spk_id >> 8); out[100] = (uint8_t)b->spk_id;
    memcpy(out + 101, b->spk, 32); memcpy(out + 133, b->sig, 64);
}
static bool e2e_bundle_unpack(const uint8_t *in, size_t n, e2e_bundle_t *b) {
    if (n != E2E_BUNDLE_LEN) return false;
    memcpy(b->ik_dh, in, 32); memcpy(b->ik_sg, in + 32, 65); b->spk_id = (uint32_t)in[97] << 24 | (uint32_t)in[98] << 16 | (uint32_t)in[99] << 8 | in[100]; memcpy(b->spk, in + 101, 32); memcpy(b->sig, in + 133, 64);
    return true;
}
static void e2e_fingerprint(const uint8_t ik_dh[32], const uint8_t ik_sg[65], uint8_t out[32]) { e2e_sha256("C-OS-FP-v1", 10, ik_dh, 32, ik_sg, 65, out); }
/* 60 digits in 12 groups of 5, the same on both sides: SHA-256 of the two fingerprints in sorted order, read as base-100000 groups */
static void e2e_safety_number(const uint8_t fa[32], const uint8_t fb[32], char out[80]) {
    const uint8_t *lo = memcmp(fa, fb, 32) <= 0 ? fa : fb, *hi = lo == fa ? fb : fa; uint8_t h[32], h2[32];
    e2e_sha256("C-OS-SAFETY-v1", 14, lo, 32, hi, 32, h); e2e_sha256(h, 32, "2", 1, NULL, 0, h2);
    size_t o = 0; for (int g = 0; g < 12; g++) { uint8_t *s = g < 6 ? h : h2; int k = (g % 6) * 5; uint64_t v = 0; for (int j = 0; j < 5; j++) v = (v << 8) | s[k + j]; v %= 100000; o += (size_t)snprintf(out + o, 80 - o, "%05u%s", (unsigned)v, g == 11 ? "" : " "); }
}

/* ---- messages ---- */
static void e2e_derive(const uint8_t dh[96], const uint8_t ik_s[32], const uint8_t ik_r[32], uint32_t spk_id, const uint8_t eph[32], uint8_t key[32], uint8_t nonce[12]) {
    br_hkdf_context hc; uint8_t info[32 + 32 + 4 + 32], km[44]; memcpy(info, ik_s, 32); memcpy(info + 32, ik_r, 32);
    info[64] = (uint8_t)(spk_id >> 24); info[65] = (uint8_t)(spk_id >> 16); info[66] = (uint8_t)(spk_id >> 8); info[67] = (uint8_t)spk_id; memcpy(info + 68, eph, 32);
    br_hkdf_init(&hc, &br_sha256_vtable, "C-OS-E2E-v1", 11); br_hkdf_inject(&hc, dh, 96); br_hkdf_flip(&hc); br_hkdf_produce(&hc, info, sizeof info, km, 44);
    memcpy(key, km, 32); memcpy(nonce, km + 32, 12); e2e_wipe(km, sizeof km);
}
static void e2e_aad(const char *ctx, uint32_t spk_id, const uint8_t eph[32], uint8_t *out, size_t *n) {
    size_t l = strlen(ctx); if (l > 200) l = 200; memcpy(out, "E2E1|", 5); memcpy(out + 5, ctx, l); size_t o = 5 + l; out[o++] = '|';
    out[o++] = (uint8_t)(spk_id >> 24); out[o++] = (uint8_t)(spk_id >> 16); out[o++] = (uint8_t)(spk_id >> 8); out[o++] = (uint8_t)spk_id; memcpy(out + o, eph, 32); *n = o + 32;
}
/* Text -> envelope bytes (`out` needs ptlen + 144). Returns the length, 0 on failure. ctx = "sender|recipient|msgid|kind". */
#define E2E_PAD 64
static size_t e2e_encrypt(const e2e_id_t *me, const e2e_bundle_t *peer, const char *ctx, const uint8_t *pt, size_t ptlen, uint8_t *out, size_t cap) {
    size_t padded = ((ptlen + 2 + E2E_PAD - 1) / E2E_PAD) * E2E_PAD;
    if (ptlen > 60000 || cap < 1 + 4 + 32 + padded + 16) return 0;
    uint8_t epriv[32], epub[32], dh[96], key[32], nonce[12], aad[256 + 40]; size_t aadl;
    e2e_rand(epriv, 32); epriv[0] &= 248; epriv[31] &= 127; epriv[31] |= 64; e2e_x25519_pub(epub, epriv);
    if (!e2e_x25519(dh, me->dh_priv, peer->spk) || !e2e_x25519(dh + 32, epriv, peer->ik_dh) || !e2e_x25519(dh + 64, epriv, peer->spk)) { e2e_wipe(epriv, 32); return 0; }
    e2e_derive(dh, me->dh_pub, peer->ik_dh, peer->spk_id, epub, key, nonce); e2e_wipe(dh, sizeof dh); e2e_wipe(epriv, 32);
    out[0] = 1; out[1] = (uint8_t)(peer->spk_id >> 24); out[2] = (uint8_t)(peer->spk_id >> 16); out[3] = (uint8_t)(peer->spk_id >> 8); out[4] = (uint8_t)peer->spk_id; memcpy(out + 5, epub, 32);
    uint8_t *ct = out + 37; ct[0] = (uint8_t)(ptlen >> 8); ct[1] = (uint8_t)ptlen; memcpy(ct + 2, pt, ptlen); memset(ct + 2 + ptlen, 0, padded - 2 - ptlen);
    e2e_aad(ctx, peer->spk_id, epub, aad, &aadl); e2e_aead(true, key, nonce, aad, aadl, ct, padded, ct + padded); e2e_wipe(key, 32);
    return 37 + padded + 16;
}
/* returns 0 ok, -1 malformed, -2 no matching prekey (too old or not ours), -3 authentication failed (forged, altered or for another message) */
static int e2e_decrypt(const e2e_id_t *me, const e2e_spk_t *spks, int nspk, const uint8_t sender_ik_dh[32], const char *ctx, const uint8_t *env, size_t len, uint8_t *pt, size_t cap, size_t *ptlen) {
    if (len < 37 + E2E_PAD + 16 || env[0] != 1 || (len - 37 - 16) % E2E_PAD != 0) return -1;
    uint32_t sid = (uint32_t)env[1] << 24 | (uint32_t)env[2] << 16 | (uint32_t)env[3] << 8 | env[4]; const uint8_t *eph = env + 5;
    const e2e_spk_t *sp = NULL; for (int i = 0; i < nspk; i++) if (spks[i].id == sid) sp = &spks[i];
    if (!sp) return -2;
    size_t padded = len - 37 - 16; if (padded > cap) return -1;
    uint8_t dh[96], key[32], nonce[12], aad[256 + 40], tag[16]; size_t aadl;
    if (!e2e_x25519(dh, sp->priv, sender_ik_dh) || !e2e_x25519(dh + 32, me->dh_priv, eph) || !e2e_x25519(dh + 64, sp->priv, eph)) return -3;
    e2e_derive(dh, sender_ik_dh, me->dh_pub, sid, eph, key, nonce); e2e_wipe(dh, sizeof dh);
    memcpy(pt, env + 37, padded); e2e_aad(ctx, sid, eph, aad, &aadl); e2e_aead(false, key, nonce, aad, aadl, pt, padded, tag); e2e_wipe(key, 32);
    if (!e2e_ct_eq(tag, env + 37 + padded, 16)) { e2e_wipe(pt, padded); return -3; }
    size_t l = (size_t)pt[0] << 8 | pt[1]; if (l + 2 > padded) { e2e_wipe(pt, padded); return -1; }
    memmove(pt, pt + 2, l); *ptlen = l; return 0;
}

/* ---- files: STREAM over 4 KiB segments ---- */
static void e2e_file_nonce(uint8_t n[12], uint64_t idx, bool last) { memset(n, 0, 12); for (int i = 0; i < 8; i++) n[10 - i] = (uint8_t)(idx >> (8 * i)); n[11] = last ? 1 : 0; }
static size_t e2e_file_cipher_size(size_t plain) { size_t segs = plain ? (plain + E2E_SEG - 1) / E2E_SEG : 1; return plain + segs * 16; }
/* encrypts segment idx in place (len <= E2E_SEG) and writes its tag after the data: the caller provides len + 16 bytes */
static void e2e_file_seal(const uint8_t key[32], uint64_t idx, bool last, uint8_t *seg, size_t len) { uint8_t n[12]; e2e_file_nonce(n, idx, last); e2e_aead(true, key, n, "C-OS-FILE-v1", 12, seg, len, seg + len); }
static bool e2e_file_open(const uint8_t key[32], uint64_t idx, bool last, uint8_t *seg, size_t len /* ciphertext length including the tag */) {
    if (len < 16) { return false; }
    uint8_t n[12], tag[16]; e2e_file_nonce(n, idx, last); e2e_aead(false, key, n, "C-OS-FILE-v1", 12, seg, len - 16, tag); return e2e_ct_eq(tag, seg + len - 16, 16);
}

/* ---- group messages ---------------------------------------------------------------------------------------------------------
 * A group has one 32-byte key per epoch (made by an admin, handed to every member wrapped with the pairwise construction above, and
 * replaced by a new epoch whenever somebody leaves, so a removed member cannot read what comes next). A message is
 *     0x02 | epoch (4) | nonce (12, random) | ChaCha20-Poly1305(padded text) | tag (16) | ECDSA-P256 signature (64)
 * with aad "G1|sender|gid|msgid|kind|" + epoch. The signature (made with the SENDER's identity key over aad, nonce, ciphertext and tag)
 * is what stops one member from writing in another member's name, which the shared key alone could not. */
static void e2e_gdigest(const uint8_t *aad, size_t aadl, const uint8_t *body, size_t bl, uint8_t out[32]) {
    br_sha256_context sc; br_sha256_init(&sc); br_sha256_update(&sc, "C-OS-GSIG-v1", 12); br_sha256_update(&sc, aad, aadl); br_sha256_update(&sc, body, bl); br_sha256_out(&sc, out);
}
static void e2e_gaad(const char *ctx, uint32_t epoch, uint8_t *out, size_t *n) {
    size_t l = strlen(ctx); if (l > 200) l = 200; memcpy(out, "G1|", 3); memcpy(out + 3, ctx, l); size_t o = 3 + l; out[o++] = '|';
    out[o++] = (uint8_t)(epoch >> 24); out[o++] = (uint8_t)(epoch >> 16); out[o++] = (uint8_t)(epoch >> 8); out[o++] = (uint8_t)epoch; *n = o;
}
#define E2E_GFIXED (1 + 4 + 12 + 16 + 64)
static size_t e2e_group_encrypt(const e2e_id_t *me, const uint8_t gk[32], uint32_t epoch, const char *ctx, const uint8_t *pt, size_t ptlen, uint8_t *out, size_t cap) {
    size_t padded = ((ptlen + 2 + E2E_PAD - 1) / E2E_PAD) * E2E_PAD;
    if (ptlen > 60000 || cap < E2E_GFIXED + padded) return 0;
    uint8_t aad[256 + 8], tag[16], dg[32]; size_t aadl; e2e_gaad(ctx, epoch, aad, &aadl);
    out[0] = 2; out[1] = (uint8_t)(epoch >> 24); out[2] = (uint8_t)(epoch >> 16); out[3] = (uint8_t)(epoch >> 8); out[4] = (uint8_t)epoch; e2e_rand(out + 5, 12);
    uint8_t *ct = out + 17; ct[0] = (uint8_t)(ptlen >> 8); ct[1] = (uint8_t)ptlen; memcpy(ct + 2, pt, ptlen); memset(ct + 2 + ptlen, 0, padded - 2 - ptlen);
    e2e_aead(true, gk, out + 5, aad, aadl, ct, padded, tag); memcpy(ct + padded, tag, 16);
    e2e_gdigest(aad, aadl, out + 5, 12 + padded + 16, dg);
    br_ec_private_key sk = { BR_EC_secp256r1, (unsigned char *)me->sg_priv, 32 };
    if (br_ecdsa_i31_sign_raw(&br_ec_prime_i31, &br_sha256_vtable, dg, &sk, ct + padded + 16) != 64) return 0;
    return E2E_GFIXED + padded;
}
static uint32_t e2e_group_epoch(const uint8_t *env, size_t len) { return len >= 5 && env[0] == 2 ? (uint32_t)env[1] << 24 | (uint32_t)env[2] << 16 | (uint32_t)env[3] << 8 | env[4] : 0; }
/* checks only the sender's signature (no group key needed): a frame that is not signed by the person it claims to come from is dropped before anything waits for it. 0 ok, -1 malformed, -3 bad signature */
static int e2e_group_verify(const uint8_t sender_sg[65], const char *ctx, const uint8_t *env, size_t len) {
    if (len < E2E_GFIXED + E2E_PAD || env[0] != 2 || (len - E2E_GFIXED) % E2E_PAD != 0) return -1;
    size_t padded = len - E2E_GFIXED; uint32_t epoch = e2e_group_epoch(env, len); uint8_t aad[256 + 8], dg[32]; size_t aadl; e2e_gaad(ctx, epoch, aad, &aadl);
    e2e_gdigest(aad, aadl, env + 5, 12 + padded + 16, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)sender_sg, 65 };
    return (sender_sg[0] == 4 && br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, env + 17 + padded + 16, 64) == 1) ? 0 : -3;
}
/* -1 malformed, -3 signature or authentication failed (forged, altered, wrong key, or for another message) */
static int e2e_group_decrypt(const uint8_t sender_sg[65], const uint8_t gk[32], const char *ctx, const uint8_t *env, size_t len, uint8_t *pt, size_t cap, size_t *ptlen) {
    if (len < E2E_GFIXED + E2E_PAD || env[0] != 2 || (len - E2E_GFIXED) % E2E_PAD != 0) return -1;
    size_t padded = len - E2E_GFIXED; if (padded > cap) return -1;
    uint32_t epoch = e2e_group_epoch(env, len); uint8_t aad[256 + 8], dg[32], tag[16]; size_t aadl; e2e_gaad(ctx, epoch, aad, &aadl);
    e2e_gdigest(aad, aadl, env + 5, 12 + padded + 16, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)sender_sg, 65 };
    if (sender_sg[0] != 4 || br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, env + 17 + padded + 16, 64) != 1) return -3;
    memcpy(pt, env + 17, padded); e2e_aead(false, gk, env + 5, aad, aadl, pt, padded, tag);
    if (!e2e_ct_eq(tag, env + 17 + padded, 16)) { e2e_wipe(pt, padded); return -3; }
    size_t l = (size_t)pt[0] << 8 | pt[1]; if (l + 2 > padded) { e2e_wipe(pt, padded); return -1; }
    memmove(pt, pt + 2, l); *ptlen = l; return 0;
}
