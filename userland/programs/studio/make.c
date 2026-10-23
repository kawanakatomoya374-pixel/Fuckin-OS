/* make.c - the `make` of C-OS Studio's terminal.
 *
 * Reads a Makefile and turns "make [target]" into an ordered list of commands
 * (MakeStep) for term.c to run - tcc.c-os compiles, ar, rm, mkdir, echo...
 * There is no shell on C-OS, so recipes are split into words and dispatched
 * by term.c instead of being handed to /bin/sh.
 *
 * Supported: NAME = / := / ?= / += , $(NAME) ${NAME}, $(NAME:.c=.o), automatic
 * variables $@ $< $^ $+ $*, pattern rules (%.o: %.c), .PHONY, include, prefix
 * characters @ - + on recipe lines, `;` and `&&` between commands, and the
 * functions wildcard patsubst subst notdir dir basename suffix addprefix
 * addsuffix strip filter filter-out sort firstword lastword word wordlist words
 * findstring abspath realpath if or and foreach call error warning info, a small
 * subset of $(shell) (pwd, echo, uname), conditionals (ifeq ifneq ifdef ifndef
 * else endif), define/endef, export/override, static pattern rules
 * (objs: %.o: %.c), $(@D) $(@F) $(<D) $(<F), $(CURDIR) and .DEFAULT_GOAL.
 * Built-in implicit rules: %.o from %.c, and %.c-os / % from %.c or %.o.
 * NOT supported (reported, not silently ignored): other $(shell ...) commands,
 * target-specific variables, $(eval).
 *
 * make_help() lists the documented targets of a Makefile (`target: ## text`),
 * which is what `make help` shows when the Makefile's own help recipe needs
 * grep/awk (there is no shell here to run it).
 *
 * "Out of date" is decided WITHOUT modification times, because C-OS's
 * cos_stat() does not report any. Instead each built target records a
 * signature - a hash of its recipe text and of the CONTENT of every file it
 * depends on - in ".cosmake" next to the Makefile. A target is rebuilt when
 * it does not exist, when its signature differs (a source or the recipe
 * changed), or when something it depends on was rebuilt in this run.
 */
#include "cosmake.h"
#include <stdarg.h>

/* ---------------------------------------------------------------- */
/* arena + string buffer                                             */
/* ---------------------------------------------------------------- */
static char *s_arena; static size_t s_used, s_cap;
static void arena_reset(void) { s_used = 0; }
static char *A(const char *s, size_t n) {
    if (!s_arena) { s_cap = 1u << 20; s_arena = (char *)malloc(s_cap); }
    if (!s_arena || s_used + n + 1 > s_cap) return NULL;
    char *p = s_arena + s_used;
    memcpy(p, s, n); p[n] = 0;
    s_used += n + 1;
    return p;
}
static char *As(const char *s) { return A(s, strlen(s)); }

typedef struct { char *p; size_t n, cap; } Buf;
static void b_put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; char *np = (char *)realloc(b->p, b->cap); if (!np) return; b->p = np; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void b_puts(Buf *b, const char *s) { b_put(b, s, strlen(s)); }
static void b_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---------------------------------------------------------------- */
/* model                                                             */
/* ---------------------------------------------------------------- */
#define MAX_VARS 128
#define MAX_RULES 192
#define MAX_DEPS 48
#define MAX_REC 32
typedef struct { char name[48]; char *val; bool lazy; } Var;
typedef struct { char *target; char *deps[MAX_DEPS]; int nd; char *order[8]; int no; char **rec; int *nrec; bool pat; } Rule;

static Var V[MAX_VARS]; static int NV;
static Rule R[MAX_RULES]; static int NRULE;
static char *PH[64]; static int NPH;
static char *DEFAULT_TARGET;
static char MKDIR[256];
static char s_err[200];
static int s_warn_n;
static void (*s_msg)(const char *);

typedef struct { const char *at, *lt, *hat, *plus, *stem; } Auto;

static void mk_err(const char *fmt, ...) {
    if (s_err[0]) return;
    va_list ap; va_start(ap, fmt); vsnprintf(s_err, sizeof s_err, fmt, ap); va_end(ap);
}

static Var *var_find(const char *name) { for (int i = 0; i < NV; ++i) if (!strcmp(V[i].name, name)) return &V[i]; return NULL; }
static void expand(const char *s, const Auto *au, Buf *out, int depth);
static char *expand_s(const char *s, const Auto *au);

static void var_set(const char *name, const char *val, bool lazy) {
    Var *v = var_find(name);
    if (!v) { if (NV >= MAX_VARS) return; v = &V[NV++]; snprintf(v->name, sizeof v->name, "%s", name); }
    v->val = As(val); v->lazy = lazy;
}

/* ---------------------------------------------------------------- */
/* word helpers                                                      */
/* ---------------------------------------------------------------- */
static int words(const char *s, char **w, int max) {           /* destructive on a copy the caller owns */
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
        if (!*s) break;
        const char *e = s; while (*e && *e != ' ' && *e != '\t' && *e != '\n') ++e;
        w[n++] = A(s, (size_t)(e - s));
        s = e;
    }
    return n;
}
static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t k = 0; while (s[k] == ' ' || s[k] == '\t') ++k;
    if (k) memmove(s, s + k, strlen(s + k) + 1);
}
/* % pattern: returns true and the stem if `s` matches `pat` */
static bool pat_match(const char *pat, const char *s, char *stem, size_t cap) {
    const char *pc = strchr(pat, '%');
    if (!pc) return !strcmp(pat, s);
    size_t pre = (size_t)(pc - pat), suf = strlen(pc + 1), sl = strlen(s);
    if (sl < pre + suf || strncmp(s, pat, pre) || strcmp(s + sl - suf, pc + 1)) return false;
    size_t n = sl - pre - suf; if (n >= cap) n = cap - 1;
    memcpy(stem, s + pre, n); stem[n] = 0;
    return true;
}
static void pat_subst(const char *pat, const char *rep, const char *s, Buf *out) {
    char stem[128];
    if (pat_match(pat, s, stem, sizeof stem) && strchr(pat, '%')) {
        const char *pc = strchr(rep, '%');
        if (pc) { b_put(out, rep, (size_t)(pc - rep)); b_puts(out, stem); b_puts(out, pc + 1); } else b_puts(out, rep);
    } else if (!strchr(pat, '%') && !strcmp(pat, s)) b_puts(out, rep);
    else b_puts(out, s);
}
static bool glob_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;; ++s) { if (glob_match(p + 1, s)) return true; if (!*s) return false; } }
    if (*s && (*p == '?' || *p == *s)) return glob_match(p + 1, s + 1);
    return false;
}
static void b_word(Buf *b, const char *w) { if (b->n) b_put(b, " ", 1); b_puts(b, w); }

static void abs_path(char *out, size_t cap, const char *name) {
    if (name[0] == '/') snprintf(out, cap, "%s", name); else path_join(out, cap, MKDIR, name);
}
static int cmp_s(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void fn_wildcard(const char *pattern, Buf *out) {
    char *w[32]; int n = words(pattern, w, 32);
    for (int i = 0; i < n; ++i) {
        char dirpart[256] = "", file[128];
        const char *sl = strrchr(w[i], '/');
        if (sl) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(sl - w[i]), w[i]); snprintf(file, sizeof file, "%s", sl + 1); } else snprintf(file, sizeof file, "%s", w[i]);
        if (!strpbrk(file, "*?")) { char p[300]; abs_path(p, sizeof p, w[i]); if (fs_exists(p)) b_word(out, w[i]); continue; }
        char d[300]; abs_path(d, sizeof d, dirpart[0] ? dirpart : ".");
        if (!dirpart[0]) snprintf(d, sizeof d, "%s", MKDIR);
        int fd = cos_opendir(d);
        if (fd < 0) continue;
        char *hit[128]; int nh = 0;
        cos_dirent_t de;
        while (nh < 128 && cos_readdir(fd, &de) == 1) {
            if (de.name[0] == '.' && file[0] != '.') continue;
            if (glob_match(file, de.name)) { char full[300]; if (dirpart[0]) snprintf(full, sizeof full, "%s/%s", dirpart, de.name); else snprintf(full, sizeof full, "%s", de.name); hit[nh++] = As(full); }
        }
        cos_close(fd);
        qsort(hit, (size_t)nh, sizeof(char *), cmp_s);
        for (int k = 0; k < nh; ++k) b_word(out, hit[k]);
    }
}

