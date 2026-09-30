/* build.c - Build state machine, compiler launch and diagnostics.
 *
 *   Idle -> Saving -> Compiling -> Running -> Exited(code)
 *                        |             \-> Killed (Stop)
 *                        \-> Failed (diagnostics parsed)
 *
 * The compiler (tcc.c-os) and the user's program are both separate
 * processes; Studio never links a compiler in, so a bad program or a
 * compiler bug cannot take the IDE with it. Their output reaches Studio
 * through a log file named in the child's COS_STDOUT environment variable
 * (libcos's cos_write honours it) - the design doc's fallback for the
 * missing pipe syscall - and is read here incrementally, every tick, so
 * the UI never blocks while a build runs.
 */
#include "studio.h"

#define TCC_PATH   "/bin/tcc.c-os"
#define SDK_INC    "/system/sdk/include"
#define BUILD_LOG  "/tmp/studio_build.log"
#define RUN_LOG    "/tmp/studio_run.log"

static const char *tcc_path(void) {
    const char *o = cos_getenv("STUDIO_TCC");         /* host test harness / developer override */
    return o && o[0] ? o : TCC_PATH;
}

void build_init(void) {
    memset(&G.bld, 0, sizeof G.bld);
    G.bld.st = B_IDLE;
    snprintf(G.bld.cfg, sizeof G.bld.cfg, "Debug");
}

