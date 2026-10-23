/* host test of userland/programs/messenger/e2e.inc: prints results as JSON-ish lines that tools/messenger_e2e_test.py checks against Python's `cryptography` */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../userland/programs/messenger/e2e.inc"
static void ent(uint8_t *b, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (!f || fread(b, 1, n, f) != n) exit(2); fclose(f); }
static void hex(const char *k, const uint8_t *b, size_t n) { printf("%s=", k); for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s\n", #c); fails++; } } while (0)
int main(int argc, char **argv) {
    e2e_rng_init(ent);
    if (argc > 1 && !strcmp(argv[1], "emit")) {                         /* material for the Python side */
        e2e_id_t a, b; e2e_spk_t sa, sb; e2e_bundle_t ba, bb; e2e_new_identity(&a); e2e_new_identity(&b); e2e_new_spk(&sa, 7, 1); e2e_new_spk(&sb, 9, 1); e2e_make_bundle(&a, &sa, &ba); e2e_make_bundle(&b, &sb, &bb);
        hex("a_dh_priv", a.dh_priv, 32); hex("a_dh_pub", a.dh_pub, 32); hex("a_sg_pub", a.sg_pub, 65); hex("a_spk_priv", sa.priv, 32); hex("a_spk_pub", sa.pub, 32);
        hex("b_dh_priv", b.dh_priv, 32); hex("b_dh_pub", b.dh_pub, 32); hex("b_sg_pub", b.sg_pub, 65); hex("b_spk_priv", sb.priv, 32); hex("b_spk_pub", sb.pub, 32);
        uint8_t pk[E2E_BUNDLE_LEN]; e2e_bundle_pack(&ba, pk); hex("a_bundle", pk, sizeof pk); e2e_bundle_pack(&bb, pk); hex("b_bundle", pk, sizeof pk);
        const char *msg = "こんにちは Bob - this goes through the relay unread"; uint8_t env[512]; size_t el = e2e_encrypt(&a, &bb, "100|200|m1|0", (const uint8_t *)msg, strlen(msg), env, sizeof env);
        hex("a_to_b_env", env, el); printf("a_to_b_ctx=100|200|m1|0\na_to_b_text=%s\n", msg);
        uint8_t key[32]; e2e_rand(key, 32); uint8_t data[9000]; for (int i = 0; i < 9000; i++) data[i] = (uint8_t)(i * 7); uint8_t cipher[9000 + 3 * 16]; size_t co = 0;
        for (size_t off = 0, idx = 0; off < 9000; off += E2E_SEG, idx++) { size_t l = 9000 - off < E2E_SEG ? 9000 - off : E2E_SEG; memcpy(cipher + co, data + off, l); e2e_file_seal(key, idx, off + l >= 9000, cipher + co, l); co += l + 16; }
        hex("file_key", key, 32); hex("file_cipher", cipher, co);
        uint8_t gk[32], genv[400]; e2e_rand(gk, 32); const char *gt = "グループ向け: hello everybody"; size_t gl = e2e_group_encrypt(&a, gk, 77, "100|g55|m9|0", (const uint8_t *)gt, strlen(gt), genv, sizeof genv);
        hex("g_key", gk, 32); hex("g_env", genv, gl); printf("g_ctx=100|g55|m9|0\ng_text=%s\n", gt); return 0;
    }
    e2e_id_t a, b; e2e_spk_t sa[2], sb[2]; e2e_bundle_t ba, bb, tmp;
    CHECK(e2e_new_identity(&a) && e2e_new_identity(&b));
    e2e_new_spk(&sa[0], 1, 100); e2e_new_spk(&sb[0], 5, 100); e2e_new_spk(&sb[1], 6, 200);
    CHECK(e2e_make_bundle(&a, &sa[0], &ba) && e2e_make_bundle(&b, &sb[1], &bb));
    CHECK(e2e_bundle_verify(&ba) && e2e_bundle_verify(&bb));
    tmp = bb; tmp.spk[3] ^= 1; CHECK(!e2e_bundle_verify(&tmp));                    /* a swapped prekey breaks the signature */
    tmp = bb; tmp.ik_dh[0] ^= 1; CHECK(!e2e_bundle_verify(&tmp));
    uint8_t pk[E2E_BUNDLE_LEN]; e2e_bundle_pack(&bb, pk); e2e_bundle_t rb; CHECK(e2e_bundle_unpack(pk, sizeof pk, &rb) && !memcmp(&rb, &bb, sizeof bb) && e2e_bundle_verify(&rb)); CHECK(!e2e_bundle_unpack(pk, 100, &rb));
    const char *m = "Hello Bob, this is secret. 日本語もOK"; uint8_t env[600], pt[600]; size_t el, pl;
    el = e2e_encrypt(&a, &bb, "A|B|m1|0", (const uint8_t *)m, strlen(m), env, sizeof env); CHECK(el > 0 && (el - 37 - 16) % 64 == 0);
    CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m1|0", env, el, pt, sizeof pt, &pl) == 0 && pl == strlen(m) && !memcmp(pt, m, pl));
    CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m2|0", env, el, pt, sizeof pt, &pl) == -3);              /* the same bytes under another message id */
    CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m1|1", env, el, pt, sizeof pt, &pl) == -3);              /* ... or kind */
    { uint8_t evil[600]; memcpy(evil, env, el); evil[50] ^= 1; CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m1|0", evil, el, pt, sizeof pt, &pl) == -3); }   /* a flipped bit */
    { uint8_t evil[600]; memcpy(evil, env, el); evil[el - 1] ^= 1; CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m1|0", evil, el, pt, sizeof pt, &pl) == -3); }
    CHECK(e2e_decrypt(&b, sb, 2, b.dh_pub, "A|B|m1|0", env, el, pt, sizeof pt, &pl) == -3);              /* claimed to be from somebody else */
    CHECK(e2e_decrypt(&a, sa, 1, a.dh_pub, "A|B|m1|0", env, el, pt, sizeof pt, &pl) < 0);                /* not for us */
    CHECK(e2e_decrypt(&b, sb, 1, a.dh_pub, "A|B|m1|0", env, el, pt, sizeof pt, &pl) == -2);              /* prekey 6 already deleted */
    CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|m1|0", env, 20, pt, sizeof pt, &pl) == -1);
    /* every message has its own ephemeral key, so the same text never looks the same twice */
    { uint8_t env2[600]; size_t e2l = e2e_encrypt(&a, &bb, "A|B|m1|0", (const uint8_t *)m, strlen(m), env2, sizeof env2); CHECK(e2l == el && memcmp(env + 5, env2 + 5, 32) != 0 && memcmp(env + 37, env2 + 37, 32) != 0); }
    /* length hiding: texts of 1..40 bytes all produce the same envelope size */
    { size_t l1 = e2e_encrypt(&a, &bb, "x", (const uint8_t *)"a", 1, env, sizeof env), l2 = e2e_encrypt(&a, &bb, "x", (const uint8_t *)"0123456789012345678901234567890123456789", 40, env, sizeof env); CHECK(l1 == l2); }
    /* empty and large messages */
    el = e2e_encrypt(&a, &bb, "x", (const uint8_t *)"", 0, env, sizeof env); CHECK(el && e2e_decrypt(&b, sb, 2, a.dh_pub, "x", env, el, pt, sizeof pt, &pl) == 0 && pl == 0);
    { static uint8_t big[3000], benv[3200], bpt[3200]; for (int i = 0; i < 3000; i++) big[i] = (uint8_t)i; size_t bl = e2e_encrypt(&a, &bb, "big", big, 3000, benv, sizeof benv); CHECK(bl && e2e_decrypt(&b, sb, 2, a.dh_pub, "big", benv, bl, bpt, sizeof bpt, &pl) == 0 && pl == 3000 && !memcmp(bpt, big, 3000)); }
    /* safety numbers agree on both sides and differ for other people */
    { uint8_t fa[32], fb[32], fc[32]; e2e_id_t c; e2e_new_identity(&c); e2e_fingerprint(a.dh_pub, a.sg_pub, fa); e2e_fingerprint(b.dh_pub, b.sg_pub, fb); e2e_fingerprint(c.dh_pub, c.sg_pub, fc);
      char s1[80], s2[80], s3[80]; e2e_safety_number(fa, fb, s1); e2e_safety_number(fb, fa, s2); e2e_safety_number(fa, fc, s3); CHECK(!strcmp(s1, s2) && strcmp(s1, s3) && strlen(s1) == 12 * 5 + 11); }
    /* base64url */
    { uint8_t raw[300], back[300]; char txt[500]; for (int i = 0; i < 300; i++) raw[i] = (uint8_t)(i * 31 + 5); for (size_t n = 0; n <= 300; n += 7) { size_t tl = e2e_b64enc(raw, n, txt); long bl = e2e_b64dec(txt, tl, back, sizeof back); CHECK(bl == (long)n && !memcmp(raw, back, n)); } CHECK(e2e_b64dec("A", 1, back, sizeof back) < 0); CHECK(e2e_b64dec("AB+/", 4, back, sizeof back) < 0); }
    /* files */
    { uint8_t key[32]; e2e_rand(key, 32); static uint8_t buf[3 * E2E_SEG], copy[3 * E2E_SEG]; for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 13);
      size_t sizes[3] = { E2E_SEG, E2E_SEG, 1000 }; static uint8_t cipher[3 * (E2E_SEG + 16)]; size_t co = 0, po = 0;
      for (int i = 0; i < 3; i++) { memcpy(cipher + co, buf + po, sizes[i]); e2e_file_seal(key, (uint64_t)i, i == 2, cipher + co, sizes[i]); co += sizes[i] + 16; po += sizes[i]; }
      CHECK(co == e2e_file_cipher_size(E2E_SEG * 2 + 1000)); memcpy(copy, cipher, co);
      size_t c0 = 0; for (int i = 0; i < 3; i++) { size_t cl = sizes[i] + 16; CHECK(e2e_file_open(key, (uint64_t)i, i == 2, copy + c0, cl) && !memcmp(copy + c0, buf + (size_t)i * E2E_SEG, sizes[i])); c0 += cl; }
      memcpy(copy, cipher, co); CHECK(!e2e_file_open(key, 1, false, copy, E2E_SEG + 16));                    /* a segment moved to another position */
      memcpy(copy, cipher, co); CHECK(!e2e_file_open(key, 0, true, copy, E2E_SEG + 16));                     /* ... or claimed to be the last one (truncation) */
      memcpy(copy, cipher, co); copy[5] ^= 1; CHECK(!e2e_file_open(key, 0, false, copy, E2E_SEG + 16));      /* an altered byte */
      CHECK(e2e_file_cipher_size(0) == 16); }
    /* groups */
    { uint8_t gk[32], gk2[32]; e2e_rand(gk, 32); e2e_rand(gk2, 32); const char *m = "hello group 日本語"; uint8_t genv[600], pt[600]; size_t gl, pl; e2e_id_t c; e2e_new_identity(&c);
      gl = e2e_group_encrypt(&a, gk, 5, "A|g1|m1|0", (const uint8_t *)m, strlen(m), genv, sizeof genv); CHECK(gl && (gl - E2E_GFIXED) % 64 == 0 && e2e_group_epoch(genv, gl) == 5);
      CHECK(e2e_group_decrypt(a.sg_pub, gk, "A|g1|m1|0", genv, gl, pt, sizeof pt, &pl) == 0 && pl == strlen(m) && !memcmp(pt, m, pl));
      CHECK(e2e_group_decrypt(a.sg_pub, gk, "A|g1|m2|0", genv, gl, pt, sizeof pt, &pl) == -3);                 /* another message id */
      CHECK(e2e_group_decrypt(a.sg_pub, gk, "A|g2|m1|0", genv, gl, pt, sizeof pt, &pl) == -3);                 /* another group */
      CHECK(e2e_group_decrypt(a.sg_pub, gk2, "A|g1|m1|0", genv, gl, pt, sizeof pt, &pl) == -3);                /* wrong group key */
      CHECK(e2e_group_decrypt(b.sg_pub, gk, "A|g1|m1|0", genv, gl, pt, sizeof pt, &pl) == -3);                 /* written in somebody else's name */
      { uint8_t evil[600]; size_t pos[5] = { 3, 8, 20, gl - 70, gl - 5 }; for (int pi = 0; pi < 5; pi++) { size_t i = pos[pi]; memcpy(evil, genv, gl); evil[i] ^= 1; CHECK(e2e_group_decrypt(a.sg_pub, gk, "A|g1|m1|0", evil, gl, pt, sizeof pt, &pl) == -3); } }
      /* a member who holds the key still cannot forge a message from another member: re-signing with their own key fails for the claimed sender */
      { uint8_t forged[600]; size_t fl = e2e_group_encrypt(&c, gk, 5, "A|g1|m1|0", (const uint8_t *)m, strlen(m), forged, sizeof forged); CHECK(fl && e2e_group_decrypt(a.sg_pub, gk, "A|g1|m1|0", forged, fl, pt, sizeof pt, &pl) == -3 && e2e_group_decrypt(c.sg_pub, gk, "A|g1|m1|0", forged, fl, pt, sizeof pt, &pl) == 0); }
      CHECK(e2e_group_decrypt(a.sg_pub, gk, "A|g1|m1|0", genv, 30, pt, sizeof pt, &pl) == -1);
      uint8_t e2[600]; size_t l2 = e2e_group_encrypt(&a, gk, 5, "A|g1|m1|0", (const uint8_t *)m, strlen(m), e2, sizeof e2); CHECK(l2 == gl && memcmp(genv + 5, e2 + 5, 12) != 0);   /* fresh nonce each time */
      /* a group key wrapped for one member with the pairwise construction */
      uint8_t wr[300], un[64]; size_t wl = e2e_encrypt(&a, &bb, "A|B|g1:5|K", gk, 32, wr, sizeof wr); CHECK(wl && e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|g1:5|K", wr, wl, un, sizeof un, &pl) == 0 && pl == 32 && !memcmp(un, gk, 32));
      CHECK(e2e_decrypt(&b, sb, 2, a.dh_pub, "A|B|g1:6|K", wr, wl, un, sizeof un, &pl) == -3); }
    puts(fails ? "FAILED" : "ALL PASS"); return fails != 0;
}
