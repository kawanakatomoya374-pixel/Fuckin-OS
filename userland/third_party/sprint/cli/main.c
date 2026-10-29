/* sprint CLI: run / check / disasm / version / features */
#define _POSIX_C_SOURCE 200809L
#include "sp.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "tools.h"

#define SPRINT_VERSION "2.31.0"

static int usage(FILE *f) {
    (void)fputs("usage: sprint run [--json] [--deterministic] [--max-heap N] [--max-instr N]\n"
                "                  [--no-quicken] [--no-jit] [--allow-read=DIR] [--allow-write=DIR] [--allow-random] [--allow-ffi] [--allow-all] [--deny-clock]\n"
                "                  FILE [ARGS...]\n"
                "       sprint check [--json] FILE\n"
                "       sprint compile [-o OUT.spc] FILE\n"
                "       sprint exec [options] FILE.spc [ARGS...]\n"
                "       sprint disasm FILE\n"
                "       sprint fmt [--check] [-w] FILE...      (FILE '-' = stdin)\n"
                "       sprint lint [--json] FILE...\n"
                "       sprint repl [--deterministic] [--allow-*] [--max-heap N] [--max-instr N]\n"
                "       sprint version | features\n", f);
    return f == stderr ? 2 : 0;
}

static void json_str(FILE *f, const char *s) {
    (void)fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') (void)fprintf(f, "\\%c", c);
        else if (c == '\n') (void)fputs("\\n", f);
        else if (c < 0x20) (void)fprintf(f, "\\u%04x", (unsigned)c);
        else (void)fputc((int)c, f);
    }
    (void)fputc('"', f);
}

static void print_diag(const Diag *d, int json) {
    if (json) {
        (void)fputs("{\"kind\":", stderr); json_str(stderr, d->kind);
        (void)fprintf(stderr, ",\"code\":%d,\"file\":", d->code); json_str(stderr, d->file);
        (void)fprintf(stderr, ",\"line\":%d,\"col\":%d,\"message\":", d->line, d->col); json_str(stderr, d->msg);
        (void)fputs("}\n", stderr);
    } else {
        (void)fprintf(stderr, "%s:%d:%d: %s: %s\n", d->file, d->line, d->col, d->kind, d->msg);
    }
}

#define MAX_ALLOW 16
typedef struct Opts {
    int json, deterministic;
    uint64_t max_heap, max_instr;
    const char *allow_r[MAX_ALLOW], *allow_w[MAX_ALLOW];
    int nr, nw;
    int allow_random, allow_ffi, deny_clock, allow_all, no_quicken, no_jit;
    const char *out;
} Opts;

/* Front end: returns program or NULL (diag filled). Kept small for setjmp safety. */
static Program *frontend(Front *F, VM *vm, const char *path, char *src, size_t len) {
    Program *volatile prog = NULL;
    if (setjmp(F->jb) == 0) prog = load_program(F, vm, path, src, len);
    return prog;
}

static int diag_exit(const Diag *d) { return d->code == E_IMPORT ? 4 : 12; }

static int parse_u64(const char *s, uint64_t *out) {
    char *end;
    if (!*s) return 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (*end) return 0;
    *out = v;
    return 1;
}