/* ---------------------------------------------------------------- */
/* expansion                                                         */
/* ---------------------------------------------------------------- */
static int split_args(const char *s, char **out, int max) {       /* split on top-level commas */
    int n = 0, depth = 0; const char *st = s;
    for (const char *p = s;; ++p) {
        if (*p == '(' || *p == '{') ++depth;
        else if (*p == ')' || *p == '}') --depth;
        if ((*p == ',' && depth == 0) || !*p) {
            if (n < max) { char *a = A(st, (size_t)(p - st)); if (a) { trim(a); out[n++] = a; } }
            st = p + 1;
            if (!*p) break;
        }
    }
    return n;
}

static void call_function(const char *name, const char *argstr, const Auto *au, Buf *out, int depth) {
    char *arg[4]; int na;
    Buf ex = {0};
    /* every function takes its arguments expanded */
    expand(argstr, au, &ex, depth + 1);
    na = split_args(ex.p ? ex.p : "", arg, 4);
    b_free(&ex);
    if (!strcmp(name, "wildcard")) { if (na) fn_wildcard(arg[0], out); return; }
    if (!strcmp(name, "patsubst") && na >= 3) { char *w[128]; int n = words(arg[2], w, 128); Buf t = {0}; for (int i = 0; i < n; ++i) { Buf one = {0}; pat_subst(arg[0], arg[1], w[i], &one); b_word(&t, one.p ? one.p : ""); b_free(&one); } if (t.p) b_puts(out, t.p); b_free(&t); return; }
    if (!strcmp(name, "subst") && na >= 3) {
        const char *s = arg[2]; size_t fl = strlen(arg[0]);
        if (!fl) { b_puts(out, s); return; }
        while (*s) { if (!strncmp(s, arg[0], fl)) { b_puts(out, arg[1]); s += fl; } else b_put(out, s++, 1); }
        return;
    }
    if (!strcmp(name, "error")) { mk_err("%s", na ? arg[0] : ""); return; }
    if (!strcmp(name, "warning") || !strcmp(name, "info")) { if (s_msg && na) { char m[200]; snprintf(m, sizeof m, "%s%s", name[0] == 'w' ? "make: warning: " : "", arg[0]); s_msg(m); } return; }
    if (!strcmp(name, "word") && na >= 2) { char *ww[128]; int nw = words(arg[1], ww, 128); int k = atoi(arg[0]); if (k >= 1 && k <= nw) b_puts(out, ww[k - 1]); return; }
    if (!strcmp(name, "wordlist") && na >= 3) { char *ww[128]; int nw = words(arg[2], ww, 128); int a = atoi(arg[0]), z = atoi(arg[1]); Buf t = {0}; for (int i = a < 1 ? 1 : a; i <= z && i <= nw; ++i) b_word(&t, ww[i - 1]); if (t.p) b_puts(out, t.p); b_free(&t); return; }
    if (!strcmp(name, "findstring") && na >= 2) { if (strstr(arg[1], arg[0])) b_puts(out, arg[0]); return; }
    if (na < 1) return;
    char *w[128]; int n = words(arg[0], w, 128);
    Buf t = {0};
    if (!strcmp(name, "lastword")) { if (n) b_word(&t, w[n - 1]); }
    else if (!strcmp(name, "abspath") || !strcmp(name, "realpath")) for (int i = 0; i < n; ++i) { char ap[300]; abs_path(ap, sizeof ap, w[i]); b_word(&t, ap); }
    else
    if (!strcmp(name, "notdir")) for (int i = 0; i < n; ++i) { const char *sl = strrchr(w[i], '/'); b_word(&t, sl ? sl + 1 : w[i]); }
    else if (!strcmp(name, "dir")) for (int i = 0; i < n; ++i) { const char *sl = strrchr(w[i], '/'); char d[256]; if (sl) snprintf(d, sizeof d, "%.*s/", (int)(sl - w[i]) , w[i]); else snprintf(d, sizeof d, "./"); b_word(&t, d); }
    else if (!strcmp(name, "basename")) for (int i = 0; i < n; ++i) { char b[256]; snprintf(b, sizeof b, "%s", w[i]); char *dot = strrchr(b, '.'), *sl = strrchr(b, '/'); if (dot && (!sl || dot > sl)) *dot = 0; b_word(&t, b); }
    else if (!strcmp(name, "suffix")) for (int i = 0; i < n; ++i) { const char *dot = strrchr(w[i], '.'), *sl = strrchr(w[i], '/'); if (dot && (!sl || dot > sl)) b_word(&t, dot); }
    else if (!strcmp(name, "addprefix") && na >= 2) { char *w2[128]; int n2 = words(arg[1], w2, 128); for (int i = 0; i < n2; ++i) { char x[300]; snprintf(x, sizeof x, "%s%s", arg[0], w2[i]); b_word(&t, x); } }
    else if (!strcmp(name, "addsuffix") && na >= 2) { char *w2[128]; int n2 = words(arg[1], w2, 128); for (int i = 0; i < n2; ++i) { char x[300]; snprintf(x, sizeof x, "%s%s", w2[i], arg[0]); b_word(&t, x); } }
    else if (!strcmp(name, "strip")) for (int i = 0; i < n; ++i) b_word(&t, w[i]);
    else if (!strcmp(name, "sort")) { qsort(w, (size_t)n, sizeof(char *), cmp_s); for (int i = 0; i < n; ++i) if (i == 0 || strcmp(w[i], w[i - 1])) b_word(&t, w[i]); }
    else if (!strcmp(name, "firstword")) { if (n) b_word(&t, w[0]); }
    else if (!strcmp(name, "words")) { char x[16]; snprintf(x, sizeof x, "%d", n); b_puts(&t, x); }
    else if ((!strcmp(name, "filter") || !strcmp(name, "filter-out")) && na >= 2) {
        bool inv = name[6] == '-';
        char *pw[16]; int np = words(arg[0], pw, 16); char *tw[128]; int nt = words(arg[1], tw, 128);
        for (int i = 0; i < nt; ++i) { bool m = false; char st[64]; for (int k = 0; k < np; ++k) if (pat_match(pw[k], tw[i], st, sizeof st)) m = true; if (m != inv) b_word(&t, tw[i]); }
    } else { s_warn_n++; if (s_msg) { char m[120]; snprintf(m, sizeof m, "make: warning: unsupported function '%s'", name); s_msg(m); } }
    if (t.p) b_puts(out, t.p);
    b_free(&t);
}

/* if/or/and/foreach/call take their arguments UNexpanded and expand only what they use */
static void call_lazy(const char *name, const char *argstr, const Auto *au, Buf *out, int depth) {
    char *arg[4]; int na = split_args(argstr, arg, 4);
    if (!strcmp(name, "if")) {
        if (!na) return;
        char *c = expand_s(arg[0], au); trim(c);
        if (c[0]) { if (na > 1) expand(arg[1], au, out, depth + 1); }
        else if (na > 2) expand(arg[2], au, out, depth + 1);
        return;
    }
    if (!strcmp(name, "or") || !strcmp(name, "and")) {
        bool isor = name[0] == 'o'; char *last = (char *)"";
        for (int i = 0; i < na; ++i) {
            last = expand_s(arg[i], au); trim(last);
            if (isor && last[0]) { b_puts(out, last); return; }
            if (!isor && !last[0]) return;
        }
        if (!isor) b_puts(out, last);
        return;
    }
    if (!strcmp(name, "foreach") && na >= 3) {
        char *vn = expand_s(arg[0], au); trim(vn);
        char *lst = expand_s(arg[1], au);
        char *w[128]; int n = words(lst, w, 128);
        Var *old = var_find(vn); char *ov = old ? old->val : NULL; bool ol = old ? old->lazy : false;
        Buf t = {0};
        for (int i = 0; i < n; ++i) {
            var_set(vn, w[i], false);
            Buf one = {0}; expand(arg[2], au, &one, depth + 1);
            if (one.p && one.p[0]) b_word(&t, one.p);
            b_free(&one);
        }
        var_set(vn, ov ? ov : "", ol);
        if (t.p) b_puts(out, t.p);
        b_free(&t);
        return;
    }
    if (!strcmp(name, "call") && na >= 1) {
        char *fn = expand_s(arg[0], au); trim(fn);
        char nm[2] = { 0, 0 };
        /* "call NAME,a,b": the args are everything after the first top-level comma, split again at up to 9 */
        const char *rest = argstr; int d = 0;
        for (; *rest; ++rest) { if (*rest == '(' || *rest == '{') ++d; else if (*rest == ')' || *rest == '}') --d; else if (*rest == ',' && !d) { ++rest; break; } }
        char *a2raw[9], *a2[9]; int n2 = *rest ? split_args(rest, a2raw, 9) : 0;
        for (int i = 0; i < n2; ++i) a2[i] = expand_s(a2raw[i], au);
        char *oldv[9]; bool oldl[9], had[9];
        for (int i = 0; i < 9; ++i) { nm[0] = (char)('1' + i); Var *o = var_find(nm); had[i] = o != NULL; oldv[i] = o ? o->val : NULL; oldl[i] = o ? o->lazy : false; if (i < n2) var_set(nm, a2[i], false); }
        Var *f = var_find(fn);
        if (f) { if (f->lazy) expand(f->val, au, out, depth + 1); else b_puts(out, f->val); }
        for (int i = 0; i < 9; ++i) { nm[0] = (char)('1' + i); if (i < n2 || had[i]) var_set(nm, had[i] ? oldv[i] : "", oldl[i]); }
        return;
    }
}

