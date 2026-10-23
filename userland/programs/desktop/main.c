/* desktop.c-os - the desktop icons, as a ring-3 program.
 *
 * Shell layer 3: a screen-sized, colour-keyed surface (COS_DESKTOP_KEY pixels are transparent) that the compositor draws over
 * the wallpaper and under every window. This program owns what the user sees and does here - selection, hover, labels, the
 * double-click, dragging icons, the rubber-band (blue rectangle) selection - and the kernel keeps the icon list, the icon artwork (drawn through cos_shell_icons with
 * negative kinds = desktop icon number) and the actions that touch files and windows (cos_desktop_ctl). Text is drawn on a
 * solid chip, because anti-aliasing against a key colour would leave fringes. If this process is gone the kernel draws and
 * handles the desktop itself.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define MAXI 64
static cui_canvas c;
static cos_dicon_t icons[MAXI], prev[MAXI];
static int nicons, nprev;
static bool selv[MAXI];                           /* the selected icons (Ctrl+click toggles, a rubber band selects many) */
static int hover = -1;
static int cand = -1, ax, ay, offx, offy;         /* drag candidate: the icon the pointer went down on */
static bool dragging;
static int gdx, gdy;                              /* how far the dragged icons have moved from where they were */
static bool banding; static int bx0, by0, bx1, by1; static bool band_base[MAXI];   /* rubber band: corners, selection at the start (kept with Ctrl) */
static int hold_n; static int hold_idx[MAXI], hold_x[MAXI], hold_y[MAXI];           /* keep the dropped positions until the kernel list catches up */
static uint64_t hold_until;
static uint64_t last_click_ms;
static int last_click = -1;

/* A chamfered rectangle drawn with plain fills: no anti-aliased edge to fringe against the key colour. */
static void chip(int x, int y, int w, int h, int r, uint32_t col)
{
    cui_fill(&c, x + r, y, w - 2 * r, h, col);
    cui_fill(&c, x, y + r, w, h - 2 * r, col);
    cui_fill(&c, x + 1, y + 1, w - 2, h - 2, col);
}

static int hit(int x, int y)
{
    for (int i = nicons - 1; i >= 0; i--)
        if (x >= icons[i].x && x < icons[i].x + icons[i].box && y >= icons[i].y && y < icons[i].y + icons[i].box + 22) return i;
    return -1;
}

static int sel_count(void) { int n = 0; for (int i = 0; i < nicons; i++) if (selv[i]) n++; return n; }
static void sel_clear(void) { memset(selv, 0, sizeof selv); }

static void pos_of(int i, int *x, int *y)
{
    *x = icons[i].x; *y = icons[i].y;
    if (dragging && selv[i]) { *x += gdx; *y += gdy; return; }
    for (int k = 0; k < hold_n && cos_time_ms() < hold_until; k++) if (hold_idx[k] == i) { *x = hold_x[k]; *y = hold_y[k]; return; }
}

/* The translucent blue of the band: pixels with an alpha byte of 1..254 are blended with the wallpaper by the compositor (a key-colour
 * surface cannot blend by itself); the one-pixel border is opaque. */
static void band_draw(void)
{
    int x0 = bx0 < bx1 ? bx0 : bx1, x1 = bx0 < bx1 ? bx1 : bx0, y0 = by0 < by1 ? by0 : by1, y1 = by0 < by1 ? by1 : by0;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= c.w) x1 = c.w - 1; if (y1 >= c.h) y1 = c.h - 1;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y <= y1; y++) {
        uint32_t *row = c.px + (size_t)y * c.stride;
        for (int x = x0; x <= x1; x++) {
            bool edge = x == x0 || x == x1 || y == y0 || y == y1;
            row[x] = edge ? 0x006FA8F0u : 0x5A4A90E2u;
        }
    }
}

static void render(cui_font *f, int64_t win)
{
    cui_fill(&c, 0, 0, c.w, c.h, COS_DESKTOP_KEY);
    cos_shell_icon_t ov[COS_SHELL_MAX_ICONS];
    int nov = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nicons; i++) {
            bool top = (dragging && selv[i]);
            if (top != (pass == 1)) continue;
            int x, y;
            pos_of(i, &x, &y);
            int box = icons[i].box;
            if (selv[i] || i == hover) {
                chip(x - 6, y - 5, box + 12, box + 30, 5, selv[i] ? 0x34527F : 0x26385A);
            }
            char lab[40];
            snprintf(lab, sizeof lab, "%s", icons[i].label);
            int maxw = icons[i].cell_w - 6;
            int tw = cui_text_width(f, lab);
            if (tw > maxw) tw = maxw;
            int lx = x + box / 2 - (tw + 10) / 2, ly = y + box + 3;
            chip(lx, ly, tw + 10, 18, 3, selv[i] ? 0x2A5FB5 : 0x151C30);
            cui_text_fit(&c, f, lx + 5, ly + 2, tw, lab, 0xEAF0FA);
            if (nov < COS_SHELL_MAX_ICONS) ov[nov++] = (cos_shell_icon_t){ -(i + 1), x, y, box };
        }
    if (banding) band_draw();
    cos_shell_icons(3, ov, nov, 0);
    cos_win2_present(win);
}

