/**
 * gui_windows.c - GUI Window Implementations
 * 
 * All window type content drawing (File Manager, Terminal, Calculator, etc.)
 * Maintains original design from gui.c
 */

#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"
#include "fs.h"
#include "storage.h"
#include "../drivers/input/mouse_minimal.h"
#include <string.h>
#include <stdio.h>
#include "cosnet_state.h"

const char* gui_text(const char* en, const char* ja);

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif



static void scopy_local(char* dst, const char* src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < max_len && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

static size_t slen_local(const char* s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}





















typedef struct {
    fs_entry_t* entry;
    bool parent;
} fm_row_item_local_t;




























/* External globals */
extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int active_window;
extern int gui_clip_x, gui_clip_y, gui_clip_w, gui_clip_h;
extern bool gui_clip_enabled;

/* Color definitions matching original */
#define C_TEXT         rgb(40,  45,  60)
#define C_CONTENT_BG   rgb(250, 252, 255)
#define C_TERM_BG      rgb(22,  28,  40)
#define C_TERM_TEXT    rgb(190, 235, 190)
#define C_TERM_PROMPT  rgb(120, 190, 255)
#define C_INPUT_BG     rgb(255, 255, 255)
#define C_INPUT_BORDER rgb(150, 165, 190)
#define C_BORDER       rgb(160, 175, 200)
#define C_SELECTED     rgb(50,  100, 180)
#define C_ACCENT       rgb(50,  100, 180)
#ifndef C_TEXT_LIGHT
#define C_TEXT_LIGHT   rgb(235, 240, 250)
#endif

/* Simplified calculator state */
static char calc_display[32] = "0";
static double calc_acc = 0;
static char calc_op = 0;
static int calc_clear_on_input = 0;

/* Simple music player state */
typedef struct {
    bool loaded;
    bool playing;
    char path[256];
    char title[64];
    uint64_t size_bytes;
    uint64_t duration_ms;
    uint64_t position_ms;
    uint64_t volume;
} music_player_state_t;

static music_player_state_t g_music = {0};

static void gui_copy_leaf_name(char* out, size_t out_size, const char* path) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char* leaf = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') leaf = p + 1;
    }
    scopy_local(out, leaf, out_size);
}

void WEAK music_player_open(const char* path) {
    memset(&g_music, 0, sizeof(g_music));
    if (!path || !path[0]) return;
    scopy_local(g_music.path, path, sizeof(g_music.path));
    gui_copy_leaf_name(g_music.title, sizeof(g_music.title), path);

    fs_entry_t* entry = fs_find(path);
    if (entry) {
        g_music.loaded = TRUE;
        g_music.size_bytes = entry->size;
        g_music.duration_ms = (entry->size > 0) ? (entry->size * 1000ULL) / 16000ULL : 0;
        g_music.volume = 75;
        g_music.position_ms = 0;
        g_music.playing = FALSE;
    }
}


/* Draw a file/folder icon */



/* Simple syntax highlight: detect keyword type */








/* Pseudo-random spectrum for visualizer */
static uint8_t mp3_vis_bars[32] = {0};
static int mp3_vis_tick = 0;

static void mp3_update_visualizer(bool playing) {
    mp3_vis_tick++;
    for (int i = 0; i < 32; i++) {
        if (playing) {
            /* Simulate spectrum with pseudo-random motion */
            int seed = (mp3_vis_tick * 17 + i * 31 + i * mp3_vis_tick / 4) & 0xFF;
            int target = (seed % 60) + (i < 8 ? 20 : i < 16 ? 30 : i < 24 ? 15 : 10);
            if (mp3_vis_bars[i] < (uint8_t)target) mp3_vis_bars[i]++;
            else if (mp3_vis_bars[i] > (uint8_t)target) mp3_vis_bars[i]--;
        } else {
            if (mp3_vis_bars[i] > 0) mp3_vis_bars[i]--;
        }
    }
}

