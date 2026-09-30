/* term.c - the TERMINAL panel: a small command line inside Studio.
 *
 * There is no shell on C-OS and no chdir(): a "current directory" here is
 * Studio's own variable (G.cwd), and every path is made absolute before it is
 * handed to a program. Built-in commands (cd, ls, cat, mkdir, rm, cp, mv,
 * touch, echo, ...) run inside Studio; `tcc`, `make` and programs run as
 * separate processes, with their output captured through a log file
 * (COS_STDOUT) and shown as it arrives - the UI never blocks.
 *
 *   cd desktop            goes to /desktop (relative to the current directory,
 *                         then to /, then to "C-OS Studio")
 *   make [target] [VAR=x] builds with the Makefile in the current directory,
 *                         using TinyCC (.o files, .a libraries, .c-os programs)
 *   tcc main.c -o app.c-os
 *   ./app.c-os            runs a program
 */
#include "studio.h"
#include <stdarg.h>

#define TERM_MAX (192 * 1024)
#define TERM_LOG "/tmp/studio_term.log"
#define TCC_EXE  "/bin/tcc.c-os"

static const char *tcc_exe(void) { const char *o = cos_getenv("STUDIO_TCC"); return o && o[0] ? o : TCC_EXE; }

/* ---------------------------------------------------------------- */
/* output buffer                                                     */
/* ---------------------------------------------------------------- */
void term_print(const char *s, size_t n) {
    if (!G.term) return;
    if (G.term_len + n + 1 > TERM_MAX) {
        size_t from = G.term_len / 2;
        while (from < G.term_len && G.term[from] != '\n') ++from;
        memmove(G.term, G.term + from, G.term_len - from);
        G.term_len -= from;
    }
    for (size_t i = 0; i < n; ++i) G.term[G.term_len++] = s[i] ? s[i] : '?';
    G.term[G.term_len] = 0;
    G.term_follow = true; G.dirty_frame = true;
}
void term_printf(const char *fmt, ...) {
    char b[700];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    if (n > 0) term_print(b, (size_t)n);
}
void term_clear(void) { G.term_len = 0; if (G.term) G.term[0] = 0; G.term_scroll = 0; G.term_follow = true; G.dirty_frame = true; }

int term_line_count(void) { int n = 0; for (size_t i = 0; i < G.term_len; ++i) if (G.term[i] == '\n') ++n; return n; }
int term_line_at(int idx, char *buf, size_t cap) {
    size_t i = 0; int l = 0;
    while (i < G.term_len && l < idx) { if (G.term[i] == '\n') ++l; ++i; }
    size_t k = 0;
    while (i < G.term_len && G.term[i] != '\n' && k + 1 < cap) buf[k++] = G.term[i++];
    buf[k] = 0;
    return (int)k;
}

void term_prompt(char *buf, size_t cap) { snprintf(buf, cap, "%s$", G.cwd); }

/* ---------------------------------------------------------------- */
/* paths                                                             */
/* ---------------------------------------------------------------- */
static char s_base[256];                       /* directory relative paths are resolved against */

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

/* FAT ignores case; show the names the way they are stored */
static void fix_case(char *path) {
    char out[512] = "";
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        char dir[512]; snprintf(dir, sizeof dir, "%s", out[0] ? out : "/");
        char found[256]; snprintf(found, sizeof found, "%s", t);
        int fd = cos_opendir(dir);
        if (fd >= 0) { cos_dirent_t de; while (cos_readdir(fd, &de) == 1) if (!strcasecmp(de.name, t)) { snprintf(found, sizeof found, "%s", de.name); break; } cos_close(fd); }
        size_t l = strlen(out); snprintf(out + l, sizeof out - l, "/%s", found);
    }
    if (out[0]) snprintf(path, 256, "%s", out);
}

/* an existing thing named `arg`: relative to the base, then to /, then to C-OS Studio */
static bool resolve_existing(const char *arg, char *out, size_t cap) {
    char tmp[600];
    if (arg[0] == '~') { snprintf(tmp, sizeof tmp, "%s%s", studio_deliverables(), arg[1] == '/' ? arg + 1 : arg + 1); norm_path(tmp, out, cap); return true; }
    if (arg[0] == '/') { norm_path(arg, out, cap); return fs_exists(out); }
    snprintf(tmp, sizeof tmp, "%s/%s", s_base, arg); norm_path(tmp, out, cap);
    if (fs_exists(out)) return true;
    snprintf(tmp, sizeof tmp, "/%s", arg); norm_path(tmp, out, cap);
    if (fs_exists(out)) return true;
    snprintf(tmp, sizeof tmp, "%s/%s", studio_home(), arg); norm_path(tmp, out, cap);
    if (fs_exists(out)) return true;
    snprintf(tmp, sizeof tmp, "%s/%s", s_base, arg); norm_path(tmp, out, cap);   /* not found: the plain relative path */
    return false;
}
/* a path for something to be created */
static void resolve_new(const char *arg, char *out, size_t cap) {
    char tmp[600];
    if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg); else snprintf(tmp, sizeof tmp, "%s/%s", s_base, arg);
    norm_path(tmp, out, cap);
}