/* $(shell ...) cannot run a shell, but the few commands Makefiles use for paths can be answered */
static void fn_shell(const char *argstr, const Auto *au, Buf *out, int depth) {
    Buf ex = {0}; expand(argstr, au, &ex, depth + 1);
    char *cmd = A(ex.p ? ex.p : "", ex.n); b_free(&ex);
    if (!cmd) return;
    char *redir = strstr(cmd, "2>"); if (redir) *redir = 0;
    trim(cmd);
    if (!strcmp(cmd, "pwd")) { b_puts(out, MKDIR); return; }
    if (!strncmp(cmd, "echo ", 5) || !strcmp(cmd, "echo")) {
        char *a = cmd + 4; while (*a == ' ') ++a;
        if (!strncmp(a, "-n ", 3)) a += 3;
        char *w[64]; int n = words(a, w, 64);
        Buf t = {0}; for (int i = 0; i < n; ++i) { size_t l = strlen(w[i]); if (l >= 2 && (w[i][0] == '"' || w[i][0] == '\'') && w[i][l - 1] == w[i][0]) { w[i][l - 1] = 0; ++w[i]; } b_word(&t, w[i]); }
        if (t.p) b_puts(out, t.p);
        b_free(&t); return;
    }
    if (!strncmp(cmd, "uname", 5)) { b_puts(out, strstr(cmd, "-m") ? "x86_64" : "C-OS"); return; }
    mk_err("$(shell %.40s) is not supported: C-OS has no shell (only pwd, echo and uname are answered)", cmd);
}

static const char *FUNCS[] = { "wildcard", "patsubst", "subst", "notdir", "dir", "basename", "suffix", "addprefix", "addsuffix", "strip", "filter", "filter-out", "sort", "firstword", "lastword", "word", "wordlist", "words", "findstring", "abspath", "realpath",
    "if", "or", "and", "foreach", "call", "error", "warning", "info", "shell", NULL };

static void expand(const char *s, const Auto *au, Buf *out, int depth) {
    if (depth > 12) { mk_err("recursive variable reference"); return; }
    for (const char *p = s; *p;) {
        if (*p != '$') { b_put(out, p++, 1); continue; }
        ++p;
        if (*p == '$') { b_put(out, "$", 1); ++p; continue; }
        if (*p == '(' || *p == '{') {
            char close = *p == '(' ? ')' : '}', open = *p;
            const char *q = p + 1; int d = 1;
            while (*q && d) { if (*q == open) ++d; else if (*q == close) --d; if (d) ++q; }
            if (d) { mk_err("unterminated variable reference"); return; }
            char *body = A(p + 1, (size_t)(q - p - 1));
            p = q + 1;
            if (!body) return;
            /* function call? */
            char *sp = strchr(body, ' ');
            if (sp) {
                *sp = 0;
                bool isfn = false; for (int i = 0; FUNCS[i]; ++i) if (!strcmp(FUNCS[i], body)) isfn = true;
                if (isfn) {
                    if (!strcmp(body, "shell")) { fn_shell(sp + 1, au, out, depth); if (s_err[0]) return; continue; }
                    if (!strcmp(body, "if") || !strcmp(body, "or") || !strcmp(body, "and") || !strcmp(body, "foreach") || !strcmp(body, "call")) { call_lazy(body, sp + 1, au, out, depth); continue; }
                    call_function(body, sp + 1, au, out, depth); if (s_err[0]) return; continue;
                }
                *sp = ' ';
            }
            /* substitution reference $(VAR:.c=.o) */
            char *colon = strchr(body, ':'), *eq = colon ? strchr(colon, '=') : NULL;
            char subfrom[64] = "", subto[64] = "";
            if (colon && eq) { *colon = 0; snprintf(subfrom, sizeof subfrom, "%.*s", (int)(eq - colon - 1), colon + 1); snprintf(subto, sizeof subto, "%s", eq + 1); }
            Buf val = {0};
            if (strlen(body) == 2 && strchr("@<^*", body[0]) && (body[1] == 'D' || body[1] == 'F')) {
                const char *src = !au ? NULL : body[0] == '@' ? au->at : body[0] == '<' ? au->lt : body[0] == '^' ? au->hat : au->stem;
                char one[300] = ""; if (src) { snprintf(one, sizeof one, "%s", src); char *sp2 = strchr(one, ' '); if (sp2) *sp2 = 0; }
                const char *sl = strrchr(one, '/');
                if (body[1] == 'F') b_puts(&val, sl ? sl + 1 : one);
                else if (sl) b_put(&val, one, (size_t)(sl - one) ? (size_t)(sl - one) : 1); else b_puts(&val, ".");
            }
            else if (body[0] == '@' && !body[1]) { if (au && au->at) b_puts(&val, au->at); }
            else if (body[0] == '<' && !body[1]) { if (au && au->lt) b_puts(&val, au->lt); }
            else if (body[0] == '^' && !body[1]) { if (au && au->hat) b_puts(&val, au->hat); }
            else if (body[0] == '+' && !body[1]) { if (au && au->plus) b_puts(&val, au->plus); }
            else if (body[0] == '*' && !body[1]) { if (au && au->stem) b_puts(&val, au->stem); }
            else { Var *v = var_find(body); if (v) { if (v->lazy) expand(v->val, au, &val, depth + 1); else b_puts(&val, v->val); } }
            if (subfrom[0] || subto[0]) {
                char *w[128]; int n = words(val.p ? val.p : "", w, 128);
                for (int i = 0; i < n; ++i) {
                    size_t wl = strlen(w[i]), fl = strlen(subfrom);
                    Buf one = {0};
                    if (wl >= fl && !strcmp(w[i] + wl - fl, subfrom)) { b_put(&one, w[i], wl - fl); b_puts(&one, subto); } else b_puts(&one, w[i]);
                    b_word(out, one.p ? one.p : ""); b_free(&one);
                }
            } else if (val.p) b_puts(out, val.p);
            b_free(&val);
            continue;
        }
        /* single-character variable: $@ $< $^ $+ $* or $X */
        char c = *p++;
        if (!c) break;
        const char *v = NULL;
        if (au) v = c == '@' ? au->at : c == '<' ? au->lt : c == '^' ? au->hat : c == '+' ? au->plus : c == '*' ? au->stem : NULL;
        if (v) b_puts(out, v);
        else { char nm[2] = { c, 0 }; Var *var = var_find(nm); if (var) { if (var->lazy) expand(var->val, au, out, depth + 1); else b_puts(out, var->val); } }
    }
}
static char *expand_s(const char *s, const Auto *au) {
    Buf b = {0};
    expand(s, au, &b, 0);
    char *r = A(b.p ? b.p : "", b.n);
    b_free(&b);
    return r ? r : (char *)"";
}

/* ---------------------------------------------------------------- */
/* parsing                                                           */
/* ---------------------------------------------------------------- */
static char **s_cur_rec[MAX_RULES]; static int s_cur_n;      /* rules that share the recipe being read */
static void parse_text(char *text, int depth);

static void add_phony(const char *list) { char *w[32]; int n = words(list, w, 32); for (int i = 0; i < n && NPH < 64; ++i) PH[NPH++] = w[i]; }
static bool is_phony(const char *t) { for (int i = 0; i < NPH; ++i) if (!strcmp(PH[i], t)) return true; return false; }

static char OVN[16][48]; static int NOVN;                      /* VAR=value given on the command line: the Makefile cannot change them */
static bool is_cmdline_var(const char *n) { for (int i = 0; i < NOVN; ++i) if (!strcmp(OVN[i], n)) return true; return false; }
static void handle_assignment(char *line, char *eq, const Auto *none) {
    (void)none;
    int op = 0;                                   /* 0 '=', 1 ':=', 2 '?=', 3 '+=' */
    char *nameend = eq;
    if (eq > line && (eq[-1] == ':' )) { op = 1; nameend = eq - 1; if (nameend > line && nameend[-1] == ':') --nameend; }
    else if (eq > line && eq[-1] == '?') { op = 2; nameend = eq - 1; }
    else if (eq > line && eq[-1] == '+') { op = 3; nameend = eq - 1; }
    char name[48]; size_t nl = (size_t)(nameend - line); if (nl >= sizeof name) nl = sizeof name - 1;
    memcpy(name, line, nl); name[nl] = 0; trim(name);
    char *val = eq + 1; while (*val == ' ' || *val == '\t') ++val;
    if (is_cmdline_var(name)) return;
    Var *old = var_find(name);
    if (op == 2 && old) return;
    if (op == 1) { char *x = expand_s(val, NULL); var_set(name, x, false); }
    else if (op == 3 && old) { Buf b = {0}; b_puts(&b, old->val); b_puts(&b, " "); if (old->lazy) b_puts(&b, val); else { char *x = expand_s(val, NULL); b_puts(&b, x); } var_set(name, b.p, old->lazy); b_free(&b); }
    else var_set(name, val, true);
}

