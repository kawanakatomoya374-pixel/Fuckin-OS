/* dlprogress.c-os - the downloads window: one row per browser download with a bar, the size, and a Cancel button.
 * The kernel (src/netsurf/cos_gui_table.c) writes /tmp/.cosdl ("ID<TAB>STATE<TAB>GOT<TAB>TOTAL<TAB>NAME" per line; STATE 0 running, 1 done, 2 failed, 3 cancelled);
 * Cancel appends the id to /tmp/.cosdl.cancel. The window closes by itself 4 seconds after the last download has ended, and is opened again by the next one. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cos.h"
#include "cos_ui.h"

#define MAXD 8
typedef struct { int id, state; unsigned long long got, total; char name[100]; } dl_t;
static dl_t d[MAXD]; static int nd;

static void load(void) {
    static char b[1500]; nd = 0;
    ssize_t n = cos_read_file("/tmp/.cosdl", b, sizeof b - 1); if (n <= 0) return;
    b[n] = 0;
    for (char *p = b; *p && nd < MAXD; ) {
        char *e = strchr(p, '\n'); if (e) *e = 0;
        dl_t *x = &d[nd]; char *t[5]; int k = 0; t[k++] = p;
        for (char *q = p; *q && k < 5; ++q) if (*q == '\t') { *q = 0; t[k++] = q + 1; }
        if (k == 5) { x->id = atoi(t[0]); x->state = atoi(t[1]); x->got = strtoull(t[2], 0, 10); x->total = strtoull(t[3], 0, 10); snprintf(x->name, sizeof x->name, "%s", t[4]); nd++; }
        if (!e) break; p = e + 1;
    }
}
static void size_str(unsigned long long v, char *o, size_t n) {
    if (v >= 1ull << 30) snprintf(o, n, "%llu.%02llu GB", v >> 30, ((v & ((1ull << 30) - 1)) * 100) >> 30);
    else if (v >= 1ull << 20) snprintf(o, n, "%llu.%02llu MB", v >> 20, ((v & ((1ull << 20) - 1)) * 100) >> 20);
    else if (v >= 1024) snprintf(o, n, "%llu KB", v >> 10);
    else snprintf(o, n, "%llu B", v);
}
static bool in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

int main(void) {
    cos_settings_t st; bool jp = cos_settings_get(&st) == 0 && st.japanese, dark = cos_settings_get(&st) != 0 || st.dark_mode != 0;
    const int W = 520, ROW = 64, H = 40 + MAXD * ROW;
    cos_win_info_t wi; int64_t h = cos_win2_create(jp ? "ダウンロード" : "Downloads", W, 40 + 2 * ROW, &wi);
    if (h <= 0) return 1;
    (void)H;
    cui_canvas c; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *f_name = cui_font_bold(14), *f_small = cui_font_default(12), *f_btn = cui_font_default(12);
    uint32_t BG = dark ? 0x1B1F2B : 0xF5F7FB, PANEL = dark ? 0x232838 : 0xFFFFFF, TEXT = dark ? 0xE8ECF5 : 0x1A2030, MUTED = dark ? 0x9AA3B8 : 0x5A6478, BTN = dark ? 0x32394D : 0xE1E6F0, ACC = 0x3D7BF0, OK = 0x3FB950, BAD = 0xE5484D;
    uint64_t idle_t = 0; int hover = -1;
    for (;;) {
        load();
        bool running = false; for (int i = 0; i < nd; ++i) if (d[i].state == 0) running = true;
        if (running) idle_t = 0; else if (!idle_t) idle_t = cos_time_ms();
        if (!running && idle_t && cos_time_ms() - idle_t > 4000) break;
        cui_fill(&c, 0, 0, wi.width, wi.height, BG);
        int y = 8;
        if (!nd) cui_text(&c, f_small, 16, y + 8, jp ? "ダウンロードはありません" : "No downloads", MUTED);
        for (int i = 0; i < nd && y + ROW <= wi.height; ++i, y += ROW) {
            dl_t *x = &d[i]; char a[40], b[40], line[120];
            cui_round_rect(&c, 8, y, W - 16, ROW - 6, 8, PANEL);
            cui_text_fit(&c, f_name, 20, y + 6, W - 150, x->name, TEXT);
            int bx = 20, bw = W - 150, by = y + 30;
            cui_round_rect(&c, bx, by, bw, 8, 4, BTN);
            int fill = x->state == 1 ? bw : x->total ? (int)((double)x->got * bw / (double)x->total) : (int)((cos_time_ms() / 12) % bw);
            if (fill > bw) fill = bw;
            if (x->state == 0 && !x->total) cui_round_rect(&c, bx + fill / 2, by, bw / 4, 8, 4, ACC);
            else if (fill > 0) cui_round_rect(&c, bx, by, fill < 8 ? 8 : fill, 8, 4, x->state >= 2 ? BAD : x->state == 1 ? OK : ACC);
            size_str(x->got, a, sizeof a); size_str(x->total, b, sizeof b);
            if (x->state == 1) snprintf(line, sizeof line, "%s%s", a, jp ? "  完了" : "  done");
            else if (x->state == 2) snprintf(line, sizeof line, "%s", jp ? "失敗しました" : "Failed");
            else if (x->state == 3) snprintf(line, sizeof line, "%s", jp ? "取り消しました" : "Cancelled");
            else if (x->total) snprintf(line, sizeof line, "%s / %s  (%d%%)", a, b, (int)(x->got * 100 / x->total));
            else snprintf(line, sizeof line, "%s", a);
            cui_text(&c, f_small, 20, y + 42, line, MUTED);
            if (x->state == 0) {
                int cx = W - 110, cy = y + 16, cw = 90, ch = 28;
                cui_round_rect(&c, cx, cy, cw, ch, 6, hover == i ? cui_mix(BTN, TEXT, 30) : BTN);
                cui_text_center(&c, f_btn, cx, cy, cw, ch, jp ? "取り消し" : "Cancel", TEXT);
            }
        }
        cos_win2_present(h);
        cos_win_event_t ev; int r = cos_win2_wait(h, &ev, 200);
        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) break;
            if (ev.type == COS_EV_MOUSE_MOVE) { hover = -1; for (int i = 0; i < nd; ++i) if (in(ev.x, ev.y, W - 110, 8 + i * ROW + 16, 90, 28)) hover = i; }
            if (ev.type == COS_EV_MOUSE_DOWN) for (int i = 0; i < nd; ++i) if (d[i].state == 0 && in(ev.x, ev.y, W - 110, 8 + i * ROW + 16, 90, 28)) {
                char b[16]; static char old[64]; ssize_t n = cos_read_file("/tmp/.cosdl.cancel", old, sizeof old - 16); if (n < 0) n = 0; old[n] = 0;
                snprintf(b, sizeof b, "%d\n", d[i].id); strcat(old, b); cos_write_file("/tmp/.cosdl.cancel", old, strlen(old));
            }
        }
    }
    cos_win2_close(h);
    return 0;
}
