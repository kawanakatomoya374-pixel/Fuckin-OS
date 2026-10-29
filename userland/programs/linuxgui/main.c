/* linuxgui.c-os - shows a Linux GUI program in a C-OS window.
 *
 * The Linux compatibility layer (docs/LINUX_COMPAT.md) runs an X server (TigerVNC's Xtigervnc, from the root file system under /linux) and the program
 * on it; this program starts them (/linux/etc/cos-gui.sh), connects to the server's VNC port over the loopback and is its viewer: the framebuffer
 * becomes the window, keys and the mouse go back as RFB key and pointer events.
 *
 * usage: linuxgui [script]     (default /linux/etc/cos-gui.sh)
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"
#include "cosime_ui.h"
#include "cos_xdrop_blob.h"
#include "cos_xwm_blob.h"
#include "cos_xinput_blob.h"

#define VNC_PORT 5901
#define LOG(...) do { char _b[160]; snprintf(_b, sizeof _b, "[LGUI] " __VA_ARGS__); cos_puts(_b); cos_puts("\n"); } while (0)

static int s_sd = -1;
static int64_t s_win;
static cos_win_info_t s_info;
static int s_w, s_h;                   /* the X screen */
static int s_cw, s_ch;                 /* what the window can show of it (the surface may be smaller) */
static uint8_t s_buttons;
static bool s_quit;
static int s_norm_w = 1000, s_norm_h = 640;        /* the X screen size for the window as it is (follows the window when it is maximized or restored) */
static bool s_fs;                      /* full screen on (F11): the X screen has the size of the C-OS screen, so nothing is letterboxed */
/* shared-memory display path (the file /system/linuxgui_shm exists): a plain Xvfb draws into /dev/shm/Xvfb_screen0, which this program maps (cos_lxshm_map) and copies the changed pixels from;
 * keyboard and mouse go to cos-xinput (XTEST) as the same RFB messages; the visible part of the big X screen is set through /tmp/cos-view (cos-wm --view). No RFB, no TCP for pixels. */
static bool s_shm; static const uint32_t *s_fb; static uint32_t *s_shadow; static int s_fb_w, s_fb_h, s_fb_stride;
static bool s_ext; static uint32_t s_scr_id, s_scr_flags;      /* ExtendedDesktopSize: the id and flags of the screen the server reports, needed to ask it for another size */

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int get16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static bool snd(const void *b, size_t n) { bool ok = cos_send_all(s_sd, b, n, 5000) == (ssize_t)n; if (!ok) { static int bad; if (bad++ < 5) LOG("send of %d bytes failed", (int)n); } return ok; }

/* ---- input -> RFB ----------------------------------------------------------------------------------------------------------------------------- */
static void key_msg(uint32_t keysym, bool down) { uint8_t m[8] = { 4, down ? 1 : 0, 0, 0 }; put32(m + 4, keysym); snd(m, 8); }
static void pointer_msg(int x, int y, uint8_t mask) { uint8_t m[6] = { 5, mask }; put16(m + 2, x < 0 ? 0 : x); put16(m + 4, y < 0 ? 0 : y); snd(m, 6); }
static uint8_t rfb_buttons(uint8_t b) { return (uint8_t)((b & 1 ? 1 : 0) | (b & 4 ? 2 : 0) | (b & 2 ? 4 : 0)); }    /* left, middle, right */

static uint32_t keysym_of(const cos_win_event_t *e) {
    switch (e->special) {
    case COS_KEY_ENTER: return 0xFF0D;   case COS_KEY_BACKSPACE: return 0xFF08; case COS_KEY_ESC: return 0xFF1B;
    case COS_KEY_UP: return 0xFF52;      case COS_KEY_DOWN: return 0xFF54;      case COS_KEY_LEFT: return 0xFF51; case COS_KEY_RIGHT: return 0xFF53;
    case COS_KEY_TAB: return 0xFF09;     case COS_KEY_DELETE: return 0xFFFF;    case COS_KEY_HOME: return 0xFF50; case COS_KEY_END: return 0xFF57;
    case COS_KEY_PGUP: return 0xFF55;    case COS_KEY_PGDN: return 0xFF56;      case COS_KEY_INSERT: return 0xFF63;
    default: break;
    }
    if (e->special >= COS_KEY_F1 && e->special <= COS_KEY_F12) return 0xFFBE + (uint32_t)(e->special - COS_KEY_F1);
    if (e->ascii == '\n' || e->ascii == '\r') return 0xFF0D;
    if (e->ascii == '\b') return 0xFF08;
    if (e->ascii == '\t') return 0xFF09;
    if (e->ascii == 27) return 0xFF1B;
    if ((e->mods & COS_MOD_CTRL) && e->ascii >= 1 && e->ascii <= 26) return (uint32_t)('a' + e->ascii - 1);
    if (e->ascii >= 32 && e->ascii < 127) return (uint32_t)(uint8_t)e->ascii;
    return 0;
}

/* ---- Japanese input -----------------------------------------------------------------------------------------------------------------------------
 * Ctrl+Space turns the C-OS Japanese input (cosime) on and off. While it is on, the keys go to it first; the converted text is sent to the X server as
 * Unicode key symbols (0x01000000 + code point, which the X server types with a spare key code). The composition and the candidates are drawn on top of the
 * window near its lower left corner (the X server does not know where the caret of the program is); when they change or go away the pixels under them are asked
 * for again from the X server. */
