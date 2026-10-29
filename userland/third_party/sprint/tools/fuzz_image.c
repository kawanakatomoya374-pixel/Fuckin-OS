/* Mutation fuzzer for the image loader, verifier and VM.
 *   build/fuzz-image IMAGE.spc [ITERATIONS [SEED]]
 * Each iteration mutates payload bytes, repairs the checksum (so the verifier,
 * not the checksum, is what gets exercised), decodes, and runs whatever
 * survives with tight limits. Build with `make fuzz` (ASan + UBSan). */
#include "sp.h"

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)fputs("usage: fuzz-image IMAGE [ITERATIONS [SEED]]\n", stderr);
        return 2;
    }
    long iters = argc > 2 ? atol(argv[2]) : 1000;
    if (argc > 3) rng_state ^= (uint64_t)atol(argv[3]) * 0x9E3779B97F4A7C15ull;
    size_t len = 0;
    char *base = sp_read_file(argv[1], &len);
    if (!base || len < 96) {
        (void)fputs("fuzz-image: cannot read image\n", stderr);
        return 2;
    }
    size_t psize = len - 96;
    long rejected = 0, ran = 0;
    uint8_t *buf = malloc(len);
    if (!buf) return 2;
    if (!freopen("/dev/null", "w", stdout)) return 2;
    for (long it = 0; it < iters; it++) {
        memcpy(buf, base, len);
        int nmut = 1 + (int)(rnd() % 3);
        for (int m = 0; m < nmut; m++) {
            size_t pos = 64 + (size_t)(rnd() % psize);
            switch (rnd() % 4) {
            case 0: buf[pos] = (uint8_t)rnd(); break;
            case 1: buf[pos] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 2: buf[pos] = (uint8_t)(rnd() % 2 ? 0xFF : 0x00); break;
            default: buf[pos] = (uint8_t)(buf[pos] + 1); break;
            }
        }
        sp_sha256(buf + 64, psize, buf + 64 + psize);
        if (getenv("FUZZ_SAVE")) {
            FILE *sf = fopen(getenv("FUZZ_SAVE"), "wb");
            if (sf) {
                (void)fwrite(buf, 1, len, sf);
                (void)fclose(sf);
            }
        }
        Limits lim;
        limits_default(&lim);
        lim.max_instructions = 100000;
        lim.max_heap_bytes = 64u << 20;
        VM *vm = vm_new(&lim, CAP_STDIO_WRITE);
        vm->deterministic = 1;
        char err[256];
        Program *pg = image_decode(vm, buf, len, err, sizeof err);
        if (!pg) {
            rejected++;
        } else {
            ran++;
            (void)vm_run_program(vm, pg, 0, NULL);
            program_free(pg);
        }
        vm_free(vm);
    }
    (void)fprintf(stderr, "fuzz-image: %ld iterations, %ld rejected, %ld ran, no crashes\n", iters, rejected, ran);
    free(buf);
    free(base);
    return 0;
}
