/* httpdl.c-os - the HTTP Downloader as a ring-3 program (it used to be drawn by the kernel).
 *
 * The transfer runs on a kernel thread (cos_net_http_start/poll/take): a blocking network syscall would freeze every
 * ring-3 window, this one keeps repainting and stays closable. The body is byte-exact and the HTTP status and
 * Content-Type are known, so an HTML page without a file extension is saved with .html.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "cos.h"
#include "cos_ui.h"

#define BG     0x141826
#define PANEL  0x1C2333
#define LINE   0x2B3550
#define TEXT   0xE8ECF5
#define MUTED  0x8891A5
#define ACCENT 0x4C9AFF
#define GOOD   0x3DD68C
#define ERRC   0xFF6B6B
#define WARN   0xF2B84B

#define MAX_BODY (12u << 20)
#define LOG_LINES 12

static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_h;
static int win_w, win_h;

static char url[256], save_as[256];
static int focus;                        /* 0 = URL, 1 = save-as */
static char logbuf[LOG_LINES][160];
static int nlog;
static char status[160] = "Enter a URL and press Download";
static int status_kind;                  /* 0 info, 1 success, 2 error */

/* download job */
static int job = -1;                     /* kernel job id, -1 when idle */
static char job_url[256], job_dest[256];
static uint64_t job_started;

static void log_line(const char *s) {
    if (nlog == LOG_LINES) { memmove(logbuf[0], logbuf[1], sizeof logbuf[0] * (LOG_LINES - 1)); nlog--; }
    snprintf(logbuf[nlog++], sizeof logbuf[0], "%s", s);
}

static void name_from_url(const char *u, char *out, size_t n) {
    snprintf(out, n, "download.bin");
    const char *p = strstr(u, "://"); p = p ? p + 3 : u;
    const char *slash = strchr(p, '/');
    if (!slash) return;
    const char *s = strrchr(slash, '/') + 1;
    size_t l = 0;
    while (s[l] && s[l] != '?' && s[l] != '#') l++;
    if (!l) return;
    if (l >= n) l = n - 1;
    memcpy(out, s, l); out[l] = 0;
    if (!strchr(out, '.') && strlen(out) + 4 < n) strcat(out, ".bin");
}

/* Called when the kernel reports the job finished: collects the body and writes the file. */
static void finish_job(const cos_http_result_t *res) {
    char l[300];
    int id = job; job = -1;
    if (res->error || res->status != 200) {
        if (res->status) snprintf(l, sizeof l, "Download failed (HTTP %d)", res->status);
        else snprintf(l, sizeof l, "Download failed (no response, DNS or TLS error)");
        cos_net_http_cancel(id);
        log_line(l); snprintf(status, sizeof status, "%s", l); status_kind = 2;
        return;
    }
    if (res->body_len == 0) { cos_net_http_cancel(id); log_line("Response was empty"); snprintf(status, sizeof status, "Empty response"); status_kind = 2; return; }
    char *body = malloc((size_t)res->body_len);
    if (!body) { cos_net_http_cancel(id); log_line("Out of memory"); snprintf(status, sizeof status, "Out of memory"); status_kind = 2; return; }
    ssize_t n = cos_net_http_take(id, body, (size_t)res->body_len);
    if (n < 0) { free(body); log_line("Could not read the response"); snprintf(status, sizeof status, "Download failed"); status_kind = 2; return; }
    char dest[256];
    snprintf(dest, sizeof dest, "%s", job_dest);
    const char *base = strrchr(dest, '/') ? strrchr(dest, '/') : dest;
    bool html = !strncmp(res->content_type, "text/html", 9) || !strncmp(res->content_type, "application/xhtml+xml", 21);
    if (html && !strchr(base, '.') && strlen(dest) + 6 < sizeof dest) strcat(dest, ".html");
    bool wrote = cos_write_file(dest, body, (size_t)n) >= 0;
    if (!wrote && strrchr(dest, '/') && strrchr(dest, '/') != dest) {   /* the folder may not exist yet: create it and retry once */
        char dir[256]; snprintf(dir, sizeof dir, "%s", dest); *strrchr(dir, '/') = 0;
        if (cos_mkdir(dir) == 0) wrote = cos_write_file(dest, body, (size_t)n) >= 0;
    }
    if (!wrote) {
        snprintf(l, sizeof l, "Could not write %s (does the folder exist?)", dest);
        log_line(l); snprintf(status, sizeof status, "Write failed"); status_kind = 2;
    } else {
        snprintf(l, sizeof l, "Saved %zd bytes to %s (%s)", n, dest, res->content_type[0] ? res->content_type : "unknown type");
        log_line(l); snprintf(status, sizeof status, "Saved %zd bytes", n); status_kind = 1;
    }
    free(body);
}

