int atoi(const char *str);
long atol(const char *str);
/* taskbar.c-os - the desktop taskbar, as a ring-3 program.
 *
 * It owns the strip at the bottom of the screen (cos_shell_create(1, ...)): the compositor draws this surface where the
 * kernel taskbar used to be, and forwards the mouse over it as ordinary window events. It lists the open windows with
 * cos_win_list() and acts on them with cos_win_ctl(): click = focus / minimise toggle, middle click = close. Start opens the
 * kernel's start menu (which moves to ring 3 in a later stage). If this process dies the kernel taskbar takes over again.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define MAXW COS_SHELL_MAX_WINDOWS

enum { T_NONE = 0, T_START, T_WIN, T_BROWSER, T_CALC, T_UPDATE };
typedef struct { int x, w, kind, idx; } hit_t;

static cui_canvas c;
static cos_winfo_t wins[MAXW];
static int nwin;
static hit_t hits[MAXW + 8];
static int nhit;
static int mx = -1, my = -1;

static const uint32_t BG = 0x232F4B, BG_TOP = 0x32456A, BTN = 0x263450, BTN_HOV = 0x41609B, ACT = 0x4169B0, ACC = 0x6FB4FF;
static const uint32_t TXT = 0xE8ECF5, MUTED = 0x9AA6BD;

static int hit_at(int x, int y)
{
    if (y < 0) return -1;
    for (int i = 0; i < nhit; i++) if (x >= hits[i].x && x < hits[i].x + hits[i].w) return i;
    return -1;
}

static void add_hit(int x, int w, int kind, int idx) { hits[nhit].x = x; hits[nhit].w = w; hits[nhit].kind = kind; hits[nhit].idx = idx; nhit++; }


/* ---- OS updates (docs/app_update.md) ------------------------------------------------------------------------------------------------------------------
 * `cosupdate poll` runs in the background (every auto.txt minutes) and writes /system/update/available.txt when there is something to install and shows a toast.
 * This program shows the C-OS icon in the tray while that file exists, with a small callout for a while; a click on the icon (or the callout) starts the
 * installation (`cosupdate install`, which writes progress.txt - shown here as a ring and a percentage) - no further question is asked. */
enum { UP_NONE = 0, UP_AVAILABLE, UP_RUNNING, UP_DONE, UP_FAILED };
static int up_state = UP_NONE;              /* what the icon shows */
static int up_pct;                          /* 0..100 while running */
static char up_os[48], up_msg[120];
static uint64_t up_callout_until;           /* the callout is shown until then */
static uint64_t up_final_until;             /* a finished / failed icon stays this long */
static int64_t up_pid = -1;
static uint64_t up_next_poll;
static int up_first = 1;

