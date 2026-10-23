/* host_shim.c - lets C-OS Studio run as an ordinary Linux process for testing.
 *
 * It implements the few cos_* calls Studio uses on top of POSIX, and replaces
 * the window with a scripted one: cos_win2_wait() plays events from the file
 * named by STUDIO_SCRIPT and "snap FILE.ppm" dumps the pixel buffer, so the
 * editor can be driven and screenshotted with no display and no OS boot.
 * NOT part of the .c-os build.
 *
 * Script commands (one per line, # comments):
 *   key [ctrl+|shift+|alt+]NAME   NAME: enter esc tab bs del up down left right home end pgup pgdn f1..f12 or one char
 *   type TEXT                      types the rest of the line, character by character
 *   click X Y [l|r|m]   dclick X Y   move X Y   drag X1 Y1 X2 Y2   wheel N
 *   open PATH                      the OS asks Studio to open a file (COS_EV_OPEN)
 *   wait MS   snap FILE   quit
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "cos.h"

/* ---- time / env / misc ---- */
uint64_t cos_time_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
int cos_rtc_now(cos_datetime_t *out) {
    if (!out) return -1;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    out->year = (uint32_t)(tmv.tm_year + 1900); out->month = (uint32_t)(tmv.tm_mon + 1); out->day = (uint32_t)tmv.tm_mday;
    out->hour = (uint32_t)tmv.tm_hour; out->minute = (uint32_t)tmv.tm_min; out->second = (uint32_t)tmv.tm_sec;
    return 0;
}
static int s_theme = 1, s_font_scale = 1, s_wallpaper = 0, s_dark = 1, s_jp = 0, s_icon = 64;
int cos_settings_get(cos_settings_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->mem_total = 4ull * 1024 * 1024 * 1024; out->mem_used = 612ull * 1024 * 1024; out->mem_free = out->mem_total - out->mem_used;
    out->storage_total = 20ull * 1024 * 1024 * 1024; out->storage_used = 3ull * 1024 * 1024 * 1024; out->storage_free = out->storage_total - out->storage_used;
    snprintf(out->cpu_vendor, sizeof out->cpu_vendor, "%s", "x86-64 Compatible (host test)");
    snprintf(out->os_version, sizeof out->os_version, "%s", "4.0.9 Alpha");
    snprintf(out->build_info, sizeof out->build_info, "%s", "C-OS host test build");
    out->screen_w = 1024; out->screen_h = 768;
    out->net_connected = 1;
    snprintf(out->net_ip, sizeof out->net_ip, "10.0.2.15");
    snprintf(out->net_mac, sizeof out->net_mac, "52:54:00:12:34:56");
    snprintf(out->net_iface, sizeof out->net_iface, "eth0");
    out->theme_idx = s_theme; out->font_scale = s_font_scale; out->wallpaper_idx = s_wallpaper; out->wallpaper_count = 4;
    out->dark_mode = s_dark; out->japanese = s_jp; out->icon_size = s_icon; out->file_count = 128;
    return 0;
}
int cos_settings_set(int key, int value) {
    switch (key) {
        case COS_SET_THEME: s_theme = value; return 0;
        case COS_SET_FONT_SCALE: s_font_scale = value; return 0;
        case COS_SET_WALLPAPER: s_wallpaper = value; return 0;
        case COS_SET_DARK_MODE: s_dark = value; return 0;
        case COS_SET_LANGUAGE: s_jp = value; return 0;
        case COS_SET_ICON_SIZE: s_icon = value; return 0;
        default: return -1;
    }
}
char *cos_getenv(const char *n) { return getenv(n); }
void cos_sleep_ms(uint64_t ms) { usleep((useconds_t)(ms * 1000)); }
void cos_exit(int s) { exit(s); }
int cos_printf(const char *fmt, ...) { va_list a; va_start(a, fmt); int n = vprintf(fmt, a); va_end(a); return n; }

/* ---- files ---- */
int cos_stat(const char *p, cos_stat_t *o) { struct stat s; if (stat(p, &s)) return -1; o->size = (uint64_t)s.st_size; o->is_dir = S_ISDIR(s.st_mode) ? 1 : 0; return 0; }
ssize_t cos_read_file(const char *p, void *b, size_t n) { int f = open(p, O_RDONLY); if (f < 0) return -1; ssize_t r = read(f, b, n); close(f); return r; }
ssize_t cos_write_file(const char *p, const void *b, size_t n) { int f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0) return -1; ssize_t r = n ? write(f, b, n) : 0; close(f); return r; }
int cos_mkdir(const char *p) { return mkdir(p, 0755); }
int cos_unlink(const char *p) { struct stat s; if (!stat(p, &s) && S_ISDIR(s.st_mode)) return rmdir(p); return unlink(p); }
int cos_rename(const char *a, const char *b) { return rename(a, b); }