static bool s_group_append;                                   /* static pattern rules: one recipe, one group per target */
static void add_rule_group(char *targets_s, char *deps_s, bool *first_default) {
    char *tw[16]; int nt = words(targets_s, tw, 16);
    char *dw[MAX_DEPS + 8]; int nd = 0, no = 0; char *ow[8];
    char *bar = strchr(deps_s, '|');
    if (bar) { *bar = 0; no = words(bar + 1, ow, 8); }
    nd = words(deps_s, dw, MAX_DEPS);
    static char *rec_store[MAX_RULES][MAX_REC]; static int rec_n[MAX_RULES];
    if (!s_group_append) s_cur_n = 0;
    for (int i = 0; i < nt && NRULE < MAX_RULES && s_cur_n < MAX_RULES; ++i) {
        Rule *r = &R[NRULE];
        memset(r, 0, sizeof *r);
        r->target = tw[i];
        for (int k = 0; k < nd; ++k) r->deps[r->nd++] = dw[k];
        for (int k = 0; k < no; ++k) r->order[r->no++] = ow[k];
        r->pat = strchr(tw[i], '%') != NULL;
        rec_n[NRULE] = 0;
        r->rec = rec_store[NRULE]; r->nrec = &rec_n[NRULE];
        s_cur_rec[s_cur_n++] = (char **)(intptr_t)NRULE;         /* remember indices, not pointers */
        if (*first_default && !r->pat && tw[i][0] != '.') { DEFAULT_TARGET = tw[i]; *first_default = false; }
        ++NRULE;
    }
}

/* ---- conditionals: ifeq ifneq ifdef ifndef else endif ---- */
#define MAX_COND 16
static struct { bool parent, active, taken; } CS[MAX_COND]; static int ncond;
static bool cond_on(void) { return ncond == 0 || CS[ncond - 1].active; }
static bool kw_is(const char *line, const char *kw) {
    size_t n = strlen(kw);
    return !strncmp(line, kw, n) && (!line[n] || line[n] == ' ' || line[n] == '\t' || line[n] == '(' || line[n] == '"' || line[n] == '\'');
}
/* the two operands of ifeq/ifneq: (a,b)  "a" "b"  'a' 'b' */
static bool cond_operands(const char *p, char **a, char **b) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '(') {
        const char *q = p + 1; int d = 0, comma = -1;
        for (; *q; ++q) { if (*q == '(') ++d; else if (*q == ')') { if (!d) break; --d; } else if (*q == ',' && !d && comma < 0) comma = (int)(q - (p + 1)); }
        if (!*q || comma < 0) return false;
        *a = A(p + 1, (size_t)comma); *b = A(p + 1 + comma + 1, (size_t)(q - (p + 1 + comma + 1)));
    } else if (*p == '"' || *p == '\'') {
        char q1 = *p; const char *e = strchr(p + 1, q1); if (!e) return false;
        *a = A(p + 1, (size_t)(e - p - 1));
        p = e + 1; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '"' && *p != '\'') return false;
        char q2 = *p; const char *e2 = strchr(p + 1, q2); if (!e2) return false;
        *b = A(p + 1, (size_t)(e2 - p - 1));
    } else return false;
    if (!*a || !*b) return false;
    *a = expand_s(*a, NULL); *b = expand_s(*b, NULL); trim(*a); trim(*b);
    return true;
}
/* 1 / 0 for a conditional directive, -1 when the line is not one */
static int cond_eval(const char *line) {
    if (kw_is(line, "ifeq") || kw_is(line, "ifneq")) {
        bool neg = line[2] == 'n'; char *a, *b;
        if (!cond_operands(line + (neg ? 5 : 4), &a, &b)) { mk_err("bad %s syntax", neg ? "ifneq" : "ifeq"); return 0; }
        return (!strcmp(a, b)) != neg;
    }
    if (kw_is(line, "ifdef") || kw_is(line, "ifndef")) {
        bool neg = line[2] == 'n'; const char *p = line + (neg ? 6 : 5);
        char *nm = expand_s(p, NULL); trim(nm);
        Var *v = var_find(nm);
        bool def = v && v->val && v->val[0];
        return def != neg;
    }
    return -1;
}