/* words -> argv, with "double" and 'single' quotes */
static int tokenize(char *line, char **argv, int max) {
    int n = 0;
    char *p = line;
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

static bool glob_ok(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;; ++s) { if (glob_ok(p + 1, s)) return true; if (!*s) return false; } }
    if (*s && (*p == '?' || *p == *s || ((*p | 32) == (*s | 32) && (*p | 32) >= 'a' && (*p | 32) <= 'z'))) return glob_ok(p + 1, s + 1);
    return false;
}
/* expands `*.o` style arguments against s_base; a pattern with no match stays literal */
static int expand_globs(int argc, char **argv, char **out, int max) {
    int n = 0;
    for (int i = 0; i < argc && n < max; ++i) {
        if (!strpbrk(argv[i], "*?") || strchr(argv[i], '/')) { out[n++] = argv[i]; continue; }
        int fd = cos_opendir(s_base), before = n;
        if (fd >= 0) {
            cos_dirent_t de;
            while (n < max && cos_readdir(fd, &de) == 1) if (de.name[0] != '.' && glob_ok(argv[i], de.name)) out[n++] = strdup(de.name);
            cos_close(fd);
        }
        if (n == before) out[n++] = argv[i];
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* deliverables                                                      */
/* ---------------------------------------------------------------- */
static bool is_under(const char *path, const char *dir) { size_t n = strlen(dir); return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]); }

/* copy `abs` into C-OS Studio/deliverables (a .c-os, .o or .a that a build just made) */
static void deliver(const char *abs) {
    if (!has_ext(abs, ".c-os") && !has_ext(abs, ".o") && !has_ext(abs, ".a")) return;
    if (is_under(abs, studio_deliverables()) || !fs_exists(abs)) return;
    studio_ensure_dirs();
    char dst[300]; path_join(dst, sizeof dst, studio_deliverables(), path_base(abs));
    size_t n; char *d = fs_read_all(abs, &n);
    if (d && (n == 0 || fs_write_all(dst, d, n))) term_printf("  -> %s\n", dst);
    else term_printf("  (could not copy %s to deliverables)\n", path_base(abs));
    free(d);
}

/* ---------------------------------------------------------------- */
/* built-in commands                                                 */
/* ---------------------------------------------------------------- */
static void help(void) {
    term_printf("C-OS Studio terminal\n"
        "  cd DIR       change directory (cd desktop, cd .., cd -, cd ~ = deliverables)\n"
        "  ls [-l] [P]  list a directory        pwd    show the directory\n"
        "  cat FILE     print a file            open F open a file in the editor\n"
        "  mkdir D      make a directory        rm [-r] F   delete   cp A B   mv A B   touch F\n"
        "  echo ...     print text              clear  clear this panel\n"
        "  make [TARGET] [VAR=val]   build with the Makefile here, using TinyCC\n"
        "  tcc ARGS     the compiler:  tcc hello.c -o hello.c-os   (-c for a .o)\n"
        "  ./prog.c-os  run a program           Ctrl+C stops what is running\n"
        "  Built files are also copied to %s\n", studio_deliverables());
}

static int cmd_cd(int argc, char **argv) {
    char target[256];
    if (argc < 2) { snprintf(target, sizeof target, "/"); }
    else if (!strcmp(argv[1], "-")) snprintf(target, sizeof target, "%s", G.cwd_prev[0] ? G.cwd_prev : G.cwd);
    else {
        char joined[256] = "";                                   /* `cd C-OS Studio` without quotes */
        for (int i = 1; i < argc; ++i) { if (i > 1) strlcat(joined, " ", sizeof joined); strlcat(joined, argv[i], sizeof joined); }
        if (!resolve_existing(joined, target, sizeof target)) { term_printf("cd: %s: No such file or directory\n", joined); return 1; }
    }
    if (!fs_is_dir(target)) { term_printf("cd: %s: Not a directory\n", target); return 1; }
    fix_case(target);
    snprintf(G.cwd_prev, sizeof G.cwd_prev, "%s", G.cwd);
    snprintf(G.cwd, sizeof G.cwd, "%s", target);
    return 0;
}

