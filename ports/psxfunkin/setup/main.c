/* psxsetup - makes the data directory of PSXFunkin on the user's own device (ports/psxfunkin/README.md).
 *
 * The game's graphics and music are not part of C-OS or of its app pack. recipe.txt (shipped with the pack, see tools/make_recipe.py) lists the files the game needs, where they
 * are in the PSXFunkin repository (github.com/IgorSou3000/psxfunkin, a pinned commit) and their SHA-256. This program downloads them from raw.githubusercontent.com, refuses
 * any file whose hash differs, turns the PNG textures into PlayStation TIM files (the same conversion as the repository's tools/funkintimpak) and writes everything to
 * /apps/psxfunkin/data. It can be run again: finished files are skipped, so an interrupted download continues. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"
#include "bearssl.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#include "stb_image.c"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define SETUP_DIR "/apps/psxfunkin/setup"
#define DATA_DIR "/apps/psxfunkin/data"
#define GAME "/apps/psxfunkin/psxfunkin.c-os"
#define MAX_ENT 160
#define MAX_FILE (8u * 1024u * 1024u)

typedef struct { bool tim; char out[80], src[120], sha[65], src2[120], sha2[65]; uint64_t size, size2; bool done; } ent_t;
static ent_t ent[MAX_ENT]; static int nent; static char commit[48]; static uint64_t total_bytes, done_bytes;

static cui_canvas c; static cui_font *f_s, *f_m, *f_b, *f_h;
static int win_w, win_h;
static char msg[200] = "";
enum { S_READY, S_RUN, S_DONE, S_ERR }; static int state = S_READY;
static int cur; static int phase;           /* which entry, and which of its (one or two) downloads */
static int job = -1; static uint64_t job_t0;
static uint8_t *first_buf; static size_t first_len;     /* a tim's PNG, kept while its parameter file is fetched */

/* ------------------------------------------------------------------ helpers */
static void sha256_hex(const void *p, size_t n, char out[65]) {
    br_sha256_context x; unsigned char h[32]; br_sha256_init(&x); br_sha256_update(&x, p, n); br_sha256_out(&x, h);
    for (int i = 0; i < 32; ++i) { out[i * 2] = "0123456789abcdef"[h[i] >> 4]; out[i * 2 + 1] = "0123456789abcdef"[h[i] & 15]; } out[64] = 0;
}
static void mkdirs(const char *full) {
    char p[300]; snprintf(p, sizeof p, "%s", full);
    for (char *s = p + 1; *s; ++s) if (*s == '/') { *s = 0; cos_mkdir(p); *s = '/'; }
}
static bool write_file(const char *path, const void *d, size_t n) {
    mkdirs(path); int fd = cos_open(path, COS_O_WRONLY_CREATE); if (fd < 0) return false;
    const uint8_t *p = d; while (n) { ssize_t w = cos_fd_write(fd, p, n > 65536 ? 65536 : n); if (w <= 0) { cos_close(fd); return false; } p += w; n -= (size_t)w; }
    cos_close(fd); return true;
}
static bool file_size_is(const char *path, uint64_t size) { cos_stat_t st; return cos_stat(path, &st) == 0 && !st.is_dir && st.size == size; }

/* ------------------------------------------------------------------ recipe */
static bool load_recipe(void) {
    static char buf[40000]; ssize_t n = cos_read_file(SETUP_DIR "/recipe.txt", buf, sizeof buf - 1); if (n <= 0) return false; buf[n] = 0;
    nent = 0; total_bytes = 0;
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n'); if (nl) *nl = 0;
        if (!strncmp(l, "commit ", 7)) snprintf(commit, sizeof commit, "%s", l + 7);
        else if (nent < MAX_ENT) {
            ent_t *e = &ent[nent]; memset(e, 0, sizeof *e); char a[80], b[120], s[65], b2[120], s2[65]; unsigned long long z = 0, z2 = 0;
            if (!strncmp(l, "copy ", 5) && sscanf(l + 5, "%79s %119s %64s %llu", a, b, s, &z) == 4) { snprintf(e->out, sizeof e->out, "%s", a); snprintf(e->src, sizeof e->src, "%s", b); snprintf(e->sha, sizeof e->sha, "%s", s); e->size = z; nent++; total_bytes += z; }
            else if (!strncmp(l, "tim ", 4) && sscanf(l + 4, "%79s %119s %64s %llu %119s %64s %llu", a, b, s, &z, b2, s2, &z2) == 7) {
                e->tim = true; snprintf(e->out, sizeof e->out, "%s", a); snprintf(e->src, sizeof e->src, "%s", b); snprintf(e->sha, sizeof e->sha, "%s", s); e->size = z;
                snprintf(e->src2, sizeof e->src2, "%s", b2); snprintf(e->sha2, sizeof e->sha2, "%s", s2); e->size2 = z2; nent++; total_bytes += z + z2; }
        }
        l = nl ? nl + 1 : NULL;
    }
    return commit[0] && nent > 0;
}
static void check_done(void) {                                            /* what is already there from an earlier run */
    done_bytes = 0;
    for (int i = 0; i < nent; ++i) {
        char p[200]; snprintf(p, sizeof p, DATA_DIR "/%s", ent[i].out);
        cos_stat_t st; ent[i].done = cos_stat(p, &st) == 0 && !st.is_dir && st.size > 0 && (ent[i].tim || st.size == ent[i].size);
        if (ent[i].done) done_bytes += ent[i].size + ent[i].size2;
    }
}

