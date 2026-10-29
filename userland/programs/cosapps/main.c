/* cosapps.c-os - "C-OS Apps": the store. Lists the apps of a signed catalog (catalog.manifest, see tools/make_catalog.py), each with its icon, a download
 * button (the pointer over it shows the download size) and a trash button, plus a search box.
 *
 * Installing starts the pack installer (/apps/linux/LinuxInstall.c-os, userland/programs/linuxpack) on the app's own address: it checks the pack manifest
 * against the update key, downloads and verifies the chunks, writes the files to /apps/ID and the shortcut to /desktop. The store follows its progress through
 * /system/linuxpack/progress.txt. Removing an app deletes /apps/ID and its shortcut. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"
#include "bearssl.h"
#include "update_pubkey.h"
#include "apps_url.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define INSTALLER "/apps/linux/LinuxInstall.c-os"
#define ICON_DIR "/system/apps/icons"
#define MAX_APPS 64
#define ICON_PX 48

typedef struct {
    char id[40], title[48], version[32], desc[120], icon[48], iconsha[65];
    uint64_t download, installed;
    uint32_t *icon_px;          /* ICON_PX x ICON_PX, 0xAARRGGBB-free RGB; NULL until fetched */
    bool icon_tried;
    long inst_serial;           /* from /apps/ID/pack.txt, -1 = not installed */
    char inst_version[32];
} app_t;
static app_t apps[MAX_APPS]; static int napps;

static cui_canvas c; static cui_font *f_s, *f_m, *f_b, *f_h;
static int win_w, win_h;
static char base_url[300];
static char search[40]; static int nsearch;
static int scroll_y, hover = -1, hover_part;     /* part: 1 = download, 2 = trash */
static int confirm_app = -1, confirm_step;       /* the app whose trash button was pressed, and which warning is showing (0-based) */
static bool ja;
/* how many warnings come before an app is removed: 0..3, set in Settings (/system/apps/uninstall_warnings), 3 by default */
static int warnings(void) { char b[8]; ssize_t n = cos_read_file("/system/apps/uninstall_warnings", b, sizeof b - 1); if (n > 0 && b[0] >= '0' && b[0] <= '3') return b[0] - '0'; return 3; }
static const char *warn_text(int step) {
    static const char *en[3] = { "Remove this app?", "Do you really want to remove it?", "Its data will be removed too. Do you really want to remove it?" };
    static const char *jp[3] = { "\xe3\x81\x93\xe3\x81\xae\xe3\x82\xa2\xe3\x83\x97\xe3\x83\xaa\xe3\x82\x92\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b?",
        "\xe6\x9c\xac\xe5\xbd\x93\xe3\x81\xab\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b?",
        "\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf\xe3\x82\x82\xe4\xb8\x80\xe7\xb7\x92\xe3\x81\xab\xe6\xb6\x88\xe3\x81\x88\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe6\x9c\xac\xe5\xbd\x93\xe3\x81\xab\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b?" };
    return ja ? jp[step] : en[step];
}
static char status[160] = "Loading the list of apps...";
static int busy_app = -1; static bool installing;

/* ------------------------------------------------------------- network */
static int job = -1; static uint64_t job_t0;
static uint8_t *job_buf; static size_t job_len;
static bool fetch_start(const char *url) { job = cos_net_http_start(url); job_t0 = cos_time_ms(); return job >= 0; }
/* 0 = still going, 1 = done (job_buf/job_len hold the body, free() it), -1 = failed */
static int fetch_step(size_t max) {
    cos_http_result_t r; int p = cos_net_http_poll(job, &r);
    if (p == 0) { if (cos_time_ms() - job_t0 > 60000) { cos_net_http_cancel(job); job = -1; return -1; } return 0; }
    if (p < 0) { job = -1; return -1; }
    int jb = job; job = -1;
    if (r.error || r.status != 200 || r.body_len > max) { cos_net_http_cancel(jb); return -1; }
    job_buf = malloc((size_t)r.body_len + 1); if (!job_buf) { cos_net_http_cancel(jb); return -1; }
    ssize_t n = cos_net_http_take(jb, job_buf, (size_t)r.body_len);
    if (n < 0 || (uint64_t)n != r.body_len) { free(job_buf); job_buf = NULL; return -1; }
    job_buf[n] = 0; job_len = (size_t)n; return 1;
}