static int cmp_names(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
static int cmd_ls(int argc, char **argv) {
    bool lng = false; const char *path = NULL;
    for (int i = 1; i < argc; ++i) { if (argv[i][0] == '-') { if (strchr(argv[i], 'l')) lng = true; } else path = argv[i]; }
    char dir[256];
    if (path) { if (!resolve_existing(path, dir, sizeof dir)) { term_printf("ls: %s: No such file or directory\n", path); return 1; } }
    else snprintf(dir, sizeof dir, "%s", s_base);
    if (!fs_is_dir(dir)) { term_printf("%s\n", path_base(dir)); return 0; }
    int fd = cos_opendir(dir);
    if (fd < 0) { term_printf("ls: cannot open %s\n", dir); return 1; }
    char **names = (char **)calloc(512, sizeof(char *)); unsigned long *sizes = (unsigned long *)calloc(512, sizeof(unsigned long));
    if (!names || !sizes) { free(names); free(sizes); cos_close(fd); return 1; }
    int n = 0; cos_dirent_t de;
    while (n < 512 && cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.') continue;
        char *nm = (char *)malloc(strlen(de.name) + 2); if (!nm) break;
        sprintf(nm, "%s%s", de.name, de.is_dir ? "/" : "");
        names[n] = nm; sizes[n] = (unsigned long)de.size; ++n;
    }
    cos_close(fd);
    /* sort names with their sizes */
    for (int i = 1; i < n; ++i) for (int j = i; j > 0 && cmp_names(&names[j - 1], &names[j]) > 0; --j) { char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t; unsigned long s = sizes[j]; sizes[j] = sizes[j - 1]; sizes[j - 1] = s; }
    if (lng) for (int i = 0; i < n; ++i) { if (names[i][strlen(names[i]) - 1] == '/') term_printf("%10s  %s\n", "<dir>", names[i]); else term_printf("%10lu  %s\n", sizes[i], names[i]); }
    else {
        char line[160] = "";
        for (int i = 0; i < n; ++i) {
            if (strlen(line) + strlen(names[i]) + 2 > 88) { term_printf("%s\n", line); line[0] = 0; }
            strlcat(line, names[i], sizeof line); strlcat(line, "   ", sizeof line);
        }
        if (line[0]) term_printf("%s\n", line);
        if (!n) term_printf("(empty)\n");
    }
    for (int i = 0; i < n; ++i) free(names[i]);
    free(names); free(sizes);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: cat FILE\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        char p[256];
        if (!resolve_existing(argv[i], p, sizeof p)) { term_printf("cat: %s: No such file\n", argv[i]); rc = 1; continue; }
        size_t n; char *d = fs_read_all(p, &n);
        if (!d) { term_printf("cat: %s: cannot read\n", argv[i]); rc = 1; continue; }
        if (n > 64 * 1024) { term_print(d, 64 * 1024); term_printf("\n... (%lu more bytes)\n", (unsigned long)(n - 64 * 1024)); }
        else { term_print(d, n); if (n && d[n - 1] != '\n') term_print("\n", 1); }
        free(d);
    }
    return rc;
}

static bool copy_path(const char *from, const char *to) {
    size_t n; char *d = fs_read_all(from, &n);
    if (!d) return false;
    bool ok = fs_write_all(to, d, n);
    free(d);
    return ok;
}
static int rm_path(const char *p, bool recursive) {
    if (fs_is_dir(p)) {
        if (!recursive) { term_printf("rm: %s is a directory (use rm -r)\n", path_base(p)); return 1; }
        int fd = cos_opendir(p);
        if (fd >= 0) {
            char *names[256]; int n = 0; cos_dirent_t de;
            while (n < 256 && cos_readdir(fd, &de) == 1) { if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue; names[n] = strdup(de.name); ++n; }
            cos_close(fd);
            for (int i = 0; i < n; ++i) { char c[300]; path_join(c, sizeof c, p, names[i]); rm_path(c, true); free(names[i]); }
        }
    }
    return cos_unlink(p) == 0 ? 0 : 1;
}