/* ------------------------------------------------------------------ the PNG -> TIM conversion (tools/funkintimpak by Regan "CuckyDev" Green, MPL-2.0 like the game) */
static bool png_to_tim(const uint8_t *png, size_t pn, const char *txt, const char *outpath, char *why, size_t wn) {
    int tex_x, tex_y, pal_x, pal_y, bpp;
    if (sscanf(txt, "%d %d %d %d %d", &tex_x, &tex_y, &pal_x, &pal_y, &bpp) != 5) { snprintf(why, wn, "bad texture parameters"); return false; }
    int max_colour, width_shift;
    if (bpp == 4) { max_colour = 16; width_shift = 2; } else if (bpp == 8) { max_colour = 256; width_shift = 1; } else { snprintf(why, wn, "bad bpp"); return false; }
    int w, h; stbi_uc *px = stbi_load_from_memory(png, (int)pn, &w, &h, NULL, 4);
    if (!px) { snprintf(why, wn, "cannot read the PNG"); return false; }
    if (w & ((1 << width_shift) - 1)) { stbi_image_free(px); snprintf(why, wn, "bad width"); return false; }
    uint16_t pal[256]; int pals_i = 0; memset(pal, 0, sizeof pal);
    size_t tex_size = ((size_t)(w << 1) >> width_shift) * (size_t)h; uint8_t *tex = malloc(tex_size);
    if (!tex) { stbi_image_free(px); snprintf(why, wn, "out of memory"); return false; }
    stbi_uc *pp = px; uint8_t *tp = tex;
    for (int i = w * h; i > 0; i--, pp += 4) {
        uint16_t rep = 0;
        if (pp[3] & 0x80) rep = (uint16_t)((pp[0] / 8) | ((pp[1] / 8) << 5) | ((pp[2] / 8) << 10) | (1 << 15));
        int pi = 0;
        for (;;) {
            if (pi >= max_colour) { free(tex); stbi_image_free(px); snprintf(why, wn, "too many colours"); return false; }
            if (pi >= pals_i) { pal[pi] = rep; pals_i++; break; }
            if (pal[pi] == rep) break;
            pi++;
        }
        if (bpp == 4) { if (i & 1) { *tp = (uint8_t)(*tp | (pi << 4)); tp++; } else *tp = (uint8_t)pi; } else *tp++ = (uint8_t)pi;
    }
    stbi_image_free(px);
    size_t total = 8 + 12 + 2 * (size_t)max_colour + 12 + tex_size; uint8_t *o = malloc(total), *q = o;
    if (!o) { free(tex); snprintf(why, wn, "out of memory"); return false; }
    #define B(v) (*q++ = (uint8_t)(v))
    B(0x10); B(0); B(0); B(0); B(bpp == 4 ? 0x08 : 0x09); B(0); B(0); B(0);
    uint32_t cl = 12 + 2 * (uint32_t)max_colour; B(cl); B(cl >> 8); B(cl >> 16); B(cl >> 24);
    B(pal_x); B(pal_x >> 8); B(pal_y); B(pal_y >> 8); B(max_colour); B(max_colour >> 8); B(1); B(0);
    for (int i = 0; i < max_colour; ++i) { B(pal[i]); B(pal[i] >> 8); }
    uint32_t tl = 12 + (uint32_t)tex_size; B(tl); B(tl >> 8); B(tl >> 16); B(tl >> 24);
    B(tex_x); B(tex_x >> 8); B(tex_y); B(tex_y >> 8); B(w >> width_shift); B((w >> width_shift) >> 8); B(h); B(h >> 8);
    memcpy(q, tex, tex_size); q += tex_size; free(tex);
    bool ok = write_file(outpath, o, (size_t)(q - o)); free(o);
    if (!ok) snprintf(why, wn, "cannot write %s", outpath);
    return ok;
}

