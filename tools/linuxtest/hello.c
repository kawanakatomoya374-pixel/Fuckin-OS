#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
int main(int argc, char **argv) {
    printf("hello from a Linux program on C-OS\n");
    for (int i = 0; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
    char *p = malloc(100000); memset(p, 7, 100000); printf("malloc ok %d\n", p[99999]);
    void *q = malloc(5 << 20); printf("big malloc %s\n", q ? "ok" : "failed");
    FILE *f = fopen("/tmp/lx_test.txt", "w"); if (f) { fprintf(f, "written by linux\n"); fclose(f); }
    f = fopen("/tmp/lx_test.txt", "r"); char buf[64] = {0}; if (f) { fgets(buf, sizeof buf, f); fclose(f); } printf("file: %s", buf);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); printf("time ok %d\n", ts.tv_sec >= 0);
    return 3;
}