static bool read_small(const char *path, char *buf, size_t cap)
{
    cos_stat_t st; if (cos_stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size >= cap) return false;
    ssize_t n = cos_read_file(path, buf, cap - 1); if (n <= 0) return false;
    buf[n] = 0; return true;
}
static const char *field(const char *text, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key); out[0] = 0;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == ' ') { size_t n = len - kl - 1; if (n >= cap) n = cap - 1; memcpy(out, p + kl + 1, n); out[n] = 0; return out; }
        if (!nl) break; p = nl + 1;
    }
    return NULL;
}
static void up_spawn(const char *a1, const char *a2)
{
    if (up_pid > 0) { int st; int64_t r = cos_waitpid(up_pid, &st, false); if (r == up_pid || r < 0) up_pid = -1; else return; }       /* one at a time */
    const char *argv[4] = { "cosupdate.c-os", a1, a2, NULL };
    up_pid = cos_spawn_argv("/bin/cosupdate.c-os", argv, NULL);
}
/* called about once a second */
static void up_poll(uint64_t now)
{
    char t[600], v[160];
    if (up_state == UP_RUNNING || up_state == UP_DONE || up_state == UP_FAILED) {
        if (read_small("/system/update/progress.txt", t, sizeof t)) {
            field(t, "state", v, sizeof v);
            if (!strcmp(v, "running")) { up_state = UP_RUNNING; field(t, "percent", v, sizeof v); up_pct = atoi(v); }
            else if (!strcmp(v, "done") && up_state != UP_DONE) { up_state = UP_DONE; up_pct = 100; up_final_until = now + 20000; up_callout_until = now + 12000; cos_puts("[TASKBAR] update finished\n"); }
            else if (!strcmp(v, "failed") && up_state != UP_FAILED) { up_state = UP_FAILED; field(t, "message", up_msg, sizeof up_msg); up_final_until = now + 30000; up_callout_until = now + 12000; cos_puts("[TASKBAR] update failed\n"); }
        }
        if ((up_state == UP_DONE || up_state == UP_FAILED) && now > up_final_until) up_state = UP_NONE;
        if (up_state != UP_NONE) return;
    }
    if (read_small("/system/update/available.txt", t, sizeof t)) {
        if (up_state == UP_NONE) {
            up_state = UP_AVAILABLE; up_callout_until = now + 20000;
            field(t, "os", up_os, sizeof up_os);
            char ser[24]; field(t, "serial", ser, sizeof ser);
            cos_puts("[TASKBAR] update available os="); cos_puts(up_os[0] ? up_os : "-"); cos_puts(" serial="); cos_puts(ser); cos_puts("\n");
        }
    } else if (up_state == UP_AVAILABLE) up_state = UP_NONE;
    /* the background check: every auto.txt minutes (default 30); a changed setting is picked up within a second */
    if (up_state == UP_NONE || up_state == UP_AVAILABLE) {
        long minutes = 30; bool on = true;
        if (read_small("/system/update/auto.txt", t, sizeof t)) { if (!strncmp(t, "off", 3)) on = false; const char *sp = strchr(t, ' '); if (sp && atol(sp + 1) > 0) minutes = atol(sp + 1); }
        bool have_url = read_small("/system/update/url.txt", t, sizeof t);
        if (up_next_poll == 0 || up_next_poll > now + (uint64_t)minutes * 60000ull) up_next_poll = now + (up_next_poll == 0 ? 30000ull : (uint64_t)minutes * 60000ull);
        if (now >= up_next_poll) {
            if (on && have_url) { up_spawn("poll", NULL); cos_puts("[TASKBAR] update check started\n"); }
            up_next_poll = now + (have_url ? (uint64_t)minutes * 60000ull : 20000ull);        /* no source yet: look again soon */
        }
    }
}
static void up_click(void)
{
    if (up_state == UP_AVAILABLE) {
        up_state = UP_RUNNING; up_pct = 0; up_callout_until = 0;
        cos_unlink("/system/update/progress.txt");
        up_spawn("install", NULL);
        cos_puts("[TASKBAR] update started\n");
    } else if (up_state == UP_FAILED || up_state == UP_DONE) { up_state = UP_NONE; up_callout_until = 0; }
}

/* the balloon above the tray (layer 6): plain fills only, nothing anti-aliased meets the key colour */
static void bchip(cui_canvas *cb, int x, int y, int w, int h, int r, uint32_t col)
{
    cui_fill(cb, x + r, y, w - 2 * r, h, col); cui_fill(cb, x, y + r, w, h - 2 * r, col); cui_fill(cb, x + 1, y + 1, w - 2, h - 2, col);
}
static bool balloon_wanted(uint64_t now) { return up_state != UP_NONE && now < up_callout_until; }
static void draw_balloon(cui_canvas *cb, uint64_t now)
{
    cui_fill(cb, 0, 0, cb->w, cb->h, COS_DESKTOP_KEY);
    if (!balloon_wanted(now)) return;
    int w = cb->w, bh = cb->h - 10;
    uint32_t edge = up_state == UP_FAILED ? 0xDC5046 : up_state == UP_DONE ? 0x50DC64 : 0x5C9BFF, fill = up_state == UP_FAILED ? 0x4A1F1F : up_state == UP_DONE ? 0x143D26 : 0x16284A;
    bchip(cb, 0, 0, w, bh, 7, edge); bchip(cb, 1, 1, w - 2, bh - 2, 6, fill);
    int ax = w - (int)(c.w - (c.w - 366 + 18)) - 0;        /* the arrow points at the middle of the icon */
    ax = 32;
    for (int i = 0; i < 9; ++i) { cui_fill(cb, ax - 9 + i, bh - 1 + i, 18 - 2 * i, 1, i == 0 ? edge : fill); }
    cui_font *fb = cui_font_bold(14), *f = cui_font_default(12);
    const char *l1, *l2; char l2b[160];
    if (up_state == UP_AVAILABLE) {
        l1 = "C-OSのアップデートが配信されました";
        snprintf(l2b, sizeof l2b, "%s%sアイコンをクリックすると更新を始めます", up_os[0] ? up_os : "", up_os[0] ? "  -  " : "");
        l2 = l2b;
    } else if (up_state == UP_RUNNING) {
        l1 = "C-OSを更新しています";
        snprintf(l2b, sizeof l2b, "%d%%  -  この間も、そのまま使えます", up_pct);
        l2 = l2b;
    } else if (up_state == UP_DONE) { l1 = "C-OSのアップデートが完了しました"; l2 = "再起動すると完全に反映されます"; }
    else { l1 = "アップデートに失敗しました"; l2 = up_msg[0] ? up_msg : "もう一度お試しください"; }
    cui_text_fit(cb, fb, 14, 12, w - 28, l1, 0xFFFFFF);
    cui_text_fit(cb, f, 14, 38, w - 28, l2, 0xC8D6F0);
    if (up_state == UP_RUNNING) { bchip(cb, 14, bh - 18, w - 28, 6, 3, 0x0C1830); int fw = (w - 28) * up_pct / 100; if (fw > 6) bchip(cb, 14, bh - 18, fw, 6, 3, 0x6FE3A0); }
}