static cui_canvas s_cv; static cui_font *s_font; static bool s_cv_ok;
static int s_ox, s_oy, s_ow, s_oh;                        /* what the overlay covered at the last refresh (s_ow = 0: nothing) */
static void shadow_fill(void) { if (s_shadow) for (size_t i = 0, n = (size_t)s_info.stride * (size_t)s_info.height; i < n; ++i) s_shadow[i] = 0xA5A5A5A5u; }
static void shm_invalidate(int x, int y, int w, int h) {       /* the pixels there are copied again at the next frame */
    if (!s_shadow) return;
    for (int r = y < 0 ? 0 : y; r < y + h && r < s_info.height; ++r) for (int c = x < 0 ? 0 : x; c < x + w && c < s_info.stride; ++c) s_shadow[(size_t)r * (size_t)s_info.stride + (size_t)c] = 0xA5A5A5A5u;
}
static void request_rect(int x, int y, int w, int h) { if (s_shm) { shm_invalidate(x, y, w, h); return; } uint8_t m[10] = { 3, 0 }; put16(m + 2, x); put16(m + 4, y); put16(m + 6, w); put16(m + 8, h); snd(m, 10); }

static void ime_refresh(bool restore) {
    if (!s_cv_ok) return;
    int ox = s_ox, oy = s_oy, ow = s_ow, oh = s_oh;           /* the old overlay: its pixels have to come back from the X server */
    s_ow = 0;
    int ix = s_cw - 52, iy = s_ch - 30;
    bool ind = imeui_on();
    imeui_draw(&s_cv, s_font, 12, s_ch - 44, 20);
    int px, py, pw, ph; bool pop = imeui_last_rect(&px, &py, &pw, &ph);
    if (ind) { imeui_indicator(&s_cv, s_font, ix, iy); }
    cos_win_rect_t r[3]; int nr = 0;
    if (pop) { r[nr].x = px; r[nr].y = py; r[nr].w = pw; r[nr].h = ph; nr++; s_ox = px; s_oy = py; s_ow = pw; s_oh = ph; }
    if (ind) { r[nr].x = ix; r[nr].y = iy; r[nr].w = 52; r[nr].h = 28; nr++; }
    if (restore && ow > 0 && !(pop && px == ox && py == oy && pw == ow && ph == oh)) request_rect(ox, oy, ow, oh);     /* only what the popup no longer covers is asked for again */
    static bool ind_was; if (restore && ind_was && !ind) request_rect(ix, iy, 52, 28); ind_was = ind;
    if (nr) cos_win2_present_rects(s_win, r, (uint32_t)nr); else cos_win2_present(s_win);
}
static void ime_redraw_cb(void) { ime_refresh(true); }

static void send_text(const char *u) {                      /* UTF-8 -> Unicode key symbols */
    for (const unsigned char *p = (const unsigned char *)u; *p; ) {
        uint32_t cp; int n;
        if (*p < 0x80) { cp = *p; n = 1; } else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; } else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; } else { cp = *p & 0x07; n = 4; }
        for (int i = 1; i < n && p[i]; ++i) cp = (cp << 6) | (p[i] & 0x3F);
        p += n;
        uint32_t ks = (cp >= 0x20 && cp < 0x7F) ? cp : 0x01000000u + cp;
        key_msg(ks, true); key_msg(ks, false);
    }
}

/* SetDesktopSize (RFB extension): asks the X server for a screen of w x h; the answer (an ExtendedDesktopSize rectangle) resizes the window surface in update() */
static void screen_resized(int w, int h);
static void set_desktop_size(int w, int h) {
    if (s_shm) { if (w > s_fb_w) w = s_fb_w; if (h > s_fb_h) h = s_fb_h; screen_resized(w, h); return; }
    if (!s_ext) return;
    uint8_t m[24] = { 251, 0 }; put16(m + 2, w); put16(m + 4, h); m[6] = 1; m[7] = 0;
    put32(m + 8, s_scr_id); put16(m + 12, 0); put16(m + 14, 0); put16(m + 16, w); put16(m + 18, h); put32(m + 20, s_scr_flags);
    snd(m, 24);
}
static void toggle_fullscreen(void) {
    int32_t sw = 0, sh = 0; cos_win2_screen_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return;
    s_fs = !s_fs;
    if (s_fs) { cos_win2_fullscreen(s_win, 1); set_desktop_size(sw, sh); }
    else { cos_win2_fullscreen(s_win, 0); set_desktop_size(s_norm_w, s_norm_h); }
    LOG("full screen %s (%dx%d)", s_fs ? "on" : "off", sw, sh);
}