static void parse_text(char *text, int depth) {
    static bool first_default;
    static Buf defbuf; static char defname[48]; static bool in_def, def_skip;
    if (depth == 0) { first_default = true; ncond = 0; in_def = false; }
    Buf logical = {0};
    /* split on '\n' by hand: empty lines must not vanish (strtok would merge them) */
    char *pos = text;
    while (*pos) {
        char *ln = pos; char *nl = strchr(pos, '\n');
        if (nl) { *nl = 0; pos = nl + 1; } else pos += strlen(pos);
        size_t n = strlen(ln); while (n && ln[n - 1] == '\r') ln[--n] = 0;
        if (in_def) {
            char *t = ln; while (*t == ' ' || *t == '\t') ++t;
            if (!strncmp(t, "endef", 5)) { if (!def_skip && defbuf.p) { if (defbuf.n && defbuf.p[defbuf.n - 1] == '\n') defbuf.p[--defbuf.n] = 0; var_set(defname, defbuf.p, true); } b_free(&defbuf); in_def = false; }
            else if (!def_skip) { b_puts(&defbuf, ln); b_puts(&defbuf, "\n"); }
            continue;
        }
        /* line continuation */
        if (n && ln[n - 1] == '\\' && ln[0] != '\t') { ln[n - 1] = ' '; b_puts(&logical, ln); continue; }
        char *line = ln;
        if (logical.n) { b_puts(&logical, ln); line = logical.p; }
        if (line[0] == '\t') {                                        /* recipe line */
            if (s_cur_n && cond_on()) {
                for (int i = 0; i < s_cur_n; ++i) {
                    int ri = (int)(intptr_t)s_cur_rec[i];
                    if (*R[ri].nrec < MAX_REC) R[ri].rec[(*R[ri].nrec)++] = As(line + 1);
                }
            }
            b_free(&logical); continue;
        }
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        trim(line);
        if (!line[0]) { b_free(&logical); continue; }
        /* conditionals are processed even in a skipped branch, to keep the nesting right */
        int cv = cond_eval(line);
        if (cv >= 0) {
            if (ncond >= MAX_COND) { mk_err("conditionals nested too deeply"); b_free(&logical); return; }
            bool par = cond_on();
            CS[ncond].parent = par; CS[ncond].active = par && cv; CS[ncond].taken = par && cv; ++ncond;
            b_free(&logical); continue;
        }
        if (kw_is(line, "else")) {
            if (!ncond) { mk_err("extraneous 'else'"); b_free(&logical); return; }
            char *rest = line + 4; while (*rest == ' ' || *rest == '\t') ++rest;
            int c = rest[0] ? cond_eval(rest) : 1;
            if (c < 0) { mk_err("bad 'else' syntax"); b_free(&logical); return; }
            if (!CS[ncond - 1].parent || CS[ncond - 1].taken) CS[ncond - 1].active = false;
            else { CS[ncond - 1].active = c; if (c) CS[ncond - 1].taken = true; }
            b_free(&logical); continue;
        }
        if (kw_is(line, "endif")) { if (!ncond) { mk_err("extraneous 'endif'"); b_free(&logical); return; } --ncond; b_free(&logical); continue; }
        if (kw_is(line, "define")) {
            char *nm = line + 6; while (*nm == ' ' || *nm == '\t') ++nm;
            char tmp[48]; snprintf(tmp, sizeof tmp, "%.47s", nm);
            size_t tl = strlen(tmp); while (tl && (tmp[tl - 1] == '=' || tmp[tl - 1] == ':' || tmp[tl - 1] == '?' || tmp[tl - 1] == '+' || tmp[tl - 1] == ' ')) tmp[--tl] = 0;
            snprintf(defname, sizeof defname, "%s", tmp); def_skip = !cond_on(); in_def = true; b_free(&defbuf);
            b_free(&logical); continue;
        }
        if (!cond_on()) { b_free(&logical); continue; }
        while (kw_is(line, "export") || kw_is(line, "override") || kw_is(line, "unexport")) {
            line += line[0] == 'u' ? 8 : line[0] == 'e' ? 6 : 8; while (*line == ' ' || *line == '\t') ++line;
        }
        if (!line[0]) { b_free(&logical); continue; }
        if (!strncmp(line, "include ", 8) || !strncmp(line, "-include ", 9) || !strncmp(line, "sinclude ", 9)) {
            bool soft = line[0] != 'i';
            char *files = expand_s(line + (line[0] == 'i' ? 8 : 9), NULL);
            char *w[8]; int nf = words(files, w, 8);
            for (int i = 0; i < nf && depth < 3; ++i) {
                char p[300]; abs_path(p, sizeof p, w[i]);
                size_t len; char *t = fs_read_all(p, &len);
                if (t) { parse_text(t, depth + 1); free(t); } else if (!soft) mk_err("%s: No such file (include)", w[i]);
            }
            b_free(&logical); continue;
        }
        if (!strncmp(line, ".PHONY", 6) && strchr(line, ':')) { add_phony(expand_s(strchr(line, ':') + 1, NULL)); b_free(&logical); continue; }
        if (!strncmp(line, "$(error", 7) || !strncmp(line, "$(warning", 9) || !strncmp(line, "$(info", 6)) { expand_s(line, NULL); b_free(&logical); continue; }
        char *eq = strchr(line, '='), *colon = strchr(line, ':');
        bool is_assign = eq && (!colon || eq < colon || (colon + 1 == eq) || (colon + 2 == eq && colon[1] == ':'));
        if (colon && colon[1] == '=' ) is_assign = true;
        if (is_assign) { handle_assignment(line, eq, NULL); s_cur_n = 0; b_free(&logical); continue; }
        if (colon && eq && eq > colon) {                               /* target: VAR = value */
            char *pv = colon + 1; while (*pv == ' ' || *pv == '\t') ++pv;
            char *e2 = pv; while (*e2 && *e2 != ' ' && *e2 != '\t' && *e2 != '=' && *e2 != ':' && *e2 != '+' && *e2 != '?') ++e2;
            while (*e2 == ' ' || *e2 == '\t') ++e2;
            if ((*e2 == '=') || ((*e2 == '+' || *e2 == '?' || *e2 == ':') && e2[1] == '=')) {
                s_warn_n++; s_cur_n = 0;
                if (s_msg) { char m[200]; snprintf(m, sizeof m, "make: warning: target-specific variable ignored: %.60s", line); s_msg(m); }
                b_free(&logical); continue;
            }
        }
        if (colon) {
            char *ex = expand_s(line, NULL);
            char *c1 = strchr(ex, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c1 && c2 && c2 != c1 + 1) {                              /* static pattern rule:  targets: %.o: %.c */
                *c1 = 0; *c2 = 0;
                char *tw[64]; int nt = words(ex, tw, 64);
                char *pat = A(c1 + 1, strlen(c1 + 1)), *dpat = A(c2 + 1, strlen(c2 + 1)); trim(pat); trim(dpat);
                s_cur_n = 0; s_group_append = true;
                for (int i = 0; i < nt; ++i) {
                    char stem[128];
                    if (!pat_match(pat, tw[i], stem, sizeof stem)) { mk_err("target '%s' doesn't match the target pattern '%s'", tw[i], pat); break; }
                    Buf dd = {0}; char *dw[MAX_DEPS]; int nd2 = words(dpat, dw, MAX_DEPS);
                    for (int k = 0; k < nd2; ++k) { const char *pc = strchr(dw[k], '%'); if (pc) { char x[200]; snprintf(x, sizeof x, "%.*s%s%s", (int)(pc - dw[k]), dw[k], stem, pc + 1); b_word(&dd, x); } else b_word(&dd, dw[k]); }
                    char *d1 = A(dd.p ? dd.p : "", dd.n); b_free(&dd);
                    add_rule_group(tw[i], d1, &first_default);
                }
                s_group_append = false;
                b_free(&logical); continue;
            }
            *colon = 0;
            char *targets = expand_s(line, NULL), *deps = expand_s(colon + 1, NULL);
            add_rule_group(targets, deps, &first_default);
            b_free(&logical); continue;
        }
        b_free(&logical);
    }
    b_free(&logical);
}

/* ---------------------------------------------------------------- */
/* hashing + stamps                                                  */
/* ---------------------------------------------------------------- */
static uint64_t fnv(uint64_t h, const void *d, size_t n) { const unsigned char *p = (const unsigned char *)d; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static bool file_hash(const char *abs, uint64_t *h, uint64_t *size) {
    cos_stat_t st;
    if (cos_stat(abs, &st) != 0 || st.is_dir) return false;
    size_t n; char *d = fs_read_all(abs, &n);
    if (!d && st.size) return false;
    *h = fnv(14695981039346656037ull, d ? d : "", n); *size = n;
    free(d);
    return true;
}

typedef struct { char target[128]; uint64_t sig; } Stamp;
static Stamp STAMPS[256]; static int NSTAMP;
static void stamps_load(void) {
    NSTAMP = 0;
    char p[300]; path_join(p, sizeof p, MKDIR, ".cosmake");
    size_t n; char *t = fs_read_all(p, &n);
    if (!t) return;
    for (char *ln = strtok(t, "\n"); ln && NSTAMP < 256; ln = strtok(NULL, "\n")) {
        char *tab = strchr(ln, '\t'); if (!tab) continue;
        *tab = 0;
        snprintf(STAMPS[NSTAMP].target, sizeof STAMPS[0].target, "%s", ln);
        STAMPS[NSTAMP].sig = strtoull(tab + 1, NULL, 16);
        ++NSTAMP;
    }
    free(t);
}
static void stamps_save(void) {
    Buf b = {0};
    for (int i = 0; i < NSTAMP; ++i) { char l[200]; snprintf(l, sizeof l, "%s\t%llx\n", STAMPS[i].target, (unsigned long long)STAMPS[i].sig); b_puts(&b, l); }
    char p[300]; path_join(p, sizeof p, MKDIR, ".cosmake");
    fs_write_all(p, b.p ? b.p : "", b.n);
    b_free(&b);
}
static Stamp *stamp_find(const char *t) { for (int i = 0; i < NSTAMP; ++i) if (!strcmp(STAMPS[i].target, t)) return &STAMPS[i]; return NULL; }

/* ---------------------------------------------------------------- */
/* planning                                                          */
/* ---------------------------------------------------------------- */
static MakePlan *P;
static bool s_always;
void make_set_always(bool on) { s_always = on; }
enum { ST_NONE, ST_BUSY, ST_DONE_UP, ST_DONE_BUILT };
static struct { char name[128]; int st; } SEEN[256]; static int NSEEN;
/* signature of every target planned in this run, so a target that depends on another target
 * that is about to be built hashes that target's SIGNATURE (known now) instead of its file
 * (which may not exist yet, and would make the first plan differ from every later one). */
static struct { char name[128]; uint64_t sig; } TSIG[256]; static int NTSIG;
static void tsig_set(const char *n, uint64_t sig) {
    for (int i = 0; i < NTSIG; ++i) if (!strcmp(TSIG[i].name, n)) { TSIG[i].sig = sig; return; }
    if (NTSIG < 256) { snprintf(TSIG[NTSIG].name, sizeof TSIG[0].name, "%s", n); TSIG[NTSIG++].sig = sig; }
}
static bool tsig_get(const char *n, uint64_t *sig) { for (int i = 0; i < NTSIG; ++i) if (!strcmp(TSIG[i].name, n)) { *sig = TSIG[i].sig; return true; } return false; }
static int seen_get(const char *n) { for (int i = 0; i < NSEEN; ++i) if (!strcmp(SEEN[i].name, n)) return SEEN[i].st; return ST_NONE; }
static void seen_set(const char *n, int st) {
    for (int i = 0; i < NSEEN; ++i) if (!strcmp(SEEN[i].name, n)) { SEEN[i].st = st; return; }
    if (NSEEN < 256) { snprintf(SEEN[NSEEN].name, sizeof SEEN[0].name, "%s", n); SEEN[NSEEN++].st = st; }
}

/* the C-OS compilers all mean tcc.c-os */
static bool is_cc_word(const char *w) { return !strcmp(w, "tcc") || !strcmp(w, "cc") || !strcmp(w, "gcc") || !strcmp(w, "clang") || !strcmp(w, "tcc.c-os") || !strcmp(w, "cos-cc"); }

static void split_command_line(const char *cmd, char **argv, int *argc, int max) {
    /* words with quote handling */
    *argc = 0;
    const char *p = cmd;
    while (*p && *argc < max - 1) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        Buf w = {0};
        char q = 0;
        for (; *p && (q || (*p != ' ' && *p != '\t')); ++p) {
            if (q) { if (*p == q) q = 0; else b_put(&w, p, 1); }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '\\' && p[1] && (p[1] == ' ' || p[1] == '"')) { ++p; b_put(&w, p, 1); }
            else b_put(&w, p, 1);
        }
        argv[(*argc)++] = A(w.p ? w.p : "", w.n);
        b_free(&w);
    }
    argv[*argc] = NULL;
}

