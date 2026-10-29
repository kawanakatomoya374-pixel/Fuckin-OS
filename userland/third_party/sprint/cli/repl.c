/* sprint repl: interactive read-eval-print loop (SPEC 22.3).
 *
 * Every input is compiled as one more module ("chunk") of a single growing Program, against
 * the module-variable scope of all earlier chunks, and executed on the same VM. Definitions
 * therefore persist (functions, classes, variables keep their identity). A lone expression is
 * evaluated as `_ = expr` and its value is printed when it is not nil. */
#define _POSIX_C_SOURCE 200809L
#include "sp.h"
#include "tools.h"
#include <unistd.h>

#define MAX_ALLOW 16

typedef struct Session {
    VM *vm;
    Program *prog;
    ReplScope *rs;
    Arena **arenas;
    int narenas, caparenas;
    int chunk_no;
} Session;

typedef struct ReplOpts {
    int deterministic, no_quicken, allow_random, allow_all, deny_clock, allow_ffi;
    uint64_t max_heap, max_instr;
    const char *allow_r[MAX_ALLOW], *allow_w[MAX_ALLOW];
    int nr, nw;
} ReplOpts;

static char *dup_str(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) exit(14);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* setjmp lives in small noinline helpers (-Wclobbered). */
__attribute__((noinline)) static int guarded_chunk(Front *F, Session *S, char *src, size_t len, const char *name, Arena **keep) {
    if (setjmp(F->jb) == 0) {
        load_chunk(F, S->vm, S->prog, S->rs, keep, src, len, name);
        return 0;
    }
    return 1;
}

__attribute__((noinline)) static int guarded_parse(Front *F, const char *src, size_t len, int *lex_err, char *msg, size_t msgcap) {
    TokenList tl;
    memset(&tl, 0, sizeof tl);
    Arena *A = arena_new();
    int rc = 0;
    *lex_err = 0;
    if (setjmp(F->jb) == 0) {
        lex_source(F, src, len, &tl);
        if (setjmp(F->jb) == 0) (void)parse_program(F, A, &tl);
        else rc = 1;
    } else {
        rc = 1;
        *lex_err = 1;
    }
    front_done(F);
    if (rc) (void)snprintf(msg, msgcap, "%s", F->diag.msg);
    toklist_free(&tl);
    arena_free(A);
    return rc;
}

/* 0 = complete and valid (or an ordinary error), 1 = the input is unfinished */
static int unfinished(const char *buf, size_t len) {
    Front F;
    memset(&F, 0, sizeof F);
    F.file = "<repl>";
    char msg[256];
    int lex_err = 0;
    char *copy = dup_str(buf, len);
    size_t n = sp_normalize(copy, len);
    int rc = guarded_parse(&F, copy, n, &lex_err, msg, sizeof msg);
    free(copy);
    if (!rc) return 0;
    if (lex_err) return strstr(msg, "unclosed bracket") != NULL || strstr(msg, "unterminated string literal") != NULL;
    return strstr(msg, "found end of file") != NULL;
}

static int is_block_opener(const char *line, size_t n) {
    /* strip a trailing comment and blanks; a block opener ends with ':' */
    size_t end = n;
    for (size_t i = 0; i < n; i++)
        if (line[i] == '#') { end = i; break; }
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\n')) end--;
    return end > 0 && line[end - 1] == ':';
}

static int is_blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') return 0;
    return 1;
}

static void keep_arena(Session *S, Arena *A) {
    if (!A) return;
    if (S->narenas == S->caparenas) {
        S->caparenas = S->caparenas ? S->caparenas * 2 : 16;
        S->arenas = realloc(S->arenas, sizeof(Arena *) * (size_t)S->caparenas);
        if (!S->arenas) exit(14);
    }
    S->arenas[S->narenas++] = A;
}

static void echo_sink(void *ud, const char *s, size_t n) {
    (void)ud;
    (void)fwrite(s, 1, n, stdout);
}

static void clear_error(VM *vm) {
    vm->uncaught = v_nil();
    vm->result = RS_OK;
}