static int cmd_file(const char *cmd, int argc, char **argv) {
    Opts o;
    memset(&o, 0, sizeof o);
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--json") == 0) o.json = 1;
        else if (strcmp(argv[i], "--deterministic") == 0) o.deterministic = 1;
        else if (strcmp(argv[i], "--max-heap") == 0 && i + 1 < argc && parse_u64(argv[i + 1], &o.max_heap)) i++;
        else if (strcmp(argv[i], "--max-instr") == 0 && i + 1 < argc && parse_u64(argv[i + 1], &o.max_instr)) i++;
        else if (strncmp(argv[i], "--allow-read=", 13) == 0 && o.nr < MAX_ALLOW) o.allow_r[o.nr++] = argv[i] + 13;
        else if (strncmp(argv[i], "--allow-write=", 14) == 0 && o.nw < MAX_ALLOW) o.allow_w[o.nw++] = argv[i] + 14;
        else if (strcmp(argv[i], "--allow-random") == 0) o.allow_random = 1;
        else if (strcmp(argv[i], "--allow-clock") == 0) o.deny_clock = 0;
        else if (strcmp(argv[i], "--deny-clock") == 0) o.deny_clock = 1;
        else if (strcmp(argv[i], "--allow-ffi") == 0) o.allow_ffi = 1;
        else if (strcmp(argv[i], "--allow-all") == 0) o.allow_all = 1;
        else if (strcmp(argv[i], "--no-quicken") == 0) o.no_quicken = 1;
        else if (strcmp(argv[i], "--no-jit") == 0) o.no_jit = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) o.out = argv[++i];
        else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else return usage(stderr);
    }
    if (i >= argc) return usage(stderr);
    const char *path = argv[i++];
    size_t len = 0;
    char *src = sp_read_file(path, &len);
    if (!src) { (void)fprintf(stderr, "sprint: cannot read '%s'\n", path); return 3; }
    int is_img = len >= 4 && memcmp(src, "SPRC", 4) == 0;
    if (strcmp(cmd, "exec") == 0 && !is_img) {
        (void)fprintf(stderr, "%s: LoadError: not a program image (use 'sprint run' for source files)\n", path);
        free(src);
        return 13;
    }
    if (!is_img) len = sp_normalize(src, len);
    Diag pre;
    memset(&pre, 0, sizeof pre);
    pre.kind = "LexError"; pre.code = E_LEX; pre.line = 1; pre.col = 1;
    (void)snprintf(pre.file, sizeof pre.file, "%s", path);
    size_t bad;
    if (!is_img && !utf8_validate(src, len, &bad)) {
        (void)snprintf(pre.msg, sizeof pre.msg, "invalid UTF-8 at byte %zu", bad);
        print_diag(&pre, o.json); free(src); return 12;
    }
    if (!is_img && memchr(src, 0, len)) {
        (void)snprintf(pre.msg, sizeof pre.msg, "NUL byte in source");
        print_diag(&pre, o.json); free(src); return 12;
    }

    Limits lim;
    limits_default(&lim);
    if (o.max_heap) lim.max_heap_bytes = o.max_heap;
    if (o.max_instr) lim.max_instructions = o.max_instr;
    int caps = CAP_STDIO_READ | CAP_STDIO_WRITE | (o.deny_clock ? 0 : CAP_CLOCK);
    if (o.allow_random || o.allow_all) caps |= CAP_RANDOM;
    if (o.allow_ffi) caps |= CAP_FFI; /* deliberately not part of --allow-all */
    VM *vm = vm_new(&lim, caps);
    vm->deterministic = o.deterministic;
    vm->no_quicken = o.no_quicken;
    if (o.no_jit) vm->jit_on = 0;
    if (o.allow_all) {
        o.allow_r[o.nr++ % MAX_ALLOW] = ".";
        o.allow_w[o.nw++ % MAX_ALLOW] = ".";
    }
    for (int k = 0; k < o.nr && k < MAX_ALLOW; k++)
        if (lib_allow_dir(vm, 0, o.allow_r[k]) != 0) {
            (void)fprintf(stderr, "sprint: cannot use --allow-read=%s\n", o.allow_r[k]);
            vm_free(vm); free(src); return 2;
        }
    for (int k = 0; k < o.nw && k < MAX_ALLOW; k++)
        if (lib_allow_dir(vm, 1, o.allow_w[k]) != 0) {
            (void)fprintf(stderr, "sprint: cannot use --allow-write=%s\n", o.allow_w[k]);
            vm_free(vm); free(src); return 2;
        }
    Front F;
    memset(&F, 0, sizeof F);
    F.file = path;
    (void)snprintf(F.diag.file, sizeof F.diag.file, "%s", path);
    Program *prog = NULL;
    char verr[256];
    verr[0] = 0;
    int rc = 0;
    if (is_img) {
        prog = image_decode(vm, (const uint8_t *)src, len, verr, sizeof verr);
        free(src);
        if (!prog) {
            (void)fprintf(stderr, "%s: LoadError: %s\n", path, verr);
            vm_free(vm);
            return 13;
        }
    } else {
        prog = frontend(&F, vm, path, src, len);
        if (prog && verify_program(prog, verr, sizeof verr)) {
            (void)fprintf(stderr, "%s: internal compiler error (verifier): %s\n", path, verr);
            program_free(prog);
            vm_free(vm);
            return 70;
        }
    }
    if (!prog) {
        print_diag(&F.diag, o.json);
        rc = diag_exit(&F.diag);
    } else if (strcmp(cmd, "check") == 0) {
        rc = 0;
    } else if (strcmp(cmd, "compile") == 0) {
        char defout[512];
        const char *outp = o.out;
        if (!outp) {
            (void)snprintf(defout, sizeof defout, "%s", path);
            char *dot = strrchr(defout, '.');
            char *sl = strrchr(defout, '/');
            if (dot && (!sl || dot > sl)) *dot = 0;
            size_t dl = strlen(defout);
            if (dl + 5 < sizeof defout) memcpy(defout + dl, ".spc", 5);
            outp = defout;
        }
        size_t ilen = 0;
        uint8_t *img = image_encode(prog, &ilen);
        char tmpn[600];
        (void)snprintf(tmpn, sizeof tmpn, "%s.tmp", outp);
        FILE *of = fopen(tmpn, "wb");
        int wok = of && fwrite(img, 1, ilen, of) == ilen;
        if (of && fclose(of) != 0) wok = 0;
        if (wok && rename(tmpn, outp) != 0) wok = 0;
        if (!wok) {
            (void)remove(tmpn);
            (void)fprintf(stderr, "sprint: cannot write '%s'\n", outp);
            rc = 3;
        }
        free(img);
    } else if (strcmp(cmd, "disasm") == 0) {
        for (int k = 0; k < prog->nprotos; k++) proto_disasm(prog->protos[k], stdout);
    } else {
        RunStatus st = vm_run_program(vm, prog, argc - i, argv + i);
        vm_out_flush(vm);
        if (st == RS_OK) rc = 0;
        else if (st == RS_EXIT) rc = vm->exit_code;
        else { vm_print_uncaught(vm, o.json); rc = (int)st; }
    }
    if (prog) program_free(prog);
    vm_free(vm);
    return rc;
}

