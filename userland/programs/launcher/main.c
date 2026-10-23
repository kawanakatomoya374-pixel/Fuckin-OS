/* launcher.c-os - the game launcher behind the former "Craft" icon. Two tabs, like the editions picker of the desktop Minecraft launcher:
 *   Craft        - fogleman/Craft, the voxel sandbox (userland/programs/craft)
 *   mc-classic-c - the Classic versions of Minecraft rewritten in C (github.com/degradka/mc-classic-c, GPL-3.0), 0.0.11a to 0.30
 * A game is a program of its own: the launcher only starts it (/apps/minecraft/<name>.c-os) and shows whether it is running. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

typedef struct { const char *id, *title, *date, *blurb, *more; } mcver_t;
static const mcver_t k_ver[] = {
    { "c0.0.11a",    "Classic 0.0.11a",    "May 17, 2009",  "Where it starts.",                       "Six tiles, a 256x256x64 level from noise heightmaps, a zombie, grass that spreads." },
    { "c0.0.13a",    "Classic 0.0.13a",    "May 22, 2009",  "Water, lava and bedrock.",               "Flowing and still water and lava, bedrock and a staged world generator." },
    { "c0.0.13a_03", "Classic 0.0.13a_03", "May 22, 2009",  "Rolling hills.",                         "A real heightmap, a soiling stage that puts grass and dirt on top." },
    { "c0.0.14a_08", "Classic 0.0.14a_08", "May 28, 2009",  "Sand, gravel and ores.",                 "Falling sand and gravel, gold, iron and coal ore." },
    { "c0.0.16a_02", "Classic 0.0.16a_02", "June 7, 2009",  "The first multiplayer build.",           "Multiplayer code is part of it; this port plays offline." },
    { "c0.0.17a",    "Classic 0.0.17a",    "June 10, 2009", "Player list and longer chat.",          "Hold Tab to see who is on a server." },
    { "c0.0.18a_02", "Classic 0.0.18a_02", "June 14, 2009", "Chat and teleport fixes.",               "More characters in the chat; servers can move you." },
    { "c0.0.19a_04", "Classic 0.0.19a_04", "June 20, 2009", "Glass, sponge and a hotbar.",            "New tiles and a hotbar at the bottom of the screen." },
    { "c0.0.20a_02", "Classic 0.0.20a_02", "June 20, 2009", "Cloth, flowers and mushrooms.",          "16 colours of cloth, gold blocks, dandelions, roses and mushrooms." },
    { "c0.0.23a_01", "Classic 0.0.23a_01", "July 11, 2009", "Sound and an Options menu.",             "Footsteps, music, invert mouse, render distance." },
    { "c0.24_st_03", "Survival Test 0.24", "Sept 1, 2009",  "Health and the first mobs.",             "Hearts, fall damage, drowning; zombie, skeleton, creeper and pig." },
    { "c0.25_05_st", "Survival Test 0.25", "Sept 3, 2009",  "Smarter mobs and arrows.",               "Mobs chase and fight each other, skeletons shoot arrows." },
    { "c0.27_st",    "Survival Test 0.27", "Oct 24, 2009",  "TNT, spiders and rain.",                 "TNT, spiders, new tiles; F5 toggles rain." },
    { "c0.28_01",    "Classic 0.28_01",    "Oct 27, 2009",  "Creative again.",                        "Block select screen (B), middle-click pick, spawn point (Enter / R)." },
    { "c0.29_02",    "Classic 0.29_02",    "Oct 30, 2009",  "Multiplayer fixes.",                     "Name tags, ops, and exact aiming at slabs and flowers." },
    { "c0.30",       "Classic 0.30",       "Nov 10, 2009",  "The last Classic.",                      "Creative and Survival. Drops, TNT, every mob." },
};
#define NVER ((int)(sizeof k_ver / sizeof k_ver[0]))

static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_h, *f_t;
static int win_w, win_h;
static int s_tab, s_sel = NVER - 1, s_scroll, s_hover = -1;
static int64_t s_pid;                     /* the game this launcher started (0 = none) */
static char s_msg[160];

