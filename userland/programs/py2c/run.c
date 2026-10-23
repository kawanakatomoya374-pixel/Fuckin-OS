/* run.c - Py2C Studio's pipeline: convert (Python -> C, or C -> Python), build with tcc.c-os against the Py2C runtime library, run with the output
 * captured into the Output panel, stop, and the helper views (AST dump, supported language). */
#include "p2.h"
#include "core/python_code_to_c.h"
#include "tools/c2py.h"

#define TCC_PATH    "/bin/tcc.c-os"
#define SDK_INC     "/system/sdk/include"
#define SDK_INC_P2C "/system/sdk/include/py2c"
#define SDK_RT      "/system/sdk/lib/libp2crt.a"
#define GEN_C       "/tmp/p2c_run.c"
#define RUN_EXE     "/tmp/p2c_run.c-os"
#define BUILD_LOG   "/tmp/p2c_build.log"
#define RUN_LOG     "/tmp/p2c_run.log"
#define STDIN_FILE  "/tmp/p2c_in.txt"

bool plug_filter_src(char *py, size_t cap);
void plug_filter_out(char *c, size_t cap);

/* the transpiler allocates through these (the 64 KiB static fallback heap of the core would limit the size of what can be converted) */
static void *al_alloc(size_t n, void *u) { (void)u; return malloc(n); }
static void *al_realloc(void *p, size_t o, size_t n, void *u) { (void)u; (void)o; return realloc(p, n); }
static void al_free(void *p, size_t n, void *u) { (void)u; (void)n; free(p); }
void p2c_platform_set_allocator(void *(*)(size_t, void *), void *(*)(void *, size_t, size_t, void *), void (*)(void *, size_t, void *), void *);
static bool s_alloc_set;

bool run_busy(void) { return A.bs == B_CONVERTING || A.bs == B_COMPILING || A.bs == B_RUNNING; }

/* ---------------------------------------------------------------- the helper panels */
static Doc *pdoc(int tab) { return A.panel_docs[tab]; }
void panel_out_clear(void) { Doc *d = pdoc(PANEL_OUTPUT); doc_set_text(d, "", 0); A.tail_pos = 0; A.line_len = 0; }
void panel_out_add(const char *s, size_t n) {
    Doc *d = pdoc(PANEL_OUTPUT);
    if (d->len + n > OUT_CAP) {                                           /* keep the newest half */
        size_t drop = d->len / 2; while (drop < d->len && d->t[drop] != '\n') drop++;
        memmove(d->t, d->t + drop, d->len - drop); d->len -= drop; d->t[d->len] = 0; d->ver++;
    }
    bool follow = d->top + A.pv.vis_lines >= (int)doc_lines(d) - 1;
    if (d->len + n + 1 > d->cap) {
        size_t nc = d->cap; while (nc < d->len + n + 1) nc *= 2;
        char *nt = (char *)realloc(d->t, nc); if (!nt) return;
        d->t = nt; d->cap = nc;
    }
    memcpy(d->t + d->len, s, n); d->len += n; d->t[d->len] = 0; d->ver++;
    d->caret = d->anchor = d->len; d->dirty = false;
    if (follow) { size_t nl = doc_lines(d); d->top = (int)nl > A.pv.vis_lines ? (int)nl - A.pv.vis_lines : 0; }
    A.dirty = true;
}
static void out_line(const char *s) { panel_out_add(s, strlen(s)); panel_out_add("\n", 1); }
static void out_linef(const char *fmt, ...) { char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); out_line(b); }