static void handle_event(const cos_win_event_t *e) {
    if (s_shm && e->type != COS_EV_MOUSE_MOVE) { static int ne; if (ne++ < 40) LOG("event %d x %d y %d buttons %d", (int)e->type, (int)e->x, (int)e->y, (int)e->buttons); }
    switch (e->type) {
    case COS_EV_KEY: {
        char commit[1024]; int ir = imeui_key(e, commit, sizeof commit);                         /* Japanese input first */
        if (ir & COSIME_COMMIT) send_text(commit);
        if (ir & (COSIME_COMMIT | COSIME_CONSUMED)) { ime_refresh(true); break; }
        uint32_t ks = keysym_of(e); if (!ks) break;
        if (ks == 0xFFC8 && !(e->mods & (COS_MOD_CTRL | COS_MOD_ALT))) toggle_fullscreen();        /* F11: the C-OS window goes full screen at the screen's own size, and Firefox gets the key too (it hides its toolbars) */
        if (e->mods & COS_MOD_CTRL) key_msg(0xFFE3, true);
        if (e->mods & COS_MOD_ALT) key_msg(0xFFE9, true);
        key_msg(ks, true); key_msg(ks, false);
        if (e->mods & COS_MOD_ALT) key_msg(0xFFE9, false);
        if (e->mods & COS_MOD_CTRL) key_msg(0xFFE3, false);
        break;
    }
    case COS_EV_MOUSE_MOVE: case COS_EV_MOUSE_DOWN: case COS_EV_MOUSE_UP:
        s_buttons = rfb_buttons(e->buttons); pointer_msg(e->x, e->y, s_buttons); break;
    case COS_EV_WHEEL: {
        uint8_t b = e->wheel > 0 ? 8 : 16;
        pointer_msg(e->x, e->y, (uint8_t)(s_buttons | b)); pointer_msg(e->x, e->y, s_buttons); break;
    }
    case COS_EV_DROP: {                                                  /* a file from the File Manager (or the desktop) dropped on the window: offered to the X program under the point */
        char cp[512];
        if (cos_win2_get_path(s_win, cp, sizeof cp) < 0 || !cp[0]) break;
        static char lp[560], xs[16], ys[16];
        if (!strncmp(cp, "/linux/", 7)) snprintf(lp, sizeof lp, "%s", cp + 6); else snprintf(lp, sizeof lp, "/cos%s", cp);          /* the C-OS file system is /cos for Linux programs */
        snprintf(xs, sizeof xs, "%d", e->x); snprintf(ys, sizeof ys, "%d", e->y);
        cos_stat_t qs;
        if (cos_stat("/linux/tmp/cos-xdrop.alive", &qs) == 0 && cos_stat("/linux/tmp/cos-xdrop.req", &qs) != 0) {          /* the resident helper (cos-xdrop --serve) takes the request: no process start, no new connection */
            static char rq[700]; int n = snprintf(rq, sizeof rq, "%s %s %s\n", xs, ys, lp);
            cos_write_file("/linux/tmp/cos-xdrop.tmp", rq, (size_t)n); cos_rename("/linux/tmp/cos-xdrop.tmp", "/linux/tmp/cos-xdrop.req");
            LOG("dropped %s at %d,%d -> %s (resident helper)", cp, e->x, e->y, lp);
            break;
        }
        cos_unlink("/linux/tmp/cos-xdrop.req");                                  /* a request nobody took: the helper is gone; one process does this drop */
        const char *av[] = { "cos-xdrop", xs, ys, lp, NULL };
        int64_t pid = cos_spawn_argv("/linux/usr/local/bin/cos-xdrop", av, NULL);
        LOG("dropped %s at %d,%d -> %s (pid %d)", cp, e->x, e->y, lp, (int)pid);
        break; }
    case COS_EV_RESIZE:                                                  /* the window was maximized, restored or tiled: the X screen takes the size of its client area (no black margin) */
        if (e->x >= 320 && e->y >= 240 && e->x <= 4096 && e->y <= 4096) { s_norm_w = e->x & ~1; s_norm_h = e->y & ~1; if (!s_fs) set_desktop_size(s_norm_w, s_norm_h); }
        break;
    case COS_EV_CLOSE: s_quit = true; break;
    default: break;
    }
}
static void pump_events(void) { cos_win_event_t e; int n = 0; while (n++ < 64 && cos_win2_poll(s_win, &e) > 0) handle_event(&e); }

