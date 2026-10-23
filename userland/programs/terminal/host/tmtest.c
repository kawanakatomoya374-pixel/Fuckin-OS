/* tmtest.c - host harness for the Terminal's make / compiler runner (testing only, not part of the .c-os build).
 * Reads commands from stdin, one per line, runs them through tm_make()/tm_exec() on top of studio/host/host_shim.c
 * (POSIX), and prints everything the Terminal would show. `cd DIR` changes the working directory; set TERMINAL_TCC to a
 * compiler that takes tcc-style arguments (e.g. a gcc wrapper). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cos.h"
#include "tmake.h"

static void out(const char *l) { printf("%s\n", l); fflush(stdout); }
int main(void) {
    char cwd[256] = "/", line[512];
    tm_init(out);
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        printf("$ %s\n", line);
        char *v[24]; int c = tm_tokenize(line, v, 24);
        if (!c) continue;
        if (!strcmp(v[0], "cd") && c > 1) { snprintf(cwd, sizeof cwd, "%s", v[1]); tm_announce(cwd); continue; }
        if (!strcmp(v[0], "make")) tm_make(c, v, cwd);
        else if (!tm_exec(c, v, cwd)) printf("%s: command not found\n", v[0]);
        for (int i = 0; tm_busy() && i < 20000; ++i) { tm_tick(); usleep(2000); }
        if (tm_busy()) { printf("TIMEOUT\n"); tm_stop(); }
    }
    return 0;
}
