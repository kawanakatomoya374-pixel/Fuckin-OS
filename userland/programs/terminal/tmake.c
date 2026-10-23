/* tmake.c - runs `make` and the compiler for the C-OS Terminal. See tmake.h.
 *
 * The plan comes from studio/make.c. A step is a command line; this file decides what it means:
 *   tcc cc gcc clang cos-cc   -> /bin/tcc.c-os   (TinyCC; "ar" becomes "tcc -ar")
 *   echo printf mkdir rm cp mv touch cat true false : pwd   -> done in-process
 *   make / $(MAKE)            -> a nested plan (make -C sub target)
 *   ./prog  /path/prog  x.c-os -> a program, with its output captured
 * Children get COS_STDOUT=<log>; the log is tailed from tm_tick(), so the window never blocks.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "cos.h"
#include "tmake.h"
#include "../studio/cosmake.h"

#define TM_LOG  "/tmp/terminal_job.log"
#define TM_TCC  "/bin/tcc.c-os"
#define MAXFR   4
#define MAXTGT  8
#define MAXOV   16

static tm_out_fn OUT;

/* ---------------------------------------------------------------- */
/* file system helpers make.c needs                                  */
/* ---------------------------------------------------------------- */
bool fs_exists(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0; }
bool fs_is_dir(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0 && st.is_dir; }
char *fs_read_all(const char *path, size_t *len) {
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.is_dir) return NULL;
    if (st.size > (64u << 20)) return NULL;
    char *b = (char *)malloc((size_t)st.size + 1);
    if (!b) return NULL;
    ssize_t n = st.size ? cos_read_file(path, b, (size_t)st.size) : 0;
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}
bool fs_write_all(const char *path, const void *data, size_t len) {
    if (len == 0) {                       /* cos_write_file() rejects an empty transfer: create/truncate by opening */
        int fd = cos_open(path, COS_O_WRONLY_CREATE);
        if (fd < 0) return false;
        cos_close(fd);
        return true;
    }
    return cos_write_file(path, data, len) >= 0;
}
void path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = strlen(a);
    if (n && a[n - 1] == '/') snprintf(out, cap, "%s%s", a, b);
    else snprintf(out, cap, "%s/%s", a, b);
}
const char *path_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
bool has_ext(const char *name, const char *ext) {
    size_t ln = strlen(name), le = strlen(ext);
    return ln > le && strcasecmp(name + ln - le, ext) == 0;
}

/* ---------------------------------------------------------------- */
/* output                                                            */
/* ---------------------------------------------------------------- */
static bool s_new_output;
static char *s_cap; static size_t s_capn, s_capcap; static bool s_capturing;   /* output of a built-in that has "> file" */
static void emit(const char *line) {
    if (s_capturing) {
        size_t l = strlen(line);
        if (s_capn + l + 2 > s_capcap) { size_t nc = (s_capn + l + 2) * 2; char *nb = (char *)realloc(s_cap, nc); if (!nb) return; s_cap = nb; s_capcap = nc; }
        memcpy(s_cap + s_capn, line, l); s_capn += l; s_cap[s_capn++] = '\n';
        return;
    }
    if (OUT) OUT(line);
    s_new_output = true;
}
static void outf(const char *fmt, ...) {
    char b[768];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    char *p = b;
    for (;;) { char *nl = strchr(p, '\n'); if (nl) *nl = 0; emit(p); if (!nl) break; p = nl + 1; if (!*p) break; }
}

/* ---------------------------------------------------------------- */
/* paths + words                                                     */
/* ---------------------------------------------------------------- */
static void norm_path(const char *in, char *out, size_t cap) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", in);
    char *parts[64]; int np = 0;
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".") || !t[0]) continue;
        if (!strcmp(t, "..")) { if (np) --np; continue; }
        if (np < 64) parts[np++] = t;
    }
    size_t k = 0;
    if (!np) { snprintf(out, cap, "/"); return; }
    for (int i = 0; i < np; ++i) k += (size_t)snprintf(out + k, cap - k, "/%s", parts[i]);
}
static void resolve(const char *dir, const char *arg, char *out, size_t cap) {
    char tmp[600];
    if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg); else snprintf(tmp, sizeof tmp, "%s/%s", dir, arg);
    norm_path(tmp, out, cap);
}
/* words with "double" and 'single' quotes */
int tm_tokenize(char *line, char **argv, int max) {
    int n = 0; char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char *w = p, *o = p; char q = 0;
        for (; *p && (q || (*p != ' ' && *p != '\t')); ++p) {
            if (q) { if (*p == q) q = 0; else *o++ = *p; }
            else if (*p == '"' || *p == '\'') q = *p;
            else *o++ = *p;
        }
        if (*p) ++p;
        *o = 0;
        argv[n++] = w;
    }
    argv[n] = NULL;
    return n;
}
bool tm_is_compiler(const char *w) {
    return !strcmp(w, "tcc") || !strcmp(w, "cc") || !strcmp(w, "gcc") || !strcmp(w, "clang") || !strcmp(w, "tcc.c-os") || !strcmp(w, "cos-cc");
}