/* Runs a built-in. Returns -1 if `argv[0]` is not one. Paths resolve against s_base. */
static int run_builtin(int argc, char **argv) {
    const char *c = argv[0];
    if (!strcmp(c, "pwd")) { term_printf("%s\n", s_base); return 0; }
    if (!strcmp(c, "help") || !strcmp(c, "?")) { help(); return 0; }
    if (!strcmp(c, "clear") || !strcmp(c, "cls")) { term_clear(); return 0; }
    if (!strcmp(c, "cd")) return cmd_cd(argc, argv);
    if (!strcmp(c, "ls") || !strcmp(c, "dir")) return cmd_ls(argc, argv);
    if (!strcmp(c, "cat") || !strcmp(c, "type")) return cmd_cat(argc, argv);
    if (!strcmp(c, "echo")) { for (int i = 1; i < argc; ++i) term_printf("%s%s", i > 1 ? " " : "", argv[i]); term_print("\n", 1); return 0; }
    if (!strcmp(c, "true") || !strcmp(c, ":")) return 0;
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "mkdir")) {
        int rc = 0;
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '-') continue;
            char p[256]; resolve_new(argv[i], p, sizeof p);
            if (fs_is_dir(p)) continue;                                   /* -p semantics: an existing dir is fine */
            if (cos_mkdir(p) != 0) { term_printf("mkdir: cannot create %s\n", argv[i]); rc = 1; }
        }
        return rc;
    }
    if (!strcmp(c, "touch")) {
        for (int i = 1; i < argc; ++i) { char p[256]; resolve_new(argv[i], p, sizeof p); if (!fs_exists(p)) fs_write_all(p, "", 0); }
        return 0;
    }
    if (!strcmp(c, "rm") || !strcmp(c, "del")) {
        bool rec = false, force = false; int rc = 0;
        char *files[64]; int nf = 0;
        for (int i = 1; i < argc; ++i) { if (argv[i][0] == '-') { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = true; if (strchr(argv[i], 'f')) force = true; } else files[nf++] = argv[i]; }
        char *ex[128]; int ne = expand_globs(nf, files, ex, 128);
        for (int i = 0; i < ne; ++i) {
            char p[256]; resolve_new(ex[i], p, sizeof p);
            if (!fs_exists(p)) { if (!force) { term_printf("rm: %s: No such file\n", ex[i]); rc = 1; } continue; }
            if (rm_path(p, rec) != 0) { term_printf("rm: cannot remove %s\n", ex[i]); rc = 1; }
            else make_forget(path_base(p));
        }
        return rc;
    }
    if (!strcmp(c, "cp") || !strcmp(c, "copy") || !strcmp(c, "mv") || !strcmp(c, "move")) {
        bool mv = c[0] == 'm';
        char *args[8]; int na = 0;
        for (int i = 1; i < argc && na < 8; ++i) if (argv[i][0] != '-') args[na++] = argv[i];
        if (na != 2) { term_printf("usage: %s SRC DST\n", c); return 1; }
        char from[256], to[256];
        if (!resolve_existing(args[0], from, sizeof from)) { term_printf("%s: %s: No such file\n", c, args[0]); return 1; }
        resolve_new(args[1], to, sizeof to);
        if (fs_is_dir(to)) { char t2[300]; path_join(t2, sizeof t2, to, path_base(from)); snprintf(to, sizeof to, "%s", t2); }
        if (mv && cos_rename(from, to) == 0) return 0;
        if (fs_is_dir(from)) { term_printf("%s: %s is a directory\n", c, args[0]); return 1; }
        if (!copy_path(from, to)) { term_printf("%s: could not write %s\n", c, args[1]); return 1; }
        if (mv) cos_unlink(from);
        return 0;
    }
    if (!strcmp(c, "open") || !strcmp(c, "edit")) {
        if (argc < 2) { term_printf("usage: open FILE\n"); return 1; }
        char p[256];
        if (!resolve_existing(argv[1], p, sizeof p)) { if (!has_ext(argv[1], ".c") && !has_ext(argv[1], ".h")) { term_printf("open: %s: No such file\n", argv[1]); return 1; } resolve_new(argv[1], p, sizeof p); }
        app_open_file(p, 0);
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* processes                                                         */
/* ---------------------------------------------------------------- */
/* Turn the relative file arguments of a compiler command line into absolute paths:
 * a program started by Studio has no working directory of its own to resolve them in. */
static int abs_args(const char *dir, int argc, char **argv, char **out, int max) {
    int n = 0;
    bool ar = argc > 1 && !strcmp(argv[1], "-ar");
    for (int i = 0; i < argc && n < max - 1; ++i) {
        char *a = argv[i], buf[300];
        if (i == 0) { out[n++] = a; continue; }
        bool prev_o = !strcmp(argv[i - 1], "-o") || !strcmp(argv[i - 1], "-include") || !strcmp(argv[i - 1], "-I") || !strcmp(argv[i - 1], "-L");
        if (a[0] == '-') {
            if ((!strncmp(a, "-I", 2) || !strncmp(a, "-L", 2)) && a[2] && a[2] != '/') { snprintf(buf, sizeof buf, "%.2s%s/%s", a, dir, a + 2); out[n++] = strdup(buf); }
            else out[n++] = a;
            continue;
        }
        if (prev_o || (!ar && (strchr(a, '.') || strchr(a, '/')))) {
            if (a[0] == '/') out[n++] = a; else { snprintf(buf, sizeof buf, "%s/%s", dir, a); char nb[300]; norm_path(buf, nb, sizeof nb); out[n++] = strdup(nb); }
        } else if (ar && (has_ext(a, ".a") || has_ext(a, ".o"))) {
            if (a[0] == '/') out[n++] = a; else { snprintf(buf, sizeof buf, "%s/%s", dir, a); char nb[300]; norm_path(buf, nb, sizeof nb); out[n++] = strdup(nb); }
        } else out[n++] = a;
    }
    out[n] = NULL;
    return n;
}

static bool is_cc(const char *w) { return !strcmp(w, "tcc") || !strcmp(w, "cc") || !strcmp(w, "gcc") || !strcmp(w, "clang") || !strcmp(w, "tcc.c-os") || !strcmp(w, "cos-cc"); }

static void job_output_line(const char *line) {
    term_print(line, strlen(line)); term_print("\n", 1);
    Diag dg;
    int k = diag_parse_line(line, &dg);
    if (k) {
        if (G.ndiag < MAX_DIAGS) G.diags[G.ndiag++] = dg;
        if (k == 1) G.job.errors++;
    }
}
static void job_tail(void) {
    TermJob *j = &G.job;
    cos_stat_t st;
    if (cos_stat(j->log, &st) != 0 || st.size <= j->tail) return;
    int fd = cos_open(j->log, COS_O_RDONLY);
    if (fd < 0) return;
    cos_lseek(fd, (int64_t)j->tail);
    char chunk[1024];
    for (;;) {
        ssize_t n = cos_fd_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        j->tail += (size_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n' || j->acc_len >= (int)sizeof j->acc - 1) {
                j->acc[j->acc_len] = 0; job_output_line(j->acc); j->acc_len = 0;
                if (c != '\n') j->acc[j->acc_len++] = c;
            } else j->acc[j->acc_len++] = c;
        }
    }
    cos_close(fd);
}
static void job_flush_partial(void) { TermJob *j = &G.job; if (j->acc_len) { j->acc[j->acc_len] = 0; job_output_line(j->acc); j->acc_len = 0; } }