/* ---------------------------------------------------------------- */
/* diagnostics                                                       */
/* ---------------------------------------------------------------- */
static bool all_digits(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* "file:LINE: error: msg" / "file:LINE:COL: warning: msg". Returns 1 error,
 * 2 warning, 0 if the line is not a diagnostic. */
int diag_parse_line(const char *line, Diag *out) {
    const char *p = line;
    /* find the first ':' followed by digits and another ':' (skips a "C:" drive letter) */
    const char *q = NULL;
    for (const char *c = p; *c; ++c) {
        if (*c != ':') continue;
        const char *e = c + 1;
        while (*e >= '0' && *e <= '9') ++e;
        if (e > c + 1 && *e == ':') { q = c; break; }
    }
    if (!q || q == p) return 0;
    const char *ls = q + 1, *le = ls;
    while (*le >= '0' && *le <= '9') ++le;
    if (!all_digits(ls, (size_t)(le - ls))) return 0;
    const char *r = le + 1;
    if (*r >= '0' && *r <= '9') { const char *cc = r; while (*cc >= '0' && *cc <= '9') ++cc; if (*cc == ':') r = cc + 1; }
    while (*r == ' ') ++r;
    int sev;
    if (!strncmp(r, "error:", 6)) { sev = 1; r += 6; }
    else if (!strncmp(r, "warning:", 8)) { sev = 2; r += 8; }
    else if (!strncmp(r, "fatal error:", 12)) { sev = 1; r += 12; }
    else return 0;
    while (*r == ' ') ++r;
    memset(out, 0, sizeof *out);
    size_t fl = (size_t)(q - p); if (fl > 255) fl = 255;
    memcpy(out->file, p, fl); out->file[fl] = 0;
    out->line = out->rep_line = atoi(ls);
    out->sev = sev == 1 ? 0 : 1;
    snprintf(out->msg, sizeof out->msg, "%s", r);
    return sev;
}

static Doc *doc_for_file(const char *file, char *path_out, size_t cap) {
    const char *base = path_base(file);
    for (int i = 0; i < G.ndocs; ++i)
        if (!strcmp(G.docs[i]->path, file) || !strcmp(path_base(G.docs[i]->path), base)) { snprintf(path_out, cap, "%s", G.docs[i]->path); return G.docs[i]; }
    /* not open: look for it in the project's sources */
    static char srcs[24][256];
    int n = G.proj.loaded ? project_sources(&G.proj, srcs, 24) : 0;
    for (int i = 0; i < n; ++i) if (!strcmp(path_base(srcs[i]), base)) { snprintf(path_out, cap, "%s", srcs[i]); return NULL; }
    snprintf(path_out, cap, "%s", file);
    return NULL;
}

static void line_bounds(const char *t, size_t n, int line, size_t *a, size_t *e) {
    size_t ls = 0; int l = 1;
    for (size_t i = 0; i < n && l < line; ++i) if (t[i] == '\n') { ++l; ls = i + 1; }
    size_t le = ls; while (le < n && t[le] != '\n') ++le;
    if (le > ls && t[le - 1] == '\r') --le;
    *a = ls; *e = le;
}

/* TinyCC reports the place it NOTICED the problem. For "';' expected (got X)"
 * that is the first token of the NEXT line, so the squiggle belongs at the end
 * of the previous line - the raw text keeps the compiler's own line. */
void diag_refine(Diag *g) {
    char path[256];
    Doc *d = doc_for_file(g->file, path, sizeof path);
    snprintf(g->path, sizeof g->path, "%s", path);
    char *own = NULL; const char *t = NULL; size_t n = 0;
    if (d) { t = buf_text(&d->b); n = buf_len(&d->b); }
    else { own = fs_read_all(path, &n); t = own; }
    if (!t) { g->col = 0; g->col_end = 1; return; }

    size_t a, e; line_bounds(t, n, g->rep_line, &a, &e);
    size_t first = a; while (first < e && (t[first] == ' ' || t[first] == '\t')) ++first;
    bool expected = strstr(g->msg, "expected") != NULL;

    if (expected && g->rep_line > 1 && first == a + (size_t)0 + (first - a)) {
        /* is the reported token the first thing on its line? then the fault is before it */
        const char *got = strstr(g->msg, "(got ");
        bool at_line_start = true;
        if (got && (got[5] == '\'' || got[5] == '"')) {
            char qc = got[5]; got += 6;
            const char *ge = strchr(got, qc);
            size_t gl = ge ? (size_t)(ge - got) : 0;
            at_line_start = gl && first + gl <= e && !strncmp(t + first, got, gl);
        }
        if (at_line_start) {
            int pl = g->rep_line - 1;
            size_t pa, pe;
            for (;;) {                       /* previous non-blank line */
                line_bounds(t, n, pl, &pa, &pe);
                size_t k = pa; while (k < pe && (t[k] == ' ' || t[k] == '\t')) ++k;
                if (k < pe || pl <= 1) break;
                --pl;
            }
            size_t end = pe; while (end > pa && (t[end - 1] == ' ' || t[end - 1] == '\t')) --end;
            g->line = pl;
            g->col = (int)(end - pa) - 1; if (g->col < 0) g->col = 0;
            g->col_end = (int)(end - pa) + 5;                 /* runs past the end: "something goes here" */
            /* quick fix for  'X' expected  */
            const char *m = g->msg;
            if (m[0] == '\'' && m[1] && m[2] == '\'' && !strncmp(m + 3, " expected", 9)) g->fix_ch = m[1];
            free(own);
            return;
        }
    }
    /* undefined symbol 'name' / implicit declaration: underline that identifier */
    const char *q1 = strchr(g->msg, '\'');
    if (q1) {
        const char *q2 = strchr(q1 + 1, '\'');
        size_t nl = q2 ? (size_t)(q2 - q1 - 1) : 0;
        if (nl > 0 && nl < 60) {
            for (size_t i = first; i + nl <= e; ++i)
                if (!strncmp(t + i, q1 + 1, nl) && (i == a || !(((t[i - 1] | 32) >= 'a' && (t[i - 1] | 32) <= 'z') || t[i - 1] == '_')) &&
                    (i + nl == e || !(((t[i + nl] | 32) >= 'a' && (t[i + nl] | 32) <= 'z') || t[i + nl] == '_' || (t[i + nl] >= '0' && t[i + nl] <= '9')))) {
                    g->col = (int)(i - a); g->col_end = (int)(i - a + nl); free(own); return;
                }
        }
    }
    size_t end = e; while (end > first && (t[end - 1] == ' ' || t[end - 1] == '\t')) --end;
    g->col = (int)(first - a); g->col_end = (int)(end - a);
    if (g->col_end <= g->col) g->col_end = g->col + 1;
    free(own);
}

bool diag_apply_fix(Doc *d, const Diag *g) {
    if (!g->fix_ch || g->line < 1) return false;
    size_t l = (size_t)g->line - 1;
    if (l >= buf_lines(&d->b)) return false;
    size_t e = buf_line_end(&d->b, l);
    while (e > buf_line_start(&d->b, l) && (buf_at(&d->b, e - 1) == ' ' || buf_at(&d->b, e - 1) == '\t')) --e;
    doc_group_begin(d);
    doc_replace(d, e, 0, &g->fix_ch, 1, false);
    doc_group_end(d);
    d->caret = d->anchor = e + 1;
    return true;
}

/* ---------------------------------------------------------------- */
/* log tailing (shared by compile and run)                           */
/* ---------------------------------------------------------------- */
static void feed_line(const char *line) {
    app_output_add(line, strlen(line));
    app_output_add("\n", 1);
    if (G.bld.st == B_COMPILING) {
        Diag dg;
        int k = diag_parse_line(line, &dg);
        if (k && G.ndiag < MAX_DIAGS) {
            G.diags[G.ndiag++] = dg;
            if (k == 1) G.bld.nerr++; else G.bld.nwarn++;
        }
    }
}

static void tail_log(void) {
    Build *b = &G.bld;
    cos_stat_t st;
    if (cos_stat(b->out_path, &st) != 0 || st.size <= b->tail_pos) return;
    int fd = cos_open(b->out_path, COS_O_RDONLY);
    if (fd < 0) return;
    cos_lseek(fd, (int64_t)b->tail_pos);
    char chunk[1024];
    for (;;) {
        ssize_t n = cos_fd_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        b->tail_pos += (size_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n' || b->line_len >= (int)sizeof b->line_acc - 1) {
                b->line_acc[b->line_len] = 0;
                feed_line(b->line_acc);
                b->line_len = 0;
                if (c != '\n') b->line_acc[b->line_len++] = c;
            } else b->line_acc[b->line_len++] = c;
        }
    }
    cos_close(fd);
}