static bool glob_ok(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;; ++s) { if (glob_ok(p + 1, s)) return true; if (!*s) return false; } }
    if (*s && (*p == '?' || *p == *s || ((*p | 32) == (*s | 32) && (*p | 32) >= 'a' && (*p | 32) <= 'z'))) return glob_ok(p + 1, s + 1);
    return false;
}
/* `*.o` style arguments (no slash) against dir; a pattern with no match stays literal. Results are malloc'd. */
static int expand_globs(const char *dir, int argc, char **argv, char **out, int max) {
    int n = 0;
    for (int i = 0; i < argc && n < max; ++i) {
        if (!strpbrk(argv[i], "*?") || strchr(argv[i], '/')) { out[n++] = strdup(argv[i]); continue; }
        int fd = cos_opendir(dir), before = n;
        if (fd >= 0) {
            cos_dirent_t de;
            while (n < max && cos_readdir(fd, &de) == 1) if (de.name[0] != '.' && glob_ok(argv[i], de.name)) out[n++] = strdup(de.name);
            cos_close(fd);
        }
        if (n == before) out[n++] = strdup(argv[i]);
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* recipe built-ins                                                  */
/* ---------------------------------------------------------------- */
static bool copy_file(const char *from, const char *to) {
    size_t n; char *d = fs_read_all(from, &n);
    if (!d) return false;
    bool ok = fs_write_all(to, d, n);
    free(d);
    return ok;
}
static int rm_path(const char *p, bool recursive) {
    if (fs_is_dir(p)) {
        if (!recursive) { outf("rm: %s is a directory (use rm -r)", path_base(p)); return 1; }
        int fd = cos_opendir(p);
        if (fd >= 0) {
            char *names[256]; int n = 0; cos_dirent_t de;
            while (n < 256 && cos_readdir(fd, &de) == 1) { if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue; names[n++] = strdup(de.name); }
            cos_close(fd);
            for (int i = 0; i < n; ++i) { char c[300]; path_join(c, sizeof c, p, names[i]); rm_path(c, true); free(names[i]); }
        }
    }
    return cos_unlink(p) == 0 ? 0 : 1;
}
static void mkdir_p(const char *abs) {
    char acc[300] = ""; char tmp[300]; snprintf(tmp, sizeof tmp, "%s", abs);
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        size_t l = strlen(acc); snprintf(acc + l, sizeof acc - l, "/%s", t);
        if (!fs_is_dir(acc)) cos_mkdir(acc);
    }
}
/* printf FORMAT [ARGS]: %s %d %i %c %% and the escapes \n \t \\ \" */
static void do_printf(int argc, char **argv) {
    if (argc < 2) return;
    char buf[1024]; size_t k = 0; int ai = 2;
    for (const char *f = argv[1]; *f && k + 40 < sizeof buf; ++f) {
        if (*f == '\\' && f[1]) { ++f; buf[k++] = *f == 'n' ? '\n' : *f == 't' ? '\t' : *f; }
        else if (*f == '%' && f[1]) {
            ++f;
            if (*f == '%') buf[k++] = '%';
            else { const char *a = ai < argc ? argv[ai++] : ""; char t[64];
                   if (*f == 'd' || *f == 'i') snprintf(t, sizeof t, "%ld", atol(a)); else if (*f == 'c') snprintf(t, sizeof t, "%c", a[0]); else snprintf(t, sizeof t, "%.600s", a);
                   size_t l = strlen(t); if (k + l < sizeof buf - 2) { memcpy(buf + k, t, l); k += l; } }
        } else buf[k++] = *f;
    }
    buf[k] = 0;
    char *p = buf;
    for (;;) { char *nl = strchr(p, '\n'); if (nl) *nl = 0; if (nl || *p) emit(p); if (!nl) break; p = nl + 1; if (!*p) break; }
}
/* drop the redirections from a command: "> f", ">> f", ">f", "2>x", "2>&1", "&>x". Returns the new argc. */
static int split_redirect(int argc, char **argv, char **vout, const char **file, bool *append, bool *discard) {
    int n = 0; *file = NULL; *append = false; *discard = false;
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (!strncmp(a, "2>", 2) || !strncmp(a, "&>", 2)) { if (!a[2] || (!strcmp(a, "&>") )) ++i; if (!strncmp(a, "&>", 2)) *discard = true; continue; }
        if (a[0] == '>') {
            const char *f = a + 1; bool app = false;
            if (*f == '>') { app = true; ++f; }
            if (!*f) { if (i + 1 >= argc) continue; f = argv[++i]; }
            if (!strcmp(f, "/dev/null")) *discard = true; else { *file = f; *append = app; }
            continue;
        }
        vout[n++] = argv[i];
    }
    vout[n] = NULL;
    return n;
}
/* Runs a built-in against `dir`. Returns -1 when argv[0] is not one. */
static int run_builtin(const char *dir, int argc, char **argv) {
    const char *c = argv[0];
    if (!strcmp(c, "true") || !strcmp(c, ":")) return 0;
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "pwd")) { emit(dir); return 0; }
    if (!strcmp(c, "echo")) {
        char line[512] = ""; int i = 1;
        if (i < argc && !strcmp(argv[i], "-n")) ++i;
        for (; i < argc; ++i) { if (line[0] || i > 1) strlcat(line, " ", sizeof line); strlcat(line, argv[i], sizeof line); }
        size_t l = strlen(line); if (l && line[0] == ' ') memmove(line, line + 1, l);
        emit(line); return 0;
    }
    if (!strcmp(c, "printf")) { do_printf(argc, argv); return 0; }
    if (!strcmp(c, "mkdir")) {
        int rc = 0; bool pflag = false;
        for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "-p")) pflag = true;
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '-') continue;
            char p[300]; resolve(dir, argv[i], p, sizeof p);
            if (fs_is_dir(p)) continue;
            if (pflag) { mkdir_p(p); if (!fs_is_dir(p)) { outf("mkdir: cannot create %s", argv[i]); rc = 1; } }
            else if (cos_mkdir(p) != 0) { outf("mkdir: cannot create %s", argv[i]); rc = 1; }
        }
        return rc;
    }
    if (!strcmp(c, "touch")) {
        for (int i = 1; i < argc; ++i) { if (argv[i][0] == '-') continue; char p[300]; resolve(dir, argv[i], p, sizeof p); if (!fs_exists(p)) fs_write_all(p, "", 0); }
        return 0;
    }
    if (!strcmp(c, "cat")) {
        int rc = 0;
        for (int i = 1; i < argc; ++i) {
            char p[300]; resolve(dir, argv[i], p, sizeof p);
            size_t n; char *d = fs_read_all(p, &n);
            if (!d) { outf("cat: %s: No such file", argv[i]); rc = 1; continue; }
            char *s = d; for (;;) { char *nl = strchr(s, '\n'); if (nl) *nl = 0; if (nl || *s) emit(s); if (!nl) break; s = nl + 1; }
            free(d);
        }
        return rc;
    }
    if (!strcmp(c, "rm") || !strcmp(c, "del")) {
        bool rec = false, force = false; int rc = 0; char *files[64]; int nf = 0;
        for (int i = 1; i < argc && nf < 64; ++i) { if (argv[i][0] == '-') { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = true; if (strchr(argv[i], 'f')) force = true; } else files[nf++] = argv[i]; }
        char *ex[128]; int ne = expand_globs(dir, nf, files, ex, 128);
        for (int i = 0; i < ne; ++i) {
            char p[300]; resolve(dir, ex[i], p, sizeof p);
            if (!fs_exists(p)) { if (!force) { outf("rm: %s: No such file", ex[i]); rc = 1; } }
            else if (rm_path(p, rec) != 0) { outf("rm: cannot remove %s", ex[i]); rc = 1; }
            else { make_use_dir(dir); make_forget(path_base(p)); }
            free(ex[i]);
        }
        return rc;
    }
    if (!strcmp(c, "cp") || !strcmp(c, "copy") || !strcmp(c, "mv") || !strcmp(c, "move")) {
        bool mv = c[0] == 'm'; char *args[8]; int na = 0;
        for (int i = 1; i < argc && na < 8; ++i) if (argv[i][0] != '-') args[na++] = argv[i];
        if (na != 2) { outf("usage: %s SRC DST", c); return 1; }
        char from[300], to[300];
        resolve(dir, args[0], from, sizeof from); resolve(dir, args[1], to, sizeof to);
        if (!fs_exists(from)) { outf("%s: %s: No such file", c, args[0]); return 1; }
        if (fs_is_dir(to)) { char t2[600]; path_join(t2, sizeof t2, to, path_base(from)); snprintf(to, sizeof to, "%s", t2); }
        if (mv && cos_rename(from, to) == 0) return 0;
        if (fs_is_dir(from)) { outf("%s: %s is a directory", c, args[0]); return 1; }
        if (!copy_file(from, to)) { outf("%s: could not write %s", c, args[1]); return 1; }
        if (mv) cos_unlink(from);
        return 0;
    }
    if (!strcmp(c, "cd")) { outf("make: 'cd' does not carry over between commands here - use  make -C DIR  or a path"); return 1; }
    return -1;
}