/* ------------------------------------------------------------------ one download at a time */
static bool fetch_start(const char *path) {
    static char base[200];      /* a test server can stand in for GitHub: its address in setup/base_url.txt (every file is still checked against the recipe's SHA-256) */
    if (!base[0]) { snprintf(base, sizeof base, "https://raw.githubusercontent.com/IgorSou3000/psxfunkin"); char b[200]; ssize_t n = cos_read_file(SETUP_DIR "/base_url.txt", b, sizeof b - 1);
        if (n > 0) { b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; if (!strncmp(b, "http", 4)) snprintf(base, sizeof base, "%s", b); } }
    char url[400]; snprintf(url, sizeof url, "%s/%s/%s", base, commit, path);
    job = cos_net_http_start(url); job_t0 = cos_time_ms(); return job >= 0;
}
/* 0 = going, 1 = done (*buf, *len), -1 = failed */
static int fetch_step(uint8_t **buf, size_t *len) {
    cos_http_result_t r; int p = cos_net_http_poll(job, &r);
    if (p == 0) { if (cos_time_ms() - job_t0 > 120000) { cos_net_http_cancel(job); job = -1; return -1; } return 0; }
    int jb = job; job = -1;
    if (p < 0 || r.error || r.status != 200 || r.body_len > MAX_FILE) { if (p >= 0) cos_net_http_cancel(jb); return -1; }
    uint8_t *b = malloc((size_t)r.body_len + 1); if (!b) { cos_net_http_cancel(jb); return -1; }
    ssize_t n = cos_net_http_take(jb, b, (size_t)r.body_len);
    if (n < 0 || (uint64_t)n != r.body_len) { free(b); return -1; }
    b[n] = 0; *buf = b; *len = (size_t)n; return 1;
}
static void fail(const char *m) { snprintf(msg, sizeof msg, "%s", m); state = S_ERR; }

static void run_step(void) {
    if (state != S_RUN) return;
    while (cur < nent && ent[cur].done) cur++;
    if (cur >= nent) {
        static char b[2000]; ssize_t n;
        if ((n = cos_read_file(SETUP_DIR "/FILES.TXT", b, sizeof b - 1)) > 0) write_file(DATA_DIR "/FILES.TXT", b, (size_t)n);
        if ((n = cos_read_file(SETUP_DIR "/OVERLAYS.TXT", b, sizeof b - 1)) > 0) write_file(DATA_DIR "/OVERLAYS.TXT", b, (size_t)n);
        state = S_DONE; snprintf(msg, sizeof msg, "Done. The game data is ready."); return;
    }
    ent_t *e = &ent[cur];
    if (job < 0) {
        if (!fetch_start(phase == 0 ? e->src : e->src2)) { fail("No network."); return; }
        snprintf(msg, sizeof msg, "Downloading %s", e->out); return;
    }
    uint8_t *buf = NULL; size_t len = 0; int r = fetch_step(&buf, &len);
    if (r == 0) return;
    if (r < 0) { fail("Download failed (check the network). Press Retry to continue."); return; }
    char sha[65]; sha256_hex(buf, len, sha);
    if (strcmp(sha, phase == 0 ? e->sha : e->sha2) != 0) { free(buf); snprintf(msg, sizeof msg, "%s does not match the expected file (refused).", e->out); state = S_ERR; return; }
    char path[200]; snprintf(path, sizeof path, DATA_DIR "/%s", e->out);
    if (!e->tim) {
        bool ok = write_file(path, buf, len); free(buf);
        if (!ok) { fail("Cannot write the file (disk full?)."); return; }
        done_bytes += len; e->done = true; cur++;
    } else if (phase == 0) { first_buf = buf; first_len = len; phase = 1; }
    else {
        char why[80]; bool ok = png_to_tim(first_buf, first_len, (const char *)buf, path, why, sizeof why);
        free(first_buf); first_buf = NULL; free(buf);
        if (!ok) { fail(why); return; }
        done_bytes += e->size + e->size2; e->done = true; cur++; phase = 0;
    }
}