static void flush_partial(void) {
    Build *b = &G.bld;
    if (b->line_len) { b->line_acc[b->line_len] = 0; feed_line(b->line_acc); b->line_len = 0; }
}

static int64_t spawn_logged(const char *exe, const char *const *argv, const char *log) {
    char env[160];
    snprintf(env, sizeof env, "COS_STDOUT=%s", log);
    const char *envp[] = { env, NULL };
    fs_write_all(log, "", 0);                         /* truncate: the child appends */
    G.bld.tail_pos = 0; G.bld.line_len = 0;
    snprintf(G.bld.out_path, sizeof G.bld.out_path, "%s", log);
    return cos_spawn_argv(exe, argv, envp);
}

/* ---------------------------------------------------------------- */
/* state machine                                                     */
/* ---------------------------------------------------------------- */
static void fmt_secs(char *out, size_t cap, uint64_t ms) { snprintf(out, cap, "%d.%02d s", (int)(ms / 1000), (int)((ms % 1000) / 10)); }

static void set_state(BuildState s) { G.bld.st = s; G.dirty_frame = true; }

static void start_run(void) {
    Build *b = &G.bld;
    const char *argv[] = { b->exe_path, NULL };
    b->t0 = cos_time_ms();
    b->pid = spawn_logged(b->exe_path, argv, RUN_LOG);
    if (b->pid <= 0) {
        app_output_addf("Could not start %s\n", b->exe_path);
        set_state(B_FAILED);
        return;
    }
    set_state(B_RUNNING);
}

static void finish_compile(int code) {
    Build *b = &G.bld;
    flush_partial();
    b->t1 = cos_time_ms();
    char secs[24]; fmt_secs(secs, sizeof secs, b->t1 - b->t0);
    for (int i = 0; i < G.ndiag; ++i) diag_refine(&G.diags[i]);
    if (code != 0 || b->nerr > 0) {
        if (b->nerr == 0) b->nerr = 1;                    /* compiler died without a parseable message */
        app_output_addf("!Build failed - %d error%s, %d warning%s   (%s)\n", b->nerr, b->nerr == 1 ? "" : "s", b->nwarn, b->nwarn == 1 ? "" : "s", secs);
        set_state(B_FAILED);
        G.panel_open = true;
        /* jump to the first error */
        for (int i = 0; i < G.ndiag; ++i) if (G.diags[i].sev == 0) { app_open_file(G.diags[i].path, G.diags[i].line); break; }
        return;
    }
    app_output_addf("Build succeeded - 0 errors, %d warning%s   (%s)\n", b->nwarn, b->nwarn == 1 ? "" : "s", secs);
    if (G.proj.loaded) project_rescan(&G.proj);                     /* the new .c-os shows up in the Explorer */
    if (b->build_only) { b->pid = 0; set_state(B_EXITED); return; }
    start_run();
}