static bool hexval(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) { char ch = s[i * 2 + k]; int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; if (d < 0) return false; v = v * 16 + d; }
        out[i] = (unsigned char)v;
    }
    return true;
}
static void sha256_hex(const void *p, size_t n, char out[65]) {
    br_sha256_context x; unsigned char h[32]; br_sha256_init(&x); br_sha256_update(&x, p, n); br_sha256_out(&x, h);
    for (int i = 0; i < 32; ++i) { out[i * 2] = "0123456789abcdef"[h[i] >> 4]; out[i * 2 + 1] = "0123456789abcdef"[h[i] & 15]; } out[64] = 0;
}
static bool name_ok(const char *s) { if (!*s || strlen(s) > 40) return false; for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) return false; return true; }

/* "COSAPPS 1" body + "\n--SIG--\n" + 128 hex digits (ECDSA P-256 / SHA-256, the update key) */
static bool catalog_parse(char *m, size_t len) {
    const char *marker = "\n--SIG--\n"; char *sg = NULL;
    for (size_t i = 0; i + 9 <= len; ++i) if (memcmp(m + i, marker, 9) == 0) { sg = m + i; break; }
    if (!sg) return false;
    size_t body_len = (size_t)(sg - m) + 1; const char *hx = sg + 9; unsigned char sig[64];
    if (strlen(hx) < 128 || !hexval(hx, sig, 64)) return false;
    unsigned char dg[32]; br_sha256_context x; br_sha256_init(&x); br_sha256_update(&x, m, body_len); br_sha256_out(&x, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)cos_update_pubkey, sizeof cos_update_pubkey };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, sig, 64) != 1) return false;
    m[body_len] = 0; napps = 0; bool header = false;
    for (char *line = m; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!strcmp(line, "COSAPPS 1")) header = true;
        else if (header && !strncmp(line, "app ", 4) && napps < MAX_APPS) {
            app_t *a = &apps[napps]; memset(a, 0, sizeof *a); a->inst_serial = -1;
            for (char *tok = strtok(line + 4, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "id")) snprintf(a->id, sizeof a->id, "%s", v);
                else if (!strcmp(tok, "title")) snprintf(a->title, sizeof a->title, "%s", v);
                else if (!strcmp(tok, "version")) snprintf(a->version, sizeof a->version, "%s", v);
                else if (!strcmp(tok, "desc")) snprintf(a->desc, sizeof a->desc, "%s", v);
                else if (!strcmp(tok, "icon")) snprintf(a->icon, sizeof a->icon, "%s", v);
                else if (!strcmp(tok, "iconsha")) snprintf(a->iconsha, sizeof a->iconsha, "%s", v);
                else if (!strcmp(tok, "download")) a->download = strtoull(v, NULL, 10);
                else if (!strcmp(tok, "installed")) a->installed = strtoull(v, NULL, 10);
            }
            for (char *q = a->title; *q; ++q) if (*q == '_') *q = ' ';
            for (char *q = a->desc; *q; ++q) if (*q == '_') *q = ' ';
            if (name_ok(a->id) && name_ok(a->icon) && a->title[0] && strlen(a->iconsha) == 64) napps++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return header;
}

/* ------------------------------------------------------------- installed state */
static void refresh_installed(void) {
    for (int i = 0; i < napps; ++i) {
        app_t *a = &apps[i]; char p[100], b[160]; a->inst_serial = -1; a->inst_version[0] = 0;
        snprintf(p, sizeof p, "/apps/%s/pack.txt", a->id);
        ssize_t n = cos_read_file(p, b, sizeof b - 1); if (n <= 0) continue; b[n] = 0;
        a->inst_serial = 0;
        for (char *l = b; l && *l; ) { char *nl = strchr(l, '\n'); if (nl) *nl = 0; if (!strncmp(l, "version ", 8)) snprintf(a->inst_version, sizeof a->inst_version, "%s", l + 8); l = nl ? nl + 1 : NULL; }
    }
}
static bool installed(const app_t *a) { return a->inst_serial >= 0; }
static bool has_update(const app_t *a) { return installed(a) && strcmp(a->inst_version, a->version) != 0; }

