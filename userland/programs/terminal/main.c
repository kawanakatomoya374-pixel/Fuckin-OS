/* terminal.c-os - the C-OS terminal: one plain window of text, a prompt at the bottom.
 *
 *  - type a command and press Enter; Up/Down: history; Left/Right/Home/End/Delete: edit; Tab: complete; Ctrl+L: clear; PageUp/PageDown or the wheel: scroll
 *  - built-ins: help pwd cd ls cat mkdir rm mv cp touch echo (with > and >>) free date jobs kill history clear about
 *  - make [help|TARGET|VAR=x]: builds with the Makefile of the current directory using TinyCC (tmake.c; the engine is studio/make.c)
 *  - any program (name.c-os, in the current directory or /bin) is started with its arguments. Its output is shown as it is produced:
 *    C-OS has no pipes, so the program is started with COS_STDOUT=<file> and the terminal reads that file (what C-OS Studio does too).
 *    Programs keep running while you type the next command; Ctrl+C stops the newest one.
 *  - Japanese input works (Ctrl+Space switches it on and off).
 * Events: the kernel delivers Enter, Backspace, the arrows ... as `special` codes with ascii 0 (COS_KEY_*), not as control characters. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "cos.h"
#include "cos_ui.h"
#include "cosime_ui.h"
#include "tmake.h"

#define WIN_W 880
#define WIN_H 540
#define MARGIN 8
#define MAX_LINES 3000
#define CMD_CAP 1024
#define HIST_CAP 64
#define MAX_JOBS 8

static const uint32_t BG = 0x0C0C0C, FG = 0xD8D8D8, DIM = 0x8A8A8A, PROMPT = 0x6FCF7F, CURSOR = 0xE8E8E8;

/* ---------------------------------------------------------------- the text */
static char *lines[MAX_LINES]; static int nlines;
static int scroll;                                   /* lines scrolled up from the bottom */
static bool dirty = true;
static cui_font *font; static int lh, avail_w;       /* line height; width of the text area in pixels */

static void redraw(void) { dirty = true; }
static void add_line(const char *s, int n) {
    if (nlines == MAX_LINES) { free(lines[0]); memmove(&lines[0], &lines[1], sizeof lines[0] * (MAX_LINES - 1)); nlines--; if (scroll) scroll--; }
    char *t = (char *)malloc((size_t)n + 1); if (!t) return; memcpy(t, s, (size_t)n); t[n] = 0; lines[nlines++] = t;
    if (scroll) scroll++;                            /* what you are reading does not move */
    redraw();
}
static int utf8_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }
/* one logical line, wrapped to the window width (tabs become spaces, other control characters are dropped) */
static void log_text(const char *s) {
    char cur[512]; int k = 0, w = 0;
    for (const char *p = s; ; ) {
        unsigned char c = (unsigned char)*p;
        if (c == 0 || c == '\n') { add_line(cur, k); k = 0; w = 0; if (!c) break; p++; continue; }
        if (c == '\r') { p++; continue; }
        if (c == '\t') { int n = 4 - (k % 4); for (int i = 0; i < n && k < 500; i++) cur[k++] = ' '; w = cui_text_width(font, (cur[k] = 0, cur)); p++; continue; }
        if (c < 32 || c == 127) { p++; continue; }
        int n = utf8_len(c); char g[5] = {0}; for (int i = 0; i < n && p[i]; i++) g[i] = p[i];
        int gw = cui_text_width(font, g);
        if ((w + gw > avail_w && k > 0) || k + n >= 500) { add_line(cur, k); k = 0; w = 0; }
        memcpy(cur + k, g, (size_t)n); k += n; w += gw; p += n;
    }
    cur[0] = 0;
}
static void log_line(const char *s) { log_text(s ? s : ""); }
static void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char *fmt, ...) { char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); log_text(b); }
static void clear_screen(void) { for (int i = 0; i < nlines; i++) free(lines[i]); nlines = 0; scroll = 0; redraw(); }

/* ---------------------------------------------------------------- the prompt line */
static char cmd[CMD_CAP]; static int cmd_len, cmd_pos;
static char history[HIST_CAP][CMD_CAP]; static int hist_count, hist_pos = -1;
static char cwd[256] = "/";
static void cmd_set(const char *s) { snprintf(cmd, sizeof cmd, "%s", s); cmd_len = (int)strlen(cmd); cmd_pos = cmd_len; redraw(); }
static void cmd_insert(const char *s) {
    int n = (int)strlen(s); if (cmd_len + n >= CMD_CAP - 1) return;
    memmove(cmd + cmd_pos + n, cmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1)); memcpy(cmd + cmd_pos, s, (size_t)n); cmd_len += n; cmd_pos += n; redraw();
}
static void cmd_erase(int from, int to) { memmove(cmd + from, cmd + to, (size_t)(cmd_len - to + 1)); cmd_len -= to - from; if (cmd_pos > from) cmd_pos = cmd_pos > to ? cmd_pos - (to - from) : from; redraw(); }
static int utf8_next(const char *s, int pos) { return pos + utf8_len((unsigned char)s[pos]); }

