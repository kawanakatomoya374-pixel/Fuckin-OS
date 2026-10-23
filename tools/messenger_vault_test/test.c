/* Host driver for userland/programs/messenger/vault_crypto.inc: scrypt and AES-256-GCM, compared with Python by tools/messenger_vault_test.sh */
#include <stdio.h>
#include "../../userland/programs/messenger/vault_crypto.inc"

br_prng_seeder br_prng_seeder_system(const char **name) { (void)name; return 0; }

static size_t unhex(const char *h, uint8_t *o) { size_t n = strlen(h) / 2; for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static void hex(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

int main(int argc, char **argv) {
    uint8_t a[4096], b[4096], c[64];
    if (argc >= 7 && !strcmp(argv[1], "scrypt")) {                  /* scrypt <pw> <salt-hex> <N> <r> <p> <dklen> */
        size_t sl = unhex(argv[3], a); size_t dk = (size_t)atoi(argv[7]);
        if (scrypt_kdf((const uint8_t *)argv[2], strlen(argv[2]), a, sl, (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), (uint32_t)atoi(argv[6]), b, dk)) return 2;
        hex(b, dk); return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "seal")) {                    /* seal <key> <nonce> <aad> <plaintext-hex> -> ciphertext||tag */
        uint8_t key[32], nonce[12], tag[16]; unhex(argv[2], key); unhex(argv[3], nonce);
        size_t n = unhex(argv[5], a); gcm_seal(key, nonce, argv[4], a, n, tag); hex(a, n); hex(tag, 16); return 0;
    }
    if (argc >= 7 && !strcmp(argv[1], "open")) {                    /* open <key> <nonce> <aad> <ciphertext-hex> <tag> */
        uint8_t key[32], nonce[12], tag[16]; unhex(argv[2], key); unhex(argv[3], nonce); unhex(argv[6], tag);
        size_t n = unhex(argv[5], a); if (!gcm_open(key, nonce, argv[4], a, n, tag)) { printf("FAIL\n"); return 0; } hex(a, n); return 0;
    }
    (void)b; (void)c; return 1;
}
