/* SHA-256 (module hashes in program images). */
#include "image_int.h"

/* ================================================================ */
/* SHA-256                                                           */
/* ================================================================ */
static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

typedef struct Sha {
    uint32_t h[8];
    uint8_t buf[64];
    size_t n;
    uint64_t total;
} Sha;

static uint32_t ror32(uint32_t x, unsigned k) { return (x >> k) | (x << (32u - k)); }

static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, iv, sizeof iv);
    s->n = 0;
    s->total = 0;
}

static void sha_update(Sha *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->total += len;
    while (len > 0) {
        size_t take = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha_final(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->total * 8u;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) sha_update(s, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sp_sha256(const void *data, size_t len, uint8_t out[32]) {
    Sha s;
    sha_init(&s);
    sha_update(&s, data, len);
    sha_final(&s, out);
}

/* the module's source digest; bundled modules defer hashing until an image is actually written */
void mod_sha(const ModInfo *m, uint8_t out[32]) {
    if (m->lazy_src) sp_sha256(m->lazy_src, m->lazy_len, out);
    else memcpy(out, m->sha, 32);
}

/* header source hash: SHA-256 over (module name, 0x00, source digest) per module */
void sp_source_hash(const Program *pg, uint8_t out[32]) {
    Sha s;
    sha_init(&s);
    for (int i = 0; i < pg->nmods; i++) {
        sha_update(&s, pg->mods[i].name, strlen(pg->mods[i].name));
        uint8_t z = 0;
        sha_update(&s, &z, 1);
        uint8_t dg[32];
        mod_sha(&pg->mods[i], dg);
        sha_update(&s, dg, 32);
    }
    sha_final(&s, out);
}
