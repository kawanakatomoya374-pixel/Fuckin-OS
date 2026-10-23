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

#define VNC_PORT 5901
#define LOG(...) do { char _b[160]; snprintf(_b, sizeof _b, "[LGUI] " __VA_ARGS__); cos_puts(_b); cos_puts("\n"); } while (0)

static int s_sd = -1;
static int64_t s_win;
static cos_win_info_t s_info;
static int s_w, s_h;                   /* the X screen */
static int s_cw, s_ch;                 /* what the window can show of it (the surface may be smaller) */
static uint8_t s_buttons;
static bool s_quit;

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int get16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static bool snd(const void *b, size_t n) { return cos_send_all(s_sd, b, n, 5000) == (ssize_t)n; }

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

static void handle_event(const cos_win_event_t *e) {
    switch (e->type) {
    case COS_EV_KEY: {
        uint32_t ks = keysym_of(e); if (!ks) break;
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

static void request(bool incremental) { uint8_t m[10] = { 3, incremental ? 1 : 0 }; put16(m + 2, 0); put16(m + 4, 0); put16(m + 6, s_w); put16(m + 8, s_h); snd(m, 10); }

static bool update(void) {
    uint8_t h[3]; if (!rd(h, 3)) return false;
    int nrect = get16(h + 1);
    cos_win_rect_t rects[64]; int nr = 0;
    for (int i = 0; i < nrect; ++i) {
        uint8_t r[12]; if (!rd(r, 12)) return false;
        int x = get16(r), y = get16(r + 2), w = get16(r + 4), hh = get16(r + 6); uint32_t enc = get32(r + 8);
        if (enc == 0) {                                            /* Raw */
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
        } else { LOG("unsupported encoding %d", (int)enc); return false; }
        if (nr < 64) { rects[nr].x = x; rects[nr].y = y; rects[nr].w = w; rects[nr].h = hh; nr++; }
    }
    if (nr == 64 || nr < nrect) cos_win2_present(s_win); else if (nr) cos_win2_present_rects(s_win, rects, (uint32_t)nr);
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
    uint8_t se[12] = { 2, 0 }; put16(se + 2, 2); put32(se + 4, 1); put32(se + 8, 0);                          /* CopyRect, Raw */
    return snd(se, 12);
}

/* The script the Linux side runs: an X server on the loopback, then the program. Written to /linux/etc/cos-gui.sh at every start so the viewer needs nothing else from the root file system. */
static const char k_script[] =
    "#!/bin/dash\n"
    "# written by linuxgui.c-os: starts an X server (VNC on the loopback, port 5901) and %s on it\n"
    "unset MOZ_HEADLESS\n"
    "export HOME=/root DISPLAY=:1 GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 GDK_DEBUG=gl-disable LIBGL_ALWAYS_SOFTWARE=1 MOZ_X11_EGL=0 NO_AT_BRIDGE=1\n"
    "mkdir -p /tmp/.X11-unix /root/guiprof\n"
    "rm -f /tmp/.X1-lock /tmp/.X11-unix/X1\n"
    "cat > /root/guiprof/user.js <<'EOF'\n"
    "user_pref(\"gfx.webrender.software\", true);\n"
    "user_pref(\"gfx.webrender.software.opengl\", false);\n"
    "user_pref(\"gfx.x11-egl.force-disabled\", true);\n"
    "user_pref(\"layers.acceleration.disabled\", true);\n"
    "user_pref(\"layers.gpu-process.enabled\", false);\n"
    "user_pref(\"media.rdd-process.enabled\", false);\n"
    "user_pref(\"media.utility-process.enabled\", false);\n"
    "user_pref(\"network.process.enabled\", false);\n"
    "user_pref(\"fission.autostart\", false);\n"
    "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
    "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
    "user_pref(\"toolkit.telemetry.enabled\", false);\n"
    "user_pref(\"app.update.enabled\", false);\n"
    "EOF\n"
    "/usr/bin/Xtigervnc :1 -geometry %dx%d -depth 24 -SecurityTypes None -rfbport 5901 -UseIPv6=0 -interface 127.0.0.1 -fp built-ins &\n"
    "sleep 8\n"
    "exec %s\n";

static bool exists(const char *p) { cos_stat_t st; return cos_stat(p, &st) == 0; }

static int64_t start_guest(const char *script) {
    const char *argv[] = { "dash", script, NULL };
    int64_t pid = cos_spawn_argv("/linux/usr/bin/dash", argv, NULL);
    LOG("started %s: pid %d", script, (int)pid);
    return pid;
}

int main(int argc, char **argv) {
    /* usage: linuxgui [firefox | program | script.sh]   (a .sh file is run as it is; anything else is started on the X server as a command) */
    const char *what = argc > 1 ? argv[1] : "firefox";
    char script_path[] = "/linux/etc/cos-gui.sh";
    if (!exists("/linux/usr/bin/Xtigervnc") || !exists("/linux/usr/bin/dash")) {
        LOG("no Linux root file system with Xtigervnc under /linux (docs/LINUX_COMPAT.md)"); return 2;
    }
    const char *script = script_path;
    size_t wl = strlen(what);
    if (wl > 3 && !strcmp(what + wl - 3, ".sh")) script = what;                 /* a script of your own */
    else {
        const char *cmd = "/usr/lib/firefox/firefox --profile /root/guiprof --no-remote about:blank";
        if (strcmp(what, "firefox") != 0) cmd = what;
        if (!exists("/linux/usr/lib/firefox/firefox") && !strcmp(what, "firefox")) { LOG("Firefox is not installed in /linux"); return 2; }
        static char body[4096]; snprintf(body, sizeof body, k_script, what, 1000, 640, cmd);
        cos_mkdir("/linux/etc"); if (cos_write_file(script_path, body, strlen(body)) < 0) { LOG("cannot write %s", script_path); return 2; }
    }
    int64_t guest = start_guest(script); (void)guest;
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
    request(false);
    while (!s_quit) {
        if (!server_message()) break;
        request(true);
    }
    cos_win2_close(s_win);
    cos_sock_close(s_sd);
    return 0;
}