/* ---------------------------------------------------------------- paths */
static void path_join(const char *arg, char *out, size_t cap) {
    char tmp[512];
    if (!arg || !*arg) snprintf(tmp, sizeof tmp, "%s", cwd);
    else if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else if (!strcmp(cwd, "/")) snprintf(tmp, sizeof tmp, "/%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
    /* collapse "." and ".." */
    char res[512]; size_t o = 0; res[0] = 0;
    for (char *seg = tmp; *seg; ) {
        while (*seg == '/') seg++; char *e = seg; while (*e && *e != '/') e++;
        size_t l = (size_t)(e - seg);
        if (l == 0) break;
        if (l == 1 && seg[0] == '.') { }
        else if (l == 2 && seg[0] == '.' && seg[1] == '.') { while (o > 0 && res[o - 1] != '/') o--; if (o > 0) o--; res[o] = 0; }
        else if (o + l + 2 < sizeof res) { res[o++] = '/'; memcpy(res + o, seg, l); o += l; res[o] = 0; }
        seg = e;
    }
    snprintf(out, cap, "%s", o ? res : "/");
}

/* ---------------------------------------------------------------- programs and their output */
typedef struct { bool used; int64_t pid; char out[40]; uint64_t off; char name[40]; char part[480]; int plen; } job_t;
static job_t jobs[MAX_JOBS];
static void job_text(job_t *j, const char *buf, size_t n) {          /* output arrives in pieces: complete lines are shown, the rest waits */
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') { j->part[j->plen] = 0; log_text(j->part); { char lb[520]; snprintf(lb, sizeof lb, "[TERMINAL] out: %s\n", j->part); cos_puts(lb); } j->plen = 0; }
        else if (j->plen < (int)sizeof j->part - 1) j->part[j->plen++] = c;
        else { j->part[j->plen] = 0; log_text(j->part); j->plen = 0; j->part[j->plen++] = c; }
    }
}
static void job_read(job_t *j) {
    cos_stat_t st; if (cos_stat(j->out, &st) < 0 || st.size <= j->off) return;
    int fd = cos_open(j->out, COS_O_RDONLY); if (fd < 0) return;
    cos_lseek(fd, (int64_t)j->off); char b[1024]; ssize_t n;
    while ((n = cos_fd_read(fd, b, sizeof b)) > 0) { job_text(j, b, (size_t)n); j->off += (uint64_t)n; }
    cos_close(fd);
}
static void jobs_poll(void) {
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) {
        job_t *j = &jobs[i]; job_read(j);
        int status = 0; int64_t r = cos_waitpid(j->pid, &status, false);
        if (r == j->pid) {
            job_read(j); if (j->plen) { j->part[j->plen] = 0; log_text(j->part); j->plen = 0; }
            if (status != 0) logf("[%s: exit %d]", j->name, status);
            { char lb[96]; snprintf(lb, sizeof lb, "[TERMINAL] exit: %s %d\n", j->name, status); cos_puts(lb); }
            cos_unlink(j->out); j->used = false; redraw();
        }
    }
}
static int jobs_active(void) { int n = 0; for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) n++; return n; }
static bool is_file(const char *p) { cos_stat_t st; return cos_stat(p, &st) >= 0 && !st.is_dir; }
/* name -> the program's path: as given, in the current directory, in /bin; also without the ".c-os" */
/* a file that starts with the ELF signature: run by path it is a Linux program (the kernel decides, see docs/LINUX_COMPAT.md) */
static bool is_elf_file(const char *path) {
    int fd = cos_open(path, COS_O_RDONLY); if (fd < 0) return false;
    unsigned char h[4] = { 0 }; ssize_t n = cos_fd_read(fd, h, 4); cos_close(fd);
    return n == 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
}
static bool find_program(const char *name, char *out, size_t cap) {
    char cand[320]; const char *dirs[3] = { NULL, cwd, "/bin" };
    bool has_slash = strchr(name, '/') != NULL;
    for (int d = 0; d < (has_slash ? 1 : 3); d++) for (int s = 0; s < 2; s++) {
        if (has_slash) path_join(name, cand, sizeof cand);
        else if (d == 0) continue;
        else snprintf(cand, sizeof cand, "%s%s%s", dirs[d], !strcmp(dirs[d], "/") ? "" : "/", name);
        if (s) { size_t l = strlen(cand); if (l + 6 >= sizeof cand) continue; strcpy(cand + l, ".c-os"); }
        size_t l = strlen(cand); if (l > 5 && !strcmp(cand + l - 5, ".c-os") && is_file(cand)) { snprintf(out, cap, "%s", cand); return true; }
        if (has_slash && s == 0 && is_file(cand) && is_elf_file(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    return false;
}
/* arguments that name existing files (or follow -o) are given as absolute paths: a started program does not know this terminal's directory */
static void start_program_ex(const char *path, char **argvs, int argc, bool raw);
static void start_program(const char *path, char **argvs, int argc) { start_program_ex(path, argvs, argc, false); }
static void start_program_ex(const char *path, char **argvs, int argc, bool raw) {
    int slot = -1; for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { slot = i; break; }
    if (slot < 0) { log_line("too many programs are running (jobs, kill)"); return; }
    static char abs_args[24][320]; const char *av[26]; int ac = 0;
    av[ac++] = path;
    for (int i = 1; i < argc && ac < 25; i++) {
        const char *a = argvs[i]; char *dst = abs_args[ac - 1];
        bool after_o = i > 1 && !strcmp(argvs[i - 1], "-o");
        if (!raw && a[0] != '-' && a[0] != '/' && (after_o || strchr(a, '.') || strchr(a, '/'))) {
            char full[320]; path_join(a, full, sizeof full);
            if (after_o || is_file(full)) { snprintf(dst, 320, "%s", full); av[ac++] = dst; continue; }
        }
        av[ac++] = a;
    }
    av[ac] = NULL;
    job_t *j = &jobs[slot]; memset(j, 0, sizeof *j);
    snprintf(j->out, sizeof j->out, "/tmp/.term%d.out", slot); cos_mkdir("/tmp"); cos_write_file(j->out, "", 0);
    char env[80]; snprintf(env, sizeof env, "COS_STDOUT=%s", j->out); const char *envp[] = { env, NULL };
    int64_t pid = cos_spawn_argv(path, av, envp);
    if (pid <= 0) { log_line("cannot start the program"); cos_unlink(j->out); return; }
    j->used = true; j->pid = pid; const char *b = strrchr(path, '/'); snprintf(j->name, sizeof j->name, "%s", b ? b + 1 : path);
    { char lb[200]; snprintf(lb, sizeof lb, "[TERMINAL] started: %s pid=%lld\n", path, (long long)pid); cos_puts(lb); }
}

/* ---------------------------------------------------------------- built-in commands */
static int split_words(char *s, char **v, int max) {
    int n = 0; char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') { v[n++] = ++p; while (*p && *p != '"') ++p; if (*p) *p++ = 0; continue; }
        v[n++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = 0;
    }
    return n;
}
static void do_ls(const char *arg) {
    char path[256]; path_join(arg, path, sizeof path);
    static cos_dirent_ex_t ent[256]; int n = cos_listdir_ex(path, ent, 256);
    if (n < 0) { log_line("ls: cannot read the directory"); return; }
    for (int i = 0; i < n; ++i) {
        if (ent[i].is_dir) logf("%s/", ent[i].name); else logf("%s   %llu", ent[i].name, (unsigned long long)ent[i].size);
    }
    if (!n) log_line("(empty)");
}
static void do_cat(const char *arg) {
    char path[256]; path_join(arg, path, sizeof path);
    static char buf[65536]; ssize_t n = cos_read_file(path, buf, sizeof buf - 1);
    if (n < 0) { log_line("cat: no such file"); return; }
    buf[n] = 0; log_text(buf);
}
static void do_cp(const char *a1, const char *a2) {
    char src[256], dst[256]; path_join(a1, src, sizeof src); path_join(a2, dst, sizeof dst);
    static char *buf; const size_t cap = 1024 * 1024; if (!buf) buf = (char *)malloc(cap);
    if (!buf) { log_line("cp: out of memory"); return; }
    ssize_t n = cos_read_file(src, buf, cap);
    if (n < 0) { log_line("cp: no such file"); return; }
    if ((size_t)n == cap) { log_line("cp: the file is over 1 MiB - use the File Manager"); return; }
    if (cos_write_file(dst, buf, (size_t)n) != (ssize_t)n) log_line("cp: cannot write");
}
static void do_free(void) {
    cos_settings_t st; cos_settings_get(&st);
    logf("memory   %llu MB of %llu MB used", (unsigned long long)(st.mem_used / (1024 * 1024)), (unsigned long long)(st.mem_total / (1024 * 1024)));
    logf("storage  %llu MB of %llu MB used", (unsigned long long)(st.storage_used / (1024 * 1024)), (unsigned long long)(st.storage_total / (1024 * 1024)));
}
static void do_echo(char **v, int n, const char *raw) {
    /* echo TEXT [> FILE | >> FILE]: the text is taken as typed (spaces kept), so a source file can be written from here */
    const char *p = raw; while (*p == ' ') p++; p += 4; while (*p == ' ') p++;       /* after "echo" */
    char text[CMD_CAP]; snprintf(text, sizeof text, "%s", p);
    char *redir = NULL; bool append = false;
    for (char *q = text; *q; q++) if (*q == '>') redir = q;                     /* the LAST '>' is the redirection: the text itself may hold some (C source: >=, ->) */
    if (redir) { if (redir > text && redir[-1] == '>') { append = true; redir[-1] = 0; } if (redir[1] == '>') { append = true; redir[1] = ' '; } *redir = 0; redir++; }
    size_t tl = strlen(text); while (tl && text[tl - 1] == ' ') text[--tl] = 0;
    if (!redir) { log_text(text); return; }
    while (*redir == ' ') redir++; char *fe = redir + strlen(redir); while (fe > redir && fe[-1] == ' ') *--fe = 0;
    if (!*redir) { log_line("echo: a file name is missing after >"); return; }
    char path[256]; path_join(redir, path, sizeof path);
    static char old[65536]; size_t ol = 0; if (append) { ssize_t r = cos_read_file(path, old, sizeof old - 2); if (r > 0) ol = (size_t)r; }
    static char outb[65536 + CMD_CAP]; memcpy(outb, old, ol); size_t k = ol; for (size_t i = 0; text[i]; i++) outb[k++] = text[i]; outb[k++] = '\n';
    if (cos_write_file(path, outb, k) != (ssize_t)k) log_line("echo: cannot write the file");
    (void)v; (void)n;
}
static void do_jobs(void) {
    if (!jobs_active()) { log_line("no programs are running"); return; }
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) logf("%lld  %s", (long long)jobs[i].pid, jobs[i].name);
}
static void do_kill(const char *arg) {
    int64_t pid = arg ? atoll(arg) : 0; bool found = false;
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].pid == pid) found = true;
    if (!found) { log_line("kill: no such running program (see jobs)"); return; }
    cos_kill((int)pid, 15);
}
#include "plugins.inc"
static const char *BUILTINS[] = { "help", "pwd", "cd", "ls", "cat", "mkdir", "rm", "mv", "cp", "touch", "echo", "free", "date", "jobs", "kill", "history", "clear", "about", "run", "make", "kmod", "audit", "lspci", "format-data", "tar", "gzip", "gunzip", "zcat", "zip", "unzip", NULL };
static void execute_command(const char *line0) {
    char line[CMD_CAP]; plug_rewrite(line0, line, sizeof line);
    char work[CMD_CAP], *v[24]; snprintf(work, sizeof work, "%s", line);
    int n = split_words(work, v, 24); if (!n) return;
    if (!strcmp(v[0], "make")) { tm_make(n, v, cwd); return; }
    if (!strcmp(v[0], "plugin") || !strcmp(v[0], "plugins")) { plug_manage(n, v); return; }
    if (plug_command(n, v)) return;
    if (!strcmp(v[0], "help")) {
        log_line("help             this list");
        log_line("pwd / cd DIR     where you are / go to a directory");
        log_line("ls [DIR]         list a directory");
        log_line("cat FILE         show a text file");
        log_line("mkdir DIR / rm PATH / mv A B / cp A B / touch FILE");
        log_line("echo TEXT [> FILE | >> FILE]   print text, or write it to a file");
        log_line("make [TARGET] [VAR=x]   build with the Makefile of this directory, using TinyCC (make help lists its targets, make -h the options)");
        log_line("free / date / about");
        log_line("jobs / kill PID  programs that are running / stop one (Ctrl+C stops the newest)");
        log_line("audit [N]        the last N (default 20) entries of the kernel audit log (module audit.c-osys): process starts/exits, sandbox refusals");
        log_line("lspci            the PCI devices (module pci.c-osys)");
        log_line("tar -czf OUT.tgz DIR / tar -xf FILE [-C DIR] / tar -tf FILE   gzip FILE / gunzip FILE.gz / zcat FILE.gz   zip OUT.zip DIR... / unzip [-l] [-d DIR] FILE.zip   (more: type the command alone)");
        log_line("format-data [exfat|fat32 --yes | cancel]   which file system the data volume has; ask for it to be formatted (erases it) at the next start");
        log_line("kmod list | load PATH [PASSWORD] | unload NAME [PASSWORD]   kernel modules (.c-osys); the password is the administrator password, if one is set");
        log_line("history / clear");
        log_line("plugin [on|off NAME]   the plugins that add commands while the terminal runs (list below when loaded)");
        plug_help();
        log_line("NAME [ARGS]      run NAME.c-os from this directory or /bin   (example:  tcc -v)");
        log_line("keys: Up/Down history, Tab complete, Ctrl+L clear, Ctrl+U erase the line, PageUp/PageDown scroll, Ctrl+Shift+J or Ctrl+Space Japanese input");
        return;
    }
    if (!strcmp(v[0], "clear")) { clear_screen(); return; }
    if (!strcmp(v[0], "pwd")) { log_line(cwd); return; }
    if (!strcmp(v[0], "ls")) { do_ls(n > 1 ? v[1] : "."); return; }
    if (!strcmp(v[0], "cat") && n > 1) { do_cat(v[1]); return; }
    if (!strcmp(v[0], "mkdir") && n > 1) { char p[256]; path_join(v[1], p, sizeof p); if (cos_mkdir(p) != 0) log_line("mkdir: cannot create the directory"); return; }
    if (!strcmp(v[0], "rm") && n > 1) { char p[256]; path_join(v[1], p, sizeof p); if (cos_unlink(p) != 0) log_line("rm: cannot remove (not found, or a directory that is not empty)"); return; }
    if (!strcmp(v[0], "mv") && n > 2) { char a[256], b[256]; path_join(v[1], a, sizeof a); path_join(v[2], b, sizeof b); if (cos_rename(a, b) != 0) log_line("mv: cannot move"); return; }
    if (!strcmp(v[0], "cp") && n > 2) { do_cp(v[1], v[2]); return; }
    if (!strcmp(v[0], "touch") && n > 1) { char p[256]; path_join(v[1], p, sizeof p); if (is_file(p)) return; if (cos_write_file(p, "", 0) != 0) log_line("touch: cannot create the file"); return; }
    if (!strcmp(v[0], "echo")) { do_echo(v, n, line); return; }
    if (!strcmp(v[0], "free")) { do_free(); return; }
    if (!strcmp(v[0], "cd")) {
        char p[256]; path_join(n > 1 ? v[1] : "/", p, sizeof p); cos_stat_t st;
        if (cos_stat(p, &st) >= 0 && st.is_dir) { snprintf(cwd, sizeof cwd, "%s", p); tm_announce(cwd); } else log_line("cd: no such directory"); return;
    }
    if (!strcmp(v[0], "date")) { cos_datetime_t t; if (!cos_rtc_now(&t)) logf("%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour, t.minute, t.second); return; }
    if (!strcmp(v[0], "about")) { log_line("C-OS Terminal"); return; }
    if (!strcmp(v[0], "audit")) {
        long next = cos_syscall3(0x1011, 0, 0, 0);
        if (next < 0) { log_line("audit: the audit module is not loaded (kmod list)"); return; }
        long want = n > 1 ? atol(v[1]) : 20; if (want < 1) want = 20; if (want > 32) want = 32;
        long from = next > want ? next - want : 0;
        static cos_audit_rec_t rec[32];
        long got = cos_syscall3(0x1010, (long)rec, want, from);
        logf("%ld event(s) since the module loaded; showing %ld", next, got < 0 ? 0 : got);
        for (long i = 0; i < got; i++) {
            const cos_audit_rec_t *r = &rec[i];
            if (r->type == 0) logf("#%llu t=%llums pid %u started (parent %llu)", (unsigned long long)r->seq, (unsigned long long)r->time_ms, r->pid, (unsigned long long)r->a);
            else if (r->type == 1) logf("#%llu t=%llums pid %u exited (status %d)", (unsigned long long)r->seq, (unsigned long long)r->time_ms, r->pid, (int)(int64_t)r->b);
            else if (r->type == 2) logf("#%llu t=%llums pid %u: system call %u refused by the sandbox (missing capabilities 0x%llx)", (unsigned long long)r->seq, (unsigned long long)r->time_ms, r->pid, (unsigned)(r->b & 0xFFFF), (unsigned long long)(r->b >> 16));
            else if (r->type == 3) logf("#%llu t=%llums pid %u narrowed its capabilities to 0x%llx", (unsigned long long)r->seq, (unsigned long long)r->time_ms, r->pid, (unsigned long long)r->b);
        }
        return;
    }
    if (!strcmp(v[0], "tar") || !strcmp(v[0], "gzip") || !strcmp(v[0], "gunzip") || !strcmp(v[0], "zcat") || !strcmp(v[0], "zip") || !strcmp(v[0], "unzip")) {
        char *av[28]; int ac = 0; av[ac++] = (char *)"cosarc"; av[ac++] = (char *)"--cwd"; av[ac++] = cwd;
        for (int i = 0; i < n && ac < 27; i++) av[ac++] = v[i];
        start_program_ex("/bin/cosarc.c-os", av, ac, true);        /* the program takes the folder with --cwd and finds its own files; the arguments stay as typed */
        return;
    }
    if (!strcmp(v[0], "format-data")) {
        int q = cos_settings_action(COS_ACTION_FS_QUERY, 0), now = q & 15, pend = (q >> 4) & 15;
        if (n < 2) {
            logf("Data volume: %s", now == 2 ? "exFAT (one file can be bigger than 4 GB)" : now == 1 ? "FAT32 (one file up to 4 GB)" : "not mounted");
            if (pend) logf("Asked for at the next start: format as %s (everything is erased)", pend == 2 ? "exFAT" : "FAT32");
            log_line("usage: format-data exfat --yes | format-data fat32 --yes | format-data cancel");
            log_line("A format erases the whole data volume (files, the Linux system and its apps) and is done at the next start.");
            return;
        }
        if (!strcmp(v[1], "cancel")) { cos_settings_action(COS_ACTION_FS_FORMAT_CANCEL, 0); log_line("The format request was cancelled."); return; }
        int act = !strcmp(v[1], "exfat") ? COS_ACTION_FS_FORMAT_EXFAT : !strcmp(v[1], "fat32") ? COS_ACTION_FS_FORMAT_FAT32 : 0;
        if (!act) { log_line("format-data: expected exfat, fat32 or cancel"); return; }
        if (n < 3 || strcmp(v[2], "--yes")) {
            log_line("This erases the whole data volume (your files, the Linux system and its apps) at the next start.");
            logf("To go on, type: format-data %s --yes", v[1]); return;
        }
        if (cos_settings_action(act, 0) != 0) { log_line("format-data: the request could not be saved."); return; }
        logf("The data volume will be formatted as %s at the next start. Restart the computer to do it, or type: format-data cancel", act == COS_ACTION_FS_FORMAT_EXFAT ? "exFAT" : "FAT32");
        return;
    }
    if (!strcmp(v[0], "lspci")) {
        long cnt = cos_syscall3(0x1020, 0, 0, 0);
        if (cnt < 0) { log_line("lspci: the pci module is not loaded (kmod list)"); return; }
        for (long i = 0; i < cnt && i < 64; i++) {
            cos_pci_info_t pi;
            if (cos_syscall3(0x1021, i, (long)&pi, 0) != 0) break;
            logf("%02x:%02x.%x  %04x:%04x  class %02x%02x", pi.bus, pi.dev, pi.fn, pi.vendor, pi.device, (pi.class_rev >> 24) & 0xFF, (pi.class_rev >> 16) & 0xFF);
        }
        return;
    }
    if (!strcmp(v[0], "kmod")) {
        if (n > 1 && !strcmp(v[1], "list")) { char b[512]; int c = cos_kmod_list(b, sizeof b); logf("%d module(s)", c); char *l = b; while (*l) { char *e = strchr(l, '\n'); if (e) *e = 0; log_line(l); if (!e) break; l = e + 1; } return; }
        if (n > 2 && !strcmp(v[1], "load")) { char p[256]; path_join(v[2], p, sizeof p); int rc = cos_kmod_load(p, n > 3 ? v[3] : NULL); log_line(rc == 0 ? "kmod: loaded" : "kmod: refused (see the kernel log)"); return; }
        if (n > 2 && !strcmp(v[1], "unload")) { int rc = cos_kmod_unload(v[2], n > 3 ? v[3] : NULL); log_line(rc == 0 ? "kmod: unloaded" : rc == -2 ? "kmod: in use" : "kmod: refused or no such module"); return; }
        log_line("usage: kmod list | load PATH [PASSWORD] | unload NAME [PASSWORD]"); return;
    }
    if (!strcmp(v[0], "jobs")) { do_jobs(); return; }
    if (!strcmp(v[0], "kill")) { do_kill(n > 1 ? v[1] : NULL); return; }
    if (!strcmp(v[0], "history")) { for (int i = 0; i < hist_count; i++) logf("%3d  %s", i + 1, history[i]); return; }
    int first = !strcmp(v[0], "run") && n > 1 ? 1 : 0;
    char path[320]; if (find_program(v[first], path, sizeof path)) { start_program(path, v + first, n - first); return; }
    logf("%s: command not found (help)", v[0]);
}
static void submit(void) {
    cmd[cmd_len] = 0;
    { char l[CMD_CAP + 300]; snprintf(l, sizeof l, "%s> %s", cwd, cmd); log_text(l); }
    { char lb[CMD_CAP + 40]; snprintf(lb, sizeof lb, "[TERMINAL] cmd: %s\n", cmd); cos_puts(lb); }
    if (cmd_len) {
        if (!hist_count || strcmp(history[hist_count - 1], cmd)) {
            if (hist_count == HIST_CAP) { memmove(history, history + 1, sizeof history[0] * (HIST_CAP - 1)); hist_count--; }
            snprintf(history[hist_count++], CMD_CAP, "%s", cmd);
        }
    }
    hist_pos = -1; char line[CMD_CAP]; snprintf(line, sizeof line, "%s", cmd); cmd_len = cmd_pos = 0; cmd[0] = 0; scroll = 0;
    execute_command(line); redraw();
}

/* ---------------------------------------------------------------- Tab completion */
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void complete(void) {
    int ws = cmd_pos; while (ws > 0 && cmd[ws - 1] != ' ') ws--;                  /* start of the word being typed */
    char word[256]; int wl = cmd_pos - ws; if (wl <= 0 || wl >= 256) return; memcpy(word, cmd + ws, (size_t)wl); word[wl] = 0;
    bool first_word = true; for (int i = 0; i < ws; i++) if (cmd[i] != ' ') first_word = false;
    static char cand[64][128]; static const char *ptrs[64]; int nc = 0;
    #define ADD(s) do { if (nc < 64 && !strncmp((s), word + (strrchr(word, '/') ? strrchr(word, '/') + 1 - word : 0), wl - (strrchr(word, '/') ? (int)(strrchr(word, '/') + 1 - word) : 0))) { snprintf(cand[nc], 128, "%s", (s)); nc++; } } while (0)
    if (first_word && !strchr(word, '/')) {
        for (int i = 0; BUILTINS[i]; i++) ADD(BUILTINS[i]);
        static cos_dirent_ex_t ent[256]; int n = cos_listdir_ex("/bin", ent, 256);
        for (int i = 0; i < n; i++) { size_t l = strlen(ent[i].name); if (l > 5 && !strcmp(ent[i].name + l - 5, ".c-os")) { char nm[128]; snprintf(nm, sizeof nm, "%.*s", (int)(l - 5), ent[i].name); ADD(nm); } }
    } else {
        char dir[256], base[256]; const char *sl = strrchr(word, '/');
        if (sl) { snprintf(base, sizeof base, "%s", sl + 1); char d[256]; snprintf(d, sizeof d, "%.*s", (int)(sl - word) + 1, word); path_join(d, dir, sizeof dir); }
        else { snprintf(base, sizeof base, "%s", word); snprintf(dir, sizeof dir, "%s", cwd); }
        static cos_dirent_ex_t ent[256]; int n = cos_listdir_ex(dir, ent, 256);
        for (int i = 0; i < n; i++) { char nm[140]; snprintf(nm, sizeof nm, "%s%s", ent[i].name, ent[i].is_dir ? "/" : ""); ADD(nm); }
    }
    #undef ADD
    if (!nc) return;
    for (int i = 0; i < nc; i++) ptrs[i] = cand[i];
    qsort(ptrs, (size_t)nc, sizeof ptrs[0], cmp_str);
    const char *bs = strrchr(word, '/'); int blen = bs ? wl - (int)(bs + 1 - word) : wl;     /* what was typed of the last name */
    size_t common = strlen(ptrs[0]); for (int i = 1; i < nc; i++) { size_t k = 0; while (k < common && ptrs[i][k] == ptrs[0][k]) k++; common = k; }
    if ((int)common > blen) { char add[128]; snprintf(add, sizeof add, "%.*s", (int)common - blen, ptrs[0] + blen); cmd_insert(add); }
    if (nc > 1 && (int)common <= blen) { char l[600] = ""; for (int i = 0; i < nc && strlen(l) < 500; i++) { strlcat(l, ptrs[i], sizeof l); strlcat(l, "   ", sizeof l); } log_text(l); }
    else if (nc == 1 && ptrs[0][strlen(ptrs[0]) - 1] != '/') cmd_insert(" ");
}

/* ---------------------------------------------------------------- drawing */
static void draw(cui_canvas *c, int w, int h) {
    cui_fill(c, 0, 0, w, h, BG);
    int rows = (h - 2 * MARGIN) / lh; if (rows < 2) rows = 2;
    int total = nlines + 1; int start = total - rows - scroll; if (start < 0) start = 0;
    for (int r = 0; r < rows; r++) {
        int idx = start + r; int y = MARGIN + r * lh;
        if (idx < nlines) { cui_text(c, font, MARGIN, y, lines[idx], FG); continue; }
        if (idx > nlines) break;
        char pr[300]; snprintf(pr, sizeof pr, "%s> ", cwd); int pw = cui_text_width(font, pr);
        cui_text(c, font, MARGIN, y, pr, PROMPT);
        char before[CMD_CAP]; snprintf(before, sizeof before, "%.*s", cmd_pos, cmd);
        int bw = cui_text_width(font, before), full = cui_text_width(font, cmd), shift = 0;
        if (pw + bw > avail_w - 6) shift = pw + bw - (avail_w - 6);                       /* the end of a long line stays visible */
        cui_clip(c, MARGIN + pw, y, avail_w - pw, lh);
        cui_text(c, font, MARGIN + pw - shift, y, cmd, FG);
        cui_fill(c, MARGIN + pw + bw - shift, y + 1, 2, lh - 3, CURSOR); (void)full;
        cui_unclip(c);
        imeui_indicator(c, font, w - 40, h - lh - 4); imeui_draw(c, font, MARGIN + pw + bw - shift, y, lh);
    }
    if (scroll) cui_text(c, font, w - 150, 4, "(scrolled: End)", DIM);
}

int main(void) {
    cos_win_info_t wi; int64_t win = cos_win2_create("Terminal", WIN_W, WIN_H, &wi); if (win <= 0) return 1;
    cui_canvas c; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    font = cui_font_default(15); lh = cui_font_height(font) + 3; avail_w = wi.width - 2 * MARGIN;
    tm_init(log_line);
    log_line("C-OS Terminal   (help: the commands)");
    plug_init();
    for (;;) {
        cos_win_event_t ev; int r = cos_win2_wait(win, &ev, 100);
        jobs_poll(); plug_poll();
        if (tm_busy() && tm_tick()) redraw();
        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) break;
            if (ev.type == COS_EV_WHEEL) { scroll += ev.wheel > 0 ? 3 : -3; if (scroll < 0) scroll = 0; if (scroll > nlines) scroll = nlines; redraw(); }
            else if (ev.type == COS_EV_KEY) {
                char commit[1024]; int ir = imeui_key(&ev, commit, sizeof commit);                 /* Japanese input first */
                if (ir & COSIME_COMMIT) cmd_insert(commit);
                redraw();
                bool ctrl = (ev.mods & COS_MOD_CTRL) != 0;
                if (ir & COSIME_CONSUMED) { }
                else if (ctrl && (ev.ascii == 'l' || ev.ascii == 'L')) clear_screen();
                else if (ctrl && (ev.ascii == 'u' || ev.ascii == 'U')) cmd_erase(0, cmd_pos);
                else if (ctrl && (ev.ascii == 'a' || ev.ascii == 'A')) { cmd_pos = 0; redraw(); }
                else if (ctrl && (ev.ascii == 'e' || ev.ascii == 'E')) { cmd_pos = cmd_len; redraw(); }
                else if (ctrl && (ev.ascii == 'c' || ev.ascii == 'C') && tm_busy()) { tm_stop(); redraw(); }
                else if (ctrl && (ev.ascii == 'c' || ev.ascii == 'C')) {
                    int newest = -1; for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && (newest < 0 || jobs[i].pid > jobs[newest].pid)) newest = i;
                    if (newest >= 0) { cos_kill((int)jobs[newest].pid, 15); logf("^C  (%s)", jobs[newest].name); } else { log_line("^C"); cmd_len = cmd_pos = 0; cmd[0] = 0; }
                }
                else if (ctrl && (ev.ascii == 'v' || ev.ascii == 'V')) { char b[CMD_CAP]; int64_t n = cos_clip_get(b, sizeof b - 1); if (n > 0) { b[n] = 0; for (char *q = b; *q; q++) if (*q == '\n' || *q == '\r') *q = ' '; cmd_insert(b); } }
                else if (ev.special == COS_KEY_ENTER || ev.ascii == '\n' || ev.ascii == '\r') submit();
                else if (ev.special == COS_KEY_BACKSPACE || ev.ascii == 8 || ev.ascii == 127) { if (cmd_pos > 0) cmd_erase((int)imeui_utf8_prev(cmd, (size_t)cmd_pos), cmd_pos); }
                else if (ev.special == COS_KEY_DELETE) { if (cmd_pos < cmd_len) cmd_erase(cmd_pos, utf8_next(cmd, cmd_pos)); }
                else if (ev.special == COS_KEY_LEFT) { if (cmd_pos > 0) cmd_pos = (int)imeui_utf8_prev(cmd, (size_t)cmd_pos); redraw(); }
                else if (ev.special == COS_KEY_RIGHT) { if (cmd_pos < cmd_len) cmd_pos = utf8_next(cmd, cmd_pos); redraw(); }
                else if (ev.special == COS_KEY_HOME) { cmd_pos = 0; redraw(); }
                else if (ev.special == COS_KEY_END) { cmd_pos = cmd_len; scroll = 0; redraw(); }
                else if (ev.special == COS_KEY_UP) { if (hist_count) { if (hist_pos < 0) hist_pos = hist_count - 1; else if (hist_pos > 0) hist_pos--; cmd_set(history[hist_pos]); } }
                else if (ev.special == COS_KEY_DOWN) { if (hist_pos >= 0) { if (hist_pos < hist_count - 1) cmd_set(history[++hist_pos]); else { hist_pos = -1; cmd_set(""); } } }
                else if (ev.special == COS_KEY_PGUP) { scroll += (WIN_H / lh) - 2; if (scroll > nlines) scroll = nlines; redraw(); }
                else if (ev.special == COS_KEY_PGDN) { scroll -= (WIN_H / lh) - 2; if (scroll < 0) scroll = 0; redraw(); }
                else if (ev.special == COS_KEY_TAB) complete();
                else if (ev.ascii >= 32 && ev.ascii < 127 && !ctrl) { char s[2] = { ev.ascii, 0 }; cmd_insert(s); scroll = 0; }
            }
        }
        if (dirty) { draw(&c, wi.width, wi.height); cos_win2_present(win); dirty = false; }
    }
    cos_plugins_unload_all(&plugs);
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) cos_unlink(jobs[i].out);
    cos_win2_close(win); return 0;
}