void panel_fill_lang(void) {
    Doc *d = pdoc(PANEL_LANG);
    if (d->len) return;
    const char *s = p2c_supported_range_string();
    doc_set_text(d, s, strlen(s));
}
void conv_ast(void) {
    Doc *d = app_doc(); if (!d) return;
    if (d->lang == L_C) { app_status("The syntax tree is for Python sources"); return; }
    if (!s_alloc_set) { p2c_platform_set_allocator(al_alloc, al_realloc, al_free, NULL); s_alloc_set = true; }
    char *dump = NULL; P2C_Result r = python_to_ast_dump(d->t, &dump);
    Doc *a = pdoc(PANEL_AST);
    if (r == P2C_OK && dump) { doc_set_text(a, dump, strlen(dump)); free(dump); app_status("Syntax tree of %s", d->name); }
    else { const char *e = p2c_last_error_details(); doc_set_text(a, e && e[0] ? e : "(no syntax tree)", strlen(e && e[0] ? e : "(no syntax tree)")); app_status("Could not parse %s", d->name); }
    app_set_panel(PANEL_AST); A.opt.show_panel = true; app_relayout();
}

/* ---------------------------------------------------------------- convert */
static void set_problem(int line, int col, const char *msg) {
    A.nprobs = 1; A.prob_sel = 0; A.prob_scroll = 0;
    A.probs[0].line = line; A.probs[0].col = col; A.probs[0].sev = 0;
    snprintf(A.probs[0].msg, sizeof A.probs[0].msg, "%s", msg);
}
static void parse_error(const char *e, int *line, int *col, char *msg, size_t cap) {
    *line = 0; *col = 0; msg[0] = 0;
    const char *p = strstr(e, "line ");
    if (p) { *line = atoi(p + 5); const char *c = strstr(p, "col "); if (c) *col = atoi(c + 4); }
    const char *m = strstr(e, ": "); const char *nl = strchr(e, '\n');
    if (m && (!nl || m < nl)) { m += 2; size_t n = nl ? (size_t)(nl - m) : strlen(m); if (n >= cap) n = cap - 1; memcpy(msg, m, n); msg[n] = 0; }
    else snprintf(msg, cap, "%.*s", (int)(nl ? (size_t)(nl - e) : strlen(e)), e);
}

void conv_run(void) {
    Doc *d = app_doc();
    if (!d) { app_status("Open or create a file first"); return; }
    A.bs = A.bs == B_RUNNING ? B_RUNNING : B_CONVERTING;
    if (!s_alloc_set) { p2c_platform_set_allocator(al_alloc, al_realloc, al_free, NULL); s_alloc_set = true; }
    uint64_t t0 = cos_time_ms();
    d->err_line = 0; A.nprobs = 0;
    Doc *out = A.cdoc;
    char *result = NULL; bool ok = false; char errmsg[600] = "";
    if (d->lang == L_C) {
        if (c_to_python(d->t, &result) == 0 && result) ok = true; else { snprintf(errmsg, sizeof errmsg, "%s", result ? result : "C to Python conversion failed"); }
    } else {
        /* plugins see (and may change) a copy of the source, not the document */
        char *src = (char *)malloc(d->len + 4096); if (!src) { app_status("Out of memory"); A.bs = B_IDLE; return; }
        memcpy(src, d->t, d->len + 1);
        if (plug_filter_src(src, d->len + 4096)) out_line("[plugins changed the source before the conversion]");
        P2C_TranspileOptions o = P2C_DEFAULT_TRANSPILER_OPTIONS;
        o.debug_comments = A.opt.comments; o.strict_c11 = A.opt.strict_c11; o.fallback_unsupported = A.opt.fallback;
        o.embed_entry = A.opt.embed[0] ? A.opt.embed : NULL;
        P2C_Result r = python_to_c(src, &o, &result);
        free(src);
        if (r == P2C_OK && result) ok = true;
        else { const char *e = p2c_last_error_details(); snprintf(errmsg, sizeof errmsg, "%s: %s", p2c_result_to_string(r), e && e[0] ? e : "unknown error"); if (result) { free(result); result = NULL; } }
    }
    A.conv_ms = cos_time_ms() - t0;
    snprintf(A.cdoc_for, sizeof A.cdoc_for, "%s", d->path[0] ? d->path : d->name);
    if (ok) {
        size_t n = strlen(result);
        if (d->lang != L_C) {
            char *buf = (char *)malloc(n + 65536);                       /* room for the plugins' output filters */
            if (buf) { memcpy(buf, result, n + 1); plug_filter_out(buf, n + 65536); free(result); result = buf; n = strlen(result); }
        }
        doc_set_text(out, result, n); out->lang = d->lang == L_C ? L_PY : L_C; out->readonly = true; out->untitled = false;
        { const char *dot = strrchr(d->name, '.'); size_t stem = dot ? (size_t)(dot - d->name) : strlen(d->name); snprintf(out->name, sizeof out->name, "%.*s%s", (int)stem, d->name, d->lang == L_C ? ".py" : ".c"); }
        free(result);
        A.conv_ok = 1; A.conv_bytes = n; A.nprobs = 0;
        size_t lines = doc_lines(out);
        app_status("Converted %s: %d lines, %d bytes (%d ms)", d->name, (int)lines, (int)n, (int)A.conv_ms);
        out_linef("[convert] %s -> %s  %d lines, %d bytes, %d ms", d->name, out->name, (int)lines, (int)n, (int)A.conv_ms);
        slog("convert ok %s lines=%d", out->name, (int)lines);
        if (!A.opt.split_c) A.opt.split_c = true;
        app_relayout();
    } else {
        int line, col; char msg[300]; parse_error(errmsg, &line, &col, msg, sizeof msg);
        A.conv_ok = 0;
        set_problem(line, col, msg[0] ? msg : "conversion failed");
        if (line > 0) d->err_line = line;
        doc_set_text(out, errmsg, strlen(errmsg)); out->lang = L_TEXT; out->readonly = true;
        snprintf(out->name, sizeof out->name, "error");
        app_status("Conversion failed: %s", msg);
        out_linef("[convert] FAILED: %s%s%d%s%s", msg, line ? " (line " : "", line, line ? ")" : "", "");
        slog("convert failed line=%d col=%d: %s", line, col, msg);
        app_set_panel(PANEL_PROBLEMS); A.opt.show_panel = true;
        if (line > 0) { view_goto_line(&A.ed, line); }
        if (!A.opt.split_c) { A.opt.split_c = true; }
        app_relayout();
    }
    A.cv_view.d = out; out->top = 0; out->left = 0; out->caret = out->anchor = 0;
    if (A.bs == B_CONVERTING) A.bs = B_IDLE;
    A.dirty = true;
}