static bool cos_stat_ok(const char *p) { cos_stat_t st; return cos_stat(p, &st) == 0; }
static bool game_path(char *out, size_t n, int tab, int ver, bool survival) {
    if (tab == 2) snprintf(out, n, "/apps/linux/Linux.c-os");
    else if (tab == 0) snprintf(out, n, "/apps/minecraft/Craft.c-os");
    else snprintf(out, n, "/apps/minecraft/mc-%s%s.c-os", k_ver[ver].id, survival ? "-survival" : "");
    cos_stat_t st; return cos_stat(out, &st) == 0;
}
static bool running(void) {
    if (!s_pid) return false;
    int st = 0; int64_t r = cos_waitpid(s_pid, &st, false);
    if (r == s_pid || r < 0) { s_pid = 0; return false; }
    return true;
}
static void play(bool survival) {
    if (running()) { snprintf(s_msg, sizeof s_msg, "A game is already running."); return; }
    char path[160];
    if (!game_path(path, sizeof path, s_tab, s_sel, survival)) { snprintf(s_msg, sizeof s_msg, "This game is not installed."); return; }
    const char *argv[3] = { path, s_tab == 2 ? "firefox" : NULL, NULL };
    s_pid = cos_spawn_argv(path, argv, NULL);
    if (s_pid <= 0) { s_pid = 0; snprintf(s_msg, sizeof s_msg, "Could not start the game."); }
    else s_msg[0] = 0;
}