/* ------------------------------------------------------------- icons */
static void load_icon(app_t *a, const uint8_t *png, size_t n) {
    char p[120], h[65]; sha256_hex(png, n, h);
    if (strcmp(h, a->iconsha) != 0) return;                                  /* not what the signed catalog names */
    cos_mkdir("/system"); cos_mkdir("/system/apps"); cos_mkdir(ICON_DIR);
    snprintf(p, sizeof p, "%s/%s.png", ICON_DIR, a->id); cos_write_file(p, png, n);
    size_t cap = 256 * 256 * sizeof(uint32_t); uint32_t *px = malloc(cap); if (!px) return;
    cos_image_info_t info;
    if (cos_image_decode(p, px, cap, &info) != 0 || !info.width || !info.height || info.width > 256 || info.height > 256) { free(px); return; }
    uint32_t *o = malloc(ICON_PX * ICON_PX * sizeof(uint32_t)); if (!o) { free(px); return; }
    int w = (int)info.width, hh = (int)info.height;
    for (int y = 0; y < ICON_PX; ++y) for (int x = 0; x < ICON_PX; ++x) {            /* box filter */
        int x0 = x * w / ICON_PX, x1 = (x + 1) * w / ICON_PX; if (x1 <= x0) x1 = x0 + 1;
        int y0 = y * hh / ICON_PX, y1 = (y + 1) * hh / ICON_PX; if (y1 <= y0) y1 = y0 + 1;
        unsigned r = 0, g = 0, b = 0, cnt = 0;
        for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { uint32_t v = px[yy * w + xx]; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; cnt++; }
        o[y * ICON_PX + x] = ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
    }
    free(px); a->icon_px = o;
}

/* ------------------------------------------------------------- install / remove */
static void rm_tree(const char *path, int depth) {
    if (depth > 6) return;
    int fd = cos_opendir(path);
    if (fd >= 0) {
        static cos_dirent_t e; char kids[64][48]; int nk = 0; bool isd[64];
        while (nk < 64 && cos_readdir(fd, &e) == 1) { if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue; snprintf(kids[nk], sizeof kids[0], "%s", e.name); isd[nk] = e.is_dir; nk++; }
        cos_close(fd);
        for (int i = 0; i < nk; ++i) { char q[200]; snprintf(q, sizeof q, "%s/%s", path, kids[i]); if (isd[i]) rm_tree(q, depth + 1); else cos_unlink(q); }
    }
    cos_unlink(path);
}
static void remove_app(app_t *a) {
    char p[120]; snprintf(p, sizeof p, "/desktop/%s.app", a->title); cos_unlink(p);
    snprintf(p, sizeof p, "/apps/%s", a->id); rm_tree(p, 0);
    refresh_installed();
    snprintf(status, sizeof status, "%s was removed.", a->title);
}
static void start_install(int i) {
    if (installing) return;
    app_t *a = &apps[i]; char url[360];
    if (!(strlen(base_url) + strlen(a->id) + 2 < sizeof url)) return;
    snprintf(url, sizeof url, "%s%s/", base_url, a->id);
    const char *iv[3] = { INSTALLER, url, NULL };
    cos_mkdir("/system"); cos_mkdir("/system/linuxpack");
    static const char starting[] = "running 0 0 0 0 starting\n"; cos_write_file("/system/linuxpack/progress.txt", starting, sizeof starting - 1);
    if (cos_spawn_argv(INSTALLER, iv, NULL) <= 0) { snprintf(status, sizeof status, "Could not start the installer."); return; }
    installing = true; busy_app = i; snprintf(status, sizeof status, "Installing %s...", a->title);
}
typedef struct { char state[12]; int done, total; unsigned long long bytes_done, bytes; char msg[96]; bool ok; } inst_t;
static inst_t read_progress(void) {
    inst_t r; memset(&r, 0, sizeof r); char b[320]; ssize_t n = cos_read_file("/system/linuxpack/progress.txt", b, sizeof b - 1);
    if (n <= 0) return r; b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; int used = 0;
    if (sscanf(b, "%11s %d %d %llu %llu %n", r.state, &r.done, &r.total, &r.bytes_done, &r.bytes, &used) >= 5) { snprintf(r.msg, sizeof r.msg, "%s", b + used); r.ok = true; }
    return r;
}

