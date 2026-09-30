/* tcc_cos.c - tcc.c-os: the TinyCC compiler as a C-OS program.
 *
 * One translation unit: it pulls in the UNMODIFIED upstream tcc.c
 * (src/third_party/tinyc, which #includes libtcc.c and the rest itself when
 * ONE_SOURCE is set), renames upstream's main() to tcc_main(), and supplies a
 * main() of its own that adds the C-OS defaults before calling it - so that
 *
 *     tcc.c-os hello.c -o hello.c-os
 *
 * just works, like tools/cos-cc does on the host: it finds the SDK headers,
 * links a static ET_EXEC into the program region, and adds the C-OS start-up
 * code and runtime. -c, -E, -S, -shared and -ar are passed through untouched.
 *
 * `tcc -run file.c [prog-args...]` is upstream TinyCC's own JIT: compile to
 * memory, relocate, mprotect the code pages executable, and jump straight to
 * main() - all inside THIS process, never touching disk. It needs real
 * mmap()/mprotect(), which tcc_shim.c now provides on top of the kernel's
 * SYS_MMAP/SYS_MPROTECT (see there for the W^X enforcement: one mprotect
 * asking for write+exec together is refused, so a JIT gets there the way
 * tccrun.c already does it unprompted - mmap the pages RW, write the
 * relocated code, mprotect to RX before running it - never RWX at once).
 * `TCC_IS_NATIVE` (tcc.h) gates -run on being a same-architecture compiler,
 * which this is (TCC_TARGET_X86_64 on an x86_64 host), so upstream's -run
 * handling runs entirely unmodified; this file only has to get it the right
 * command line. -run does not use cos_crt0.o - TinyCC supplies its own
 * tiny entry stub (runmain.o, built into tcc.c) that calls main() and
 * captures its return value directly - but it still needs libcos.a and
 * libtcc1.a linked in ahead of "-run" so printf(), cos_win2_create() and
 * the rest resolve as ordinary relocations against those archives, the same
 * way they would for a normal static build, rather than through dlsym()
 * (there is no dynamic linker here for -run to fall back on). */
#include "compat/tcc_posix.h"

#define main tcc_main
#include "tcc.c"
#undef main

#include <stdlib.h>
#include <string.h>
#include "cos.h"

#define SDK_INC "/system/sdk/include"
#define SDK_LIB "/system/sdk/lib"

/* strtold: the libc has strtod only; a long double literal in the program being
 * compiled (1.5L) is parsed with double precision and widened. */
long double strtold(const char *s, char **end) { return (long double)strtod(s, end); }

/* `tcc -run` needs libcos.a/libtcc1.a linked in for printf() and friends to resolve, but
 * TinyCC's own command-line parser ends ALL further option/file processing the instant it
 * sees the one file named after "-run" (dorun: in tcc_parse_args - by design, so plain
 * tokens afterward become the program's own argv, not more compiler input). That makes it
 * impossible to place an archive AFTER the source on the command line, and archives are
 * resolved eagerly, in the order tcc sees them (tcc_load_alacarte pulls in only members
 * needed by symbols ALREADY undefined at that moment) - so an archive placed BEFORE the
 * source, as the plain command-line path would require, silently fails to resolve symbols
 * the source doesn't reference until it's compiled a moment later. Driving the same libtcc
 * API tcc_main() itself sits on top of - rather than going through argv a second time -
 * sidesteps both problems: the source is added first, so printf() etc. are already
 * undefined by the time libcos.a and libtcc1.a are added right after. */
static int run_tcc_run(int argc, char **argv, int run_at)
{
    if (run_at + 1 >= argc) { fprintf(stderr, "tcc: -run needs a source file\n"); return 1; }
    const char *src = argv[run_at + 1];

    TCCState *s = tcc_new();
    if (!s) { fprintf(stderr, "tcc: could not initialize\n"); return 1; }

    /* tcc_lib_path (== "{B}" in tcc's own path templates) must be set before
     * tcc_set_output_type(), which is what actually populates library_paths from it -
     * this is how tcc_add_support("runmain.o") later finds it under SDK_LIB rather than
     * one directory up, where CONFIG_TCCDIR points by default. */
    tcc_set_lib_path(s, SDK_LIB);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    /* whatever compiler options (-I, -D, -O, -g, -W...) appeared before "-run" on this
     * process's own command line, plus the same defaults a normal build gets. Concatenated
     * into one string for tcc_set_options(), which parses it exactly like argv - files
     * would be accepted here too, so this deliberately carries options only; the source and
     * the archives below are added straight through tcc_add_file() instead, in the order
     * that makes their symbols resolve correctly (see the function comment above). */
    char optbuf[1024]; size_t k = (size_t)snprintf(optbuf, sizeof optbuf, "-nostdinc -I" SDK_INC " -nostdlib");
    for (int i = 1; i < run_at && k < sizeof optbuf - 64; ++i) k += (size_t)snprintf(optbuf + k, sizeof optbuf - k, " %s", argv[i]);
    if (tcc_set_options(s, optbuf) < 0) { tcc_delete(s); return 1; }

    if (tcc_add_file(s, src) < 0) { tcc_delete(s); return 1; }
    if (tcc_add_file(s, SDK_LIB "/libcos.a") < 0) { tcc_delete(s); return 1; }
    if (tcc_add_file(s, SDK_LIB "/libtcc1.a") < 0) { tcc_delete(s); return 1; }

    char *pav[64]; int pn = 0;
    pav[pn++] = (char *)src;                                 /* argv[0] for the program: its source name, as upstream tcc does */
    for (int i = run_at + 2; i < argc && pn < 63; ++i) pav[pn++] = argv[i];
    pav[pn] = NULL;

    int rc = tcc_run(s, pn, pav);
    tcc_delete(s);
    return rc;
}

int main(int argc, char **argv)
{
    static char *av[512];
    int n = 0, i;
    int link = 1, tool = 0, run_at = -1;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (i == 1 && !strcmp(a, "-ar")) tool = 1;
        else if (!strcmp(a, "-run")) { run_at = i; break; }      /* handled separately below - see run_tcc_run */
        else if (!strcmp(a, "-c") || !strcmp(a, "-E") || !strcmp(a, "-S") || !strcmp(a, "-shared") ||
                 !strcmp(a, "-v") || !strcmp(a, "-vv") || !strcmp(a, "-h") || !strcmp(a, "-hh") ||
                 !strcmp(a, "-dumpversion") || !strcmp(a, "-dumpmachine") || !strncmp(a, "-print-", 7) ||
                 !strcmp(a, "-M") || !strcmp(a, "-MM"))
            link = 0;
    }
    if (run_at >= 0) return run_tcc_run(argc, argv, run_at);
    if (argc > 480) return tcc_main(argc, argv);         /* absurd command line: do not truncate it silently */

    av[n++] = argv[0];
    if (!tool) {
        av[n++] = "-nostdinc";
        av[n++] = "-I" SDK_INC;
        if (link) {
            av[n++] = "-static";
            av[n++] = "-nostdlib";
            /* the program region (PML4[1]); see docs/ELF_LOADER.md and the ELF loader
             * conformance test, which loads exactly this shape of binary */
            av[n++] = "-Wl,-Ttext=0x8000001000";
        }
    }
    for (i = 1; i < argc; ++i) av[n++] = argv[i];
    if (!tool && link) {
        av[n++] = SDK_LIB "/cos_crt0.o";
        av[n++] = SDK_LIB "/libcos.a";
        av[n++] = SDK_LIB "/libtcc1.a";
    }
    av[n] = NULL;
    return tcc_main(n, av);
}