typedef struct { int x, y, w, h; } rect_t;
static bool in(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

#define TAB_H 44
static rect_t tab_rect(int i) { return (rect_t){ 16 + i * 170, 8, 160, TAB_H - 8 }; }
static rect_t list_rect(void) { return (rect_t){ 16, TAB_H + 12, 300, win_h - TAB_H - 28 }; }
#define ROW_H 44
static rect_t play_rect(void) { return (rect_t){ win_w - 232, win_h - 76, 208, 52 }; }
static rect_t surv_rect(void) { return (rect_t){ win_w - 232, win_h - 140, 208, 52 }; }
static bool has_survival(void) { return s_tab == 1 && !strcmp(k_ver[s_sel].id, "c0.30"); }

static void button(rect_t r, const char *label, bool enabled, bool hot, uint32_t col) {
    uint32_t base = enabled ? col : RGB(70, 74, 82);
    if (enabled && hot) base = cui_mix(base, RGB(255, 255, 255), 40);
    cui_round_rect(&c, r.x, r.y + 3, r.w, r.h, 8, cui_mix(base, RGB(0, 0, 0), 110));
    cui_round_rect(&c, r.x, r.y, r.w, r.h, 8, base);
    cui_text_center(&c, f_b, r.x, r.y, r.w, r.h, label, enabled ? RGB(255, 255, 255) : RGB(150, 154, 162));
}

static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, RGB(30, 32, 38));
    cui_fill(&c, 0, 0, win_w, TAB_H, RGB(22, 24, 29));
    cui_fill(&c, 0, TAB_H - 2, win_w, 2, RGB(52, 56, 66));
    static const char *tabs[3] = { "Craft", "mc-classic-c", "Linux" };
    for (int i = 0; i < 3; ++i) {
        rect_t r = tab_rect(i); bool on = s_tab == i;
        cui_round_rect(&c, r.x, r.y, r.w, r.h + 4, 8, on ? RGB(30, 32, 38) : (s_hover == 100 + i ? RGB(40, 44, 52) : RGB(22, 24, 29)));
        cui_text_center(&c, on ? f_b : f_m, r.x, r.y, r.w, r.h, tabs[i], on ? RGB(255, 255, 255) : RGB(170, 176, 188));
        if (on) cui_fill(&c, r.x + 14, TAB_H - 4, r.w - 28, 3, RGB(76, 175, 80));
    }
    bool run = running();
    if (s_tab == 0) {
        int x = 40, y = TAB_H + 36;
        cui_text(&c, f_h, x, y, "Craft", RGB(255, 255, 255)); y += 56;
        cui_text(&c, f_m, x, y, "A voxel sandbox: dig, build, explore an endless world.", RGB(200, 205, 215)); y += 28;
        cui_text(&c, f_s, x, y, "Software 3-D renderer, texture atlas, day and night, signs, multiplayer-ready.", RGB(150, 156, 170)); y += 40;
        cui_text(&c, f_s, x, y, "W A S D move   Space jump   Tab fly   mouse look   left / right click break / place", RGB(150, 156, 170)); y += 22;
        cui_text(&c, f_s, x, y, "E / R choose the block   1-9 hotbar   T chat   F12 full screen   Esc release the mouse", RGB(150, 156, 170)); y += 22;
        cui_text(&c, f_s, x, y, "Craft is MIT licensed (github.com/fogleman/Craft).", RGB(110, 116, 130));
    } else if (s_tab == 2) {
        int x = 40, y = TAB_H + 36; bool fx = cos_stat_ok("/linux/usr/lib/firefox/firefox") && cos_stat_ok("/linux/usr/bin/Xtigervnc");
        cui_text(&c, f_h, x, y, "Firefox", RGB(255, 255, 255)); y += 56;
        cui_text(&c, f_m, x, y, "Mozilla Firefox, a Linux program, in a C-OS window.", RGB(200, 205, 215)); y += 28;
        cui_text(&c, f_s, x, y, "The Linux layer runs an X server (TigerVNC) and Firefox; this window is its viewer.", RGB(150, 156, 170)); y += 22;
        cui_text(&c, f_s, x, y, "The first start takes minutes: the Linux programs are emulated and loaded from disk.", RGB(150, 156, 170)); y += 40;
        cui_text(&c, f_s, x, y, fx ? "Linux root file system found in /linux." : "Needs a Linux root file system with Firefox and Xtigervnc in /linux (docs/LINUX_COMPAT.md).", fx ? RGB(120, 200, 130) : RGB(240, 180, 90)); y += 22;
        cui_text(&c, f_s, x, y, "Firefox and the Linux programs are not part of C-OS: they come with the root file system you provide.", RGB(110, 116, 130));
    } else {
        rect_t lr = list_rect();
        cui_round_rect(&c, lr.x, lr.y, lr.w, lr.h, 8, RGB(24, 26, 31));
        cui_clip(&c, lr.x, lr.y, lr.w, lr.h);
        for (int i = 0; i < NVER; ++i) {
            int y = lr.y + 4 + i * ROW_H - s_scroll;
            if (y + ROW_H < lr.y || y > lr.y + lr.h) continue;
            bool sel = i == s_sel; char p[160];
            bool have = game_path(p, sizeof p, 1, i, false);
            if (sel) cui_round_rect(&c, lr.x + 4, y, lr.w - 8, ROW_H - 4, 6, RGB(46, 92, 52));
            else if (s_hover == i) cui_round_rect(&c, lr.x + 4, y, lr.w - 8, ROW_H - 4, 6, RGB(38, 42, 50));
            cui_text(&c, f_m, lr.x + 14, y + 4, k_ver[i].title, have ? RGB(240, 242, 246) : RGB(110, 114, 124));
            cui_text(&c, f_s, lr.x + 14, y + 23, k_ver[i].date, RGB(140, 146, 160));
        }
        cui_unclip(&c);
        int x = lr.x + lr.w + 28, y = TAB_H + 36;
        cui_text(&c, f_h, x, y, k_ver[s_sel].title, RGB(255, 255, 255)); y += 50;
        cui_text(&c, f_s, x, y, k_ver[s_sel].date, RGB(140, 146, 160)); y += 28;
        cui_text(&c, f_m, x, y, k_ver[s_sel].blurb, RGB(210, 214, 224)); y += 30;
        cui_text_fit(&c, f_s, x, y, win_w - x - 24, k_ver[s_sel].more, RGB(150, 156, 170)); y += 40;
        cui_text_fit(&c, f_s, x, y, win_w - x - 24, "mc-classic-c by degradka (GPL-3.0): Minecraft Classic rewritten in C.", RGB(110, 116, 130));
    }
    char p[160]; bool have = game_path(p, sizeof p, s_tab, s_sel, false);
    if (s_msg[0]) cui_text(&c, f_s, win_w - 232, win_h - 164 - (has_survival() ? 64 : 0), s_msg, RGB(240, 180, 90));
    if (has_survival()) {
        rect_t sr = surv_rect(); char q[160]; bool hs = game_path(q, sizeof q, 1, s_sel, true);
        button(sr, run ? "Running..." : "Play Survival", hs && !run, s_hover == 202, RGB(190, 110, 40));
    }
    rect_t pr = play_rect();
    button(pr, run ? "Running..." : (has_survival() ? "Play Creative" : s_tab == 2 ? "Start" : "Play"), have && !run, s_hover == 201, RGB(62, 150, 70));
}