static bool spawn_external(const char *exe, char **argv) {
    TermJob *j = &G.job;
    char env[96]; snprintf(env, sizeof env, "COS_STDOUT=%s", TERM_LOG);
    const char *envp[] = { env, NULL };
    fs_write_all(TERM_LOG, "", 0);
    j->tail = 0; j->acc_len = 0; j->t0 = cos_time_ms();
    snprintf(j->log, sizeof j->log, "%s", TERM_LOG);
    j->pid = cos_spawn_argv(exe, (const char *const *)argv, envp);
    return j->pid > 0;
}

static void job_end(void) {
    TermJob *j = &G.job;
    if (j->is_make) make_plan_free(&j->plan);
    j->active = false; j->is_make = false; j->pid = 0; j->stopping = false;
    G.dirty_frame = true;
}

/* run make steps until one has to wait for a process (or all are done) */
static void make_advance(void) {
    TermJob *j = &G.job;
    while (j->step < j->plan.n) {
        MakeStep *s = &j->plan.steps[j->step];
        if (!s->silent) term_printf("%s\n", s->show);
        char *v[MAKE_MAXARGV];
        int rc = 0, bi;
        snprintf(s_base, sizeof s_base, "%s", j->dir);
        if (is_cc(s->argv[0]) || !strcmp(s->argv[0], "ar")) {
            char *sub[MAKE_MAXARGV]; int sn = 0;
            sub[sn++] = (char *)tcc_exe();
            if (!strcmp(s->argv[0], "ar")) sub[sn++] = "-ar";
            for (int i = 1; i < s->argc; ++i) sub[sn++] = s->argv[i];
            sub[sn] = NULL;
            abs_args(j->dir, sn, sub, v, MAKE_MAXARGV);
            if (!spawn_external(tcc_exe(), v)) { term_printf("make: cannot start the compiler (%s)\n", tcc_exe()); rc = 127; }
            else return;                                                   /* wait for it in term_tick */
        } else if ((bi = run_builtin(s->argc, s->argv)) >= 0) {
            rc = bi;
        } else if (s->argv[0][0] == '.' || s->argv[0][0] == '/' || has_ext(s->argv[0], ".c-os")) {
            char p[300]; resolve_new(s->argv[0], p, sizeof p);
            char *run[MAKE_MAXARGV]; int rn = 0; run[rn++] = p;
            for (int i = 1; i < s->argc; ++i) run[rn++] = s->argv[i];
            run[rn] = NULL;
            if (!spawn_external(p, run)) { term_printf("make: cannot run %s\n", s->argv[0]); rc = 127; } else return;
        } else { term_printf("make: %s: command not found\n", s->argv[0]); rc = 127; }
        if (rc != 0 && !s->ignore_err) { term_printf("make: *** [%s] Error %d\n", j->plan.goal, rc); job_end(); return; }
        make_commit_step(s);
        if (s->output[0]) { char ap[300]; snprintf(ap, sizeof ap, "%s/%s", j->dir, s->output); char nb[300]; if (s->output[0] == '/') snprintf(nb, sizeof nb, "%s", s->output); else norm_path(ap, nb, sizeof nb); deliver(nb); }
        j->step++;
    }
    term_printf("make: done (%s)\n", j->plan.goal);
    if (G.proj.loaded) project_rescan(&G.proj);
    job_end();
}

