/* memhog.c - touches more anonymous memory than the machine has (default 3 GiB): the kernel must stop this program, not itself.
 * usage: /linux-test/memhog [MiB] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
int main(int argc, char **argv) {
    size_t mib = argc > 1 ? (size_t)atol(argv[1]) : 3072, chunk = 16;
    size_t got = 0;
    for (size_t i = 0; i < mib / chunk; ++i) {
        char *p = mmap(NULL, chunk << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("memhog: mmap failed after %zu MiB\n", got); return 3; }
        memset(p, 1, chunk << 20); got += chunk;
        if (got % 256 == 0) { printf("memhog: %zu MiB\n", got); fflush(stdout); }
    }
    printf("memhog: touched %zu MiB\n", got);
    return 0;
}