/* ------------------------------------------------------------------ window */
typedef struct { int x, y, w, h; } rect_t;
static bool in(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static rect_t btn_rect(void) { return (rect_t){ win_w - 200, win_h - 70, 176, 46 }; }
static int hover;
static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, RGB(30, 32, 38));
    cui_text(&c, f_h, 24, 18, "PSXFunkin data", RGB(255, 255, 255));
    cui_text(&c, f_m, 24, 70, "The game's graphics and music are not part of C-OS.", RGB(210, 214, 224));
    cui_text(&c, f_s, 24, 98, "This downloads them from the PSXFunkin repository on GitHub (github.com/IgorSou3000/psxfunkin),", RGB(150, 156, 170));
    cui_text(&c, f_s, 24, 118, "checks every file against its SHA-256 and prepares them for the game.", RGB(150, 156, 170));
    char t[160]; snprintf(t, sizeof t, "About %u MB to download, the same again on the disk.", (unsigned)((total_bytes + 524288) >> 20)); cui_text(&c, f_s, 24, 142, t, RGB(240, 180, 90));
    cui_text(&c, f_s, 24, 162, "The artwork and music belong to the Friday Night Funkin' team and the song authors.", RGB(110, 116, 130));
    int bw = win_w - 48; unsigned pct = total_bytes ? (unsigned)(done_bytes * 100 / total_bytes) : 0; if (pct > 100) pct = 100;
    cui_round_rect(&c, 24, 200, bw, 16, 8, RGB(44, 48, 56)); if (pct) cui_round_rect(&c, 24, 200, bw * (int)pct / 100 < 16 ? 16 : bw * (int)pct / 100, 16, 8, state == S_ERR ? RGB(190, 110, 40) : RGB(62, 150, 70));
    snprintf(t, sizeof t, "%u%%", pct); cui_text(&c, f_s, 24, 224, t, RGB(170, 176, 190));
    cui_text_fit(&c, f_s, 70, 224, win_w - 94, msg, state == S_ERR ? RGB(240, 180, 90) : RGB(170, 176, 190));
    rect_t b = btn_rect(); const char *label = state == S_READY ? "Download" : state == S_RUN ? "Working..." : state == S_DONE ? "Play" : "Retry";
    bool on = state != S_RUN; uint32_t base = on ? RGB(62, 150, 70) : RGB(70, 74, 82); if (on && hover) base = cui_mix(base, RGB(255, 255, 255), 40);
    cui_round_rect(&c, b.x, b.y, b.w, b.h, 8, base); cui_text_center(&c, f_b, b.x, b.y, b.w, b.h, label, on ? RGB(255, 255, 255) : RGB(150, 154, 162));
}
static void press(void) {
    if (state == S_DONE) { const char *av[2] = { GAME, NULL }; cos_spawn_argv(GAME, av, NULL); return; }
    if (state == S_RUN) return;
    check_done(); cur = 0; phase = 0; if (job >= 0) { cos_net_http_cancel(job); job = -1; } msg[0] = 0; state = S_RUN;
}

int main(int argc, char **argv) {
    cos_win_info_t wi; int64_t h = cos_win2_create("PSXFunkin data", 640, 330, &wi); if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(17); f_h = cui_font_bold(28);
    if (!load_recipe()) { snprintf(msg, sizeof msg, "The list of files is missing: reinstall PSXFunkin from C-OS Apps."); state = S_ERR; } else check_done();
    if (nent && done_bytes >= total_bytes) { state = S_DONE; snprintf(msg, sizeof msg, "The game data is already there."); }
    if (argc > 1 && !strcmp(argv[1], "go") && nent && state == S_READY) press();          /* "go": start at once (the game starts it when its data is missing) */
    for (;;) {
        run_step(); draw(); cos_win2_present(h);
        cos_win_event_t ev; int r = cos_win2_wait(h, &ev, state == S_RUN ? 20 : 300);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_MOUSE_MOVE) hover = in(btn_rect(), ev.x, ev.y);
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT && in(btn_rect(), ev.x, ev.y) && nent) press();
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