void conv_save_c(void) {
    if (A.conv_ok != 1) { conv_run(); if (A.conv_ok != 1) return; }
    app_status("Choose where to save the converted text");
    dlg_open(DLG_SAVE_C);
}

/* ---------------------------------------------------------------- build and run */
static void feed_line(const char *line) {
    panel_out_add(line, strlen(line)); panel_out_add("\n", 1); slog("out: %s", line);
    const char *g = strstr(line, "p2c_run.c:");                 /* a compiler error in the generated file: say which line of it */
    if (g && A.cdoc) { int n = atoi(g + 10); if (n > 0 && (size_t)n <= doc_lines(A.cdoc)) { size_t s = doc_line_start(A.cdoc, (size_t)n - 1), e = doc_line_end(A.cdoc, (size_t)n - 1); slog("generated line %d: %.*s", n, (int)(e - s > 200 ? 200 : e - s), A.cdoc->t + s); } }
}
static void tail_log(void) {
    cos_stat_ex_t st;
    if (cos_stat_ex(A.out_path, &st) != 0 || st.size <= A.tail_pos) return;
    int fd = cos_open(A.out_path, COS_O_RDONLY);
    if (fd < 0) return;
    cos_lseek(fd, (int64_t)A.tail_pos);
    char chunk[1024];
    for (;;) {
        ssize_t n = cos_fd_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        A.tail_pos += (size_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n' || A.line_len >= (int)sizeof A.line_acc - 1) {
                A.line_acc[A.line_len] = 0; feed_line(A.line_acc); A.line_len = 0;
                if (c != '\n') A.line_acc[A.line_len++] = c;
            } else A.line_acc[A.line_len++] = c;
        }
    }
    cos_close(fd);
}
static void flush_partial(void) { if (A.line_len) { A.line_acc[A.line_len] = 0; feed_line(A.line_acc); A.line_len = 0; } }
static int64_t spawn_logged(const char *exe, const char *const *argv, const char *log, const char *stdin_path) {
    char env1[160], env2[160];
    snprintf(env1, sizeof env1, "COS_STDOUT=%s", log);
    const char *envp[3] = { env1, NULL, NULL };
    if (stdin_path) { snprintf(env2, sizeof env2, "COS_STDIN=%s", stdin_path); envp[1] = env2; }
    { cos_unlink(log); int lf = cos_open(log, COS_O_WRONLY_CREATE); if (lf >= 0) cos_close(lf); }   /* start empty: the child appends (a zero-length write_file is refused, so it cannot truncate) */
    A.tail_pos = 0; A.line_len = 0;
    snprintf(A.out_path, sizeof A.out_path, "%s", log);
    return cos_spawn_argv(exe, argv, envp);
}
static bool s_run_after;
static char s_target[200];