/* ------------------------------------------------------------- list / drawing */
typedef struct { int x, y, w, h; } rect_t;
static bool in(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
#define HEAD_H 64
#define ROW_H 68
static int shown[MAX_APPS], nshown;
static bool ci_find(const char *h, const char *n) {
    size_t ln = strlen(n); if (!ln) return true;
    for (; *h; ++h) { size_t k = 0; while (k < ln && h[k] && ((h[k] | 32) == (n[k] | 32))) k++; if (k == ln) return true; }
    return false;
}
static void filter(void) { nshown = 0; for (int i = 0; i < napps; ++i) if (ci_find(apps[i].title, search) || ci_find(apps[i].desc, search) || ci_find(apps[i].id, search)) shown[nshown++] = i; }
static rect_t list_rect(void) { return (rect_t){ 0, HEAD_H, win_w, win_h - HEAD_H - 30 }; }
static rect_t search_rect(void) { return (rect_t){ win_w - 280, 16, 264, 32 }; }
static rect_t dl_rect(int row) { return (rect_t){ win_w - 108, list_rect().y + row * ROW_H - scroll_y + 14, 40, 40 }; }
static rect_t tr_rect(int row) { return (rect_t){ win_w - 60, list_rect().y + row * ROW_H - scroll_y + 14, 40, 40 }; }
static void clamp_scroll(void) { int max = nshown * ROW_H + 8 - list_rect().h; if (max < 0) max = 0; if (scroll_y > max) scroll_y = max; if (scroll_y < 0) scroll_y = 0; }

static void glyph_download(rect_t r, uint32_t col) {                    /* an arrow into a tray */
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    cui_line(&c, cx, cy - 11, cx, cy + 3, 3.5f, col);
    cui_triangle(&c, cx - 8, cy, cx + 8, cy, cx, cy + 10, col);
    cui_line(&c, cx - 11, cy + 13, cx + 11, cy + 13, 3.0f, col);
    cui_line(&c, cx - 11, cy + 8, cx - 11, cy + 13, 3.0f, col);
    cui_line(&c, cx + 11, cy + 8, cx + 11, cy + 13, 3.0f, col);
}
static void glyph_trash(rect_t r, uint32_t col) {                       /* a bin with a lid */
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    cui_line(&c, cx - 10, cy - 8, cx + 10, cy - 8, 3.0f, col);
    cui_line(&c, cx - 3, cy - 12, cx + 3, cy - 12, 3.0f, col);
    cui_line(&c, cx - 8, cy - 5, cx - 6, cy + 12, 3.0f, col);
    cui_line(&c, cx + 8, cy - 5, cx + 6, cy + 12, 3.0f, col);
    cui_line(&c, cx - 6, cy + 12, cx + 6, cy + 12, 3.0f, col);
    cui_line(&c, cx - 2.5f, cy - 3, cx - 2.5f, cy + 8, 2.0f, col);
    cui_line(&c, cx + 2.5f, cy - 3, cx + 2.5f, cy + 8, 2.0f, col);
}
static void fmt_size(uint64_t b, char *o, size_t n) {
    if (b >= 1048576) snprintf(o, n, "%u.%u MB", (unsigned)(b >> 20), (unsigned)((b * 10 >> 20) % 10));
    else snprintf(o, n, "%u KB", (unsigned)((b + 1023) >> 10));
}

static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, RGB(30, 32, 38));
    cui_fill(&c, 0, 0, win_w, HEAD_H, RGB(22, 24, 29)); cui_fill(&c, 0, HEAD_H - 2, win_w, 2, RGB(52, 56, 66));
    cui_text(&c, f_h, 20, 12, "C-OS Apps", RGB(255, 255, 255));
    rect_t sr = search_rect();
    cui_round_rect(&c, sr.x, sr.y, sr.w, sr.h, 8, RGB(40, 44, 52));
    if (nsearch) cui_text_fit(&c, f_m, sr.x + 12, sr.y + 6, sr.w - 24, search, RGB(240, 242, 246));
    else cui_text(&c, f_m, sr.x + 12, sr.y + 6, "Search apps", RGB(120, 126, 140));
    rect_t lr = list_rect();
    cui_clip(&c, lr.x, lr.y, lr.w, lr.h);
    inst_t pr = read_progress();
    for (int row = 0; row < nshown; ++row) {
        int y = lr.y + row * ROW_H - scroll_y; if (y + ROW_H < lr.y || y > lr.y + lr.h) continue;
        int ai = shown[row]; app_t *a = &apps[ai];
        if (hover == ai) cui_round_rect(&c, 8, y + 2, win_w - 16, ROW_H - 4, 8, RGB(38, 42, 50));
        if (a->icon_px) cui_blit(&c, 20, y + 10, a->icon_px, ICON_PX, ICON_PX, ICON_PX);
        else cui_round_rect(&c, 20, y + 10, ICON_PX, ICON_PX, 10, RGB(52, 56, 66));
        cui_text(&c, f_b, 84, y + 10, a->title, RGB(244, 246, 250));
        char line[200];
        if (installing && busy_app == ai) {
            unsigned pct = pr.ok && pr.bytes ? (unsigned)(pr.bytes_done * 100 / pr.bytes) : 0; if (pct > 100) pct = 100;
            int bw = win_w - 84 - 140; if (bw > 360) bw = 360;
            cui_round_rect(&c, 84, y + 40, bw, 10, 5, RGB(44, 48, 56));
            if (pct) cui_round_rect(&c, 84, y + 40, bw * (int)pct / 100 < 10 ? 10 : bw * (int)pct / 100, 10, 5, RGB(62, 150, 70));
            snprintf(line, sizeof line, "%u%%  %s", pct, pr.ok ? pr.msg : "starting"); cui_text(&c, f_s, 84 + bw + 10, y + 36, line, RGB(170, 176, 190));
        } else {
            char sz[24]; fmt_size(a->installed, sz, sizeof sz);
            snprintf(line, sizeof line, "%s%s%s   (%s on disk)", a->desc, a->desc[0] ? "   " : "", installed(a) ? (has_update(a) ? "Update available" : "Installed") : "", sz);
            cui_text_fit(&c, f_s, 84, y + 36, win_w - 84 - 130, line, installed(a) ? RGB(130, 200, 140) : RGB(150, 156, 170));
        }
        bool can_dl = !installed(a) || has_update(a);
        rect_t d = dl_rect(row), t = tr_rect(row);
        bool hd = hover == ai && hover_part == 1, ht = hover == ai && hover_part == 2;
        cui_round_rect(&c, d.x, d.y, d.w, d.h, 10, can_dl && !installing ? (hd ? RGB(72, 170, 84) : RGB(58, 140, 68)) : RGB(46, 50, 58));
        glyph_download(d, can_dl && !installing ? RGB(255, 255, 255) : RGB(110, 114, 124));
        bool can_rm = installed(a) && !installing;
        cui_round_rect(&c, t.x, t.y, t.w, t.h, 10, can_rm ? (confirm_app == ai ? RGB(200, 60, 50) : ht ? RGB(110, 66, 66) : RGB(70, 50, 54)) : RGB(40, 43, 50));
        glyph_trash(t, can_rm ? RGB(255, 255, 255) : RGB(90, 94, 104));
    }
    if (nshown == 0) cui_text(&c, f_m, 24, lr.y + 24, napps ? "Nothing matches the search." : status, RGB(170, 176, 190));
    cui_unclip(&c);
    cui_fill(&c, 0, win_h - 30, win_w, 30, RGB(22, 24, 29));
    cui_text_fit(&c, f_s, 16, win_h - 24, win_w - 32, status, RGB(150, 156, 170));
    if (hover >= 0 && hover_part == 1) {                                        /* tooltip: how much the download is */
        int row = -1; for (int r = 0; r < nshown; ++r) if (shown[r] == hover) row = r;
        if (row >= 0) {
            char sz[24], tip[80]; fmt_size(apps[hover].download, sz, sizeof sz);
            snprintf(tip, sizeof tip, installed(&apps[hover]) && !has_update(&apps[hover]) ? "Installed" : "Download size: %s", sz);
            int tw = cui_text_width(f_s, tip) + 20; rect_t d = dl_rect(row); int tx = d.x + d.w - tw, ty = d.y + d.h + 4; if (ty + 28 > win_h - 30) ty = d.y - 30;
            cui_round_rect(&c, tx, ty, tw, 26, 6, RGB(10, 12, 16)); cui_round_rect_outline(&c, tx, ty, tw, 26, 6, 1.0f, RGB(80, 86, 100));
            cui_text(&c, f_s, tx + 10, ty + 5, tip, RGB(235, 238, 245));
        }
    }
}