/* ---------------------------------------------------------------- */
/* job state                                                         */
/* ---------------------------------------------------------------- */
typedef struct { MakePlan plan; int step; char dir[256]; } Frame;
static struct {
    bool active, is_make, stopping, dry, silent;
    int64_t pid; size_t tail; char acc[512]; int acc_len; uint64_t t0;
    Frame fr[MAXFR]; int nfr;
    char makefile[64]; char dir[256];
    char targets[MAXTGT][128]; int nt, ti;
    char ovb[MAXOV][96]; const char *ov[MAXOV]; int nov;
} J;
/* argument strings made for one spawn (absolute paths); freed before the next one */
static char *s_held[MAKE_MAXARGV]; static int s_nheld;
static void held_free(void) { for (int i = 0; i < s_nheld; ++i) free(s_held[i]); s_nheld = 0; }
static char *held(char *s) { if (s_nheld < MAKE_MAXARGV) s_held[s_nheld++] = s; return s; }

static const char *tcc_exe(void) { const char *o = cos_getenv("TERMINAL_TCC"); return o && o[0] ? o : TM_TCC; }

/* Turn the relative file arguments of a compiler command line into absolute paths: a program
 * started from here has no working directory of its own to resolve them in. */
static int abs_args(const char *dir, int argc, char **argv, char **out, int max) {
    int n = 0;
    bool ar = argc > 1 && !strcmp(argv[1], "-ar");
    for (int i = 0; i < argc && n < max - 1; ++i) {
        char *a = argv[i], buf[300], nb[300];
        if (i == 0) { out[n++] = a; continue; }
        bool prev_o = !strcmp(argv[i - 1], "-o") || !strcmp(argv[i - 1], "-include") || !strcmp(argv[i - 1], "-I") || !strcmp(argv[i - 1], "-L");
        if (a[0] == '-') {
            if ((!strncmp(a, "-I", 2) || !strncmp(a, "-L", 2)) && a[2] && a[2] != '/') { snprintf(buf, sizeof buf, "%.2s%s/%s", a, dir, a + 2); out[n++] = held(strdup(buf)); }
            else out[n++] = a;
            continue;
        }
        if (prev_o || (!ar && (strchr(a, '.') || strchr(a, '/'))) || (ar && (has_ext(a, ".a") || has_ext(a, ".o")))) {
            if (a[0] == '/') out[n++] = a; else { snprintf(buf, sizeof buf, "%s/%s", dir, a); norm_path(buf, nb, sizeof nb); out[n++] = held(strdup(nb)); }
        } else out[n++] = a;
    }
    out[n] = NULL;
    return n;
}

