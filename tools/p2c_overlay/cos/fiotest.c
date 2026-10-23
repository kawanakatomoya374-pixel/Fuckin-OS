/* fiotest.c - the file calls `tcc -ar` makes (fopen "wb+", write, fseek/ftell, read back; reading a whole object file), checked on C-OS.
 * Build and run on the device:  tcc fiotest.c -o /tmp/fiotest  then  /tmp/fiotest [OBJECT.o]   (prints FIO PASS / FIO FAIL lines) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
static void check(const char *n, int ok) { printf("FIO %s %s\n", ok ? "PASS" : "FAIL", n); if (!ok) fails++; }
int main(int argc, char **argv) {
    static const int sizes[] = { 10, 1000, 4000, 8000, 20000, 70000 };
    unsigned char *pat = malloc(80000), *back = malloc(80000);
    for (int i = 0; i < 80000; i++) pat[i] = (unsigned char)(i * 7 + 3);
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int n = sizes[k]; char nm[96];
        FILE *f = fopen("/tmp/fio.tmp", "wb+");
        snprintf(nm, sizeof nm, "w+ %d bytes: open", n); check(nm, f != NULL); if (!f) return 1;
        size_t w = fwrite(pat, n, 1, f);
        fseek(f, 0, SEEK_END); long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        memset(back, 0xEE, (size_t)n);
        size_t r = fread(back, n, 1, f);
        int bad = -1; for (int i = 0; i < n; i++) if (back[i] != pat[i]) { bad = i; break; }
        printf("FIO INFO n=%d fwrite=%d size=%ld fread=%d first_mismatch=%d (got %02x want %02x)\n", n, (int)w, sz, (int)r, bad, bad >= 0 ? back[bad] : 0, bad >= 0 ? pat[bad] : 0);
        snprintf(nm, sizeof nm, "w+ %d bytes: written, size right, read back equal", n);
        check(nm, w == 1 && sz == n && r == 1 && bad < 0);
        fclose(f);
        /* and once more from a fresh read-only open: what is on disk */
        f = fopen("/tmp/fio.tmp", "rb"); memset(back, 0xEE, (size_t)n); r = f ? fread(back, n, 1, f) : 0; if (f) fclose(f);
        snprintf(nm, sizeof nm, "w+ %d bytes: after fclose a read-only open reads the same", n);
        check(nm, r == 1 && memcmp(back, pat, (size_t)n) == 0);
        remove("/tmp/fio.tmp");
    }
    if (argc > 1) {
        FILE *o = fopen(argv[1], "rb");
        check("fopen object", o != NULL);
        if (o) {
            fseek(o, 0, SEEK_END); long s = ftell(o); fseek(o, 0, SEEK_SET);
            printf("FIO INFO object size %ld\n", s);
            unsigned char *b = malloc((size_t)s + 1); size_t r = fread(b, s, 1, o); fclose(o);
            check("fread whole object", r == 1 && s > 64);
            check("ELF magic and class 64", s > 20 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F' && b[4] == 2);
            unsigned long long shoff; memcpy(&shoff, b + 0x28, 8); unsigned short shnum; memcpy(&shnum, b + 0x3c, 2);
            printf("FIO INFO e_shoff %llu e_shnum %u\n", shoff, shnum);
            free(b);
        }
    }
    printf("FIO DONE failures=%d\n", fails);
    return fails != 0;
}