static rect_t dlg_rect(void) { return (rect_t){ win_w / 2 - 250, win_h / 2 - 90, 500, 180 }; }
static rect_t dlg_no(void) { rect_t d = dlg_rect(); return (rect_t){ d.x + d.w - 330, d.y + d.h - 62, 150, 42 }; }
static rect_t dlg_yes(void) { rect_t d = dlg_rect(); return (rect_t){ d.x + d.w - 170, d.y + d.h - 62, 150, 42 }; }
static void draw_dialog(void) {
    if (confirm_app < 0) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, RGB(0, 0, 0), 150);
    rect_t d = dlg_rect(); cui_round_rect(&c, d.x, d.y + 4, d.w, d.h, 12, RGB(8, 9, 12)); cui_round_rect(&c, d.x, d.y, d.w, d.h, 12, RGB(40, 44, 54));
    cui_text_fit(&c, f_b, d.x + 24, d.y + 20, d.w - 48, apps[confirm_app].title, RGB(255, 255, 255));
    cui_text_fit(&c, f_m, d.x + 24, d.y + 56, d.w - 48, warn_text(confirm_step), RGB(240, 200, 120));
    char t[24]; snprintf(t, sizeof t, "%d / %d", confirm_step + 1, warnings()); cui_text(&c, f_s, d.x + 24, d.y + d.h - 44, t, RGB(130, 136, 150));
    rect_t n = dlg_no(), y = dlg_yes();
    cui_round_rect(&c, n.x, n.y, n.w, n.h, 8, RGB(70, 74, 84)); cui_text_center(&c, f_b, n.x, n.y, n.w, n.h, ja ? "\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab" : "Cancel", RGB(255, 255, 255));
    cui_round_rect(&c, y.x, y.y, y.w, y.h, 8, RGB(200, 60, 50)); cui_text_center(&c, f_b, y.x, y.y, y.w, y.h, ja ? "\xe5\x89\x8a\xe9\x99\xa4" : "Remove", RGB(255, 255, 255));
}