/* the C-OS logo: a rounded tile with a "C" ring; a badge shows the state */
static void draw_update(int x, int y, int sz, bool hov, uint64_t now)
{
    uint32_t tile = up_state == UP_FAILED ? 0x8C3030 : up_state == UP_DONE ? 0x1F7A46 : 0x2F5C9E;
    if (hov) tile = cui_mix(tile, 0xFFFFFF, 40);
    cui_round_rect(&c, x, y, sz, sz, 9, tile);
    float cx = (float)x + sz / 2.0f, cy = (float)y + sz / 2.0f;
    cui_ring(&c, cx, cy, sz * 0.30f, 3.0f, 0xFFFFFF);                      /* the "C" */
    cui_fill(&c, x + sz - 13, y + sz / 2 - 3, 12, 6, tile);                   /* open its right side */
    if (up_state == UP_AVAILABLE) {                                           /* arrow down badge, pulsing */
        uint32_t b = ((now / 500) & 1) ? 0xFFD54A : 0xFFB300;
        cui_circle(&c, (float)(x + sz - 6), (float)(y + 7), 7.0f, b);
        cui_triangle(&c, (float)(x + sz - 10), (float)(y + 6), (float)(x + sz - 2), (float)(y + 6), (float)(x + sz - 6), (float)(y + 12), 0x3A2A00);
    } else if (up_state == UP_RUNNING) {
        cui_ring(&c, cx, cy, sz * 0.46f, 2.5f, 0x16203A);
        int seg = up_pct * 24 / 100;                                          /* a ring that fills */
        static const float ring_pts[24][2] = { {0.0000f, -1.0000f}, {0.2588f, -0.9659f}, {0.5000f, -0.8660f}, {0.7071f, -0.7071f}, {0.8660f, -0.5000f}, {0.9659f, -0.2588f}, {1.0000f, 0.0000f}, {0.9659f, 0.2588f}, {0.8660f, 0.5000f}, {0.7071f, 0.7071f}, {0.5000f, 0.8660f}, {0.2588f, 0.9659f}, {0.0000f, 1.0000f}, {-0.2588f, 0.9659f}, {-0.5000f, 0.8660f}, {-0.7071f, 0.7071f}, {-0.8660f, 0.5000f}, {-0.9659f, 0.2588f}, {-1.0000f, 0.0000f}, {-0.9659f, -0.2588f}, {-0.8660f, -0.5000f}, {-0.7071f, -0.7071f}, {-0.5000f, -0.8660f}, {-0.2588f, -0.9659f} };     /* cos / sin of -90 deg + 15 deg * i (libcos has no sincosf) */
        for (int i = 0; i < seg && i < 24; ++i) cui_circle(&c, cx + (sz * 0.46f) * ring_pts[i][0], cy + (sz * 0.46f) * ring_pts[i][1], 1.6f, 0x6FE3A0);
    } else if (up_state == UP_DONE) cui_circle(&c, (float)(x + sz - 6), (float)(y + 7), 6.0f, 0x6FE3A0);
    else if (up_state == UP_FAILED) cui_circle(&c, (float)(x + sz - 6), (float)(y + 7), 6.0f, 0xFF6B6B);
}

