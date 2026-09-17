/* ctest.c - proves the C runtime works end to end. */
#include "cos.h"

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) { failures++; cos_printf("CRT_FAIL %s\n", what); }
}

int main(int argc, char **argv) {
    /* argc/argv delivered by crt0 from the kernel's System V stack */
    check(argc == 1, "argc==1");
    check(argv != NULL && argv[0] != NULL, "argv[0] non-null");
    check(argv[1] == NULL, "argv NULL-terminated");
    cos_printf("CRT argv[0]=%s argc=%d\n", argv[0], argc);

    /* printf conversions */
    char b[64];
    cos_snprintf(b, sizeof(b), "%d|%u|%x|%s|%c|%%", -42, 42u, 255u, "str", 'Z');
    check(cos_strcmp(b, "-42|42|ff|str|Z|%") == 0, "snprintf conversions");
    cos_printf("CRT snprintf -> %s\n", b);

    /* strings */
    char s[32];
    cos_strcpy(s, "abc");
    cos_strcat(s, "def");
    check(cos_strcmp(s, "abcdef") == 0, "strcpy/strcat");
    check(cos_strlen(s) == 6, "strlen");
    check(cos_atoi("-123") == -123, "atoi");

    /* malloc: allocate, write a pattern, verify, free, reuse */
    int *arr = cos_malloc(256 * sizeof(int));
    check(arr != NULL, "malloc");
    for (int i = 0; i < 256; ++i) arr[i] = i * 3;
    bool ok = true;
    for (int i = 0; i < 256; ++i) if (arr[i] != i * 3) ok = false;
    check(ok, "malloc read-back");
    cos_free(arr);

    void *a = cos_malloc(1000);
    void *c = cos_malloc(1000);
    check(a && c && a != c, "two distinct allocations");
    cos_free(a); cos_free(c);

    /* calloc must zero */
    unsigned char *z = cos_calloc(100, 1);
    check(z != NULL, "calloc");
    bool zeroed = true;
    for (int i = 0; i < 100; ++i) if (z[i]) zeroed = false;
    check(zeroed, "calloc zeroes");
    cos_free(z);

    /* realloc preserves contents */
    char *r = cos_malloc(16);
    cos_strcpy(r, "preserve");
    r = cos_realloc(r, 512);
    check(r && cos_strcmp(r, "preserve") == 0, "realloc preserves data");
    cos_free(r);

    /* file I/O through the C API, into heap memory */
    const char *msg = "written by a C program";
    check(cos_write_file("/ctest.txt", msg, cos_strlen(msg)) == (ssize_t)cos_strlen(msg),
          "write_file");
    char *rb = cos_malloc(128);
    ssize_t got = cos_read_file("/ctest.txt", rb, 128);
    check(got == (ssize_t)cos_strlen(msg), "read_file length");
    check(cos_memcmp(rb, msg, cos_strlen(msg)) == 0, "read_file contents");
    cos_free(rb);

    /* syscalls */
    check(cos_getpid() > 0, "getpid");
    cos_sleep_ms(10);

    if (failures == 0) cos_printf("CRT_PASS all C runtime checks passed\n");
    else cos_printf("CRT_FAIL %d checks failed\n", failures);
    return failures;
}