void build_start(bool run_after) {
    Build *b = &G.bld;
    if (b->st == B_COMPILING) return;
    if (b->st == B_RUNNING) build_stop();
    set_state(B_SAVING);
    if (G.save_before_run) {
        for (int i = 0; i < G.ndocs; ++i) {
            Doc *d = G.docs[i];
            if (!d->dirty) continue;
            if (d->untitled) { app_status("Save the new file first (Ctrl+S)"); set_state(B_IDLE); return; }
            if (!doc_save(d, NULL)) { app_status("Could not save %s - build cancelled", d->name); set_state(B_FAILED); return; }
        }
    }
    static char srcs[24][256];
    int n = 0;
    Doc *cur = app_doc();
    /* Build the project the current file belongs to. A file outside the open project is
     * built with ITS project (found by walking up to a cosproj.txt), else on its own. */
    bool has_file = cur && cur->path[0] && !cur->untitled;
    if (has_file && (!G.proj.loaded || strncmp(cur->path, G.proj.root, strlen(G.proj.root)) || cur->path[strlen(G.proj.root)] != '/')) {
        char root[256];
        if (project_find_root(cur->path, root, sizeof root)) project_open(&G.proj, root);
    }
    bool in_proj = G.proj.loaded && (!has_file || (!strncmp(cur->path, G.proj.root, strlen(G.proj.root)) && cur->path[strlen(G.proj.root)] == '/'));
    if (in_proj) n = project_sources(&G.proj, srcs, 24);
    if (n == 0 && has_file) { snprintf(srcs[0], 256, "%s", cur->path); n = 1; in_proj = false; }
    if (n == 0) { app_status("Nothing to build"); set_state(B_IDLE); return; }

    const char *stem = in_proj ? G.proj.target : path_base(srcs[0]);
    char name[64]; snprintf(name, sizeof name, "%s", stem);
    char *dot = strrchr(name, '.'); if (dot && !in_proj) *dot = 0;
    studio_ensure_dirs();
    bool shared = in_proj && !strcmp(G.proj.type, "shared");
    if (shared) run_after = false;                                  /* a .c-osll cannot be launched directly */
    snprintf(b->exe_path, sizeof b->exe_path, "%s/%s.%s", studio_deliverables(), name, shared ? "c-osll" : "c-os");
    snprintf(b->src, sizeof b->src, "%s", srcs[0]);

    G.ndiag = 0; b->nerr = b->nwarn = 0;
    b->pid = 0;
    b->build_only = !run_after;

    /* argv: tcc.c-os -I<sdk> [-Wall] [-Werror] [-Ddef]... [-Iinc]... [cflags] sources... [libs]... -o exe */
    static const char *argv[80]; static char cflag_buf[128];
    static char def_buf[192], inc_buf[192], lib_buf[192];
    static char def_tok[16][80], inc_tok[16][256], lib_tok[16][256];
    int ac = 0;
    argv[ac++] = "tcc.c-os";
    argv[ac++] = "-I" SDK_INC;
    if (in_proj && G.proj.warn_all) argv[ac++] = "-Wall";
    if (in_proj && G.proj.werror) argv[ac++] = "-Werror";
    if (in_proj && G.proj.defines[0]) {
        snprintf(def_buf, sizeof def_buf, "%s", G.proj.defines);
        int dn = 0;
        for (char *tok = strtok(def_buf, ","); tok && dn < 16 && ac < 76; tok = strtok(NULL, ",")) {
            while (*tok == ' ') ++tok;
            snprintf(def_tok[dn], sizeof def_tok[0], "-D%s", tok);
            argv[ac++] = def_tok[dn++];
        }
    }
    if (in_proj && G.proj.includes[0]) {
        snprintf(inc_buf, sizeof inc_buf, "%s", G.proj.includes);
        int in_ = 0;
        for (char *tok = strtok(inc_buf, ","); tok && in_ < 16 && ac < 76; tok = strtok(NULL, ",")) {
            while (*tok == ' ') ++tok;
            if (tok[0] == '/') snprintf(inc_tok[in_], sizeof inc_tok[0], "-I%s", tok);
            else { char full[256]; path_join(full, sizeof full, G.proj.root, tok); snprintf(inc_tok[in_], sizeof inc_tok[0], "-I%s", full); }
            argv[ac++] = inc_tok[in_++];
        }
    }
    if (shared) argv[ac++] = "-shared";
    snprintf(cflag_buf, sizeof cflag_buf, "%s", in_proj ? G.proj.cflags : "-O0");
    if (!strcmp(b->cfg, "Debug")) argv[ac++] = "-g";
    else argv[ac++] = "-DNDEBUG";
    for (char *tok = strtok(cflag_buf, " "); tok && ac < 76; tok = strtok(NULL, " ")) argv[ac++] = tok;
    for (int i = 0; i < n && ac < 76; ++i) argv[ac++] = srcs[i];
    if (in_proj && G.proj.libs[0]) {
        snprintf(lib_buf, sizeof lib_buf, "%s", G.proj.libs);
        int ln = 0;
        for (char *tok = strtok(lib_buf, ","); tok && ln < 16 && ac < 78; tok = strtok(NULL, ",")) {
            while (*tok == ' ') ++tok;
            if (tok[0] == '/') snprintf(lib_tok[ln], sizeof lib_tok[0], "%s", tok);
            else path_join(lib_tok[ln], sizeof lib_tok[0], G.proj.root, tok);
            argv[ac++] = lib_tok[ln++];
        }
    }
    argv[ac++] = "-o"; argv[ac++] = b->exe_path; argv[ac] = NULL;

    char cmd[512]; size_t k = (size_t)snprintf(cmd, sizeof cmd, "$ tcc -I" SDK_INC);
    for (int i = 2; i < ac && k < sizeof cmd - 80; ++i) {
        const char *a = argv[i];
        if (i >= 2 && a[0] != '-' && strcmp(argv[i - 1], "-o") != 0) a = path_base(a);       /* show file names, not full paths */
        k += (size_t)snprintf(cmd + k, sizeof cmd - k, " %s", a);
    }
    app_output_clear();
    G.panel_open = true; G.panel_tab = PANEL_OUTPUT;                 /* the build's output, not a stale terminal */
    app_output_addf("%s\n", cmd);

    b->t0 = cos_time_ms();
    int64_t pid = spawn_logged(tcc_path(), argv, BUILD_LOG);
    if (pid <= 0) {
        app_output_addf("!Cannot start the compiler (%s)\n", tcc_path());
        app_output_add("!Build failed - 1 error, 0 warnings\n", 36);
        b->nerr = 1;
        set_state(B_FAILED);
        return;
    }
    b->pid = pid;
    set_state(B_COMPILING);
}