static void start_run(void) {
    Doc *in = A.panel_docs[PANEL_INPUT];
    p2_write_file(STDIN_FILE, in->t, in->len);
    const char *argv[] = { A.exe_path, NULL };
    A.t0 = cos_time_ms();
    A.pid = spawn_logged(A.exe_path, argv, RUN_LOG, STDIN_FILE);
    if (A.pid <= 0) { out_linef("Could not start %s", A.exe_path); A.bs = B_FAILED; return; }
    A.bs = B_RUNNING;
    out_linef("[run] %s", A.exe_path);
}

void run_start(bool exec) {
    Doc *d = app_doc();
    if (!d) { app_status("Open or create a file first"); return; }
    if (d->lang == L_C) { app_status("Run is for Python sources (this is a C file: Convert makes Python)"); return; }
    if (run_busy()) run_stop();
    if (A.opt.autosave_run && d->dirty && !d->untitled && d->path[0]) doc_save(d, NULL);
    conv_run();
    if (A.conv_ok != 1) return;
    if (A.opt.embed[0]) { app_status("An embed entry makes a function, not a program: clear it in Options to run"); out_line("[run] skipped: --embed-entry is set (no main())"); app_set_panel(PANEL_OUTPUT); return; }
    p2_write_file(GEN_C, A.cdoc->t, A.cdoc->len);
    s_run_after = exec;
    const char *stem = d->name; char nm[64]; snprintf(nm, sizeof nm, "%s", stem); char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
    if (!nm[0]) snprintf(nm, sizeof nm, "program");
    if (exec) snprintf(A.exe_path, sizeof A.exe_path, "%s", RUN_EXE);
    else {
        cos_mkdir("/C-OS Studio"); cos_mkdir("/C-OS Studio/deliverables");
        snprintf(A.exe_path, sizeof A.exe_path, "/C-OS Studio/deliverables/%s.c-os", nm);
    }
    snprintf(s_target, sizeof s_target, "%s", nm);
    panel_out_clear(); app_set_panel(PANEL_OUTPUT); A.opt.show_panel = true; app_relayout();
    out_line("$ tcc.c-os -I" SDK_INC " -I" SDK_INC_P2C " " GEN_C " " SDK_RT " -o <exe>");
    static const char *argv[10];
    int ac = 0;
    argv[ac++] = "tcc.c-os"; argv[ac++] = "-I" SDK_INC; argv[ac++] = "-I" SDK_INC_P2C; argv[ac++] = "-O0";
    argv[ac++] = GEN_C; argv[ac++] = SDK_RT; argv[ac++] = "-o"; argv[ac++] = A.exe_path; argv[ac] = NULL;
    A.t0 = cos_time_ms();
    A.pid = spawn_logged(TCC_PATH, argv, BUILD_LOG, NULL);
    if (A.pid <= 0) { out_line("!Cannot start the compiler (/bin/tcc.c-os)"); A.bs = B_FAILED; app_status("Cannot start tcc.c-os"); return; }
    A.bs = B_COMPILING;
    app_status("Compiling %s ...", d->name);
}

static void fmt_secs(char *out, size_t cap, uint64_t ms) { snprintf(out, cap, "%d.%02d s", (int)(ms / 1000), (int)((ms % 1000) / 10)); }