/* ---------------------------------------------------------------- */
/* output tail                                                       */
/* ---------------------------------------------------------------- */
static void job_line(const char *l) { emit(l); }
static void job_tail(void) {
    cos_stat_t st;
    if (cos_stat(TM_LOG, &st) != 0 || st.size <= J.tail) return;
    int fd = cos_open(TM_LOG, COS_O_RDONLY);
    if (fd < 0) return;
    cos_lseek(fd, (int64_t)J.tail);
    char chunk[1024];
    for (;;) {
        ssize_t n = cos_fd_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        J.tail += (size_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n' || J.acc_len >= (int)sizeof J.acc - 1) {
                J.acc[J.acc_len] = 0; job_line(J.acc); J.acc_len = 0;
                if (c != '\n') J.acc[J.acc_len++] = c;
            } else J.acc[J.acc_len++] = c;
        }
    }
    cos_close(fd);
}
static void job_flush(void) { if (J.acc_len) { J.acc[J.acc_len] = 0; job_line(J.acc); J.acc_len = 0; } }

static bool spawn_external(const char *exe, char **argv) {
    char env[96]; snprintf(env, sizeof env, "COS_STDOUT=%s", TM_LOG);
    const char *envp[] = { env, NULL };
    fs_write_all(TM_LOG, "", 0);
    J.tail = 0; J.acc_len = 0; J.t0 = cos_time_ms();
    J.pid = cos_spawn_argv(exe, (const char *const *)argv, envp);
    return J.pid > 0;
}