static void layout(int width)
{
    nhit = 0;
    add_hit(8, 86, T_START, 0);
    int right = width - 330 - (up_state != UP_NONE ? 40 : 0);   /* the tray starts here; quick-launch sits just left of it */
    if (up_state != UP_NONE) add_hit(right + 4, 36, T_UPDATE, 0);      /* the C-OS icon, next to the tray */
    add_hit(right - 76, 34, T_BROWSER, 0);
    add_hit(right - 38, 34, T_CALC, 0);
    int x0 = 110;                                    /* icon-only buttons, 46px slots; the compositor draws the icons (gui_shell_draw_icons) */
    for (int i = 0; i < nwin && x0 + 46 * (i + 1) <= right - 84; i++) add_hit(x0 + i * 46, 46, T_WIN, i);
}

static void draw(int hover, int mem_pct, int usb, const cos_datetime_t *t)
{
    int W = c.w, H = c.h;
    cui_fill(&c, 0, 0, W, H, BG);
    cui_fill(&c, 0, 0, W, 1, BG_TOP);
    cui_font *f = cui_font_default(14), *fb = cui_font_bold(14);

    for (int i = 0; i < nhit; i++) {
        hit_t *h = &hits[i];
        bool hov = (i == hover);
        int by = 5, bh = H - 10;
        switch (h->kind) {
        case T_START:
            cui_round_rect(&c, h->x, by, h->w, bh, 10, hov ? BTN_HOV : BTN);
            cui_round_rect_outline(&c, h->x, by, h->w, bh, 10, 1.0f, 0x5677B3);
            cui_text_center(&c, fb, h->x, by, h->w, bh, "Start", TXT);
            break;
        case T_BROWSER: case T_CALC:
            cui_round_rect(&c, h->x, by, h->w, bh, 8, hov ? BTN_HOV : BTN);
            cui_text_center(&c, f, h->x, by, h->w, bh, h->kind == T_BROWSER ? "Web" : "Calc", MUTED);
            break;
        case T_UPDATE:
            draw_update(h->x, by + 1, bh - 2, hov, cos_time_ms());
            break;
        case T_WIN: {
            cos_winfo_t *w = &wins[h->idx];
            if (w->focused || hov) cui_round_rect(&c, h->x + 2, 4, h->w - 4, H - 10, 8, w->focused ? ACT : 0x32425F);
            if (w->focused) cui_fill(&c, h->x + h->w / 2 - 8, H - 4, 16, 3, ACC);
            else if (!w->minimized) cui_fill(&c, h->x + h->w / 2 - 5, H - 4, 10, 2, 0x8CA0C8);
            break;
        }
        }
    }

    /* tray */
    int tx = W - 320;
    char b[48];
    snprintf(b, sizeof b, "MEM %d%%", mem_pct);
    cui_text(&c, f, tx, 8, b, MUTED);
    cui_round_rect(&c, tx, H - 14, 62, 5, 2, 0x16203A);
    int fw = 62 * mem_pct / 100;
    if (fw > 0) cui_round_rect(&c, tx, H - 14, fw, 5, 2, mem_pct > 85 ? 0xE05A4B : 0x5CC98A);
    snprintf(b, sizeof b, "USB %d", usb);
    cui_text(&c, f, tx + 90, 8, b, usb ? 0x6FB4FF : MUTED);
    snprintf(b, sizeof b, "%02d:%02d", (int)t->hour, (int)t->minute);
    cui_text(&c, fb, W - 170, 6, b, TXT);
    snprintf(b, sizeof b, "%04d-%02d-%02d", (int)t->year, (int)t->month, (int)t->day);
    cui_text(&c, f, W - 170, 24, b, MUTED);
    cui_text(&c, f, W - 78, 14, "C-OS", MUTED);
}