static int hit(int x, int y, int *part) {
    *part = 0; if (!in(list_rect(), x, y)) return -1;
    for (int row = 0; row < nshown; ++row) {
        if (in(dl_rect(row), x, y)) { *part = 1; return shown[row]; }
        if (in(tr_rect(row), x, y)) { *part = 2; return shown[row]; }
        int ry = list_rect().y + row * ROW_H - scroll_y; if (y >= ry && y < ry + ROW_H) return shown[row];
    }
    return -1;
}
static void on_click(int x, int y) {
    if (confirm_app >= 0) {                                       /* a warning is showing: it takes every click */
        if (in(dlg_yes(), x, y)) { if (++confirm_step >= warnings()) { int i = confirm_app; confirm_app = -1; remove_app(&apps[i]); } }
        else if (in(dlg_no(), x, y) || !in(dlg_rect(), x, y)) confirm_app = -1;
        return;
    }
    int part; int ai = hit(x, y, &part);
    if (ai < 0) return;
    app_t *a = &apps[ai];
    if (part == 1) { if (!installed(a) || has_update(a)) start_install(ai); }
    else if (part == 2 && installed(a) && !installing) { if (warnings() == 0) remove_app(a); else { confirm_app = ai; confirm_step = 0; } }
}

/* ------------------------------------------------------------- loading (one transfer at a time, polled from the event loop) */
enum { L_CATALOG, L_ICONS, L_IDLE };
static int phase = L_CATALOG, icon_i; static bool started;
static void load_step(void) {
    if (phase == L_IDLE) return;
    char url[400];
    if (!started) {
        if (phase == L_CATALOG) snprintf(url, sizeof url, "%scatalog.manifest", base_url);
        else {
            while (icon_i < napps && (apps[icon_i].icon_px || apps[icon_i].icon_tried)) icon_i++;
            if (icon_i >= napps) { phase = L_IDLE; return; }
            apps[icon_i].icon_tried = true; snprintf(url, sizeof url, "%s%s/%s", base_url, apps[icon_i].id, apps[icon_i].icon);
        }
        if (!fetch_start(url)) { snprintf(status, sizeof status, "No network."); phase = L_IDLE; return; }
        started = true; return;
    }
    int r = fetch_step(phase == L_CATALOG ? 64 * 1024 : 512 * 1024);
    if (r == 0) return;
    started = false;
    if (phase == L_CATALOG) {
        if (r < 0) { snprintf(status, sizeof status, "Could not reach the app server (check the network)."); phase = L_IDLE; return; }
        bool ok = catalog_parse((char *)job_buf, job_len); free(job_buf); job_buf = NULL;
        if (!ok) { snprintf(status, sizeof status, "The list of apps is not trusted (signature mismatch)."); napps = 0; phase = L_IDLE; return; }
        refresh_installed(); filter(); status[0] = 0; phase = L_ICONS; icon_i = 0;
        snprintf(status, sizeof status, "%d app(s)", napps);
    } else {
        if (r > 0) { load_icon(&apps[icon_i], job_buf, job_len); free(job_buf); job_buf = NULL; }
        icon_i++;
    }
}