int main(int argc, char **argv) {
#ifdef __GLIBC__
    /* big arrays and strings are mmap'ed and handed back to the system as soon as they are freed (glibc would otherwise
     * raise the threshold after the first big free and keep such blocks in the heap): peak memory follows live data */
    (void)mallopt(M_MMAP_THRESHOLD, 256 * 1024);
#endif
    if (argc < 2) return usage(stderr);
    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0) { (void)puts("sprint " SPRINT_VERSION); return 0; }
    if (strcmp(cmd, "features") == 0) {
        (void)puts("core\nmodules: math str table io sys time gc coroutine json random fs");
        return 0;
    }
    if (strcmp(cmd, "run") == 0 || strcmp(cmd, "check") == 0 || strcmp(cmd, "disasm") == 0 ||
        strcmp(cmd, "compile") == 0 ||
        strcmp(cmd, "exec") == 0)
        return cmd_file(cmd, argc - 2, argv + 2);
    if (strcmp(cmd, "fmt") == 0) { int r = tool_fmt(argc - 2, argv + 2); return r == 2 ? usage(stderr) : r; }
    if (strcmp(cmd, "repl") == 0) { int ue; int r = tool_repl(argc - 2, argv + 2, &ue); return ue ? usage(stderr) : r; }
    if (strcmp(cmd, "lint") == 0) { int r = tool_lint(argc - 2, argv + 2); return r == 2 ? usage(stderr) : r; }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) return usage(stdout);
    return usage(stderr);
}