static void emit_step(const char *show, char **argv, int argc, const char *output, bool silent, bool ignore, const char *sigtarget, uint64_t sig, bool last) {
    if (P->n >= P->cap) {
        int nc = P->cap ? P->cap * 2 : 32;
        MakeStep *ns = (MakeStep *)realloc(P->steps, (size_t)nc * sizeof(MakeStep));
        if (!ns) { mk_err("out of memory"); return; }
        P->steps = ns; P->cap = nc;
    }
    MakeStep *s = &P->steps[P->n++];
    memset(s, 0, sizeof *s);
    snprintf(s->show, sizeof s->show, "%s", show);
    s->argc = argc > MAKE_MAXARGV - 1 ? MAKE_MAXARGV - 1 : argc;
    for (int i = 0; i < s->argc; ++i) s->argv[i] = strdup(argv[i]);
    if (output) snprintf(s->output, sizeof s->output, "%s", output);
    s->silent = silent; s->ignore_err = ignore;
    if (sigtarget) snprintf(s->sigtarget, sizeof s->sigtarget, "%s", sigtarget);
    s->sig = sig; s->last_of_target = last;
}

static Rule *find_rule(const char *t, bool want_recipe) {
    Rule *best = NULL;
    for (int i = 0; i < NRULE; ++i) if (!R[i].pat && !strcmp(R[i].target, t)) { if (*R[i].nrec) return &R[i]; if (!want_recipe && !best) best = &R[i]; }
    return best;
}

/* built-in implicit rules, expressed as ordinary rules so they go through one code path */
static const char *IMPLICIT[][3] = {
    { "%.o", "%.c", "$(CC) $(CFLAGS) -c $< -o $@" },
    { "%.c-os", "%.c", "$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@" },
    { "%.c-os", "%.o", "$(CC) $(LDFLAGS) $^ -o $@" },
};

static int build_target(const char *t, const char *needed_by);

static int run_rule(const char *t, const char *stem, char **deps, int nd, char **order, int no, char **rec, int nrec, bool phony, const char *needed_by) {
    (void)needed_by;
    Auto au = { t, NULL, NULL, NULL, stem };
    bool rebuilt = false;
    Buf hat = {0}, plus = {0};
    for (int i = 0; i < nd; ++i) {
        int r = build_target(deps[i], t);
        if (r < 0) { b_free(&hat); b_free(&plus); return -1; }
        if (r == 1) rebuilt = true;
        b_word(&plus, deps[i]);
        bool dup = false; { char *w[128]; int n = words(hat.p ? hat.p : "", w, 128); for (int k = 0; k < n; ++k) if (!strcmp(w[k], deps[i])) dup = true; }
        if (!dup) b_word(&hat, deps[i]);
    }
    for (int i = 0; i < no; ++i) if (build_target(order[i], t) < 0) { b_free(&hat); b_free(&plus); return -1; }
    au.lt = nd ? deps[0] : ""; au.hat = hat.p ? hat.p : ""; au.plus = plus.p ? plus.p : "";

    /* expanded recipe text + content hash of every real-file dependency = the signature */
    uint64_t sig = 14695981039346656037ull;
    char *lines[MAX_REC];
    for (int i = 0; i < nrec; ++i) { lines[i] = expand_s(rec[i], &au); sig = fnv(sig, lines[i], strlen(lines[i])); }
    for (int i = 0; i < nd; ++i) {
        char ap[300]; abs_path(ap, sizeof ap, deps[i]);
        uint64_t h, sz, ds;
        if (is_phony(deps[i])) continue;
        if (tsig_get(deps[i], &ds)) { sig = fnv(sig, deps[i], strlen(deps[i])); sig = fnv(sig, &ds, sizeof ds); }      /* a built target: its signature */
        else if (file_hash(ap, &h, &sz)) { sig = fnv(sig, deps[i], strlen(deps[i])); sig = fnv(sig, &h, sizeof h); sig = fnv(sig, &sz, sizeof sz); }   /* a source: its content */
    }
    tsig_set(t, sig);
    char tap[300]; abs_path(tap, sizeof tap, t);
    Stamp *stp = stamp_find(t);
    bool need = s_always || phony || rebuilt || !fs_exists(tap) || !stp || stp->sig != sig;
    if (!nrec) {                                             /* nothing to run: a bare `all: a b` is fine, a missing file is not */
        if (!fs_exists(tap) && !phony && nd == 0) { mk_err("Don't know how to make '%s'%s%s", t, needed_by ? ", needed by " : "", needed_by ? needed_by : ""); b_free(&hat); b_free(&plus); return -1; }
        b_free(&hat); b_free(&plus);
        return rebuilt ? 1 : 0;
    }
    if (!need) { b_free(&hat); b_free(&plus); return 0; }

    for (int i = 0; i < nrec; ++i) {
        char *line = lines[i];
        bool silent = false, ignore = false;
        while (*line == '@' || *line == '-' || *line == '+' || *line == ' ') { if (*line == '@') silent = true; else if (*line == '-') ignore = true; ++line; }
        /* split into commands at `;` and `&&` (outside quotes) */
        char *cmds[8]; int ncmd = 0; Buf cur = {0}; char q = 0;
        for (const char *p = line;; ++p) {
            bool split = false;
            if (!*p) split = true;
            else if (!q && *p == ';') split = true;
            else if (!q && p[0] == '&' && p[1] == '&') { split = true; ++p; }
            else { if (q) { if (*p == q) q = 0; } else if (*p == '"' || *p == '\'') q = *p; b_put(&cur, p, 1); }
            if (split) { if (cur.n && ncmd < 8) { char *c = A(cur.p, cur.n); trim(c); if (c[0]) cmds[ncmd++] = c; } b_free(&cur); if (!*p) break; }
        }
        b_free(&cur);
        for (int c = 0; c < ncmd; ++c) {
            char *argv[MAKE_MAXARGV]; int argc;
            split_command_line(cmds[c], argv, &argc, MAKE_MAXARGV);
            if (!argc) continue;
            /* find -o so the output can be delivered to deliverables/ */
            const char *output = NULL;
            for (int k = 0; k + 1 < argc; ++k) if (!strcmp(argv[k], "-o")) output = argv[k + 1];
            bool last = (i == nrec - 1) && (c == ncmd - 1);
            emit_step(cmds[c], argv, argc, output, silent, ignore, last ? t : NULL, sig, last);
        }
    }
    b_free(&hat); b_free(&plus);
    return 1;
}

static int build_target(const char *t, const char *needed_by) {
    int st = seen_get(t);
    if (st == ST_BUSY) { mk_err("Circular dependency dropped: %s", t); return -1; }
    if (st == ST_DONE_UP) return 0;
    if (st == ST_DONE_BUILT) return 1;
    seen_set(t, ST_BUSY);
    int r = -1;
    bool phony = is_phony(t);
    Rule *er = find_rule(t, false);          /* an explicit rule; `all: a b` with no recipe counts */
    if (er) {
        /* merge dependencies that other recipe-less rules for the same target add (GNU make semantics) */
        char *deps[MAX_DEPS * 2]; int nd = 0;
        for (int i = 0; i < NRULE; ++i) if (!R[i].pat && !strcmp(R[i].target, t)) for (int k = 0; k < R[i].nd && nd < MAX_DEPS * 2; ++k) deps[nd++] = R[i].deps[k];
        r = run_rule(t, NULL, deps, nd, er->order, er->no, er->rec, *er->nrec, phony, needed_by);
    } else {
        /* user pattern rules, then the built-in ones */
        char stem[128] = "";
        bool done = false;
        for (int pass = 0; pass < 2 && !done; ++pass) {
            int count = pass == 0 ? NRULE : (int)(sizeof IMPLICIT / sizeof IMPLICIT[0]);
            for (int i = 0; i < count && !done; ++i) {
                const char *pt; char **rec; int nrec;
                char *pdeps[MAX_DEPS]; int npd = 0;
                char *recbuf[1];
                if (pass == 0) {
                    Rule *pr = &R[i];
                    if (!pr->pat || !*pr->nrec) continue;
                    pt = pr->target; rec = pr->rec; nrec = *pr->nrec;
                    for (int k = 0; k < pr->nd; ++k) pdeps[npd++] = pr->deps[k];       /* ALL prerequisites, not just the first */
                } else {
                    pt = IMPLICIT[i][0]; recbuf[0] = (char *)IMPLICIT[i][2]; rec = recbuf; nrec = 1;
                    pdeps[npd++] = (char *)IMPLICIT[i][1];
                }
                if (!pat_match(pt, t, stem, sizeof stem)) continue;
                /* substitute the stem into every prerequisite; the first one is the "source" and must be there */
                char *deps[MAX_DEPS]; int nd = 0; bool ok = true;
                for (int k = 0; k < npd; ++k) {
                    char dep[200];
                    const char *pc = strchr(pdeps[k], '%');
                    if (pc) snprintf(dep, sizeof dep, "%.*s%s%s", (int)(pc - pdeps[k]), pdeps[k], stem, pc + 1); else snprintf(dep, sizeof dep, "%s", pdeps[k]);
                    char dap[300]; abs_path(dap, sizeof dap, dep);
                    if (k == 0 && !fs_exists(dap) && !find_rule(dep, false)) { ok = false; break; }   /* no source -> this rule does not apply */
                    deps[nd++] = As(dep);
                }
                if (!ok) continue;
                r = run_rule(t, stem, deps, nd, NULL, 0, rec, nrec, false, needed_by);
                done = true;
            }
        }
        if (!done) {
            char ap[300]; abs_path(ap, sizeof ap, t);
            if (fs_exists(ap)) r = 0;                                          /* a plain source file */
            else { mk_err("No rule to make target '%s'%s%s", t, needed_by ? ", needed by " : "", needed_by ? needed_by : ""); r = -1; }
        }
    }
    seen_set(t, r == 1 ? ST_DONE_BUILT : ST_DONE_UP);
    return r;
}