#define MAXFD 64
static DIR *s_dirs[MAXFD]; static int s_fds[MAXFD];
int cos_open(const char *p, uint32_t fl) {
    int f = -1;
    switch (fl) {
        case COS_O_RDONLY: f = open(p, O_RDONLY); break;
        case COS_O_WRONLY_CREATE: f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); break;
        case COS_O_WRONLY_APPEND: f = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); break;
        default: f = open(p, O_RDWR | O_CREAT, 0644); break;
    }
    if (f < 0) return -1;
    for (int i = 0; i < MAXFD; ++i) if (!s_fds[i] && !s_dirs[i]) { s_fds[i] = f + 1; return 100 + i; }
    close(f); return -1;
}
int cos_opendir(const char *p) {
    DIR *d = opendir(p); if (!d) return -1;
    for (int i = 0; i < MAXFD; ++i) if (!s_fds[i] && !s_dirs[i]) { s_dirs[i] = d; return 100 + i; }
    closedir(d); return -1;
}
ssize_t cos_fd_read(int fd, void *b, size_t n) { int i = fd - 100; if (i < 0 || i >= MAXFD || !s_fds[i]) return -1; return read(s_fds[i] - 1, b, n); }
ssize_t cos_fd_write(int fd, const void *b, size_t n) { int i = fd - 100; if (i < 0 || i >= MAXFD || !s_fds[i]) return -1; return write(s_fds[i] - 1, b, n); }
int64_t cos_lseek(int fd, int64_t off) { int i = fd - 100; if (i < 0 || i >= MAXFD || !s_fds[i]) return -1; return lseek(s_fds[i] - 1, off, SEEK_SET); }
int cos_readdir(int fd, cos_dirent_t *o) {
    int i = fd - 100; if (i < 0 || i >= MAXFD || !s_dirs[i]) return -1;
    struct dirent *e = readdir(s_dirs[i]); if (!e) return 0;
    snprintf(o->name, sizeof o->name, "%s", e->d_name);
    o->is_dir = e->d_type == DT_DIR; o->size = 0;
    return 1;
}
int cos_close(int fd) { int i = fd - 100; if (i < 0 || i >= MAXFD) return -1; if (s_dirs[i]) { closedir(s_dirs[i]); s_dirs[i] = NULL; } if (s_fds[i]) { close(s_fds[i] - 1); s_fds[i] = 0; } return 0; }

/* ---- processes: COS_STDOUT in the environment redirects the child's output to a file, exactly what libcos does on the device ---- */
int64_t cos_spawn_argv(const char *path, const char *const *argv, const char *const *envp) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        for (const char *const *e = envp; e && *e; ++e) {
            if (!strncmp(*e, "COS_STDOUT=", 11)) {
                int f = open(*e + 11, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (f >= 0) { dup2(f, 1); dup2(f, 2); close(f); }
            } else putenv((char *)*e);
        }
        execv(path, (char *const *)argv);
        _exit(127);
    }
    return p;
}
int64_t cos_waitpid(int64_t pid, int *status, bool blocking) {
    int st = 0; pid_t r = waitpid((pid_t)pid, &st, blocking ? 0 : WNOHANG);
    if (r <= 0) return r == 0 ? 0 : -1;
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return r;
}
int cos_kill(int pid, int sig) { return kill(pid, sig); }

/* ---- scripted window ---- */
typedef struct { int type, x, y, button, mods, special, wheel; char ascii; char text[256]; } Step;
static uint32_t *s_px; static int s_w, s_h;
static FILE *s_script; static char s_open_path[256];
static Step s_queue[512]; static int s_qh, s_qt;
static int s_wait_left;                     /* ms still to sleep before the next command */
static char s_line[512];

