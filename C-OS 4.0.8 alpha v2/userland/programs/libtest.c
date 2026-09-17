/* libtest.c - exercises the FILE* stdio layer, stdlib staples, and the
 * network syscalls: the surface a small external C program would need. */
#include "cos.h"

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { failures++; cos_printf("LIB_FAIL %s\n", what); }
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    /* ---- FILE* write, then read back line by line ---- */
    cos_FILE *f = cos_fopen("/libtest.txt", "w");
    check(f != NULL, "fopen for write");
    if (f) {
        cos_fprintf(f, "line %d\n", 1);
        cos_fputs("line 2\n", f);
        for (const char *p = "line 3\n"; *p; ++p) cos_fputc(*p, f);
        check(cos_fclose(f) == 0, "fclose flushes");
    }

    f = cos_fopen("/libtest.txt", "r");
    check(f != NULL, "fopen for read");
    if (f) {
        char line[64];
        check(cos_fgets(line, sizeof(line), f) && cos_strcmp(line, "line 1\n") == 0,
              "fgets line 1");
        check(cos_fgets(line, sizeof(line), f) && cos_strcmp(line, "line 2\n") == 0,
              "fgets line 2");
        check(cos_fgets(line, sizeof(line), f) && cos_strcmp(line, "line 3\n") == 0,
              "fgets line 3");
        check(cos_fgets(line, sizeof(line), f) == NULL, "fgets returns NULL at EOF");
        cos_fclose(f);
    }

    /* ---- fseek/ftell/ungetc ---- */
    f = cos_fopen("/libtest.txt", "r");
    if (f) {
        check(cos_fseek(f, 5) == 0, "fseek");
        check(cos_ftell(f) == 5, "ftell after fseek");
        int c = cos_fgetc(f);
        check(c == '1', "fgetc after fseek lands on the right byte");
        cos_ungetc(c, f);
        check(cos_fgetc(f) == '1', "ungetc pushes the byte back");
        cos_fclose(f);
    }

    /* ---- fread/fwrite on binary data ---- */
    unsigned char blob[256];
    for (int i = 0; i < 256; ++i) blob[i] = (unsigned char)(i ^ 0x5A);
    f = cos_fopen("/libtest.bin", "w");
    check(f && cos_fwrite(blob, 1, 256, f) == 256, "fwrite 256 bytes");
    if (f) cos_fclose(f);

    unsigned char back[256];
    f = cos_fopen("/libtest.bin", "r");
    check(f && cos_fread(back, 1, 256, f) == 256, "fread 256 bytes");
    if (f) cos_fclose(f);
    check(cos_memcmp(blob, back, 256) == 0, "binary round-trip is byte-exact");

    /* ---- stdlib staples ---- */
    char *end = NULL;
    check(cos_strtol("  -1234xyz", &end, 10) == -1234 && *end == 'x', "strtol base 10");
    check(cos_strtol("0xFF", &end, 16) == 255, "strtol base 16");
    check(cos_strtol("0x1A", &end, 0) == 26, "strtol auto-detect hex");
    /* "no digits" must leave end at the original string, not past it -
     * that is how a caller tells "parsed 0" from "parsed nothing". */
    const char *bad = "zzz";
    check(cos_strtol(bad, &end, 10) == 0 && end == bad, "strtol reports no-digits");

    int arr[] = { 42, 7, 99, 1, 23, 7 };
    cos_qsort(arr, 6, sizeof(int), cmp_int);
    check(arr[0] == 1 && arr[1] == 7 && arr[2] == 7 && arr[3] == 23
          && arr[4] == 42 && arr[5] == 99, "qsort sorts (incl. duplicates)");

    char *d = cos_strdup("duplicated");
    check(d && cos_strcmp(d, "duplicated") == 0, "strdup");
    cos_free(d);

    check(cos_strstr("hello world", "o w") != NULL, "strstr finds");
    check(cos_strstr("hello", "xyz") == NULL, "strstr misses");
    check(cos_strrchr("a/b/c", '/') != NULL && *(cos_strrchr("a/b/c", '/') + 1) == 'c',
          "strrchr finds the last one");
    check(cos_abs(-5) == 5, "abs");

    cos_srand(12345);
    int r1 = cos_rand(), r2 = cos_rand();
    check(r1 != r2, "rand advances");
    cos_srand(12345);
    check(cos_rand() == r1, "rand is reproducible from a seed");

    /* ---- networking ---- */
    bool net = cos_net_available();
    cos_printf("LIB net_available=%d\n", net ? 1 : 0);
    /* Availability is reported, not asserted: the test VM is deliberately
     * booted with --no-network, so demanding a successful request here
     * would fail for a reason that has nothing to do with the code. What
     * IS checked is that the syscall returns cleanly either way rather
     * than faulting or hanging. */
    uint8_t ip[4] = {0,0,0,0};
    int rc = cos_net_resolve("example.com", ip);
    cos_printf("LIB net_resolve rc=%d\n", rc);
    check(rc == 0 || rc == -1, "net_resolve returns a defined result");

    cos_unlink("/libtest.txt");
    cos_unlink("/libtest.bin");

    if (failures == 0) cos_printf("LIB_PASS stdio, stdlib and net syscalls all verified\n");
    else cos_printf("LIB_FAIL %d checks failed\n", failures);
    return failures;
}
