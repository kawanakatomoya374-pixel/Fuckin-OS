/* cosarc.c-os - tar, gzip, gunzip, zcat, zip and unzip for the Terminal (userland/lib/cosarc*.c).
 * The Terminal starts it like any program, so a long job runs in the background (jobs, kill) while the window stays alive.
 * usage: cosarc --cwd DIR [--progress FILE] COMMAND [ARGS...]      (the Terminal gives its current folder with --cwd) */
#include <stdint.h>
#include <string.h>
#include "cos.h"
#include "cosarc.h"

static void line(const char *s) { cos_puts(s); cos_puts("\n"); }

int main(int argc, char **argv) {
    const char *cwd = "/"; int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "--cwd")) { cwd = argv[i + 1]; i += 2; }
    if (i + 1 < argc && !strcmp(argv[i], "--progress")) { cosarc_set_progress(argv[i + 1]); i += 2; }
    if (i >= argc || !cosarc_is_command(argv[i])) {
        line("usage: tar -c|-x|-t [-z] [-v] -f ARCHIVE [-C DIR] [PATH...]   gzip [-k] FILE   gunzip FILE.gz   zcat FILE.gz   zip ARCHIVE.zip PATH...   unzip [-l] [-d DIR] ARCHIVE.zip");
        return 2;
    }
    return cosarc_main(argc - i, argv + i, cwd, line);
}