static void frames_free(void) { for (int i = 0; i < J.nfr; ++i) make_plan_free(&J.fr[i].plan); J.nfr = 0; }
static void job_end(void) {
    frames_free();
    held_free();
    J.active = false; J.is_make = false; J.pid = 0; J.stopping = false; J.nt = J.ti = 0;
}

/* ---------------------------------------------------------------- */
/* make                                                              */
/* ---------------------------------------------------------------- */
static void note(const char *m) { emit(m); }
static void make_advance(void);

/* plan `target` in `dir` and push it. 1 pushed, 0 nothing to run (help listed / up to date), -1 error (reported) */
static int begin_target(const char *dir, const char *makefile, const char *target) {
    if (J.nfr >= MAXFR) { outf("make: *** recursion too deep.  Stop."); return -1; }
    Frame *f = &J.fr[J.nfr];
    char err[200];
    if (!make_plan(dir, makefile, target, J.ov, J.nov, &f->plan, err, sizeof err, note)) { outf("make: *** %s.  Stop.", err); return -1; }
    if (f->plan.builtin_help) {
        make_help(dir, makefile, emit);
        make_plan_free(&f->plan); return 0;
    }
    if (f->plan.n == 0) { outf("make: '%s' is up to date.", f->plan.goal); make_plan_free(&f->plan); return 0; }
    snprintf(f->dir, sizeof f->dir, "%s", dir); f->step = 0;
    J.nfr++;
    return 1;
}

static void make_fail(int rc) {
    const char *goal = J.nfr ? J.fr[J.nfr - 1].plan.goal : "?";
    outf("make: *** [%s] Error %d", goal, rc);
    job_end();
}

/* options shared by `make` and a nested `$(MAKE)`: returns the directory / Makefile / targets */
static bool parse_make_args(int argc, char **argv, const char *basedir, char *dir, size_t dcap, char *mkfile, size_t mcap,
                            char targets[][128], int *nt, bool top) {
    snprintf(dir, dcap, "%s", basedir);
    mkfile[0] = 0; *nt = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc) snprintf(mkfile, mcap, "%s", argv[++i]);
        else if (!strcmp(a, "-C") && i + 1 < argc) {
            char p[300]; resolve(dir, argv[++i], p, sizeof p);
            if (!fs_is_dir(p)) { outf("make: *** %s: No such directory.  Stop.", argv[i]); return false; }
            snprintf(dir, dcap, "%s", p);
        }
        else if (a[0] == '-' && (a[1] == 'j' || a[1] == 'k' || a[1] == 'w' || a[1] == 'e' || a[1] == 'i')) { if (a[1] == 'j' && !a[2] && i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') ++i; }
        else if (top && a[0] == '-' && a[1] != '-') { for (const char *f = a + 1; *f; ++f) { if (*f == 'n') J.dry = true; else if (*f == 's') J.silent = true; else if (*f == 'B') make_set_always(true); } }
        else if (top && !strcmp(a, "--dry-run")) J.dry = true;
        else if (top && !strcmp(a, "--silent")) J.silent = true;
        else if (top && !strcmp(a, "--always-make")) make_set_always(true);
        else if (strchr(a, '=') && a[0] != '-') { if (J.nov < MAXOV) { snprintf(J.ovb[J.nov], sizeof J.ovb[0], "%s", a); J.ov[J.nov] = J.ovb[J.nov]; J.nov++; } }
        else if (a[0] != '-' && *nt < MAXTGT) snprintf(targets[(*nt)++], 128, "%s", a);
    }
    return true;
}

static void start_next_target(void) {
    while (J.ti < J.nt) {
        const char *t = J.targets[J.ti][0] ? J.targets[J.ti] : NULL;
        J.ti++;
        int r = begin_target(J.dir, J.makefile, t);
        if (r > 0) { make_advance(); return; }
        if (r < 0) break;                                          /* the error is reported; stop */
    }
    job_end();
}