static void start_download(void) {
    if (job >= 0) return;
    if (!url[0]) { snprintf(status, sizeof status, "Enter a URL first"); status_kind = 2; return; }
    if (!strstr(url, "://")) { char t[256]; snprintf(t, sizeof t, "http://%s", url); snprintf(url, sizeof url, "%s", t); }
    if (!cos_net_available()) { snprintf(status, sizeof status, "No network connection"); status_kind = 2; log_line("Network is not available"); return; }
    if (save_as[0]) snprintf(job_dest, sizeof job_dest, "%s", save_as);
    else { char nm[160]; name_from_url(url, nm, sizeof nm); snprintf(job_dest, sizeof job_dest, "/downloads/%s", nm); }
    snprintf(job_url, sizeof job_url, "%s", url);
    int id = cos_net_http_start(job_url);
    if (id < 0) { snprintf(status, sizeof status, "Could not start the transfer"); status_kind = 2; log_line("No free download slot"); return; }
    job = id; job_started = cos_time_ms();
    char l[300]; snprintf(l, sizeof l, "GET %s", job_url); log_line(l);
    snprintf(status, sizeof status, "Downloading..."); status_kind = 0;
}

static void field(int idx, int y, const char *label, char *val) {
    cui_text(&c, f_b, 20, y, label, MUTED);
    cui_round_rect(&c, 20, y + 22, win_w - 40, 32, 7, 0x0F131C);
    cui_round_rect_outline(&c, 20, y + 22, win_w - 40, 32, 7, 1.5f, focus == idx ? ACCENT : LINE);
    cui_text_fit(&c, f_m, 30, y + 29, win_w - 60, val, TEXT);
    if (focus == idx && ((cos_time_ms() / 500) & 1) == 0) cui_fill(&c, 30 + cui_text_width(f_m, val), y + 28, 2, 20, ACCENT);
}

static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, BG);
    cui_vgradient(&c, 0, 0, win_w, 60, 0x1F3A6E, BG);
    cui_text(&c, f_h, 20, 14, "HTTP Downloader", TEXT);
    field(0, 72, "URL", url);
    field(1, 138, "Save as (optional - defaults to /downloads/<file name>)", save_as);
    bool busy = job >= 0;
    cui_round_rect(&c, 20, 212, 140, 34, 8, busy ? 0x2A3550 : 0x3D7BE0);
    cui_text_center(&c, f_b, 20, 212, 140, 34, busy ? "Downloading..." : "Download", TEXT);
    cui_round_rect(&c, 170, 212, 130, 34, 8, 0x2A3550);
    cui_text_center(&c, f_b, 170, 212, 130, 34, "Open folder", TEXT);
    cui_text_fit(&c, f_m, 316, 220, win_w - 336, status, job >= 0 ? WARN : status_kind == 1 ? GOOD : status_kind == 2 ? ERRC : MUTED);
    cui_round_rect(&c, 20, 262, win_w - 40, win_h - 282, 8, 0x0F1320);
    for (int i = 0; i < nlog; i++) cui_text_fit(&c, f_s, 32, 272 + i * 18, win_w - 64, logbuf[i], MUTED);
}

static void on_key(const cos_win_event_t *ev) {
    char *f = focus ? save_as : url;
    size_t cap = focus ? sizeof save_as : sizeof url;
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == 'v' || ev->ascii == 'V')) {
        char t[256]; int64_t n = cos_clip_get(t, sizeof t - 1);
        for (int64_t i = 0; i < n && strlen(f) + 1 < cap; i++) if (t[i] >= 32 && t[i] < 127) { size_t l = strlen(f); f[l] = t[i]; f[l + 1] = 0; }
        return;
    }
    if (ev->special == COS_KEY_TAB) { focus ^= 1; return; }
    if (ev->special == COS_KEY_ENTER || ev->ascii == '\n' || ev->ascii == '\r') { start_download(); return; }
    if (ev->special == COS_KEY_BACKSPACE || ev->ascii == 8 || ev->ascii == 127) { size_t l = strlen(f); if (l) f[l - 1] = 0; return; }
    if (ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & COS_MOD_CTRL)) { size_t l = strlen(f); if (l + 1 < cap) { f[l] = ev->ascii; f[l + 1] = 0; } }
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("HTTP Downloader", 680, 460, &wi);
    if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(15); f_b = cui_font_bold(14); f_h = cui_font_bold(24);

    for (;;) {
        if (job >= 0) {
            cos_http_result_t res;
            int st = cos_net_http_poll(job, &res);
            if (st == 1) finish_job(&res);
            else if (st < 0) { job = -1; log_line("The transfer was lost"); snprintf(status, sizeof status, "Download failed"); status_kind = 2; }
            else snprintf(status, sizeof status, "Downloading... %llu s", (unsigned long long)((cos_time_ms() - job_started) / 1000));
        }
        draw();
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, job >= 0 ? 250 : 400);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) { if (job >= 0) cos_net_http_cancel(job); goto done; }
            if (ev.type == COS_EV_KEY) on_key(&ev);
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                if (ev.y >= 94 && ev.y < 126) focus = 0;
                else if (ev.y >= 160 && ev.y < 192) focus = 1;
                else if (ev.y >= 212 && ev.y < 246 && ev.x >= 20 && ev.x < 160) start_download();
                else if (ev.y >= 212 && ev.y < 246 && ev.x >= 170 && ev.x < 300) {
                    char d[256] = "/downloads";
                    if (save_as[0] && strrchr(save_as, '/') && strrchr(save_as, '/') != save_as) { snprintf(d, sizeof d, "%s", save_as); *strrchr(d, '/') = 0; }
                    cos_open_path(d);
                }
            }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