int main(void)
{
    cos_win_info_t wi;
    int64_t h = cos_shell_create(1, &wi);
    if (h <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cos_win_info_t wbi; cui_canvas cbal; int64_t hb = cos_shell_create(6, &wbi);        /* the balloon (layer 6); without it the toast still shows */
    if (hb > 0) { cui_canvas_init(&cbal, wbi.pixels, wbi.width, wbi.height, wbi.stride); cui_fill(&cbal, 0, 0, cbal.w, cbal.h, COS_DESKTOP_KEY); cos_win2_present(hb); }
    uint32_t last_bkey = 0xFFFFFFFFu;

    int down_hit = -1, down_btn = 0;
    uint32_t last_key = 0; uint32_t last_pulse = 0;
    for (;;) {
        bool dirty = false;
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 120);
        while (r > 0) {
            if (ev.type == COS_EV_MOUSE_MOVE) { if (ev.x != mx || ev.y != my) { mx = ev.x; my = ev.y; dirty = true; } }
            else if (ev.type == COS_EV_MOUSE_DOWN) { mx = ev.x; my = ev.y; down_hit = hit_at(mx, my); down_btn = ev.button; dirty = true; }
            else if (ev.type == COS_EV_MOUSE_UP) {
                mx = ev.x; my = ev.y;
                int up = hit_at(mx, my);
                if (up >= 0 && up == down_hit) {
                    hit_t *ht = &hits[up];
                    if (ht->kind == T_START) { if (down_btn == 1) cos_win_ctl(COS_WINCTL_START_MENU, 0, 0); }
                    else if (ht->kind == T_UPDATE) { if (down_btn == 1) up_click(); }
                    else if (ht->kind == T_BROWSER) cos_win_ctl(COS_WINCTL_LAUNCH, 0, COS_LAUNCH_BROWSER);
                    else if (ht->kind == T_CALC) cos_win_ctl(COS_WINCTL_LAUNCH, 0, COS_LAUNCH_CALC);
                    else if (ht->kind == T_WIN && ht->idx < nwin) {
                        if (down_btn == 1) cos_win_ctl(COS_WINCTL_TOGGLE, wins[ht->idx].uid, 0);
                        else if (down_btn == 4) cos_win_ctl(COS_WINCTL_CLOSE, wins[ht->idx].uid, 0);
                    }
                }
                down_hit = -1; dirty = true;
            }
            r = cos_win2_poll(h, &ev);
        }

        cos_winfo_t nw[MAXW];
        int n = cos_win_list(nw, MAXW);
        if (n < 0) n = 0;
        cos_datetime_t t; cos_rtc_now(&t);
        static int mem, usb;
        static uint64_t last_slow;
        uint64_t now = cos_time_ms();
        if (now - last_slow >= 1000) {
            last_slow = now;
            cos_sysinfo_t si;
            if (cos_sysinfo(&si) == 0 && si.pages_total) mem = (int)(100 - (si.pages_free * 100 / si.pages_total));
            cos_device_info_t dv[16]; usb = 0;
            int nd = cos_devices_list(dv, 16);
            for (int i = 0; i < nd; i++) if (dv[i].kind == COS_DEV_USB_DEVICE && dv[i].present) usb++;
        }

        {   /* updates: look at the files about once a second; the icon and the balloon follow */
            static uint64_t last_up;
            if (now - last_up >= 1000 || (up_state == UP_RUNNING && now - last_up >= 400)) { last_up = now; int os_before = up_state, pct_before = up_pct; up_poll(now); if (os_before != up_state || pct_before != up_pct) dirty = true; }
            bool bw = balloon_wanted(now);
            uint32_t bkey = (uint32_t)(up_state * 1000 + up_pct * 3 + (bw ? 1 : 0));
            if (bkey != last_bkey) { last_bkey = bkey; dirty = true; if (hb > 0) { draw_balloon(&cbal, now); cos_win2_present(hb); } }
        }
        uint32_t key = (uint32_t)(t.minute * 7 + t.hour * 131 + mem * 17 + usb * 3 + n * 1009 + up_state * 53 + up_pct);
        if (up_state == UP_AVAILABLE && ((now / 500) & 1) != last_pulse) { last_pulse = (now / 500) & 1; dirty = true; }
        if (n != nwin || memcmp(nw, wins, (size_t)n * sizeof(cos_winfo_t)) != 0) { memcpy(wins, nw, (size_t)n * sizeof(cos_winfo_t)); nwin = n; dirty = true; }
        if (key != last_key) { last_key = key; dirty = true; }
        if (!dirty) continue;

        layout(c.w);
        draw(hit_at(mx, my), mem, usb, &t);
        {
            cos_shell_icon_t ic[COS_SHELL_MAX_ICONS];
            int ni = 0;
            for (int i = 0; i < nhit && ni < COS_SHELL_MAX_ICONS; i++)
                if (hits[i].kind == T_WIN) { ic[ni].kind = wins[hits[i].idx].kind; ic[ni].x = hits[i].x + 7; ic[ni].y = (c.h - 32) / 2 - 2; ic[ni].size = 32; ni++; }
            cos_shell_icons(1, ic, ni, 0);
        }
        cos_win2_present(h);
    }
}