/* a nested `make` step: push its plan(s) on top of the running one. false: error (reported) */
static bool nested_make(Frame *cur, MakeStep *s) {
    char dir[256], mk[64], tg[MAXTGT][128]; int nt = 0;
    int saved_nov = J.nov;
    if (!parse_make_args(s->argc, s->argv, cur->dir, dir, sizeof dir, mk, sizeof mk, tg, &nt, false)) return false;
    if (!mk[0]) { const char *f = tm_find_makefile(dir); if (!f) { outf("make: *** No Makefile found in %s.  Stop.", dir); return false; } snprintf(mk, sizeof mk, "%s", f); }
    outf("make: Entering directory '%s'", dir);
    cur->step++;
    bool ok = true;
    if (nt == 0) ok = begin_target(dir, mk, NULL) >= 0;
    else for (int i = nt - 1; i >= 0 && ok; --i) ok = begin_target(dir, mk, tg[i]) >= 0;     /* last first: the top frame runs first */
    J.nov = saved_nov;
    return ok;
}

/* run steps until one has to wait for a process (or all are done) */
static void make_advance(void) {
    while (J.nfr > 0) {
        Frame *f = &J.fr[J.nfr - 1];
        if (f->step >= f->plan.n) {
            if (J.nfr == 1) { if (!J.silent && !J.dry) outf("make: done (%s)", f->plan.goal); make_plan_free(&f->plan); J.nfr--; start_next_target(); return; }
            outf("make: Leaving directory '%s'", f->dir);
            make_plan_free(&f->plan); J.nfr--; continue;
        }
        MakeStep *s = &f->plan.steps[f->step];
        if (J.dry) { outf("%s", s->show); f->step++; continue; }
        if (!s->silent && !J.silent) outf("%s", s->show);
        char *v[MAKE_MAXARGV];
        int rc = 0, bi;
        held_free();
        char *rv[MAKE_MAXARGV]; const char *rfile; bool rapp, rdis;
        int rn = split_redirect(s->argc, s->argv, rv, &rfile, &rapp, &rdis);
        if ((rfile || rdis) && rn > 0 && !tm_is_compiler(rv[0]) && strcmp(rv[0], "make")) { s_capturing = true; s_capn = 0; }
        if (tm_is_compiler(s->argv[0]) || !strcmp(s->argv[0], "ar")) {
            char *sub[MAKE_MAXARGV]; int sn = 0;
            sub[sn++] = (char *)tcc_exe();
            if (!strcmp(s->argv[0], "ar")) sub[sn++] = "-ar";
            for (int i = 1; i < s->argc && sn < MAKE_MAXARGV - 1; ++i) sub[sn++] = s->argv[i];
            sub[sn] = NULL;
            abs_args(f->dir, sn, sub, v, MAKE_MAXARGV);
            if (!spawn_external(tcc_exe(), v)) { outf("make: cannot start the compiler (%s)", tcc_exe()); rc = 127; }
            else return;                                                   /* wait for it in tm_tick */
        } else if (!strcmp(s->argv[0], "make")) {
            if (!nested_make(f, s)) { job_end(); return; }
            continue;                                                      /* the frame's step was advanced; its plan(s) are on top */
        } else if ((bi = run_builtin(f->dir, s_capturing ? rn : s->argc, s_capturing ? rv : s->argv)) >= 0) {
            rc = bi;
            if (s_capturing) {
                s_capturing = false;
                if (rfile && !rdis) {
                    char fp[300]; resolve(f->dir, rfile, fp, sizeof fp);
                    size_t on = 0; char *old = rapp ? fs_read_all(fp, &on) : NULL;
                    char *all = (char *)malloc(on + s_capn + 1);
                    if (all) { if (old) memcpy(all, old, on); memcpy(all + on, s_cap ? s_cap : "", s_capn); if (!fs_write_all(fp, all, on + s_capn)) { outf("make: cannot write %s", rfile); rc = 1; } free(all); }
                    free(old);
                }
            }
        } else if (s->argv[0][0] == '.' || s->argv[0][0] == '/' || has_ext(s->argv[0], ".c-os")) {
            char p[300]; resolve(f->dir, s->argv[0], p, sizeof p);
            char *run[MAKE_MAXARGV]; int rn = 0; run[rn++] = p;
            for (int i = 1; i < s->argc && rn < MAKE_MAXARGV - 1; ++i) run[rn++] = s->argv[i];
            run[rn] = NULL;
            if (!spawn_external(p, run)) { outf("make: cannot run %s", s->argv[0]); rc = 127; } else return;
        } else { s_capturing = false; outf("make: %s: command not found", s->argv[0]); rc = 127; }
        s_capturing = false;
        if (rc != 0 && !s->ignore_err) { make_fail(rc); return; }
        if (rc != 0) outf("make: [%s] Error %d (ignored)", f->plan.goal, rc);
        make_use_dir(f->dir); make_commit_step(s);
        f->step++;
    }
    start_next_target();
}