/* the icons the band touches (the whole cell: picture and label), added to what was selected when it started */
static void band_select(void)
{
    int x0 = bx0 < bx1 ? bx0 : bx1, x1 = bx0 < bx1 ? bx1 : bx0, y0 = by0 < by1 ? by0 : by1, y1 = by0 < by1 ? by1 : by0;
    for (int i = 0; i < nicons; i++) {
        bool in = icons[i].x < x1 && icons[i].x + icons[i].box > x0 && icons[i].y < y1 && icons[i].y + icons[i].box + 22 > y0;
        selv[i] = band_base[i] || in;
    }
}

int main(void)
{
    cos_win_info_t wi;
    int64_t win = cos_shell_create(3, &wi);
    if (win <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *f = cui_font_default(12);

    bool first = true;
    for (;;) {
        bool dirty = first;
        first = false;
        cos_win_event_t ev;
        int r = cos_win2_wait(win, &ev, 150);
        while (r > 0) {
            bool ctrl = (ev.mods & COS_MOD_CTRL) != 0;
            if (ev.type == COS_EV_MOUSE_MOVE) {
                if (cand >= 0 && (ev.buttons & 1)) {
                    int ddx = ev.x - ax, ddy = ev.y - ay;
                    if (!dragging && (ddx > 4 || ddx < -4 || ddy > 4 || ddy < -4)) { dragging = true; last_click = -1; }
                    if (dragging) { gdx = ddx; gdy = ddy; dirty = true; }
                } else if (banding && (ev.buttons & 1)) {
                    bx1 = ev.x; by1 = ev.y; band_select(); dirty = true;
                } else {
                    int h = ev.x < 0 ? -1 : hit(ev.x, ev.y);
                    if (h != hover) { hover = h; dirty = true; }
                }
            } else if (ev.type == COS_EV_MOUSE_DOWN) {
                int h = hit(ev.x, ev.y);
                if (ev.button == 1) {
                    uint64_t now = cos_time_ms();
                    if (h >= 0) {
                        if (h == last_click && now - last_click_ms < 500 && !ctrl) { cos_desktop_ctl(COS_DESKCTL_OPEN, h, 0, 0); last_click = -1; cand = -1; selv[h] = true; }
                        else {
                            last_click = h; last_click_ms = now;
                            if (ctrl) { selv[h] = !selv[h]; cand = selv[h] ? h : -1; }
                            else { if (!selv[h]) { sel_clear(); selv[h] = true; } cand = h; }
                            ax = ev.x; ay = ev.y; offx = ev.x - icons[h].x; offy = ev.y - icons[h].y;
                        }
                    } else {
                        last_click = -1; cos_desktop_ctl(COS_DESKCTL_FOCUS, 0, 0, 0);
                        memcpy(band_base, selv, sizeof band_base);
                        if (!ctrl) { sel_clear(); memset(band_base, 0, sizeof band_base); }
                        banding = true; bx0 = bx1 = ev.x; by0 = by1 = ev.y; cand = -1;
                    }
                } else if (ev.button == 2) {
                    if (h >= 0 && !selv[h]) { sel_clear(); selv[h] = true; }
                    else if (h < 0) sel_clear();
                    cos_desktop_ctl(COS_DESKCTL_CONTEXT, h, ev.x, ev.y);
                }
                dirty = true;
            } else if (ev.type == COS_EV_MOUSE_UP && ev.button == 1) {
                if (dragging && cand >= 0) {
                    int moved = 0, n = sel_count();
                    hold_n = 0;
                    for (int i = 0; i < nicons && hold_n < MAXI; i++) {
                        if (!selv[i]) continue;
                        int nx = icons[i].x + gdx, ny = icons[i].y + gdy;
                        cos_desktop_ctl(COS_DESKCTL_MOVE, i, nx, ny);
                        hold_idx[hold_n] = i; hold_x[hold_n] = nx; hold_y[hold_n] = ny; hold_n++;
                        moved++;
                    }
                    if (n == 1) cos_desktop_ctl(COS_DESKCTL_DROP, cand, ev.x, ev.y);          /* one icon: it may land in a folder or on another app icon */
                    else for (int i = 0; i < nicons; i++) if (selv[i]) cos_desktop_ctl(COS_DESKCTL_DROP, i, -1, -1);   /* a group only snaps to the grid */
                    hold_until = cos_time_ms() + 300;
                    (void)moved;
                }
                dragging = false; banding = false; cand = -1; gdx = gdy = 0; dirty = true;
            }
            r = cos_win2_poll(win, &ev);
        }
        int n = cos_desktop_list(icons, MAXI);
        if (n < 0) n = 0;
        if (n != nprev || memcmp(icons, prev, (size_t)n * sizeof(cos_dicon_t)) != 0) {
            if (n != nprev) { sel_clear(); cand = -1; dragging = false; banding = false; hold_n = 0; }
            nicons = n; nprev = n;
            memcpy(prev, icons, (size_t)n * sizeof(cos_dicon_t));
            dirty = true;
        } else if (hold_n && cos_time_ms() >= hold_until) { hold_n = 0; dirty = true; }
        if (dirty) render(f, win);
    }
}