/* ---------------------------------------------------------------- */
/* public                                                            */
/* ---------------------------------------------------------------- */
static int help_kind_text(char *text);
bool make_plan(const char *dir, const char *makefile, const char *target, const char *const *overrides, int nov,
               MakePlan *plan, char *err, size_t errcap, void (*msg)(const char *)) {
    memset(plan, 0, sizeof *plan);
    arena_reset();
    NV = NRULE = NPH = NSEEN = NTSIG = 0; DEFAULT_TARGET = NULL; s_err[0] = 0; s_warn_n = 0; s_msg = msg;
    snprintf(MKDIR, sizeof MKDIR, "%s", dir);
    P = plan;
    /* defaults that make C-OS Makefiles work without any boilerplate */
    var_set("CC", "tcc", false); var_set("AR", "ar", false); var_set("RM", "rm -f", false);
    var_set("CFLAGS", "", false); var_set("LDFLAGS", "", false); var_set("MAKE", "make", false);
    var_set("CURDIR", dir, false); var_set("MAKEFILE_LIST", makefile, false); var_set("MAKECMDGOALS", target ? target : "", false);

    char mp[300]; path_join(mp, sizeof mp, dir, makefile);
    size_t n; char *text = fs_read_all(mp, &n);
    if (!text) { snprintf(err, errcap, "cannot read %s", makefile); return false; }
    s_cur_n = 0;
    NOVN = 0;
    for (int i = 0; i < nov; ++i) {                                  /* command-line VAR=value beat the Makefile, also in its conditionals */
        char *eq = strchr(overrides[i], '='); if (!eq || NOVN >= 16) continue;
        snprintf(OVN[NOVN], sizeof OVN[0], "%.*s", (int)(eq - overrides[i] > 47 ? 47 : eq - overrides[i]), overrides[i]);
        var_set(OVN[NOVN], eq + 1, false); ++NOVN;
    }
    parse_text(text, 0);
    free(text);
    if (s_err[0]) { snprintf(err, errcap, "%s", s_err); return false; }

    Var *dg = var_find(".DEFAULT_GOAL");
    if (dg && dg->val[0]) { char *g = expand_s(dg->val, NULL); trim(g); if (g[0]) DEFAULT_TARGET = g; }
    const char *goal = target && target[0] ? target : DEFAULT_TARGET;
    if (!goal) { snprintf(err, errcap, "No targets in %s", makefile); return false; }
    snprintf(plan->goal, sizeof plan->goal, "%s", goal);
    if (!strcmp(goal, "help") && make_help_kind(dir, makefile) != 1) { plan->builtin_help = true; plan->up_to_date = true; return true; }
    stamps_load();
    int r = build_target(goal, NULL);
    if (r < 0 || s_err[0]) { snprintf(err, errcap, "%s", s_err[0] ? s_err : "make failed"); make_plan_free(plan); return false; }
    plan->up_to_date = plan->n == 0;
    return true;
}

void make_plan_free(MakePlan *p) {
    for (int i = 0; i < p->n; ++i) for (int k = 0; k < p->steps[i].argc; ++k) free(p->steps[i].argv[k]);
    free(p->steps);
    memset(p, 0, sizeof *p);
}

/* a target's recipe finished successfully: remember its signature */
void make_commit_step(const MakeStep *s) {
    if (!s->last_of_target || !s->sigtarget[0]) return;
    Stamp *st = stamp_find(s->sigtarget);
    if (!st) { if (NSTAMP >= 256) return; st = &STAMPS[NSTAMP++]; snprintf(st->target, sizeof st->target, "%s", s->sigtarget); }
    st->sig = s->sig;
    stamps_save();
}

/* A caller that runs plans from several directories (nested `make -C sub`) must say which one a
 * commit / forget belongs to: the signatures live in <dir>/.cosmake. */
void make_use_dir(const char *dir) {
    if (!strcmp(MKDIR, dir)) return;
    snprintf(MKDIR, sizeof MKDIR, "%s", dir);
    stamps_load();
}

/* `make clean` and friends delete files: forget their signatures */
void make_forget(const char *target) {
    for (int i = 0; i < NSTAMP; ++i) if (!strcmp(STAMPS[i].target, target)) { STAMPS[i] = STAMPS[--NSTAMP]; stamps_save(); return; }
}

/* ---------------------------------------------------------------- */
/* make help                                                         */
/* ---------------------------------------------------------------- */
/* Everything here reads the Makefile as text, line by line, without expanding it: documentation is
 * what a person wrote next to a target, and a listing must work even when the Makefile uses a
 * construct the planner does not support. */

/* does the unquoted part of a recipe line contain a pipe, redirect or backtick? */
static bool needs_shell(const char *c) {
    char q = 0;
    for (; *c; ++c) {
        if (q) { if (*c == q) q = 0; continue; }
        if (*c == '"' || *c == '\'') q = *c;
        else if (*c == '|' || *c == '>' || *c == '<' || *c == '`') return true;
    }
    return false;
}

/* 0 no help rule, 1 recipe uses only echo/printf/true, 2 anything else */
static int help_kind_text(char *text) {
    int kind = 0; bool in_help = false; int nrec = 0; bool bad = false;
    for (char *pos = text; *pos;) {
        char *ln = pos, *nl = strchr(pos, '\n');
        if (nl) { *nl = 0; pos = nl + 1; } else pos += strlen(pos);
        size_t n = strlen(ln); while (n && ln[n - 1] == '\r') ln[--n] = 0;
        if (ln[0] == '\t') {
            if (!in_help) continue;
            char *c = ln + 1;
            while (*c == '@' || *c == '-' || *c == '+' || *c == ' ' || *c == '\t') ++c;
            ++nrec;
            char first[16]; size_t k = 0; while (c[k] && c[k] != ' ' && c[k] != '\t' && k < sizeof first - 1) { first[k] = c[k]; ++k; } first[k] = 0;
            if (needs_shell(c) || !(!strcmp(first, "echo") || !strcmp(first, "printf") || !strcmp(first, "true") || !strcmp(first, ":"))) bad = true;
            continue;
        }
        char *t = ln; while (*t == ' ') ++t;
        if (!*t || *t == '#') continue;
        in_help = false;
        if (!strncmp(t, "help", 4)) { const char *r = t + 4; while (*r == ' ' || *r == '\t') ++r; if (*r == ':' && r[1] != '=') { in_help = true; kind = 1; } }
    }
    if (kind == 1 && (bad || nrec == 0)) kind = 2;
    return kind;
}
int make_help_kind(const char *dir, const char *makefile) {
    char mp[300]; path_join(mp, sizeof mp, dir, makefile);
    size_t n; char *text = fs_read_all(mp, &n);
    if (!text) return 0;
    int k = help_kind_text(text);
    free(text);
    return k;
}

typedef struct { char name[64]; char doc[140]; char deps[100]; int sect; bool phony; } HTarget;
typedef struct { char name[40]; char val[48]; char doc[120]; } HVar;
#define HMAX 160
#define HSECT 16