void term_tick(void) {
    TermJob *j = &G.job;
    if (!j->active) return;
    job_tail();
    int status = 0;
    int64_t r = cos_waitpid(j->pid, &status, false);
    if (r != j->pid) { G.dirty_frame = true; return; }
    job_tail(); job_flush_partial();
    uint64_t ms = cos_time_ms() - j->t0;
    if (j->stopping) { term_printf("[stopped]\n"); job_end(); return; }
    if (j->is_make) {
        MakeStep *s = &j->plan.steps[j->step];
        if (status != 0 && !s->ignore_err) { term_printf("make: *** [%s] Error %d\n", j->plan.goal, status); if (j->errors) app_status("make failed - %d error%s", j->errors, j->errors == 1 ? "" : "s"); job_end(); return; }
        make_commit_step(s);
        if (s->output[0]) { char ap[300], nb[300]; if (s->output[0] == '/') snprintf(nb, sizeof nb, "%s", s->output); else { snprintf(ap, sizeof ap, "%s/%s", j->dir, s->output); norm_path(ap, nb, sizeof nb); } deliver(nb); }
        j->step++;
        make_advance();
        return;
    }
    term_printf("[exit code %d]  (%d.%02d s)\n", status, (int)(ms / 1000), (int)((ms % 1000) / 10));
    if (status == 0 && G.proj.loaded) project_rescan(&G.proj);
    job_end();
}

void term_stop(void) {
    TermJob *j = &G.job;
    if (!j->active) return;
    j->stopping = true;
    cos_kill((int)j->pid, 9);
    int st;
    for (int i = 0; i < 50; ++i) { if (cos_waitpid(j->pid, &st, false) == j->pid) break; cos_sleep_ms(4); }
    job_tail(); job_flush_partial();
    term_printf("[stopped]\n");
    job_end();
}
bool term_busy(void) { return G.job.active; }

/* ---------------------------------------------------------------- */
/* command line                                                      */
/* ---------------------------------------------------------------- */
static void note(const char *m) { term_printf("%s\n", m); }

static void do_make(int argc, char **argv) {
    const char *makefile = NULL, *target = NULL; const char *ov[16]; int nov = 0;
    char dir[256]; snprintf(dir, sizeof dir, "%s", s_base);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) makefile = argv[++i];
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) { char p[256]; if (!resolve_existing(argv[++i], p, sizeof p) || !fs_is_dir(p)) { term_printf("make: *** %s: No such directory\n", argv[i]); return; } snprintf(dir, sizeof dir, "%s", p); }
        else if (strchr(argv[i], '=') && argv[i][0] != '-' && nov < 16) ov[nov++] = argv[i];
        else if (argv[i][0] != '-') target = argv[i];
    }
    static const char *const names[] = { "Makefile", "makefile", "GNUmakefile", "Makefile.txt", NULL };
    char mkpath[300];
    if (!makefile) { for (int i = 0; names[i]; ++i) { path_join(mkpath, sizeof mkpath, dir, names[i]); if (fs_exists(mkpath)) { makefile = names[i]; break; } } }
    if (!makefile) { term_printf("make: *** No Makefile found in %s.  Stop.\n", dir); return; }
    G.ndiag = 0; G.job.errors = 0;
    char err[200];
    TermJob *j = &G.job;
    if (!make_plan(dir, makefile, target, ov, nov, &j->plan, err, sizeof err, note)) { term_printf("make: *** %s.  Stop.\n", err); return; }
    if (j->plan.n == 0) { term_printf("make: '%s' is up to date.\n", j->plan.goal); make_plan_free(&j->plan); return; }
    j->active = true; j->is_make = true; j->step = 0; j->stopping = false; j->pid = 0;
    snprintf(j->dir, sizeof j->dir, "%s", dir);
    term_printf("make: %d command%s for '%s'\n", j->plan.n, j->plan.n == 1 ? "" : "s", j->plan.goal);
    make_advance();
}