void build_poll(void) {
    Build *b = &G.bld;
    if (b->st != B_COMPILING && b->st != B_RUNNING) return;
    tail_log();
    int status = 0;
    int64_t r = cos_waitpid(b->pid, &status, false);
    if (r != b->pid) return;
    tail_log();
    if (b->st == B_COMPILING) {
        finish_compile(status);
        G.dirty_frame = true;
    } else {
        flush_partial();
        b->t1 = cos_time_ms();
        b->exit_code = status;
        char secs[24]; fmt_secs(secs, sizeof secs, b->t1 - b->t0);
        app_output_addf("\n[last run]  %s  pid %d  exit code %d   (%s)\n", path_base(b->exe_path), (int)r, status, secs);
        set_state(B_EXITED);
    }
}

void build_stop(void) {
    Build *b = &G.bld;
    if (b->st != B_COMPILING && b->st != B_RUNNING) return;
    cos_kill((int)b->pid, 9);
    int st;
    for (int i = 0; i < 50; ++i) { if (cos_waitpid(b->pid, &st, false) == b->pid) break; cos_sleep_ms(4); }
    tail_log(); flush_partial();
    app_output_addf("\n[stopped]  %s\n", b->st == B_COMPILING ? "build cancelled" : path_base(b->exe_path));
    set_state(B_KILLED);
}