bool make_help(const char *dir, const char *makefile, void (*out)(const char *line)) {
    char mp[300]; path_join(mp, sizeof mp, dir, makefile);
    size_t n; char *text = fs_read_all(mp, &n);
    if (!text) return false;
    HTarget *T = (HTarget *)calloc(HMAX, sizeof *T); HVar *Vs = (HVar *)calloc(64, sizeof *Vs);
    char (*sects)[64] = (char (*)[64])calloc(HSECT, 64);
    char *phony = (char *)calloc(1, 2048);
    if (!T || !Vs || !sects || !phony) { free(T); free(Vs); free(sects); free(phony); free(text); return false; }
    int nt = 0, nv = 0, ns = 0, cur_sect = -1;
    char pending[140] = ""; bool in_block = false;
    char dflt[64] = "", dgoal[64] = "";
    char line[400];
    for (char *pos = text; *pos;) {
        char *ln = pos, *nl = strchr(pos, '\n');
        if (nl) { *nl = 0; pos = nl + 1; } else pos += strlen(pos);
        snprintf(line, sizeof line, "%s", ln);
        size_t ll = strlen(line); while (ll && (line[ll - 1] == '\r' || line[ll - 1] == ' ' || line[ll - 1] == '\t')) line[--ll] = 0;
        if (line[0] == '\t') continue;
        const char *t = line; while (*t == ' ') ++t;
        if (!*t) { pending[0] = 0; in_block = false; continue; }
        if (t[0] == '#') {
            if (t[1] == '#' && t[2] == '@') {                                   /* ##@ Section */
                const char *s2 = t + 3; while (*s2 == ' ') ++s2;
                if (*s2 && ns < HSECT) { snprintf(sects[ns], 64, "%s", s2); cur_sect = ns++; }
                pending[0] = 0; in_block = false; continue;
            }
            const char *c = t; while (*c == '#') ++c; while (*c == ' ' || *c == '\t') ++c;
            if (!*c || !strncmp(c, "---", 3) || !strncmp(c, "===", 3)) { in_block = false; continue; }
            if (!in_block) { snprintf(pending, sizeof pending, "%s", c); in_block = true; }
            continue;
        }
        in_block = false;
        char doc[140] = ""; char body[400];
        const char *dd = strstr(t, "##");
        if (dd) { const char *d2 = dd + 2; while (*d2 == ' ' || *d2 == '#') ++d2; snprintf(doc, sizeof doc, "%s", d2); }
        snprintf(body, sizeof body, "%s", t);
        { char *h = strchr(body, '#'); if (h) *h = 0; }
        { size_t bl = strlen(body); while (bl && (body[bl - 1] == ' ' || body[bl - 1] == '\t')) body[--bl] = 0; }
        if (!body[0]) { pending[0] = 0; continue; }
        char *eq = strchr(body, '='), *colon = strchr(body, ':');
        bool is_assign = eq && (!colon || eq < colon || colon + 1 == eq);
        if (is_assign) {
            char *ne = eq; char op = 0;
            if (ne > body && (ne[-1] == ':' || ne[-1] == '?' || ne[-1] == '+')) { op = ne[-1]; --ne; if (op == ':' && ne > body && ne[-1] == ':') --ne; }
            char nm[40]; size_t nl2 = (size_t)(ne - body); if (nl2 >= sizeof nm) nl2 = sizeof nm - 1; memcpy(nm, body, nl2); nm[nl2] = 0; trim(nm);
            const char *v = eq + 1; while (*v == ' ' || *v == '\t') ++v;
            if (!strcmp(nm, ".DEFAULT_GOAL")) snprintf(dgoal, sizeof dgoal, "%s", v);
            bool user = nm[0] && nm[0] != '.' && !strchr(nm, ' ') && !strchr(nm, '$');
            if (user && (op == '?' || doc[0] || pending[0]) && nv < 64) {
                bool dup = false; for (int i = 0; i < nv; ++i) if (!strcmp(Vs[i].name, nm)) dup = true;
                if (!dup) { snprintf(Vs[nv].name, sizeof Vs[nv].name, "%s", nm); snprintf(Vs[nv].val, sizeof Vs[nv].val, "%s", v); snprintf(Vs[nv].doc, sizeof Vs[nv].doc, "%s", doc[0] ? doc : pending); ++nv; }
            }
            pending[0] = 0; continue;
        }
        if (!colon) { pending[0] = 0; continue; }
        if (!strncmp(body, ".PHONY", 6)) { size_t pl = strlen(phony); const char *r = colon + 1; if (pl + strlen(r) + 2 < 2048) { phony[pl] = ' '; strcpy(phony + pl + 1, r); } pending[0] = 0; continue; }
        if (body[0] == '.' ) { pending[0] = 0; continue; }
        char tg[300]; snprintf(tg, sizeof tg, "%.*s", (int)(colon - body), body);
        if (colon[1] == ':') ++colon;
        const char *dp = colon + 1; while (*dp == ' ' || *dp == '\t') ++dp;
        if (strchr(tg, '%') || strchr(tg, '$') || strchr(dp, ':')) { pending[0] = 0; continue; }
        char deps[100]; snprintf(deps, sizeof deps, "%s", dp);
        { char *bar = strchr(deps, '|'); if (bar) *bar = 0; trim(deps); }
        for (char *save = NULL, *w = strtok_r(tg, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
            bool dup = false; for (int i = 0; i < nt; ++i) if (!strcmp(T[i].name, w)) { dup = true; if (!T[i].doc[0] && (doc[0] || pending[0])) snprintf(T[i].doc, sizeof T[i].doc, "%s", doc[0] ? doc : pending); if (!T[i].deps[0]) snprintf(T[i].deps, sizeof T[i].deps, "%s", deps); }
            if (dup || nt >= HMAX) continue;
            snprintf(T[nt].name, sizeof T[nt].name, "%s", w);
            snprintf(T[nt].doc, sizeof T[nt].doc, "%s", doc[0] ? doc : pending);
            snprintf(T[nt].deps, sizeof T[nt].deps, "%s", deps);
            T[nt].sect = cur_sect; ++nt;
            if (!dflt[0]) snprintf(dflt, sizeof dflt, "%s", w);
        }
        pending[0] = 0;
    }
    free(text);
    if (dgoal[0]) snprintf(dflt, sizeof dflt, "%s", dgoal);
    /* phony marks */
    { char *w[64]; int pn = 0; char *sv = NULL; for (char *p = strtok_r(phony, " \t", &sv); p && pn < 64; p = strtok_r(NULL, " \t", &sv)) w[pn++] = p;
      for (int i = 0; i < nt; ++i) for (int k = 0; k < pn; ++k) if (!strcmp(T[i].name, w[k])) T[i].phony = true; }

    char b[400];
    out("Usage: make [-f FILE] [-C DIR] [-n] [-B] [-s] [TARGET] [VAR=value ...]");
    snprintf(b, sizeof b, "Makefile: %s%s%s", makefile, dflt[0] ? "   default target: " : "", dflt); out(b);
    bool any = false;
    /* sections in order of appearance; -1 = before the first ##@ heading; documented first, then plain phony targets */
    for (int s2 = -1; s2 < ns; ++s2) {
        bool head = false;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < nt; ++i) {
                if (T[i].sect != s2) continue;
                bool documented = T[i].doc[0] != 0;
                bool plain = !documented && (T[i].phony || !strpbrk(T[i].name, "./") || !strcmp(T[i].name, dflt));
                if (!strcmp(T[i].name, "help") && !documented) { plain = false; }
                if (pass == 0 ? !documented : !plain) continue;
                if (!head) { out(""); snprintf(b, sizeof b, "%s:", s2 < 0 ? (ns ? "Targets" : "Targets") : sects[s2]); if (s2 < 0 && !any) {} out(b); head = true; }
                char dep[130] = "";
                if (!documented && T[i].deps[0]) snprintf(dep, sizeof dep, "(needs: %.90s)", T[i].deps);
                snprintf(b, sizeof b, "  %-12s - %s%s", T[i].name, documented ? T[i].doc : (dep[0] ? dep : "(no description)"), !strcmp(T[i].name, dflt) ? "  [default]" : "");
                out(b); any = true;
            }
        }
    }
    if (!any) { out(""); out("(no targets found in this Makefile)"); }
    if (nv) {
        out(""); out("Variables (override with  make VAR=value):");
        for (int i = 0; i < nv; ++i) { snprintf(b, sizeof b, "  %-12s - %s%s%s%s", Vs[i].name, Vs[i].doc, Vs[i].doc[0] ? "  " : "", Vs[i].val[0] ? "(default: " : "", Vs[i].val[0] ? Vs[i].val : ""); size_t bl = strlen(b); if (Vs[i].val[0] && bl + 2 < sizeof b) { b[bl] = ')'; b[bl + 1] = 0; } out(b); }
    }
    free(T); free(Vs); free(sects); free(phony);
    return true;
}