static void do_external(int argc, char **argv) {
    char *v[MAKE_MAXARGV];
    if (is_cc(argv[0])) {
        char *sub[MAKE_MAXARGV]; int sn = 0;
        sub[sn++] = (char *)tcc_exe();
        for (int i = 1; i < argc && sn < MAKE_MAXARGV - 1; ++i) sub[sn++] = argv[i];
        sub[sn] = NULL;
        abs_args(s_base, sn, sub, v, MAKE_MAXARGV);
        G.ndiag = 0; G.job.errors = 0;
        if (!spawn_external(tcc_exe(), v)) { term_printf("tcc: cannot start the compiler (%s)\n", tcc_exe()); return; }
        G.job.active = true; G.job.is_make = false; G.job.stopping = false;
        return;
    }
    /* a program: ./name, /path/name, name.c-os, or a bare name that exists in the current directory */
    const char *first = argv[0];
    if (!strcmp(first, "run") && argc > 1) { ++argv; --argc; first = argv[0]; }
    char p[256];
    bool found = resolve_existing(first, p, sizeof p);
    if (!found && !has_ext(first, ".c-os")) {
        char withext[280]; snprintf(withext, sizeof withext, "%s.c-os", first);
        found = resolve_existing(withext, p, sizeof p);
    }
    if (!found || fs_is_dir(p)) { term_printf("%s: command not found  (type help)\n", argv[0]); return; }
    char *run[MAKE_MAXARGV]; int rn = 0; run[rn++] = p;
    for (int i = 1; i < argc && rn < MAKE_MAXARGV - 1; ++i) run[rn++] = argv[i];
    run[rn] = NULL;
    G.ndiag = 0;
    if (!spawn_external(p, run)) { term_printf("%s: cannot start\n", argv[0]); return; }
    G.job.active = true; G.job.is_make = false; G.job.stopping = false;
    term_printf("[started %s, pid %d]  (Ctrl+C to stop)\n", path_base(p), (int)G.job.pid);
}

static void submit(const char *text) {
    char echo[300]; term_prompt(echo, sizeof echo);
    term_printf("%s %s\n", echo, text);
    char line[256]; snprintf(line, sizeof line, "%s", text);
    char *argv[48]; int argc = tokenize(line, argv, 48);
    if (!argc) return;
    if (G.term_hist_n == 0 || strcmp(G.term_hist[(G.term_hist_n - 1) % 32], text)) { snprintf(G.term_hist[G.term_hist_n % 32], 256, "%s", text); ++G.term_hist_n; }
    G.term_hist_i = G.term_hist_n;
    snprintf(s_base, sizeof s_base, "%s", G.cwd);
    if (!strcmp(argv[0], "exit")) { app_do(CMD_VIEW_PANEL); return; }
    if (!strcmp(argv[0], "make")) { do_make(argc, argv); return; }
    int rc = run_builtin(argc, argv);
    if (rc >= 0) { if (rc == 0 && G.proj.loaded && (!strcmp(argv[0], "mkdir") || !strcmp(argv[0], "rm") || !strcmp(argv[0], "cp") || !strcmp(argv[0], "mv") || !strcmp(argv[0], "touch"))) project_rescan(&G.proj); return; }
    do_external(argc, argv);
}