int64_t cos_win2_create(const char *title, int32_t w, int32_t h, cos_win_info_t *o) {
    (void)title;
    s_w = w; s_h = h; s_px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    o->handle = 1; o->pixels = s_px; o->width = w; o->height = h; o->stride = w; o->reserved = 0;
    const char *sc = getenv("STUDIO_SCRIPT");
    if (sc) s_script = fopen(sc, "r");
    return 1;
}
int cos_win2_present(int64_t h) { (void)h; return 0; }
int cos_win2_close(int64_t h) { (void)h; return 0; }
int cos_win2_set_title(int64_t h, const char *t) { (void)h; (void)t; return 0; }
int cos_win2_screen_size(int32_t *w, int32_t *h) { *w = 1280; *h = 800; return 0; }
int cos_win2_get_path(int64_t h, char *b, size_t cap) { (void)h; snprintf(b, cap, "%s", s_open_path); return 0; }

static void snap(const char *file) {
    FILE *f = fopen(file, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
    for (int i = 0; i < s_w * s_h; ++i) { unsigned c = s_px[i]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f); }
    fclose(f);
}
static void push(Step s) { if (s_qt < 512) s_queue[s_qt++] = s; }
static int key_special(const char *n) {
    static const struct { const char *n; int k; } T[] = { {"enter", COS_KEY_ENTER}, {"bs", COS_KEY_BACKSPACE}, {"esc", COS_KEY_ESC}, {"up", COS_KEY_UP}, {"down", COS_KEY_DOWN},
        {"left", COS_KEY_LEFT}, {"right", COS_KEY_RIGHT}, {"tab", COS_KEY_TAB}, {"del", COS_KEY_DELETE}, {"home", COS_KEY_HOME}, {"end", COS_KEY_END}, {"pgup", COS_KEY_PGUP}, {"pgdn", COS_KEY_PGDN} };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; ++i) if (!strcmp(T[i].n, n)) return T[i].k;
    if (n[0] == 'f' && n[1] >= '1' && n[1] <= '9') return COS_KEY_F1 + atoi(n + 1) - 1;
    return -1;
}
static void parse_line(char *l) {
    while (*l == ' ') ++l;
    size_t n = strlen(l); while (n && (l[n - 1] == '\n' || l[n - 1] == '\r')) l[--n] = 0;
    if (!*l || *l == '#') return;
    char cmd[32] = ""; int a = 0, b = 0, c = 0, d = 0; char rest[300] = "";
    sscanf(l, "%31s", cmd);
    const char *arg = l + strlen(cmd); while (*arg == ' ') ++arg;
    Step s; memset(&s, 0, sizeof s);
    if (!strcmp(cmd, "key")) {
        char spec[64]; sscanf(arg, "%63s", spec);
        char *p = spec; int mods = 0;
        for (;;) {
            if (!strncmp(p, "ctrl+", 5)) { mods |= COS_MOD_CTRL; p += 5; }
            else if (!strncmp(p, "shift+", 6)) { mods |= COS_MOD_SHIFT; p += 6; }
            else if (!strncmp(p, "alt+", 4)) { mods |= COS_MOD_ALT; p += 4; }
            else break;
        }
        s.type = COS_EV_KEY; s.mods = mods;
        int sp = key_special(p);
        if (sp >= 0) s.special = sp; else if (p[0] && !p[1]) s.ascii = p[0];
        else if (!strcmp(p, "space")) s.ascii = ' ';
        else if (!strcmp(p, "plus")) s.ascii = '+';
        else if (!strcmp(p, "minus")) s.ascii = '-';
        push(s);
    } else if (!strcmp(cmd, "type")) {
        for (const char *t = arg; *t; ++t) { Step k; memset(&k, 0, sizeof k); k.type = COS_EV_KEY; if (*t == '\\' && t[1] == 'n') { k.special = COS_KEY_ENTER; ++t; } else k.ascii = *t; push(k); }
    } else if (!strcmp(cmd, "click") || !strcmp(cmd, "dclick")) {
        char bt[16] = "l"; int mods = 0;
        {   /* "x y [ctrl] [shift] [alt] [l|r|m]" - skip the two numeric tokens, then read modifier/button words */
            int x, y, n = 0;
            sscanf(arg, "%d %d%n", &x, &y, &n);
            a = x; b = y;
            const char *rest = arg + n;
            char word[16];
            while (sscanf(rest, "%15s%n", word, &n) == 1) {
                if (!strcmp(word, "ctrl")) mods |= COS_MOD_CTRL;
                else if (!strcmp(word, "shift")) mods |= COS_MOD_SHIFT;
                else if (!strcmp(word, "alt")) mods |= COS_MOD_ALT;
                else snprintf(bt, sizeof bt, "%s", word);
                rest += n;
            }
        }
        int btn = bt[0] == 'r' ? COS_MOUSE_BTN_RIGHT : bt[0] == 'm' ? COS_MOUSE_BTN_MIDDLE : COS_MOUSE_BTN_LEFT;
        for (int r = 0; r < (cmd[0] == 'd' ? 2 : 1); ++r) {
            Step m; memset(&m, 0, sizeof m); m.type = COS_EV_MOUSE_MOVE; m.x = a; m.y = b; push(m);
            Step dn; memset(&dn, 0, sizeof dn); dn.type = COS_EV_MOUSE_DOWN; dn.x = a; dn.y = b; dn.button = btn; dn.mods = mods; push(dn);
            Step up; memset(&up, 0, sizeof up); up.type = COS_EV_MOUSE_UP; up.x = a; up.y = b; up.button = btn; up.mods = mods; push(up);
        }
    } else if (!strcmp(cmd, "move")) { sscanf(arg, "%d %d", &a, &b); s.type = COS_EV_MOUSE_MOVE; s.x = a; s.y = b; push(s); }
    else if (!strcmp(cmd, "drag")) {
        sscanf(arg, "%d %d %d %d", &a, &b, &c, &d);
        Step m; memset(&m, 0, sizeof m); m.type = COS_EV_MOUSE_MOVE; m.x = a; m.y = b; push(m);
        Step dn; memset(&dn, 0, sizeof dn); dn.type = COS_EV_MOUSE_DOWN; dn.x = a; dn.y = b; dn.button = COS_MOUSE_BTN_LEFT; push(dn);
        for (int i = 1; i <= 5; ++i) { Step mv; memset(&mv, 0, sizeof mv); mv.type = COS_EV_MOUSE_MOVE; mv.x = a + (c - a) * i / 5; mv.y = b + (d - b) * i / 5; mv.button = 1; mv.mods = 0x80; push(mv); }
        Step up; memset(&up, 0, sizeof up); up.type = COS_EV_MOUSE_UP; up.x = c; up.y = d; up.button = COS_MOUSE_BTN_LEFT; push(up);
    } else if (!strcmp(cmd, "wheel")) { sscanf(arg, "%d", &a); s.type = COS_EV_WHEEL; s.wheel = a; push(s); }
    else if (!strcmp(cmd, "open")) { s.type = COS_EV_OPEN; snprintf(s.text, sizeof s.text, "%s", arg); push(s); }
    else if (!strcmp(cmd, "wait")) { sscanf(arg, "%d", &a); s.type = -2; s.x = a; push(s); }
    else if (!strcmp(cmd, "snap")) { s.type = -3; snprintf(s.text, sizeof s.text, "%s", arg); push(s); }
    else if (!strcmp(cmd, "quit")) { s.type = -4; push(s); }
    (void)rest;
}

