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
static bool s_installing;                 /* the Linux app pack installer is running (followed through /system/linuxpack/progress.txt) */

#define FX_PATH "/linux/usr/lib/firefox/firefox"
#define VNC_PATH "/linux/usr/bin/Xtigervnc"
#define INSTALLER_PATH "/apps/linux/LinuxInstall.c-os"
static bool cos_stat_ok(const char *p);
static bool fx_installed(void) { return cos_stat_ok(FX_PATH) && cos_stat_ok(VNC_PATH); }
/* what the installer reports in /system/linuxpack/progress.txt: "<running|done|error> <chunks done> <chunks> <bytes done> <bytes> <message>" */
typedef struct { char state[12]; int done, total; unsigned long long bytes_done, bytes; char msg[96]; bool ok; } inst_t;
static inst_t read_progress(void) {
    inst_t r; memset(&r, 0, sizeof r);
    char b[320]; ssize_t n = cos_read_file("/system/linuxpack/progress.txt", b, sizeof b - 1);
    if (n <= 0) return r; b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0;
    int used = 0;
    if (sscanf(b, "%11s %d %d %llu %llu %n", r.state, &r.done, &r.total, &r.bytes_done, &r.bytes, &used) >= 5) { snprintf(r.msg, sizeof r.msg, "%s", b + used); r.ok = true; }
    return r;
}

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
    if (s_installing) { snprintf(s_msg, sizeof s_msg, "The installation is running."); return; }
    if (running()) { snprintf(s_msg, sizeof s_msg, "A game is already running."); return; }
    if (s_tab == 2 && !fx_installed()) {                                       /* nothing to start yet: download and install it */
        const char *iv[2] = { INSTALLER_PATH, NULL };
        if (!cos_stat_ok(INSTALLER_PATH)) { snprintf(s_msg, sizeof s_msg, "The installer is missing."); return; }
        cos_mkdir("/system"); cos_mkdir("/system/linuxpack");
        static const char starting[] = "running 0 0 0 0 starting\n"; cos_write_file("/system/linuxpack/progress.txt", starting, sizeof starting - 1);     /* the old result must not be taken for this run's */
        /* cos_waitpid(WNOHANG) cannot tell "still running" from "no such child" (both -1), so the installer is followed through its progress file */
        int64_t ip = cos_spawn_argv(INSTALLER_PATH, iv, NULL);
        if (ip <= 0) { snprintf(s_msg, sizeof s_msg, "Could not start the installer."); } else { s_installing = true; s_msg[0] = 0; }
        return;
    }
    char path[160];
    if (!game_path(path, sizeof path, s_tab, s_sel, survival)) { snprintf(s_msg, sizeof s_msg, "Not installed: get Minecraft Classic in C-OS Apps."); return; }
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
        int x = 40, y = TAB_H + 36; bool fx = fx_installed();
        cui_text(&c, f_h, x, y, "Firefox", RGB(255, 255, 255)); y += 56;
        cui_text(&c, f_m, x, y, "Mozilla Firefox, a Linux program, in a C-OS window.", RGB(200, 205, 215)); y += 28;
        cui_text(&c, f_s, x, y, "The Linux layer runs an X server (TigerVNC) and Firefox; this window is its viewer.", RGB(150, 156, 170)); y += 22;
        cui_text(&c, f_s, x, y, "The first start takes minutes: the Linux programs are emulated and loaded from disk.", RGB(150, 156, 170)); y += 40;
        if (fx) {
            cui_text(&c, f_s, x, y, "Firefox is installed. Press Start.", RGB(120, 200, 130)); y += 22;
        } else {
            inst_t pr = read_progress();
            if (s_installing && pr.ok && (!strcmp(pr.state, "done") || !strcmp(pr.state, "error"))) s_installing = false;
            bool busy = s_installing;
            cui_text(&c, f_s, x, y, "Firefox is not installed yet. Install downloads it (about 330 MB) and needs about 1 GB on the disk.", RGB(240, 180, 90)); y += 22;
            cui_text(&c, f_s, x, y, "The download is checked against a signature before anything is installed.", RGB(150, 156, 170)); y += 30;
            if (busy || (pr.ok && !strcmp(pr.state, "error"))) {
                int bw = win_w - x - 40; if (bw > 520) bw = 520;
                unsigned pct = pr.ok && pr.bytes ? (unsigned)(pr.bytes_done * 100 / pr.bytes) : 0; if (pct > 100) pct = 100;
                cui_round_rect(&c, x, y, bw, 18, 8, RGB(38, 42, 50));
                if (pct) cui_round_rect(&c, x, y, bw * (int)pct / 100 < 16 ? 16 : bw * (int)pct / 100, 18, 8, busy ? RGB(62, 150, 70) : RGB(190, 110, 40));
                y += 26; char t[160];
                if (busy) snprintf(t, sizeof t, "%u%%   part %d of %d   %s", pct, pr.ok ? pr.done : 0, pr.ok ? pr.total : 0, pr.ok ? pr.msg : "starting");
                else snprintf(t, sizeof t, "Stopped: %s. Press Install to continue where it stopped.", pr.msg);
                cui_text_fit(&c, f_s, x, y, win_w - x - 24, t, busy ? RGB(200, 205, 215) : RGB(240, 180, 90));
            }
        }
        cui_text(&c, f_s, x, win_h - 120, "Firefox and the Linux programs are not part of C-OS: they are downloaded from C-OS-APPS on GitHub.", RGB(110, 116, 130));
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
        cui_text_fit(&c, f_s, x, y, win_w - x - 24, "mc-classic-c by degradka (GPL-3.0): Minecraft Classic rewritten in C.", RGB(110, 116, 130)); y += 30;
        { char q[160]; if (!game_path(q, sizeof q, 1, s_sel, false)) cui_text_fit(&c, f_s, x, y, win_w - x - 24, "Not installed yet: open C-OS Apps and download Minecraft Classic.", RGB(240, 180, 90)); }
    }
    char p[160]; bool have = game_path(p, sizeof p, s_tab, s_sel, false);
    bool need_install = s_tab == 2 && !fx_installed();
    if (need_install) have = cos_stat_ok(INSTALLER_PATH);
    if (s_msg[0]) cui_text(&c, f_s, win_w - 232, win_h - 164 - (has_survival() ? 64 : 0), s_msg, RGB(240, 180, 90));
    if (has_survival()) {
        rect_t sr = surv_rect(); char q[160]; bool hs = game_path(q, sizeof q, 1, s_sel, true);
        button(sr, run ? "Running..." : "Play Survival", hs && !run, s_hover == 202, RGB(190, 110, 40));
    }
    rect_t pr = play_rect();
    button(pr, s_installing ? "Installing..." : run ? "Running..." : (has_survival() ? "Play Creative" : s_tab == 2 ? (need_install ? "Install" : "Start") : "Play"), have && !run && !s_installing, s_hover == 201, RGB(62, 150, 70));
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