/* Evaluate one input. Returns -1 to continue the session, or an exit code (>= 0) to stop it. */
static int session_eval(Session *S, const char *text, size_t tlen, int echo) {
    VM *vm = S->vm;
    Front F;
    memset(&F, 0, sizeof F);
    F.file = "<repl>";
    (void)snprintf(F.diag.file, sizeof F.diag.file, "<repl>");
    char *src = dup_str(text, tlen);
    size_t len = sp_normalize(src, tlen);
    if (len == 0 || src[len - 1] != '\n') {
        src = realloc(src, len + 2);
        if (!src) exit(14);
        src[len++] = '\n';
        src[len] = 0;
    }
    size_t bad;
    if (!utf8_validate(src, len, &bad) || memchr(src, 0, len)) {
        (void)fputs("<repl>: LexError: invalid UTF-8 or NUL byte in input\n", stderr);
        free(src);
        return -1;
    }
    char name[32];
    (void)snprintf(name, sizeof name, "<repl:%d>", ++S->chunk_no);
    Arena *keep = NULL;
    replscope_echo(S->rs, echo);
    if (guarded_chunk(&F, S, src, len, name, &keep)) {
        (void)fprintf(stderr, "%s:%d:%d: %s: %s\n", F.diag.file, F.diag.line, F.diag.col, F.diag.kind, F.diag.msg);
        return -1;
    }
    keep_arena(S, keep);
    RunStatus st = vm_run_chunk(vm, S->prog);
    vm_out_flush(vm);
    if (st == RS_EXIT) return vm->exit_code;
    if (st != RS_OK) {
        vm_print_uncaught(vm, 0);
        clear_error(vm);
        return -1;
    }
    if (replscope_echoed(S->rs)) {
        int slot = replscope_slot(S->rs, "_");
        if (slot >= 0 && vm->slots[slot].type != T_NIL && vm->slots[slot].type != T_UNINIT) {
            Out o = {0};
            o.sink = echo_sink;
            fmt_value(vm, vm->slots[slot], 0, 1, &o);
            (void)fputc('\n', stdout);
        }
    }
    return -1;
}

static int session_open(Session *S, const ReplOpts *o) {
    memset(S, 0, sizeof *S);
    Limits lim;
    limits_default(&lim);
    if (o->max_heap) lim.max_heap_bytes = o->max_heap;
    if (o->max_instr) lim.max_instructions = o->max_instr;
    int caps = CAP_STDIO_READ | CAP_STDIO_WRITE | (o->deny_clock ? 0 : CAP_CLOCK);
    if (o->allow_random || o->allow_all) caps |= CAP_RANDOM;
    if (o->allow_ffi) caps |= CAP_FFI;
    S->vm = vm_new(&lim, caps);
    S->vm->deterministic = o->deterministic;
    S->vm->no_quicken = o->no_quicken;
    for (int k = 0; k < o->nr; k++)
        if (lib_allow_dir(S->vm, 0, o->allow_r[k]) != 0) return 2;
    for (int k = 0; k < o->nw; k++)
        if (lib_allow_dir(S->vm, 1, o->allow_w[k]) != 0) return 2;
    if (o->allow_all) {
        if (lib_allow_dir(S->vm, 0, ".") != 0 || lib_allow_dir(S->vm, 1, ".") != 0) return 2;
    }
    S->prog = program_new();
    S->prog->hostvm = S->vm;
    (void)snprintf(S->prog->file, sizeof S->prog->file, "<repl>");
    S->rs = replscope_new();
    (void)session_eval(S, "let _ = nil\n", 12, 0);
    return 0;
}

static void session_close(Session *S) {
    vm_free(S->vm);
    program_free(S->prog);
    replscope_free(S->rs);
    for (int i = 0; i < S->narenas; i++) arena_free(S->arenas[i]);
    free(S->arenas);
}

static void print_help(void) {
    (void)puts("Sprint REPL. Enter a statement or an expression; a block ends with an empty line.\n"
               "  :help      this text\n"
               "  :names     list the names defined in this session\n"
               "  :reset     discard all definitions and start over\n"
               "  :quit      leave (also Ctrl-D)\n"
               "The value of the last expression is shown, and is also available as `_`.");
}

static void print_names(const Session *S) {
    int n = replscope_count(S->rs);
    for (int i = 0; i < n && i < S->vm->slots_have; i++) {
        const char *nm = replscope_name(S->rs, i);
        size_t len = replscope_name_len(S->rs, i);
        if (!nm || (len == 1 && nm[0] == '_')) continue;
        /* only the newest binding of a name */
        int newest = 1;
        for (int j = i + 1; j < n; j++) {
            const char *o = replscope_name(S->rs, j);
            if (o && replscope_name_len(S->rs, j) == len && memcmp(o, nm, len) == 0) { newest = 0; break; }
        }
        if (!newest) continue;
        SpValue v = S->vm->slots[i];
        const char *tn = replscope_is_import(S->rs, i) ? "module" : v.type == T_UNINIT ? "<uninitialised>" : type_name(v);
        (void)printf("%.*s: %s\n", (int)len, nm, tn);
    }
}