/* ---------------------------------------------------------------- */
/* keyboard                                                          */
/* ---------------------------------------------------------------- */
static void tab_complete(void) {
    /* complete the last word against the entries of the directory it names */
    char *sp = strrchr(G.term_line, ' ');
    char *word = sp ? sp + 1 : G.term_line;
    char dirpart[256] = "", prefix[128];
    const char *sl = strrchr(word, '/');
    if (sl) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(sl - word + 1), word); snprintf(prefix, sizeof prefix, "%s", sl + 1); } else snprintf(prefix, sizeof prefix, "%s", word);
    char dir[256];
    snprintf(s_base, sizeof s_base, "%s", G.cwd);
    if (dirpart[0]) { char d[256]; snprintf(d, sizeof d, "%s", dirpart); if (!resolve_existing(d, dir, sizeof dir)) return; } else snprintf(dir, sizeof dir, "%s", G.cwd);
    int fd = cos_opendir(dir);
    if (fd < 0) return;
    char match[256] = ""; int nm = 0; bool isdir = false; cos_dirent_t de;
    size_t pl = strlen(prefix);
    while (cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.' && pl == 0) continue;
        if (strncasecmp(de.name, prefix, pl)) continue;
        if (nm == 0) { snprintf(match, sizeof match, "%s", de.name); isdir = de.is_dir != 0; }
        else { size_t k = 0; while (match[k] && (match[k] | 32) == (de.name[k] | 32)) ++k; match[k] = 0; }
        ++nm;
    }
    cos_close(fd);
    if (!nm) return;
    char nw[300]; snprintf(nw, sizeof nw, "%s%s%s", dirpart, match, (nm == 1 && isdir) ? "/" : "");
    if (strchr(nw, ' ')) { char q[320]; snprintf(q, sizeof q, "\"%s\"", nw); snprintf(nw, sizeof nw, "%s", q); }
    size_t keep = (size_t)(word - G.term_line);
    if (keep + strlen(nw) < sizeof G.term_line) { strcpy(G.term_line + keep, nw); }
}

void term_key(const cos_win_event_t *ev) {
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0;
    G.caret_t = cos_time_ms(); G.dirty_frame = true;
    if (ctrl && ev->special == COS_KEY_NONE) {
        char c = ev->ascii | 32;
        if (c == 'c') { if (G.job.active) term_stop(); else { term_printf("%s ^C\n", G.cwd); G.term_line[0] = 0; } return; }
        if (c == 'l') { term_clear(); return; }
        if (c == 'u') { G.term_line[0] = 0; return; }
        return;
    }
    switch (ev->special) {
        case COS_KEY_ENTER:
            if (G.job.active) { term_printf("(a command is still running - Ctrl+C stops it)\n"); return; }
            { char t[256]; snprintf(t, sizeof t, "%s", G.term_line); G.term_line[0] = 0; submit(t); }
            return;
        case COS_KEY_BACKSPACE: { size_t n = strlen(G.term_line); while (n > 0 && ((unsigned char)G.term_line[n - 1] & 0xC0) == 0x80) --n; if (n) G.term_line[n - 1] = 0; return; }
        case COS_KEY_UP: if (G.term_hist_i > 0 && G.term_hist_i > G.term_hist_n - 32) { --G.term_hist_i; snprintf(G.term_line, sizeof G.term_line, "%s", G.term_hist[G.term_hist_i % 32]); } return;
        case COS_KEY_DOWN: if (G.term_hist_i < G.term_hist_n) { ++G.term_hist_i; if (G.term_hist_i < G.term_hist_n) snprintf(G.term_line, sizeof G.term_line, "%s", G.term_hist[G.term_hist_i % 32]); else G.term_line[0] = 0; } return;
        case COS_KEY_TAB: tab_complete(); return;
        case COS_KEY_PGUP: G.term_follow = false; G.term_scroll -= 6; if (G.term_scroll < 0) G.term_scroll = 0; return;
        case COS_KEY_PGDN: G.term_scroll += 6; return;
        case COS_KEY_ESC: G.term_focus = false; return;
        case COS_KEY_NONE:
            if (ev->ascii >= 32 && ev->ascii < 127) { size_t n = strlen(G.term_line); if (n + 1 < sizeof G.term_line) { G.term_line[n] = ev->ascii; G.term_line[n + 1] = 0; } }
            return;
        default: return;
    }
}

void term_init(void) {
    G.term = (char *)malloc(TERM_MAX + 1);
    if (G.term) G.term[0] = 0;
    G.term_len = 0; G.term_follow = true;
    if (G.proj.loaded) snprintf(G.cwd, sizeof G.cwd, "%s", G.proj.root); else snprintf(G.cwd, sizeof G.cwd, "/");
    snprintf(G.cwd_prev, sizeof G.cwd_prev, "%s", G.cwd);
    term_printf("C-OS Studio terminal - type 'help'.  cd desktop, then make.\n");
}