const char *tm_find_makefile(const char *dir) {
    static const char *const names[] = { "Makefile", "makefile", "GNUmakefile", "Makefile.txt", NULL };
    char p[300];
    for (int i = 0; names[i]; ++i) { path_join(p, sizeof p, dir, names[i]); if (fs_exists(p) && !fs_is_dir(p)) return names[i]; }
    return NULL;
}

static void make_usage(void) {
    emit("make - build with a Makefile, using TinyCC");
    emit("  make                   build the default target of ./Makefile");
    emit("  make TARGET            build TARGET (several targets are built one after another)");
    emit("  make help              list the targets and variables of the Makefile");
    emit("  make VAR=value         set a variable (also inside the Makefile's ifeq tests)");
    emit("  make -f FILE | -C DIR  another Makefile / another directory");
    emit("  make -n                show the commands without running them");
    emit("  make -B                rebuild everything     make -s   do not echo the commands");
    emit("Compiler words in a recipe (tcc, cc, gcc, $(CC)) run /bin/tcc.c-os; there is no shell, so");
    emit("recipes may use echo printf mkdir rm cp mv touch cat, $(MAKE) -C DIR, the compiler and programs.");
}

void tm_make(int argc, char **argv, const char *cwd) {
    if (J.active) { emit("(a command is still running - Ctrl+C stops it)"); return; }
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { make_usage(); return; }
    memset(&J, 0, sizeof J);
    make_set_always(false);
    char targets[MAXTGT][128]; int nt = 0;
    if (!parse_make_args(argc, argv, cwd, J.dir, sizeof J.dir, J.makefile, sizeof J.makefile, targets, &nt, true)) return;
    if (!J.makefile[0]) {
        const char *f = tm_find_makefile(J.dir);
        if (!f) { outf(nt ? "make: *** No rule to make target '%s' (and no Makefile in %s).  Stop." : "make: *** No targets specified and no Makefile found in %s.  Stop.", nt ? targets[0] : J.dir, J.dir); return; }
        snprintf(J.makefile, sizeof J.makefile, "%s", f);
    } else { char mp[300]; resolve(J.dir, J.makefile, mp, sizeof mp); if (!fs_exists(mp)) { outf("make: *** %s: No such file.  Stop.", J.makefile); return; } }
    /* -l / --list are the same as `make help` */
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) { snprintf(targets[0], 128, "help"); nt = 1; }
    for (int i = 0; i < nt; ++i) snprintf(J.targets[i], 128, "%s", targets[i]);
    J.nt = nt ? nt : 1; J.ti = 0;                                          /* no target = the Makefile's default */
    if (!nt) J.targets[0][0] = 0;
    J.active = true; J.is_make = true; J.t0 = cos_time_ms();
    start_next_target();
}