int tool_repl(int argc, char **argv, int *usage_error) {
    *usage_error = 0;
    ReplOpts o;
    memset(&o, 0, sizeof o);
    int i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        char *end;
        if (strcmp(argv[i], "--deterministic") == 0) o.deterministic = 1;
        else if (strcmp(argv[i], "--no-quicken") == 0) o.no_quicken = 1;
        else if (strcmp(argv[i], "--allow-random") == 0) o.allow_random = 1;
        else if (strcmp(argv[i], "--allow-ffi") == 0) o.allow_ffi = 1;
        else if (strcmp(argv[i], "--allow-all") == 0) o.allow_all = 1;
        else if (strcmp(argv[i], "--deny-clock") == 0) o.deny_clock = 1;
        else if (strcmp(argv[i], "--max-heap") == 0 && i + 1 < argc) { o.max_heap = strtoull(argv[++i], &end, 10); if (*end) { *usage_error = 1; return 2; } }
        else if (strcmp(argv[i], "--max-instr") == 0 && i + 1 < argc) { o.max_instr = strtoull(argv[++i], &end, 10); if (*end) { *usage_error = 1; return 2; } }
        else if (strncmp(argv[i], "--allow-read=", 13) == 0 && o.nr < MAX_ALLOW) o.allow_r[o.nr++] = argv[i] + 13;
        else if (strncmp(argv[i], "--allow-write=", 14) == 0 && o.nw < MAX_ALLOW) o.allow_w[o.nw++] = argv[i] + 14;
        else { *usage_error = 1; return 2; }
    }
    if (i != argc) { *usage_error = 1; return 2; }
    int tty = isatty(0);
    Session S;
    int rc = session_open(&S, &o);
    if (rc) {
        (void)fputs("sprint: cannot use an --allow-* directory\n", stderr);
        session_close(&S);
        return 2;
    }
    if (tty) (void)puts("Sprint 2.1 REPL  (:help for help, Ctrl-D to quit)");
    char *line = NULL;
    size_t cap = 0;
    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    int block = 0;
    int result = 0;
    for (;;) {
        if (tty) (void)fputs(blen ? "... " : ">>> ", stdout), (void)fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (blen && !is_blank(buf, blen)) {
                int r = session_eval(&S, buf, blen, 1);
                if (r >= 0) result = r;
            }
            if (tty) (void)fputc('\n', stdout);
            break;
        }
        size_t ln = (size_t)n;
        if (blen == 0) {
            if (is_blank(line, ln)) continue;
            /* meta commands */
            if (line[0] == ':') {
                char cmd[32];
                size_t k = 0;
                while (k + 1 < ln && k + 1 < sizeof cmd && line[k + 1] != '\n' && line[k + 1] != ' ') { cmd[k] = line[k + 1]; k++; }
                cmd[k] = 0;
                if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 || strcmp(cmd, "exit") == 0) break;
                if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) { print_help(); continue; }
                if (strcmp(cmd, "names") == 0) { print_names(&S); continue; }
                if (strcmp(cmd, "reset") == 0) {
                    session_close(&S);
                    if (session_open(&S, &o) != 0) { result = 2; break; }
                    continue;
                }
                (void)fprintf(stderr, "unknown command ':%s' (try :help)\n", cmd);
                continue;
            }
            block = is_block_opener(line, ln);
        }
        int blank = is_blank(line, ln);
        if (blen + ln + 1 > bcap) {
            bcap = (blen + ln + 1) * 2;
            buf = realloc(buf, bcap);
            if (!buf) exit(14);
        }
        memcpy(buf + blen, line, ln);
        blen += ln;
        if (blen && buf[blen - 1] != '\n') buf[blen++] = '\n';
        int more;
        if (blank) more = unfinished(buf, blen);              /* an empty line ends a block, but not an open bracket */
        else more = block || unfinished(buf, blen);
        if (more) continue;
        while (blen && is_blank(buf + blen - 1, 1)) blen--;
        buf[blen] = 0;
        int r = session_eval(&S, buf, blen, 1);
        blen = 0;
        block = 0;
        if (r >= 0) { result = r; break; }
    }
    free(line);
    free(buf);
    session_close(&S);
    return result;
}