static void on_click(int x, int y) {
    for (int i = 0; i < 3; ++i) if (in(tab_rect(i), x, y)) { s_tab = i; s_msg[0] = 0; return; }
    if (in(play_rect(), x, y)) { play(false); return; }
    if (has_survival() && in(surv_rect(), x, y)) { play(true); return; }
    if (s_tab == 1 && in(list_rect(), x, y)) {
        int i = (y - list_rect().y - 4 + s_scroll) / ROW_H;
        if (i >= 0 && i < NVER) { s_sel = i; s_msg[0] = 0; }
    }
}
static void clamp_scroll(void) {
    int max = NVER * ROW_H + 8 - list_rect().h; if (max < 0) max = 0;
    if (s_scroll > max) s_scroll = max; if (s_scroll < 0) s_scroll = 0;
}
static void ensure_visible(void) {
    rect_t lr = list_rect(); int top = 4 + s_sel * ROW_H;
    if (top < s_scroll) s_scroll = top; else if (top + ROW_H > s_scroll + lr.h) s_scroll = top + ROW_H - lr.h;
    clamp_scroll();
}
static void on_key(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ENTER) play(false);
    else if (ev->special == COS_KEY_TAB) { s_tab = (s_tab + 1) % 3; s_msg[0] = 0; }
    else if (s_tab == 1 && ev->special == COS_KEY_UP && s_sel > 0) { s_sel--; ensure_visible(); }
    else if (s_tab == 1 && ev->special == COS_KEY_DOWN && s_sel < NVER - 1) { s_sel++; ensure_visible(); }
}
static void on_move(int x, int y) {
    s_hover = -1;
    for (int i = 0; i < 3; ++i) if (in(tab_rect(i), x, y)) s_hover = 100 + i;
    if (in(play_rect(), x, y)) s_hover = 201;
    else if (has_survival() && in(surv_rect(), x, y)) s_hover = 202;
    else if (s_tab == 1 && in(list_rect(), x, y)) { int i = (y - list_rect().y - 4 + s_scroll) / ROW_H; if (i >= 0 && i < NVER) s_hover = i; }
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Launcher", 860, 520, &wi);
    if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(17); f_h = cui_font_bold(30); f_t = cui_font_bold(15);
    (void)f_t;
    ensure_visible();
    for (;;) {
        draw();
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 300);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_KEY) on_key(&ev);
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) on_click(ev.x, ev.y);
            else if (ev.type == COS_EV_MOUSE_MOVE) on_move(ev.x, ev.y);
            else if (ev.type == COS_EV_WHEEL && s_tab == 1) { s_scroll -= ev.wheel * 30; clamp_scroll(); }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
