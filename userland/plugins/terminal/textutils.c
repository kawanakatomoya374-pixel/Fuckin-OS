/* textutils.c - plugin of the Terminal: grep, head, tail, wc (on files). */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"
#include "cos_plugin_util.h"
static const term_api_t *A;
#define CAP 49152
static char buf[CAP];

static long load(const term_io_t *io, const char *name, char *abs, size_t acap) {
    io->resolve(name, abs, acap);
    long n = A->base.read_file(abs, 0, buf, CAP - 1);
    if (n < 0) { char m[200]; A->base.snprintf(m, sizeof m, "%s: cannot read", name); io->print(m); return -1; }
    buf[n] = 0; return n;
}
static int cmd_grep(int argc, char **argv, const term_io_t *io) {
    int icase = 0, i = 1, num = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) { for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'i') icase = 1; else if (*f == 'n') num = 1; } i++; }
    if (i + 1 >= argc) { io->print("usage: grep [-i] [-n] PATTERN FILE..."); return 1; }
    const char *pat = argv[i++]; int hits = 0;
    for (; i < argc; i++) {
        char abs[256]; if (load(io, argv[i], abs, sizeof abs) < 0) continue;
        int ln = 1;
        for (char *l = buf; *l; ln++) {
            char *e = l; while (*e && *e != '\n') e++;
            char save = *e; *e = 0;
            if (pu_find(l, pat, icase)) {
                char m[400]; if (num) A->base.snprintf(m, sizeof m, "%s:%d: %s", argv[i], ln, l); else A->base.snprintf(m, sizeof m, "%s: %s", argv[i], l);
                io->print(m); hits++;
            }
            *e = save; l = *e ? e + 1 : e;
        }
    }
    if (!hits) io->print("(no match)");
    return 0;
}
static int count_arg(int argc, char **argv, int *file) {
    int n = 10; *file = 1;
    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'n') { n = (int)pu_atol(argv[2]); *file = 3; }
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') { n = (int)pu_atol(argv[1] + 1); *file = 2; }
    return n < 1 ? 1 : n;
}
static int cmd_head(int argc, char **argv, const term_io_t *io) {
    int f, n = count_arg(argc, argv, &f); if (f >= argc) { io->print("usage: head [-n N] FILE"); return 1; }
    char abs[256]; if (load(io, argv[f], abs, sizeof abs) < 0) return 1;
    int ln = 0;
    for (char *l = buf; *l && ln < n; ln++) { char *e = l; while (*e && *e != '\n') e++; char s = *e; *e = 0; io->print(l); *e = s; l = *e ? e + 1 : e; }
    return 0;
}
static int cmd_tail(int argc, char **argv, const term_io_t *io) {
    int f, n = count_arg(argc, argv, &f); if (f >= argc) { io->print("usage: tail [-n N] FILE"); return 1; }
    char abs[256]; long len = load(io, argv[f], abs, sizeof abs); if (len < 0) return 1;
    int total = 0; for (long i = 0; i < len; i++) if (buf[i] == '\n') total++;
    if (len && buf[len - 1] != '\n') total++;
    int skip = total > n ? total - n : 0, ln = 0;
    for (char *l = buf; *l; ln++) { char *e = l; while (*e && *e != '\n') e++; char s = *e; *e = 0; if (ln >= skip) io->print(l); *e = s; l = *e ? e + 1 : e; }
    return 0;
}
static int cmd_wc(int argc, char **argv, const term_io_t *io) {
    if (argc < 2) { io->print("usage: wc FILE..."); return 1; }
    for (int i = 1; i < argc; i++) {
        char abs[256]; long len = load(io, argv[i], abs, sizeof abs); if (len < 0) continue;
        long lines = 0, words = 0; int in = 0;
        for (long k = 0; k < len; k++) { char c = buf[k]; if (c == '\n') lines++; if (c == ' ' || c == '\n' || c == '\t' || c == '\r') in = 0; else if (!in) { in = 1; words++; } }
        char m[300]; A->base.snprintf(m, sizeof m, "%7ld %7ld %7ld %s", lines, words, len, argv[i]); io->print(m);
    }
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const term_api_t *)h->api;
    if (!A || A->abi != TERMP_ABI) return -1;
    if (A->add_command("grep", "grep [-i] [-n] PATTERN FILE...   lines that contain the text", cmd_grep)) return -1;
    A->add_command("head", "head [-n N] FILE   the first lines", cmd_head);
    A->add_command("tail", "tail [-n N] FILE   the last lines", cmd_tail);
    A->add_command("wc", "wc FILE...   lines, words, bytes", cmd_wc);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("terminal", "textutils", "1.0", init, fini);