static int next_step(cos_win_event_t *ev) {
    if (s_wait_left > 0) { int ms = s_wait_left > 20 ? 20 : s_wait_left; usleep((useconds_t)ms * 1000); s_wait_left -= ms; return 0; }
    if (s_qh == s_qt) {
        s_qh = s_qt = 0;
        if (!s_script || !fgets(s_line, sizeof s_line, s_script)) return -1;
        parse_line(s_line);
        if (s_qh == s_qt) return 0;
    }
    Step s = s_queue[s_qh++];
    if (s.type == -2) { s_wait_left = s.x; return 0; }
    if (s.type == -3) { snap(s.text); return 0; }
    if (s.type == -4) return -1;
    memset(ev, 0, sizeof *ev);
    ev->type = (uint32_t)s.type; ev->x = s.x; ev->y = s.y; ev->button = (uint8_t)s.button; ev->mods = (uint8_t)(s.mods & 0x7F);
    ev->buttons = (s.mods & 0x80) ? COS_MOUSE_BTN_LEFT : 0;
    ev->special = (uint8_t)s.special; ev->ascii = s.ascii; ev->wheel = s.wheel;
    if (s.type == COS_EV_OPEN) snprintf(s_open_path, sizeof s_open_path, "%s", s.text);
    return 1;
}
int cos_win2_wait(int64_t h, cos_win_event_t *ev, uint32_t t) { (void)h; (void)t; return next_step(ev); }
int cos_win2_poll(int64_t h, cos_win_event_t *ev) {
    (void)h;
    if (s_qh == s_qt || s_wait_left > 0) return 0;
    int t = s_queue[s_qh].type;
    if (t < 0) return 0;                        /* snapshots/waits/quit are handled between frames, in wait() */
    return next_step(ev) > 0 ? 1 : 0;
}