void run_poll(void) {
    if (A.bs != B_COMPILING && A.bs != B_RUNNING) return;
    tail_log();
    int status = 0;
    int64_t r = cos_waitpid(A.pid, &status, false);
    if (r != A.pid) { A.dirty = true; return; }
    tail_log(); flush_partial();
    A.t1 = cos_time_ms();
    char secs[24]; fmt_secs(secs, sizeof secs, A.t1 - A.t0);
    if (A.bs == B_COMPILING) {
        if (status != 0) {
            out_linef("!Build failed (tcc exit %d, %s)", status, secs); slog("build failed %d", status);
            A.bs = B_FAILED; app_status("Build failed - see Output"); A.dirty = true; return;
        }
        out_linef("Build succeeded (%s)", secs); slog("build ok");
        if (s_run_after) { start_run(); app_status("Running %s", s_target); }
        else { A.bs = B_EXITED; out_linef("Saved %s", A.exe_path); app_status("Built %s", A.exe_path); explorer_refresh(); }
    } else {
        A.exit_code = status;
        out_linef("\n[exited]  code %d   (%s)", status, secs); slog("exited code=%d", status);
        A.bs = B_EXITED; app_status("Program exited with code %d (%s)", status, secs);
    }
    A.dirty = true;
}

void run_stop(void) {
    if (A.bs != B_COMPILING && A.bs != B_RUNNING) return;
    cos_kill((int)A.pid, 9);
    int st;
    for (int i = 0; i < 50; ++i) { if (cos_waitpid(A.pid, &st, false) == A.pid) break; cos_sleep_ms(4); }
    tail_log(); flush_partial();
    out_linef("\n[stopped]  %s", A.bs == B_COMPILING ? "build cancelled" : "program stopped");
    A.bs = B_KILLED; app_status("Stopped"); A.dirty = true;
}

void run_open_studio(void) {
    if (A.conv_ok != 1) { conv_run(); if (A.conv_ok != 1) return; }
    Doc *d = app_doc();
    char path[256];
    if (d && d->path[0]) { snprintf(path, sizeof path, "%s", d->path); char *dot = strrchr(path, '.'); if (dot) *dot = 0; }
    else snprintf(path, sizeof path, "/desktop/generated");
    strncat(path, A.cdoc->lang == L_PY ? ".py" : ".c", sizeof path - strlen(path) - 1);
    if (p2_write_file(path, A.cdoc->t, A.cdoc->len) != (ssize_t)A.cdoc->len) { app_status("Could not save %s", path); return; }
    const char *argv[] = { "C-OS Studio.c-os", path, NULL };
    int64_t pid = cos_spawn_argv("/C-OS Studio/C-OS Studio.c-os", argv, NULL);
    app_status(pid > 0 ? "Opened %s in C-OS Studio" : "Could not start C-OS Studio for %s", path);
    explorer_refresh();
}

/* The whole Py2C source tree lives in /apps/Py2C (a cosproj.txt and a Makefile for tcc are there): open it in C-OS Studio, or build it
 * completely - tool, runtime library and install (`studio /apps/Py2C --make install`) - and watch it in Studio's terminal. */
static bool fs_is_dir_p2(const char *p) { cos_stat_t st; return cos_stat(p, &st) == 0 && st.is_dir; }
void run_open_source(bool build) {
    if (!fs_is_dir_p2("/apps/Py2C")) { app_status("The Py2C source tree is not installed (/apps/Py2C)"); return; }
    const char *argv_open[] = { "C-OS Studio.c-os", "/apps/Py2C", NULL };
    const char *argv_make[] = { "C-OS Studio.c-os", "/apps/Py2C", "--make", "install", NULL };
    int64_t pid = cos_spawn_argv("/C-OS Studio/C-OS Studio.c-os", build ? argv_make : argv_open, NULL);
    app_status(pid > 0 ? (build ? "Building Py2C with tcc in C-OS Studio (make install)" : "Opened the Py2C source in C-OS Studio") : "Could not start C-OS Studio");
}