/* ---- receiving: waits in small steps so the window keeps answering ---------------------------------------------------------------------------- */
static bool rd(void *dst, size_t n) {
    uint8_t *p = (uint8_t *)dst; size_t got = 0;
    while (got < n) {
        if (s_quit) return false;
        ssize_t r = cos_recv(s_sd, p + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return false;                                  /* the server closed */
        if (r != -COS_EAGAIN) return false;
        pump_events();
        cos_sock_wait(s_sd, COS_POLLIN, 5);          /* an idle screen sends nothing: wait as long as the window is open */
    }
    return true;
}
static bool skip(size_t n) { uint8_t b[512]; while (n) { size_t k = n < sizeof b ? n : sizeof b; if (!rd(b, k)) return false; n -= k; } return true; }

static void request(bool incremental) { if (s_shm) return; uint8_t m[10] = { 3, incremental ? 1 : 0 }; put16(m + 2, 0); put16(m + 4, 0); put16(m + 6, s_w); put16(m + 8, s_h); snd(m, 10); }

/* the X screen has a new size: the window surface follows and everything is asked for again */
static void screen_resized(int w, int h) {
    if (w <= 0 || h <= 0 || (w == s_w && h == s_h)) return;
    s_w = w; s_h = h;
    cos_win_info_t ni;
    if (cos_win2_resize(s_win, w, h, &ni) != 0) { LOG("cannot resize the window surface to %dx%d", w, h); s_quit = true; return; }
    s_info = ni; s_cw = ni.width < w ? ni.width : w; s_ch = ni.height < h ? ni.height : h;
    memset(s_info.pixels, 0, (size_t)s_info.stride * (size_t)s_ch * 4);
    cui_canvas_init(&s_cv, s_info.pixels, s_cw, s_ch, s_info.stride); s_ow = 0;
    if (s_shm) {
        char v[32]; int n = snprintf(v, sizeof v, "%d %d\n", w, h); cos_write_file("/linux/tmp/cos-view", v, (size_t)n);
        if (s_shadow) cos_free(s_shadow);
        s_shadow = (uint32_t *)cos_malloc((size_t)s_info.stride * (size_t)s_info.height * 4); shadow_fill();
    }
    request(false);
    LOG("screen %dx%d", w, h);
}

/* copies what changed in the X screen (the shared memory) into the window surface; true when something was copied */
static bool shm_frame(void) {
    if (!s_fb || !s_shadow) return false;
    int w = s_cw < s_fb_w ? s_cw : s_fb_w, h = s_ch < s_fb_h ? s_ch : s_fb_h;
    int minx = w, maxx = -1, miny = -1, maxy = -1;
    for (int y = 0; y < h; ++y) {
        const uint32_t *f = s_fb + (size_t)y * (size_t)s_fb_stride; uint32_t *sh = s_shadow + (size_t)y * (size_t)s_info.stride;
        if (memcmp(f, sh, (size_t)w * 4) == 0) continue;
        int a = 0; while (a < w && f[a] == sh[a]) ++a;
        int b = w - 1; while (b > a && f[b] == sh[b]) --b;
        memcpy(sh + a, f + a, (size_t)(b - a + 1) * 4);
        memcpy(s_info.pixels + (size_t)y * (size_t)s_info.stride + (size_t)a, f + a, (size_t)(b - a + 1) * 4);
        if (a < minx) minx = a; if (b > maxx) maxx = b; if (miny < 0) miny = y; maxy = y;
    }
    if (maxx < 0) { static int dbg; if (dbg++ < 3) LOG("shm_frame: no change (w %d h %d fb %08x shadow %08x stride %d/%d)", w, h, (unsigned)s_fb[0], (unsigned)s_shadow[0], s_fb_stride, s_info.stride); return false; }
    cos_win_rect_t r = { minx, miny, maxx - minx + 1, maxy - miny + 1 };
    cos_win2_present_rects(s_win, &r, 1);
    if (imeui_on()) ime_refresh(false);
    return true;
}

static bool update(void) {
    uint8_t h[3]; if (!rd(h, 3)) return false;
    int nrect = get16(h + 1);
    cos_win_rect_t rects[64]; int nr = 0;
    for (int i = 0; i < nrect; ++i) {
        uint8_t r[12]; if (!rd(r, 12)) return false;
        int x = get16(r), y = get16(r + 2), w = get16(r + 4), hh = get16(r + 6); uint32_t enc = get32(r + 8);
        if (enc == 0) {                                            /* Raw */
            if (x == 0 && w == s_info.stride && y + hh <= s_ch && x + w <= s_cw) {      /* rows follow each other in memory: one read */
                if (!rd(s_info.pixels + (size_t)y * (size_t)s_info.stride, (size_t)w * (size_t)hh * 4)) return false;
            } else
            for (int row = 0; row < hh; ++row) {
                uint32_t *dst = s_info.pixels + (size_t)(y + row) * (size_t)s_info.stride + (size_t)x;
                if (y + row < s_ch && x + w <= s_cw) { if (!rd(dst, (size_t)w * 4)) return false; }
                else if (!skip((size_t)w * 4)) return false;
            }
        } else if (enc == 1) {                                     /* CopyRect */
            uint8_t s[4]; if (!rd(s, 4)) return false; int sx = get16(s), sy = get16(s + 2);
            if (sx + w <= s_cw && sy + hh <= s_ch && x + w <= s_cw && y + hh <= s_ch) {
                if (sy >= y) for (int row = 0; row < hh; ++row) memmove(s_info.pixels + (size_t)(y + row) * (size_t)s_info.stride + (size_t)x, s_info.pixels + (size_t)(sy + row) * (size_t)s_info.stride + (size_t)sx, (size_t)w * 4);
                else for (int row = hh - 1; row >= 0; --row) memmove(s_info.pixels + (size_t)(y + row) * (size_t)s_info.stride + (size_t)x, s_info.pixels + (size_t)(sy + row) * (size_t)s_info.stride + (size_t)sx, (size_t)w * 4);
            }
        } else if (enc == 0xFFFFFF21u) {                           /* DesktopSize */
            screen_resized(w, hh);
        } else if (enc == 0xFFFFFECCu) {                           /* ExtendedDesktopSize: x = who asked, y = result (0 = ok), w x h = the screen now */
            uint8_t eh[4]; if (!rd(eh, 4)) return false;
            for (int k = 0; k < eh[0]; ++k) { uint8_t sc[16]; if (!rd(sc, 16)) return false; if (k == 0) { s_scr_id = get32(sc); s_scr_flags = get32(sc + 12); s_ext = true; } }
            if (!(x == 1 && y != 0)) screen_resized(w, hh);
        } else { LOG("unsupported encoding %d", (int)enc); return false; }
        if (nr < 64) { rects[nr].x = x; rects[nr].y = y; rects[nr].w = w; rects[nr].h = hh; nr++; }
    }
    if (nr == 64 || nr < nrect) cos_win2_present(s_win); else if (nr) cos_win2_present_rects(s_win, rects, (uint32_t)nr);
    if (imeui_on()) ime_refresh(false);                                    /* the update may have painted over the composition */
    return true;
}

static bool server_message(void) {
    uint8_t t; if (!rd(&t, 1)) return false;
    switch (t) {
    case 0: return update();
    case 1: { uint8_t h[5]; if (!rd(h, 5)) return false; return skip((size_t)get16(h + 3) * 6); }          /* SetColourMapEntries */
    case 2: return true;                                                                                  /* Bell */
    case 3: { uint8_t h[7]; if (!rd(h, 7)) return false; return skip(get32(h + 3)); }                      /* ServerCutText */
    default: LOG("unknown server message %d", t); return false;
    }
}

static bool handshake(void) {
    char ver[12]; if (!rd(ver, 12)) return false;
    if (!snd("RFB 003.008\n", 12)) return false;
    uint8_t n; if (!rd(&n, 1) || n == 0) return false;
    uint8_t types[255]; if (!rd(types, n)) return false;
    bool none = false; for (int i = 0; i < n; ++i) if (types[i] == 1) none = true;
    if (!none) { LOG("the server wants a password"); return false; }
    uint8_t one = 1; if (!snd(&one, 1)) return false;
    uint8_t res[4]; if (!rd(res, 4) || get32(res) != 0) return false;
    if (!snd(&one, 1)) return false;                                                                       /* ClientInit: shared */
    uint8_t si[24]; if (!rd(si, 24)) return false;
    s_w = get16(si); s_h = get16(si + 2); uint32_t nl = get32(si + 20); skip(nl);
    uint8_t pf[20] = { 0 };                                                                                 /* SetPixelFormat: 32 bpp 0x00RRGGBB little endian */
    pf[4] = 32; pf[5] = 24; pf[6] = 0; pf[7] = 1; put16(pf + 8, 255); put16(pf + 10, 255); put16(pf + 12, 255); pf[14] = 16; pf[15] = 8; pf[16] = 0;
    if (!snd(pf, 20)) return false;
    uint8_t se[20] = { 2, 0 }; put16(se + 2, 4); put32(se + 4, 1); put32(se + 8, 0); put32(se + 12, 0xFFFFFF21u); put32(se + 16, 0xFFFFFECCu);   /* CopyRect, Raw, DesktopSize, ExtendedDesktopSize */
    return snd(se, 20);
}

/* The script the Linux side runs: an X server on the loopback, then the program. Written to /linux/etc/cos-gui.sh at every start so the viewer needs nothing else from the root file system. */
static const char k_script[] =
    "#!/bin/dash\n"
    "# written by linuxgui.c-os: starts an X server (VNC on the loopback, port 5901) and %s on it\n"
    "unset MOZ_HEADLESS\n"
    "export HOME=/root DISPLAY=:1 GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 GDK_DEBUG=gl-disable LIBGL_ALWAYS_SOFTWARE=1 LIBGL_DRIVERS_PATH=/nonexistent MOZ_X11_EGL=0 NO_AT_BRIDGE=1\n"
    "mkdir -p /tmp/.X11-unix /root/guiprof /root/.config/gtk-3.0\n"
    "cat > /root/.config/gtk-3.0/bookmarks <<'EOF'\n"
    "file:///cos C-OS\n"
    "file:///cos/documents Documents\n"
    "file:///cos/downloads Downloads\n"
    "file:///cos/pictures Pictures\n"
    "file:///cos/desktop Desktop\n"
    "EOF\n"
    "rm -f /tmp/.X1-lock /tmp/.X11-unix/X1\n"
    "cat > /root/guiprof/user.js <<'EOF'\n"
    "user_pref(\"gfx.webrender.software\", true);\n"
    "user_pref(\"gfx.webrender.software.opengl\", false);\n"
    "user_pref(\"gfx.x11-egl.force-disabled\", true);\n"
    "user_pref(\"layers.acceleration.disabled\", true);\n"
    "user_pref(\"layers.gpu-process.enabled\", false);\n"
    "user_pref(\"media.rdd-process.enabled\", false);\n"
    "user_pref(\"media.utility-process.enabled\", false);\n"
    "user_pref(\"media.rdd-ffvpx.enabled\", false);\n"
    "user_pref(\"network.process.enabled\", false);\n"
    "user_pref(\"fission.autostart\", false);\n"
    "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
    "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
    "user_pref(\"toolkit.telemetry.enabled\", false);\n"
    "user_pref(\"app.update.enabled\", false);\n"
    "user_pref(\"browser.safebrowsing.malware.enabled\", false);\n"
    "user_pref(\"browser.safebrowsing.phishing.enabled\", false);\n"
    "user_pref(\"browser.safebrowsing.downloads.enabled\", false);\n"
    "user_pref(\"browser.sessionstore.interval\", 600000);\n"
    "user_pref(\"datareporting.healthreport.uploadEnabled\", false);\n"
    "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
    "user_pref(\"extensions.pocket.enabled\", false);\n"
    "user_pref(\"browser.newtabpage.activity-stream.feeds.telemetry\", false);\n"
    "user_pref(\"browser.urlbar.suggest.quicksuggest.sponsored\", false);\n"
    "user_pref(\"network.dns.disablePrefetch\", true);\n"
    "user_pref(\"network.prefetch-next\", false);\n"
    "user_pref(\"general.smoothScroll\", false);\n"
    "user_pref(\"toolkit.cosmeticAnimations.enabled\", false);\n"
    "user_pref(\"dom.ipc.processPrelaunch.enabled\", false);\n"
    "user_pref(\"dom.ipc.processCount\", 1);\n"
    "user_pref(\"app.normandy.enabled\", false);\n"
    "user_pref(\"network.captive-portal-service.enabled\", false);\n"
    "user_pref(\"network.connectivity-service.enabled\", false);\n"
    "user_pref(\"extensions.update.enabled\", false);\n"
    "user_pref(\"browser.search.suggest.enabled\", false);\n"
    "user_pref(\"browser.urlbar.suggest.searches\", false);\n"
    "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
    "user_pref(\"browser.newtabpage.activity-stream.feeds.topsites\", false);\n"
    "user_pref(\"browser.newtabpage.activity-stream.feeds.section.topstories\", false);\n"
    "user_pref(\"browser.newtabpage.activity-stream.feeds.snippets\", false);\n"
    "user_pref(\"browser.newtabpage.activity-stream.showSponsoredTopSites\", false);\n"
    "user_pref(\"browser.tabs.animate\", false);\n"
    "user_pref(\"layout.frame_rate\", 30);\n"
    "user_pref(\"intl.accept_languages\", \"ja, en-US, en\");\n"
    "user_pref(\"browser.download.folderList\", 2);\n"
    "user_pref(\"browser.download.dir\", \"/cos/downloads\");\n"
    "user_pref(\"browser.download.useDownloadDir\", true);\n"
    "user_pref(\"browser.download.alwaysOpenPanel\", true);\n"
    "EOF\n"
    "[ -f /etc/cos-user.js ] && cat /etc/cos-user.js >> /root/guiprof/user.js\n"
    "%s"
    "exec %s\n";

/* the part of the script that starts the X server (and the helpers on it) */
static const char k_srv_vnc[] =
    "/usr/bin/Xtigervnc :1 -geometry %dx%d -depth 24 -SecurityTypes None -rfbport 5901 -UseIPv6=0 -interface 127.0.0.1 -fp built-ins &\n"
    "sleep 8\n"
    "[ -f /usr/local/bin/cos-wm ] && { /usr/local/bin/cos-wm; echo \"[cos-wm] ended: $?\"; } &\n"
    "[ -f /usr/local/bin/cos-xdrop ] && /usr/local/bin/cos-xdrop --serve &\n"
    "sleep 1\n";
static const char k_srv_shm[] =
    "rm -f /dev/shm/Xvfb_screen0\n"
    "/usr/bin/Xvfb :1 -screen 0 %dx%dx24 -fbdir /dev/shm -nolisten tcp -fp built-ins &\n"
    "sleep 10\n"
    "[ -f /usr/local/bin/cos-wm ] && { /usr/local/bin/cos-wm --view; echo \"[cos-wm] ended: $?\"; } >/tmp/cos-wm.log 2>&1 &\n"
    "[ -f /usr/local/bin/cos-xinput ] && { /usr/local/bin/cos-xinput; echo \"[cos-xinput] ended: $?\"; } >/tmp/cos-xinput.log 2>&1 &\n"
    "[ -f /usr/local/bin/cos-xdrop ] && /usr/local/bin/cos-xdrop --serve &\n"
    "sleep 1\n";

static bool exists(const char *p) { cos_stat_t st; return cos_stat(p, &st) == 0; }

/* Firefox draws Japanese with whatever font fontconfig finds; the root file system has only DejaVu. The Japanese font C-OS itself uses (/system/fonts, kana, punctuation and the JIS X 0208 kanji)
 * is copied under /linux/usr/share/fonts once, so web pages and the address bar show Japanese. */
static void install_jp_font(void) {
    const char *src = "/system/fonts/NotoSansJP-Regular.ttf", *dst = "/linux/usr/share/fonts/truetype/cos-jp/NotoSansJP-Regular.ttf";
    cos_stat_t st; if (cos_stat(dst, &st) == 0) return;
    if (cos_stat(src, &st) != 0 || st.size == 0 || st.size > 32u * 1024 * 1024) return;
    void *buf = cos_malloc((size_t)st.size); if (!buf) return;
    ssize_t n = cos_read_file(src, buf, (size_t)st.size);
    if (n == (ssize_t)st.size) {
        cos_mkdir("/linux/usr/share"); cos_mkdir("/linux/usr/share/fonts"); cos_mkdir("/linux/usr/share/fonts/truetype"); cos_mkdir("/linux/usr/share/fonts/truetype/cos-jp");
        if (cos_write_file(dst, buf, (size_t)n) == n) LOG("installed the Japanese font for Linux programs"); else LOG("could not install the Japanese font");
    }
    cos_free(buf);
}

/* the file-drop helper (tools/xdrop/xdrop.c) goes into the root file system, and /cos (an empty folder there) is where the program sees the C-OS files (cos_linux.c) */
static void install_xdrop(void) {
    cos_mkdir("/linux/cos"); cos_mkdir("/linux/usr/local"); cos_mkdir("/linux/usr/local/bin");
    const char *dst = "/linux/usr/local/bin/cos-xdrop"; cos_stat_t st;
    if (cos_stat(dst, &st) != 0 || st.size != cos_xdrop_blob_len) { if (cos_write_file(dst, cos_xdrop_blob, cos_xdrop_blob_len) < 0) LOG("could not install cos-xdrop"); }
    dst = "/linux/usr/local/bin/cos-xinput";
    if (cos_stat(dst, &st) != 0 || st.size != cos_xinput_blob_len) { if (cos_write_file(dst, cos_xinput_blob, cos_xinput_blob_len) < 0) LOG("could not install cos-xinput"); }
    dst = "/linux/usr/local/bin/cos-wm";                                                      /* the window manager that makes the program fill the screen */
    if (cos_stat(dst, &st) != 0 || st.size != cos_xwm_blob_len) { if (cos_write_file(dst, cos_xwm_blob, cos_xwm_blob_len) < 0) LOG("could not install cos-wm"); }
}

static int64_t start_guest(const char *script) {
    const char *argv[] = { "dash", script, NULL };
    int64_t pid = cos_spawn_argv("/linux/usr/bin/dash", argv, NULL);
    LOG("started %s: pid %d", script, (int)pid);
    return pid;
}

static uint32_t be32(const uint8_t *p) { return get32(p); }
/* the shared-memory display path: find the X screen in memory, connect the input helper, then copy what changed, about 60 times a second while it moves */
static int shm_main(void) {
    const uint8_t *base = NULL; uint64_t size = 0;
    for (int attempt = 0; attempt < 900 && !base; ++attempt) {
        const uint8_t *m = (const uint8_t *)cos_lxshm_map("Xvfb_screen0", &size);
        if (m && size > 200 && be32(m + 16) > 0 && be32(m + 20) > 0) base = m; else { if (attempt % 10 == 0) LOG("waiting for the X screen (%d)", attempt); cos_sleep_ms(1000); }
    }
    if (!base) { LOG("no X screen in shared memory"); return 1; }
    uint32_t hs = be32(base), bpp = be32(base + 44), bpl = be32(base + 48), ncol = be32(base + 76);
    s_fb_w = (int)be32(base + 16); s_fb_h = (int)be32(base + 20); s_fb_stride = (int)(bpl / 4);
    if (bpp != 32 || (uint64_t)hs + ncol * 12ull + (uint64_t)bpl * (uint64_t)s_fb_h > size) { LOG("unexpected X screen format (bpp %d, %d bytes)", (int)bpp, (int)size); return 1; }
    s_fb = (const uint32_t *)(base + hs + ncol * 12u);
    LOG("X screen %dx%d in shared memory (%d bytes)", s_fb_w, s_fb_h, (int)size);
    for (int attempt = 0; attempt < 20 && s_sd < 0; ++attempt) { s_sd = cos_tcp_connect(COS_IP(127, 0, 0, 1), 5902, 1000); if (s_sd < 0) cos_sleep_ms(500); }
    if (s_sd < 0) {
        LOG("no input helper (cos-xinput): the window is view only");
        static char lg[256]; ssize_t n = cos_read_file("/linux/tmp/cos-xinput.log", lg, sizeof lg - 1); if (n > 0) { lg[n] = 0; LOG("cos-xinput.log: %s", lg); } else LOG("cos-xinput.log: none (not started yet)");
    }
    s_w = s_norm_w; s_h = s_norm_h;
    s_win = cos_win2_create("Linux", s_w, s_h, &s_info);
    if (s_win < 0) { LOG("no window"); return 1; }
    s_cw = s_info.width < s_w ? s_info.width : s_w; s_ch = s_info.height < s_h ? s_info.height : s_h;
    LOG("window surface %dx%d stride %d", s_info.width, s_info.height, s_info.stride);
    memset(s_info.pixels, 0, (size_t)s_info.stride * (size_t)s_ch * 4);
    cui_canvas_init(&s_cv, s_info.pixels, s_cw, s_ch, s_info.stride); s_font = cui_font_default(15); s_cv_ok = s_font != NULL;
    imeui_set_redraw(ime_redraw_cb);
    s_shadow = (uint32_t *)cos_malloc((size_t)s_info.stride * (size_t)s_info.height * 4);
    if (!s_shadow) { LOG("out of memory"); return 1; }
    shadow_fill();
    uint64_t frames = 0, t0 = cos_time_ms(), tl = t0; int idle = 0;
    while (!s_quit) {
        pump_events();
        if (shm_frame()) { ++frames; idle = 0; cos_sleep_ms(8); } else { if (idle < 4) ++idle; cos_sleep_ms(8 * idle); }
        uint64_t now = cos_time_ms(); if (now - tl >= 10000) { LOG("%d frames in %d ms", (int)frames, (int)(now - tl)); frames = 0; tl = now;
            for (int ci = 0; ci <= 15; ++ci) {                        /* debugging: Firefox's own log (MOZ_LOG_FILE=/tmp/moz.log in /etc/cos.env), the lines that matter for video */
                char lp[64]; if (ci) snprintf(lp, sizeof lp, "/linux/tmp/moz.log.child-%d.moz_log", ci); else snprintf(lp, sizeof lp, "/linux/tmp/moz.log.moz_log"); static char mb[1 << 20];
                ssize_t mn = cos_read_file(lp, mb, sizeof mb - 1); if (mn <= 0) continue; mb[mn] = 0;
                int shown = 0; for (char *ln = mb; ln && *ln && shown < 6; ) { char *nl = strchr(ln, '\n'); if (nl) *nl = 0;
                    if (*ln) { char one[150]; snprintf(one, sizeof one, "%s", ln); LOG("moz%d: %s", ci, one); ++shown; }
                    ln = nl ? nl + 1 : NULL; }
            }
            static char xl[1024]; ssize_t xn = cos_read_file("/linux/tmp/cos-xinput.log", xl, sizeof xl - 1); if (xn > 0) { xl[xn] = 0; LOG("cos-xinput.log: %s", xl); } }
    }
    cos_win2_close(s_win);
    return 0;
}

int main(int argc, char **argv) {
    /* usage: linuxgui [firefox | program | script.sh]   (a .sh file is run as it is; anything else is started on the X server as a command) */
    const char *what = argc > 1 ? argv[1] : "firefox";
    char script_path[] = "/linux/etc/cos-gui.sh";
    s_shm = (exists("/system/linuxgui_shm") || exists("/linux/etc/cos-gui-shm")) && exists("/linux/usr/bin/Xvfb");
    if (s_shm) LOG("shared-memory display path");
    if ((!s_shm && !exists("/linux/usr/bin/Xtigervnc")) || !exists("/linux/usr/bin/dash")) {
        LOG("no Linux root file system with Xtigervnc under /linux (docs/LINUX_COMPAT.md)"); return 2;
    }
    install_jp_font();
    install_xdrop();
    cos_unlink("/linux/tmp/cos-xdrop.alive"); cos_unlink("/linux/tmp/cos-xdrop.req"); cos_unlink("/linux/tmp/cos-xdrop.tmp");      /* leftovers of an earlier session: the resident helper writes the first one when it runs */
    const char *script = script_path;
    size_t wl = strlen(what);
    if (wl > 3 && !strcmp(what + wl - 3, ".sh")) script = what;                 /* a script of your own */
    else {
        const char *cmd = "/usr/lib/firefox/firefox --profile /root/guiprof --no-remote about:blank";
        static char urlcmd[512];
        if (!strcmp(what, "firefox") && argc > 2) {                                                         /* linuxgui firefox ADDRESS: opens the address (only plain URL characters pass) */
            const char *u = argv[2]; bool okurl = strlen(u) > 0 && strlen(u) < 300;
            for (const char *q = u; *q && okurl; ++q) if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || strchr(":/._?=&%#~+-@", *q))) okurl = false;
            if (okurl) { snprintf(urlcmd, sizeof urlcmd, "/usr/lib/firefox/firefox --profile /root/guiprof --no-remote %s", u); cmd = urlcmd; }
            else LOG("the address has characters that are not allowed; opening about:blank");
        }
        if (strcmp(what, "firefox") != 0) cmd = what;
        if (!exists("/linux/usr/lib/firefox/firefox") && !strcmp(what, "firefox")) { LOG("Firefox is not installed in /linux"); return 2; }
        static char srv[1024], body[8192];
        if (s_shm) snprintf(srv, sizeof srv, k_srv_shm, 1920, 1080); else snprintf(srv, sizeof srv, k_srv_vnc, 1000, 640);
        snprintf(body, sizeof body, k_script, what, srv, cmd);
        cos_mkdir("/linux/etc"); if (cos_write_file(script_path, body, strlen(body)) < 0) { LOG("cannot write %s", script_path); return 2; }
    }
    if (s_shm) { cos_mkdir("/linux/tmp"); cos_write_file("/linux/tmp/cos-view", "1000 640\n", 9); }
    int64_t guest = start_guest(script); (void)guest;
    if (s_shm) return shm_main();
    for (int attempt = 0; attempt < 600 && s_sd < 0; ++attempt) {                                          /* the X server needs a while (the first start of a big program is slow) */
        s_sd = cos_tcp_connect(COS_IP(127, 0, 0, 1), VNC_PORT, 1000);
        if (s_sd < 0) { if (attempt % 10 == 0) LOG("waiting for the X server (%d)", s_sd); cos_sleep_ms(1000); }
    }
    if (s_sd < 0) { LOG("no X server"); return 1; }
    LOG("connected");
    if (!handshake()) { LOG("handshake failed"); return 1; }
    LOG("screen %dx%d", s_w, s_h);
    s_win = cos_win2_create("Linux", s_w, s_h, &s_info);
    if (s_win < 0) { LOG("no window"); return 1; }
    s_cw = s_info.width < s_w ? s_info.width : s_w; s_ch = s_info.height < s_h ? s_info.height : s_h;
    LOG("window surface %dx%d stride %d", s_info.width, s_info.height, s_info.stride);
    memset(s_info.pixels, 0, (size_t)s_info.stride * (size_t)s_ch * 4);
    cui_canvas_init(&s_cv, s_info.pixels, s_cw, s_ch, s_info.stride); s_font = cui_font_default(15); s_cv_ok = s_font != NULL;
    imeui_set_redraw(ime_redraw_cb);
    request(false);
    while (!s_quit) {
        if (!server_message()) break;
        request(true);
    }
    cos_win2_close(s_win);
    cos_sock_close(s_sd);
    return 0;
}