void WEAK draw_music_player(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 4, cy = w->y + 34, cw = w->w - 8, ch = w->h - 38;
    minimal_mouse_t* ms = _get_mouse();

    /* Dark theme background */
    vga_fill_rect(cx, cy, cw, ch, rgb(18, 22, 34));
    vga_draw_rect(cx, cy, cw, ch, rgb(50, 70, 120));

    /* Header gradient */
    for (int gy = 0; gy < 36; gy++) {
        float t = (float)gy / 36.0f;
        int r = (int)(30 - t * 8);
        int gv = (int)(36 - t * 10);
        int bv = (int)(56 - t * 14);
        vga_fill_rect(cx, cy + gy, cw, 1, rgb((uint8_t)r, (uint8_t)gv, (uint8_t)bv));
    }
    vga_fill_rect(cx, cy + 36, cw, 1, rgb(50, 70, 120));

    /* Music note icon */
    vga_fill_rect(cx + 14, cy + 10, 3, 12, rgb(100, 180, 255));
    vga_fill_rect(cx + 14, cy + 10, 8, 3, rgb(100, 180, 255));
    vga_fill_rect(cx + 19, cy + 10, 3, 9, rgb(80, 160, 235));
    vga_fill_circle(cx + 15, cy + 22, 3, rgb(100, 180, 255));
    vga_fill_circle(cx + 20, cy + 19, 3, rgb(80, 160, 235));

    /* Title */
    vga_draw_string(cx + 36, cy + 10, gui_text("Music Player", "音楽プレーヤー"), rgb(180, 210, 255), 0xFFFFFFFF);

    if (!g_music.loaded) {
        /* Empty state */
        int box_y = cy + 50;
        vga_fill_rounded_rect(cx + 16, box_y, cw - 32, 80, 8, rgb(28, 34, 52));
        vga_draw_rounded_rect(cx + 16, box_y, cw - 32, 80, 8, rgb(60, 80, 130));
        /* Big music note */
        vga_fill_rect(cx + cw/2 - 10, box_y + 15, 4, 20, rgb(80, 100, 150));
        vga_fill_rect(cx + cw/2 - 10, box_y + 15, 14, 5, rgb(80, 100, 150));
        vga_fill_rect(cx + cw/2 + 0, box_y + 15, 4, 16, rgb(70, 90, 140));
        vga_fill_circle(cx + cw/2 - 8, box_y + 35, 5, rgb(80, 100, 150));
        vga_fill_circle(cx + cw/2 + 2, box_y + 31, 5, rgb(70, 90, 140));
        vga_draw_string(cx + cw/2 - (int)slen_local(gui_text("No track loaded", "トラックが読み込まれていません")) * FONT_W / 2, box_y + 55, gui_text("No track loaded", "トラックが読み込まれていません"), rgb(120, 140, 180), 0xFFFFFFFF);

        /* Load button */
        int lb_x = cx + cw/2 - 60, lb_y = box_y + 96;
        bool hov_load = (ms && ms->x >= lb_x && ms->x < lb_x + 120 && ms->y >= lb_y && ms->y < lb_y + 28);
        vga_fill_rounded_rect(lb_x, lb_y, 120, 28, 6, hov_load ? rgb(80, 150, 255) : rgb(60, 120, 220));
        vga_draw_rounded_rect(lb_x, lb_y, 120, 28, 6, rgb(100, 160, 255));
        vga_draw_string(lb_x + (120 - (int)slen_local(gui_text("Open File Manager", "ファイルマネージャーを開く")) * FONT_W) / 2, lb_y + (28 - FONT_H) / 2, gui_text("Open File Manager", "ファイルマネージャーを開く"), rgb(220, 235, 255), 0xFFFFFFFF);
        return;
    }

    /* Album art placeholder */
    int art_x = cx + 16, art_y = cy + 44, art_size = 80;
    vga_fill_rounded_rect(art_x, art_y, art_size, art_size, 8, rgb(30, 40, 65));
    vga_draw_rounded_rect(art_x, art_y, art_size, art_size, 8, rgb(60, 90, 150));
    /* Vinyl record effect */
    vga_fill_circle(art_x + art_size/2, art_y + art_size/2, 30, rgb(22, 28, 44));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 30, rgb(28, 46, 84));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 22, rgb(40, 55, 90));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 14, rgb(35, 48, 78));
    vga_fill_circle(art_x + art_size/2, art_y + art_size/2, 5, rgb(60, 80, 130));
    /* Rotating indicator if playing */
    if (g_music.playing) {
        int angle_idx = (int)(g_music.position_ms / 50) % 8;
        int dx[] = {0, 8, 11, 8, 0, -8, -11, -8};
        int dy[] = {-11, -8, 0, 8, 11, 8, 0, -8};
        vga_fill_rect(art_x + art_size/2 + dx[angle_idx] - 1,
                      art_y + art_size/2 + dy[angle_idx] - 1, 3, 3, rgb(100, 180, 255));
    }

    /* Track info */
    int info_x = art_x + art_size + 12;
    int info_y = art_y;
    vga_draw_string(info_x, info_y, g_music.title, rgb(220, 235, 255), 0xFFFFFFFF);
    /* Truncate path */
    char short_path[40];
    int pl = 0;
    const char* pp = g_music.path;
    while (*pp) pp++;
    pp = g_music.path;
    while (*pp) { short_path[pl++] = *pp++; if (pl >= 38) break; }
    short_path[pl] = '\0';
    vga_draw_string(info_x, info_y + FONT_H + 8, short_path, rgb(72, 88, 116), 0xFFFFFFFF);

    /* Duration */
    char dur_buf[32];
    uint64_t pos_s = g_music.position_ms / 1000ULL;
    uint64_t dur_s = g_music.duration_ms / 1000ULL;
    int di = 0;
    /* pos mm:ss */
    uint64_t pm = pos_s / 60, ps = pos_s % 60;
    dur_buf[di++] = (char)('0' + pm / 10); dur_buf[di++] = (char)('0' + pm % 10);
    dur_buf[di++] = ':'; dur_buf[di++] = (char)('0' + ps / 10); dur_buf[di++] = (char)('0' + ps % 10);
    dur_buf[di++] = ' '; dur_buf[di++] = '/'; dur_buf[di++] = ' ';
    uint64_t dm = dur_s / 60, ds = dur_s % 60;
    dur_buf[di++] = (char)('0' + dm / 10); dur_buf[di++] = (char)('0' + dm % 10);
    dur_buf[di++] = ':'; dur_buf[di++] = (char)('0' + ds / 10); dur_buf[di++] = (char)('0' + ds % 10);
    dur_buf[di] = '\0';
    vga_draw_string(info_x, info_y + (FONT_H + 8) * 2, dur_buf, rgb(140, 170, 220), 0xFFFFFFFF);

    /* Volume bar */
    vga_draw_string(info_x, info_y + (FONT_H + 8) * 3, gui_text("Vol:", "音量"), rgb(72, 88, 116), 0xFFFFFFFF);
    int vol_bar_x = info_x + 32, vol_bar_y = info_y + (FONT_H + 8) * 3 + 2, vol_bar_w = 80, vol_bar_h = 12;
    vga_fill_rounded_rect(vol_bar_x, vol_bar_y, vol_bar_w, vol_bar_h, 3, rgb(30, 40, 65));
    int vol_fill = (int)(vol_bar_w * g_music.volume / 100);
    if (vol_fill > 0) {
        vga_fill_rounded_rect(vol_bar_x, vol_bar_y, vol_fill, vol_bar_h, 3, rgb(80, 180, 120));
    }
    vga_draw_rounded_rect(vol_bar_x, vol_bar_y, vol_bar_w, vol_bar_h, 3, rgb(28, 46, 84));

    /* Progress bar */
    int prog_y = art_y + art_size + 20;
    int prog_x = cx + 16, prog_w = cw - 32, prog_h = 10;
    vga_fill_rounded_rect(prog_x, prog_y, prog_w, prog_h, 4, rgb(28, 36, 56));
    uint64_t fill_w = (g_music.duration_ms > 0) ? ((uint64_t)prog_w * g_music.position_ms) / g_music.duration_ms : 0;
    if (fill_w > (uint64_t)prog_w) fill_w = (uint64_t)prog_w;
    if (fill_w > 0) {
        /* Gradient progress */
        for (int px = 0; px < (int)fill_w; px++) {
            float pt = (float)px / (float)prog_w;
            int pr = (int)(60 + pt * 80);
            int pg = (int)(140 + pt * 40);
            int pb = (int)(255 - pt * 60);
            vga_fill_rect(prog_x + px, prog_y, 1, prog_h, rgb((uint8_t)pr, (uint8_t)pg, (uint8_t)pb));
        }
        /* Thumb */
        vga_fill_circle(prog_x + (int)fill_w, prog_y + prog_h/2, 6, rgb(200, 230, 255));
    }
    vga_draw_rounded_rect(prog_x, prog_y, prog_w, prog_h, 4, rgb(28, 46, 84));

    /* Spectrum visualizer */
    int vis_y = prog_y + prog_h + 14;
    int vis_h = 50;
    int vis_x = cx + 16;
    int vis_w = cw - 32;
    vga_fill_rect(vis_x, vis_y, vis_w, vis_h, rgb(18, 24, 38));
    vga_draw_rect(vis_x, vis_y, vis_w, vis_h, rgb(40, 55, 90));
    mp3_update_visualizer(g_music.playing);
    int bar_count = 32;
    int bar_w2 = (vis_w - 2) / bar_count;
    for (int i = 0; i < bar_count; i++) {
        int bh2 = (int)mp3_vis_bars[i] * vis_h / 80;
        if (bh2 < 1) bh2 = 1;
        int bx = vis_x + 1 + i * bar_w2;
        int by2 = vis_y + vis_h - bh2;
        /* Color gradient: blue -> cyan -> green */
        float fi = (float)i / (float)bar_count;
        int br = (int)(40 + fi * 60);
        int bg2 = (int)(120 + fi * 100);
        int bb = (int)(255 - fi * 120);
        vga_fill_rect(bx, by2, bar_w2 - 1, bh2, rgb((uint8_t)br, (uint8_t)bg2, (uint8_t)bb));
        /* Peak dot */
        vga_fill_rect(bx, by2 - 2, bar_w2 - 1, 2, rgb(220, 240, 255));
    }

    /* Control buttons */
    int ctrl_y = vis_y + vis_h + 14;
    int btn_y_center = ctrl_y + 18;
    int center_x = cx + cw / 2;

    /* Prev button */
    int prev_x = center_x - 120;
    bool hov_prev = (ms && ms->x >= prev_x - 18 && ms->x < prev_x + 18 && ms->y >= btn_y_center - 18 && ms->y < btn_y_center + 18);
    vga_fill_circle(prev_x, btn_y_center, 16, hov_prev ? rgb(60, 80, 130) : rgb(35, 48, 78));
    vga_draw_circle(prev_x, btn_y_center, 16, rgb(60, 90, 150));
    /* << symbol */
    vga_fill_rect(prev_x - 8, btn_y_center - 6, 3, 12, rgb(160, 190, 230));
    vga_fill_rect(prev_x - 4, btn_y_center - 6, 8, 12, rgb(160, 190, 230));

    /* Play/Pause button (larger) */
    bool hov_play = (ms && ms->x >= center_x - 22 && ms->x < center_x + 22 && ms->y >= btn_y_center - 22 && ms->y < btn_y_center + 22);
    vga_fill_circle(center_x, btn_y_center, 20, hov_play ? rgb(80, 160, 255) : rgb(60, 130, 220));
    vga_draw_circle(center_x, btn_y_center, 20, rgb(120, 180, 255));
    if (g_music.playing) {
        /* Pause icon */
        vga_fill_rect(center_x - 7, btn_y_center - 8, 5, 16, rgb(240, 248, 255));
        vga_fill_rect(center_x + 2, btn_y_center - 8, 5, 16, rgb(240, 248, 255));
    } else {
        /* Play triangle */
        for (int py = 0; py < 14; py++) {
            int pw = py * 12 / 14;
            vga_fill_rect(center_x - 5 + py/2, btn_y_center - 7 + py, pw, 1, rgb(240, 248, 255));
        }
    }

    /* Next button */
    int next_x = center_x + 120;
    bool hov_next = (ms && ms->x >= next_x - 18 && ms->x < next_x + 18 && ms->y >= btn_y_center - 18 && ms->y < btn_y_center + 18);
    vga_fill_circle(next_x, btn_y_center, 16, hov_next ? rgb(60, 80, 130) : rgb(35, 48, 78));
    vga_draw_circle(next_x, btn_y_center, 16, rgb(60, 90, 150));
    /* >> symbol */
    vga_fill_rect(next_x - 4, btn_y_center - 6, 8, 12, rgb(160, 190, 230));
    vga_fill_rect(next_x + 5, btn_y_center - 6, 3, 12, rgb(160, 190, 230));

    /* Stop button */
    int stop_x = center_x - 60;
    bool hov_stop = (ms && ms->x >= stop_x - 14 && ms->x < stop_x + 14 && ms->y >= btn_y_center - 14 && ms->y < btn_y_center + 14);
    vga_fill_circle(stop_x, btn_y_center, 12, hov_stop ? rgb(220, 80, 80) : rgb(180, 60, 60));
    vga_draw_circle(stop_x, btn_y_center, 12, rgb(240, 100, 100));
    vga_fill_rect(stop_x - 5, btn_y_center - 5, 10, 10, rgb(240, 248, 255));

    /* Vol- button */
    int volm_x = center_x + 60;
    bool hov_volm = (ms && ms->x >= volm_x - 14 && ms->x < volm_x + 14 && ms->y >= btn_y_center - 14 && ms->y < btn_y_center + 14);
    vga_fill_circle(volm_x, btn_y_center, 12, hov_volm ? rgb(80, 100, 160) : rgb(50, 70, 120));
    vga_draw_circle(volm_x, btn_y_center, 12, rgb(80, 110, 170));
    vga_fill_rect(volm_x - 5, btn_y_center - 1, 10, 2, rgb(200, 220, 255));
    vga_fill_rect(volm_x + 3, btn_y_center - 4, 2, 8, rgb(200, 220, 255));

    /* Handle clicks */
    if (ms && ms->left_click) {
        if (hov_play) g_music.playing = !g_music.playing;
        else if (hov_stop) { g_music.playing = FALSE; g_music.position_ms = 0; }
        else if (hov_prev && g_music.position_ms > 3000) { g_music.position_ms = 0; }
        else if (hov_next && g_music.duration_ms > 0) { g_music.position_ms = g_music.duration_ms; }
        else if (hov_volm && g_music.volume < 100) { g_music.volume += 5; }
        /* Click on progress bar to seek */
        else if (ms->x >= prog_x && ms->x < prog_x + prog_w &&
                 ms->y >= prog_y - 4 && ms->y < prog_y + prog_h + 4 && g_music.duration_ms > 0) {
            g_music.position_ms = (uint64_t)(ms->x - prog_x) * g_music.duration_ms / (uint64_t)prog_w;
        }
    }

    /* Advance position */
    if (g_music.playing && g_music.duration_ms > 0 && g_music.position_ms < g_music.duration_ms) {
        g_music.position_ms += 16;
        if (g_music.position_ms >= g_music.duration_ms) {
            g_music.position_ms = g_music.duration_ms;
            g_music.playing = FALSE;
        }
    }
}




/* File manager click handling */
extern const char* fs_read_file_at(const char* path, const char* name);







