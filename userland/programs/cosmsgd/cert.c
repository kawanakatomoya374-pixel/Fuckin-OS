/* A self-signed certificate made on the machine itself, so no private key ever has to be built into a program or copied around.
 * Clients (C-OS Message) pin the certificate on first use, so no certificate authority is involved. */
#include <string.h>
#include "bearssl.h"
#include "core.h"
#include "cert.h"

typedef struct { unsigned char *p; size_t n; } buf_t;
static void put(buf_t *b, const void *d, size_t n) { memcpy(b->p + b->n, d, n); b->n += n; }
static void put1(buf_t *b, unsigned char x) { b->p[b->n++] = x; }
/* writes tag + length + body */
static void tlv(buf_t *b, unsigned char tag, const unsigned char *body, size_t len) {
    put1(b, tag);
    if (len < 128) put1(b, (unsigned char)len); else if (len < 256) { put1(b, 0x81); put1(b, (unsigned char)len); } else { put1(b, 0x82); put1(b, (unsigned char)(len >> 8)); put1(b, (unsigned char)len); }
    put(b, body, len);
}
static const unsigned char OID_ECDSA_SHA256[] = { 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const unsigned char OID_EC_PUBKEY[] = { 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const unsigned char OID_P256[] = { 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };

bool cert_make(unsigned char *der, size_t *der_len, unsigned char key[32]) {
    unsigned char seed[32]; plat_random(seed, sizeof seed);
    br_hmac_drbg_context drbg; br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, sizeof seed);
    br_ec_private_key sk; br_ec_public_key pk; unsigned char pub[80];
    if (br_ec_keygen(&drbg.vtable, &br_ec_prime_i31, &sk, key, BR_EC_secp256r1) != 32) return false;
    if (br_ec_compute_pub(&br_ec_prime_i31, &pk, pub, &sk) != 65) return false;

    unsigned char t2[300], t3[300], tbsbody[400], serial[9];
    buf_t tbs = { tbsbody, 0 };
    /* version [0] { INTEGER 2 } */
    { static const unsigned char v[] = { 0xA0, 0x03, 0x02, 0x01, 0x02 }; put(&tbs, v, sizeof v); }
    /* serial: positive 8 random bytes */
    plat_random(serial + 1, 8); serial[0] = 0x01; serial[1] &= 0x7F; tlv(&tbs, 0x02, serial, 9);
    /* signature algorithm */
    { buf_t t = { t2, 0 }; put(&t, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256); tlv(&tbs, 0x30, t2, t.n); }
    /* issuer / subject: CN=cosmsg */
    unsigned char name[40]; size_t nl;
    { buf_t a = { name, 0 }; static const unsigned char cn[] = { 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x06, 'c', 'o', 's', 'm', 's', 'g' };
      unsigned char atv[20]; buf_t x = { atv, 0 }; tlv(&x, 0x30, cn, sizeof cn);               /* SEQUENCE { OID, UTF8String } */
      unsigned char set[24]; buf_t s = { set, 0 }; tlv(&s, 0x31, atv, x.n);                      /* SET */
      tlv(&a, 0x30, set, s.n); nl = a.n; }
    put(&tbs, name, nl);
    /* validity */
    { buf_t v = { t2, 0 }; static const unsigned char nb[] = { 0x17, 0x0D, '2','4','0','1','0','1','0','0','0','0','0','0','Z' }; static const unsigned char na[] = { 0x18, 0x0F, '2','1','0','0','0','1','0','1','0','0','0','0','0','0','Z' };
      put(&v, nb, sizeof nb); put(&v, na, sizeof na); tlv(&tbs, 0x30, t2, v.n); }
    put(&tbs, name, nl);
    /* subjectPublicKeyInfo */
    { buf_t alg = { t2, 0 }; put(&alg, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY); put(&alg, OID_P256, sizeof OID_P256);
      buf_t spki = { t3, 0 }; tlv(&spki, 0x30, t2, alg.n);
      unsigned char bs[70]; bs[0] = 0; memcpy(bs + 1, pub, 65); tlv(&spki, 0x03, bs, 66);
      tlv(&tbs, 0x30, t3, spki.n); }
    /* TBSCertificate = SEQUENCE { ... } */
    unsigned char tbsder[420]; buf_t td = { tbsder, 0 }; tlv(&td, 0x30, tbsbody, tbs.n);
    /* sign */
    unsigned char hash[32]; br_sha256_context hc; br_sha256_init(&hc); br_sha256_update(&hc, tbsder, td.n); br_sha256_out(&hc, hash);
    unsigned char sig[80]; size_t sl = br_ecdsa_i31_sign_asn1(&br_ec_prime_i31, &br_sha256_vtable, hash, &sk, sig);
    if (!sl) return false;
    /* Certificate = SEQUENCE { tbs, sigalg, BIT STRING sig } */
    unsigned char body[560]; buf_t cb = { body, 0 };
    put(&cb, tbsder, td.n);
    { buf_t t = { t2, 0 }; put(&t, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256); tlv(&cb, 0x30, t2, t.n); }
    { unsigned char bs[90]; bs[0] = 0; memcpy(bs + 1, sig, sl); tlv(&cb, 0x03, bs, sl + 1); }
    buf_t out = { der, 0 }; tlv(&out, 0x30, body, cb.n);
    *der_len = out.n; return true;
}