void build_clean(void) {
    char p[160];
    snprintf(p, sizeof p, "%s", G.bld.exe_path);
    if (p[0]) cos_unlink(p);
    app_output_addf("Cleaned %s\n", p[0] ? p : "(nothing to clean)");
}

/* One-shot actions on the file currently being edited: compile to a .o without linking,
 * preprocess and stop, or produce a standalone .c-osll from this one file. None of these
 * is something F5 would "Run" afterward, so they always land in B_EXITED rather than
 * B_RUNNING - but they go through the exact same state machine (finish_compile, the OUTPUT
 * panel, diagnostics parsing) as a normal build, so a syntax error is reported the same way. */
void build_quick(int mode) {
    Build *b = &G.bld;
    if (b->st == B_COMPILING) { app_status("A build is already running"); return; }
    Doc *d = app_doc();
    if (!d || d->untitled || !d->path[0]) { app_status("Open or save a file first"); return; }
    if (d->dirty && !doc_save(d, NULL)) { app_status("Could not save %s", d->name); return; }

    bool in_proj = G.proj.loaded && !strncmp(d->path, G.proj.root, strlen(G.proj.root)) && d->path[strlen(G.proj.root)] == '/';
    char name[64]; snprintf(name, sizeof name, "%s", path_base(d->path));
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    studio_ensure_dirs();
    const char *ext = mode == QUICK_OBJ ? "o" : mode == QUICK_PREPROCESS ? "i" : "c-osll";
    snprintf(b->exe_path, sizeof b->exe_path, "%s/%s.%s", studio_deliverables(), name, ext);
    snprintf(b->src, sizeof b->src, "%s", d->path);

    G.ndiag = 0; b->nerr = b->nwarn = 0; b->pid = 0; b->build_only = true;

    static const char *argv[24]; static char inc_tok[8][256]; static char inc_buf[192];
    int ac = 0;
    argv[ac++] = "tcc.c-os";
    argv[ac++] = "-I" SDK_INC;
    if (in_proj && G.proj.includes[0]) {
        snprintf(inc_buf, sizeof inc_buf, "%s", G.proj.includes);
        int in_ = 0;
        for (char *tok = strtok(inc_buf, ","); tok && in_ < 8 && ac < 20; tok = strtok(NULL, ",")) {
            while (*tok == ' ') ++tok;
            if (tok[0] == '/') snprintf(inc_tok[in_], sizeof inc_tok[0], "-I%s", tok);
            else { char full[256]; path_join(full, sizeof full, G.proj.root, tok); snprintf(inc_tok[in_], sizeof inc_tok[0], "-I%s", full); }
            argv[ac++] = inc_tok[in_++];
        }
    }
    argv[ac++] = mode == QUICK_OBJ ? "-c" : mode == QUICK_PREPROCESS ? "-E" : "-shared";
    argv[ac++] = d->path;
    argv[ac++] = "-o"; argv[ac++] = b->exe_path; argv[ac] = NULL;

    char cmd[300]; size_t k = (size_t)snprintf(cmd, sizeof cmd, "$ tcc %s %s -o %s", argv[ac - 4], path_base(d->path), path_base(b->exe_path));
    (void)k;
    app_output_clear();
    G.panel_open = true; G.panel_tab = PANEL_OUTPUT;
    app_output_addf("%s\n", cmd);

    b->t0 = cos_time_ms();
    int64_t pid = spawn_logged(tcc_path(), argv, BUILD_LOG);
    if (pid <= 0) {
        app_output_addf("!Cannot start the compiler (%s)\n", tcc_path());
        b->nerr = 1;
        set_state(B_FAILED);
        return;
    }
    b->pid = pid;
    set_state(B_COMPILING);
}

const char *build_state_text(char *buf, size_t cap) {
    switch (G.bld.st) {
        case B_COMPILING: snprintf(buf, cap, "Compiling..."); break;
        case B_SAVING:    snprintf(buf, cap, "Saving..."); break;
        case B_RUNNING:   snprintf(buf, cap, "Running pid %d", (int)G.bld.pid); break;
        case B_FAILED:    snprintf(buf, cap, "Build failed"); break;
        case B_KILLED:    snprintf(buf, cap, "Stopped"); break;
        default:          snprintf(buf, cap, "Ready"); break;
    }
    return buf;
}