/* ---------------------------------------------------------------- */
/* running things                                                    */
/* ---------------------------------------------------------------- */
bool tm_exec(int argc, char **argv, const char *cwd) {
    if (J.active) { emit("(a command is still running - Ctrl+C stops it)"); return true; }
    memset(&J, 0, sizeof J);
    char *v[MAKE_MAXARGV];
    held_free();
    if (tm_is_compiler(argv[0])) {
        char *sub[MAKE_MAXARGV]; int sn = 0;
        sub[sn++] = (char *)tcc_exe();
        for (int i = 1; i < argc && sn < MAKE_MAXARGV - 1; ++i) sub[sn++] = argv[i];
        sub[sn] = NULL;
        abs_args(cwd, sn, sub, v, MAKE_MAXARGV);
        if (!spawn_external(tcc_exe(), v)) { outf("tcc: cannot start the compiler (%s)", tcc_exe()); return true; }
        J.active = true; J.is_make = false;
        return true;
    }
    /* a program: ./name, /path/name, name.c-os, or a bare name in the current directory or /bin */
    char p[300];
    resolve(cwd, argv[0], p, sizeof p);
    bool found = fs_exists(p) && !fs_is_dir(p);
    if (!found && !has_ext(argv[0], ".c-os")) {
        char q[320]; snprintf(q, sizeof q, "%s.c-os", p);
        if (fs_exists(q) && !fs_is_dir(q)) { snprintf(p, sizeof p, "%s", q); found = true; }
    }
    if (!found && argv[0][0] != '/' && argv[0][0] != '.' && !strchr(argv[0], '/')) {
        char q[320]; snprintf(q, sizeof q, "/bin/%s", argv[0]);
        if (fs_exists(q) && !fs_is_dir(q)) { snprintf(p, sizeof p, "%s", q); found = true; }
        else { snprintf(q, sizeof q, "/bin/%s.c-os", argv[0]); if (fs_exists(q) && !fs_is_dir(q)) { snprintf(p, sizeof p, "%s", q); found = true; } }
    }
    if (!found) return false;
    char *run[MAKE_MAXARGV]; int rn = 0; run[rn++] = p;
    for (int i = 1; i < argc && rn < MAKE_MAXARGV - 1; ++i) run[rn++] = argv[i];
    run[rn] = NULL;
    if (!strncmp(p, "/bin/", 5) || !strncmp(p, "/apps/", 6)) {            /* system / sample apps: windows, so start them in the background */
        int64_t pid = cos_spawn_argv(p, (const char *const *)run, NULL);
        if (pid < 0) outf("%s: launch failed", argv[0]); else outf("Started %s (pid %lld)", p, (long long)pid);
        return true;
    }
    if (!spawn_external(p, run)) { outf("%s: cannot start", argv[0]); return true; }
    J.active = true; J.is_make = false;
    outf("[started %s, pid %d]  (Ctrl+C to stop)", path_base(p), (int)J.pid);
    return true;
}

bool tm_busy(void) { return J.active; }

bool tm_tick(void) {
    s_new_output = false;
    if (!J.active) return false;
    if (J.pid > 0) {
        job_tail();
        int status = 0;
        int64_t r = cos_waitpid(J.pid, &status, false);
        if (r != J.pid) return s_new_output;
        job_tail(); job_flush();
        J.pid = 0;
        if (J.stopping) { emit("[stopped]"); job_end(); return true; }
        if (J.is_make && J.nfr > 0) {
            Frame *f = &J.fr[J.nfr - 1];
            MakeStep *s = &f->plan.steps[f->step];
            if (status != 0 && !s->ignore_err) { make_fail(status); return true; }
            if (status != 0) outf("make: [%s] Error %d (ignored)", f->plan.goal, status);
            make_use_dir(f->dir); make_commit_step(s);
            f->step++;
            make_advance();
            return true;
        }
        uint64_t ms = cos_time_ms() - J.t0;
        outf("[exit code %d]  (%d.%02d s)", status, (int)(ms / 1000), (int)((ms % 1000) / 10));
        job_end();
        return true;
    }
    return s_new_output;
}

void tm_stop(void) {
    if (!J.active) return;
    J.stopping = true;
    if (J.pid > 0) {
        cos_kill((int)J.pid, 9);
        int st;
        for (int i = 0; i < 50; ++i) { if (cos_waitpid(J.pid, &st, false) == J.pid) break; cos_sleep_ms(4); }
        job_tail(); job_flush();
    }
    emit(J.is_make ? "make: interrupted" : "[stopped]");
    job_end();
}

/* ---------------------------------------------------------------- */
/* "Makefile found"                                                  */
/* ---------------------------------------------------------------- */
static int an_targets; static bool an_vars; static char an_default[64];
static void an_line(const char *l) {
    if (!strncmp(l, "Makefile:", 9)) { const char *d = strstr(l, "default target: "); if (d) snprintf(an_default, sizeof an_default, "%s", d + 16); return; }
    if (!strncmp(l, "Variables", 9)) { an_vars = true; return; }
    if (!an_vars && l[0] == ' ' && l[1] == ' ' && l[2] != ' ') ++an_targets;
}
void tm_init(tm_out_fn out) { OUT = out; }
void tm_announce(const char *dir) {
    const char *mk = tm_find_makefile(dir);
    if (!mk) return;
    an_targets = 0; an_vars = false; an_default[0] = 0;
    if (!make_help(dir, mk, an_line)) return;
    char sp[80] = ""; char *sw = strchr(an_default, ' '); if (sw) *sw = 0;
    if (an_default[0]) snprintf(sp, sizeof sp, ", default '%s'", an_default);
    outf("Makefile found: %s - %d target%s%s.  'make help' lists them, 'make' builds with TinyCC.", mk, an_targets, an_targets == 1 ? "" : "s", sp);
}