int main(void) {
    snprintf(base_url, sizeof base_url, "%s", COS_APPS_URL);
    { char b[300]; ssize_t n = cos_read_file("/system/apps/url.txt", b, sizeof b - 1); if (n > 0) { b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; if (!strncmp(b, "http", 4)) snprintf(base_url, sizeof base_url, "%s", b); } }
    size_t bl = strlen(base_url); if (bl && base_url[bl - 1] != '/' && bl + 1 < sizeof base_url) { base_url[bl] = '/'; base_url[bl + 1] = 0; }
    cos_win_info_t wi; int64_t h = cos_win2_create("C-OS Apps", 760, 520, &wi); if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(17); f_h = cui_font_bold(28);
    ja = (cos_ui_prefs() & COS_UI_JAPANESE) != 0;
    if (ja) {                                                       /* the Japanese warnings need the Japanese font */
        cos_stat_t fs; static uint8_t *ttf;
        if (cos_stat("/system/fonts/NotoSansJP-Regular.ttf", &fs) == 0 && fs.size > 0 && fs.size < (8u << 20) && (ttf = malloc(fs.size)) && cos_read_file("/system/fonts/NotoSansJP-Regular.ttf", ttf, fs.size) == (ssize_t)fs.size) {
            cui_font_set_fallback(f_s, cui_font_load(ttf, 13)); cui_font_set_fallback(f_m, cui_font_load(ttf, 16)); cui_font_set_fallback(f_b, cui_font_load(ttf, 17));
        } else ja = false;
    }
    for (;;) {
        load_step();
        if (installing) {
            inst_t pr = read_progress();
            if (pr.ok && (!strcmp(pr.state, "done") || !strcmp(pr.state, "error"))) {
                installing = false; refresh_installed();
                snprintf(status, sizeof status, !strcmp(pr.state, "done") ? "%s is installed. Its icon is on the desktop." : "Stopped: %s", !strcmp(pr.state, "done") ? apps[busy_app].title : pr.msg);
                busy_app = -1;
            }
        }
        draw(); draw_dialog(); cos_win2_present(h);
        cos_win_event_t ev; int r = cos_win2_wait(h, &ev, (phase != L_IDLE || installing) ? 60 : 400);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_KEY) {
                if (ev.special == COS_KEY_BACKSPACE) { if (nsearch) search[--nsearch] = 0; filter(); scroll_y = 0; }
                else if (ev.ascii >= 32 && ev.ascii < 127 && nsearch < (int)sizeof search - 1) { search[nsearch++] = ev.ascii; search[nsearch] = 0; filter(); scroll_y = 0; }
            } else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) on_click(ev.x, ev.y);
            else if (ev.type == COS_EV_MOUSE_MOVE) hover = hit(ev.x, ev.y, &hover_part);
            else if (ev.type == COS_EV_WHEEL) { scroll_y -= ev.wheel * 30; clamp_scroll(); }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
