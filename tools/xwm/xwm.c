/* cos-wm - the smallest window manager that makes a Linux program fill the C-OS window: raw X11 wire protocol, no libraries (built freestanding like cos-xdrop).
 *
 * linuxgui.c-os starts it right after the X server (Xtigervnc). Without a window manager a program keeps the size it asked for and cannot be made bigger, so a maximized or
 * full-screen C-OS window showed it with a black margin. This one:
 *   - gives every normal top-level window the size of the X screen (no title bar, no border) and follows the screen when linuxgui asks the X server for another size,
 *   - centers dialogs (transient windows and windows with a non-normal _NET_WM_WINDOW_TYPE) at the size they asked for, and focuses what it maps,
 *   - speaks enough EWMH (_NET_SUPPORTED, _NET_SUPPORTING_WM_CHECK, _NET_WM_STATE_FULLSCREEN) that Firefox's F11 really switches to its full-screen layout (toolbars hidden). */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long u64; typedef long i64;

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f; i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_read 0
#define SYS_write 1
#define SYS_poll 7
#define SYS_open 2
#define SYS_close 3
#define SYS_socket 41
#define SYS_connect 42
#define SYS_exit 60
static void xexit(int c) { sc6(SYS_exit, c, 0, 0, 0, 0, 0); for (;;) {} }

static int fd = -1; static u16 seq;
static u32 root, my_id_base, my_id_mask, win;
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 get32(const u8 *p) { return get16(p) | get16(p + 2) << 16; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void mcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void mzero(void *d, u64 n) { u8 *a = d; while (n--) *a++ = 0; }
static int meq(const void *a, const void *b, int n) { const u8 *x = a, *y = b; while (n--) if (*x++ != *y++) return 0; return 1; }

static int wr_all(const void *b, u64 n) { const u8 *p = b; while (n) { i64 w = sc6(SYS_write, fd, (i64)p, (i64)n, 0, 0, 0); if (w <= 0) return -1; p += w; n -= (u64)w; } return 0; }
static int rd_all(void *b, u64 n) { u8 *p = b; while (n) { i64 r = sc6(SYS_read, fd, (i64)p, (i64)n, 0, 0, 0); if (r <= 0) return -1; p += r; n -= (u64)r; } return 0; }
static int wait_readable(int ms) { struct { int fd; short ev, rev; } p = { fd, 1, 0 }; return (int)sc6(SYS_poll, (i64)&p, 1, ms, 0, 0, 0) > 0; }

/* a pending event that was read while waiting for a reply */
static u8 evq[16][32]; static int evn;
static void stash(const u8 *e) { if (evn < 16) mcpy(evq[evn++], e, 32); }

/* the reply to the request with sequence number `want` (extra data after the 32 bytes goes to *extra, up to cap) */
static int get_reply(u16 want, u8 *rep, u8 *extra, u32 cap, u32 *extra_len) {
    for (;;) {
        u8 b[32]; if (rd_all(b, 32)) return -1;                                  /* a plain blocking read: it waits as long as the (busy, emulated) X server needs */
        if (b[0] == 0) return -2;                                               /* an X error (e.g. a window that is gone) */
        if (b[0] == 1) {
            u32 ex = get32(b + 4) * 4; if (extra_len) *extra_len = ex;
            if (ex) { if (ex > cap) { u8 tmp[512]; u32 left = ex; if (extra) while (left) { u32 k = left < sizeof tmp ? left : (u32)sizeof tmp; if (rd_all(tmp, k)) return -1; left -= k; } return -3; } if (rd_all(extra, ex)) return -1; }
            if (get16(b + 2) != want) continue;                                  /* the late reply of an earlier request: not ours */
            mcpy(rep, b, 32); return 0;
        }
        stash(b);
    }
}
static void req(u8 op, u8 data, u8 *body, u32 body_bytes_padded) {            /* body_bytes_padded: everything after the 4-byte header, a multiple of 4 */
    u8 h[4]; h[0] = op; h[1] = data; put16(h + 2, 1 + body_bytes_padded / 4);
    wr_all(h, 4); if (body_bytes_padded) wr_all(body, body_bytes_padded); seq++;
}
static u32 intern(const char *name) {
    u8 buf[64]; int n = slen(name); mzero(buf, sizeof buf); put16(buf, (u32)n); mcpy(buf + 4, name, (u64)n);
    req(16, 0, buf, 4 + (u32)((n + 3) & ~3));
    u8 rep[32]; if (get_reply(seq, rep, 0, 0, 0)) return 0; return get32(rep + 8);
}
static u32 sw, sh;                         /* the X screen */
static u32 A_state, A_fs, A_check, A_name, A_utf8, A_supported, A_wtype, A_wtype_normal;
#define MAXW 32
static u32 mainw[MAXW]; static u8 fsw[MAXW]; static int nmain;
static u32 allocated;
static u32 new_id(void) { return my_id_base | ((++allocated) & my_id_mask); }

static void configure(u32 w, u32 mask, const i64 *v) {            /* ConfigureWindow: values in mask order (x, y, width, height, border, sibling, stack mode) */
    u8 b[40]; mzero(b, sizeof b); put32(b, w); put16(b + 4, mask); int n = 0;
    for (int bit = 0; bit < 7; ++bit) if (mask & (1u << bit)) put32(b + 8 + 4 * n++, (u32)v[bit]);
    req(12, 0, b, 8 + 4 * (u32)n);
}
static void map_window(u32 w) { u8 b[4]; put32(b, w); req(8, 0, b, 4); }
static void set_prop(u32 w, u32 prop, u32 type, int format, const void *data, int n) {      /* ChangeProperty (replace); n items of format bits */
    static u8 cp[512]; int bytes = n * (format / 8); mzero(cp, 20); put32(cp, w); put32(cp + 4, prop); put32(cp + 8, type); cp[12] = (u8)format; put32(cp + 16, (u32)n); mcpy(cp + 20, data, (u64)bytes);
    u32 padded = (u32)((bytes + 3) & ~3); u8 h[4]; h[0] = 18; h[1] = 0; put16(h + 2, 1 + (20 + padded) / 4); wr_all(h, 4); wr_all(cp, 20 + padded); seq++;
}
static void synth_configure_notify(u32 w, int x, int y, u32 cw, u32 ch) {
    u8 b[40]; mzero(b, sizeof b); put32(b, w); put32(b + 4, 0x20000u /* StructureNotify */);
    u8 *e = b + 8; e[0] = 22; put32(e + 4, w); put32(e + 8, w); put32(e + 12, 0); put16(e + 16, (u32)x); put16(e + 18, (u32)y); put16(e + 20, cw); put16(e + 22, ch); put16(e + 24, 0);
    req(25, 0, b, 40);
}
static void fill_screen(u32 w) { i64 v[7] = { 0, 0, sw, sh, 0, 0, 0 }; configure(w, 1 | 2 | 4 | 8 | 16, v); synth_configure_notify(w, 0, 0, sw, sh); }
static int is_main(u32 w) { for (int i = 0; i < nmain; ++i) if (mainw[i] == w) return i + 1; return 0; }
static void forget(u32 w) { int k = is_main(w); if (k) { --k; mainw[k] = mainw[nmain - 1]; fsw[k] = fsw[nmain - 1]; --nmain; } }

static int floating(u32 w) {                                                       /* a dialog: transient for another window, or a window type that is not "normal" */
    { u8 b[20]; put32(b, w); put32(b + 4, 68 /* WM_TRANSIENT_FOR */); put32(b + 8, 0); put32(b + 12, 0); put32(b + 16, 1); req(20, 0, b, 20); }
    u8 r[32], d[16]; u32 l = 0; int tr = 0; if (!get_reply(seq, r, d, sizeof d, &l) && get32(r + 16) >= 1) tr = 1;
    { u8 b[20]; put32(b, w); put32(b + 4, A_wtype); put32(b + 8, 0); put32(b + 12, 0); put32(b + 16, 1); req(20, 0, b, 20); }
    int odd = 0; if (!get_reply(seq, r, d, sizeof d, &l) && get32(r + 16) >= 1 && l >= 4 && get32(d) != A_wtype_normal) odd = 1;
    return tr || odd;
}
static void manage(u32 w) {
    if (floating(w)) {
        { u8 b[4]; put32(b, w); req(14, 0, b, 4); } u8 g[32]; i64 v[7] = { 0 };
        if (!get_reply(seq, g, 0, 0, 0)) {
            u32 gw = get16(g + 16), gh = get16(g + 18);
            if (gw > sw) gw = sw; if (gh > sh) gh = sh;
            v[0] = (i64)(sw - gw) / 2; v[1] = (i64)(sh - gh) / 2; v[2] = gw; v[3] = gh; configure(w, 1 | 2 | 4 | 8, v);
        }
    } else if (nmain < MAXW) { mainw[nmain] = w; fsw[nmain++] = 0; fill_screen(w); }
    else fill_screen(w);
    i64 top[7] = { 0, 0, 0, 0, 0, 0, 0 }; configure(w, 64, top);                          /* raised */
    map_window(w);
    { u8 b[8]; put32(b, w); put32(b + 4, 0); req(42, 1, b, 8); }                          /* SetInputFocus (revert to PointerRoot) */
}
static void set_state(u32 w, int on) { u32 a = A_fs; if (on) set_prop(w, A_state, 4 /* ATOM */, 32, &a, 1); else set_prop(w, A_state, 4, 32, &a, 0); }

/* /tmp/cos-view "W H": the part of the (larger) X screen the C-OS window shows; the program is resized to it, no X restart needed */
static u32 view_w, view_h; static int view_mode;                /* only with the argument --view (the shared-memory display path) */
static void view_check(void) {
    char b[32]; i64 f = sc6(SYS_open, (i64)"/tmp/cos-view", 0, 0, 0, 0, 0); if (f < 0) return;
    i64 n = sc6(SYS_read, f, (i64)b, 31, 0, 0, 0); sc6(SYS_close, f, 0, 0, 0, 0, 0); if (n <= 0) return; b[n] = 0;
    u32 v[2] = { 0, 0 }; int k = 0; for (i64 i = 0; i < n && k < 2; ++i) { if (b[i] >= '0' && b[i] <= '9') v[k] = v[k] * 10 + (u32)(b[i] - '0'); else if (v[k]) ++k; }
    if (k < 1 || !v[0] || !v[1] || v[0] > 8192 || v[1] > 8192 || (v[0] == view_w && v[1] == view_h)) return;
    view_w = v[0]; view_h = v[1]; sw = v[0]; sh = v[1];
    for (int i = 0; i < nmain; ++i) fill_screen(mainw[i]);
}

int main_c(int argc, char **argv) {
    view_mode = argc > 1;
    struct { u16 fam; char path[108]; } sa; mzero(&sa, sizeof sa); sa.fam = 1; { const char *p = "/tmp/.X11-unix/X1"; mcpy(sa.path, p, (u64)slen(p) + 1); }
    fd = (int)sc6(SYS_socket, 1, 1, 0, 0, 0, 0); if (fd < 0) xexit(3);
    if (sc6(SYS_connect, fd, (i64)&sa, 2 + slen("/tmp/.X11-unix/X1") + 1, 0, 0, 0) < 0) xexit(4);
    { u8 s[12]; mzero(s, sizeof s); s[0] = 'l'; put16(s + 2, 11); if (wr_all(s, 12)) xexit(5); }
    u8 hd[8]; if (rd_all(hd, 8) || hd[0] != 1) xexit(6);
    static u8 setup[262144]; u32 sl = get16(hd + 6) * 4; if (sl > sizeof setup || rd_all(setup, sl)) xexit(7);
    my_id_base = get32(setup + 4); my_id_mask = get32(setup + 8);
    u32 sb; { u32 vl = get16(setup + 16), nf = setup[21]; sb = 32 + ((vl + 3) & ~3u) + 8 * nf; root = get32(setup + sb); sw = get16(setup + sb + 20); sh = get16(setup + sb + 22); }
    A_state = intern("_NET_WM_STATE"); A_fs = intern("_NET_WM_STATE_FULLSCREEN"); A_check = intern("_NET_SUPPORTING_WM_CHECK"); A_name = intern("_NET_WM_NAME"); A_utf8 = intern("UTF8_STRING");
    A_supported = intern("_NET_SUPPORTED"); A_wtype = intern("_NET_WM_WINDOW_TYPE"); A_wtype_normal = intern("_NET_WM_WINDOW_TYPE_NORMAL");
    /* take the root: from now on the map and configure requests of the other programs come here */
    { u8 b[12]; put32(b, root); put32(b + 4, 0x800); put32(b + 8, (1u << 20) | (1u << 19) | (1u << 17)); req(2, 0, b, 12); }
    /* the EWMH check window and the list of what is supported */
    u32 chk = new_id();
    { u8 b[28]; mzero(b, sizeof b); put32(b, chk); put32(b + 4, root); put16(b + 12, 1); put16(b + 14, 1); put16(b + 18, 2); req(1, 0, b, 28); }
    set_prop(root, A_check, 33 /* WINDOW */, 32, &chk, 1); set_prop(chk, A_check, 33, 32, &chk, 1); set_prop(chk, A_name, A_utf8, 8, "cos-wm", 6);
    { u32 sup[4] = { A_check, A_state, A_fs, A_name }; set_prop(root, A_supported, 4, 32, sup, 4); }
    for (;;) {
        u8 e[32];
        if (!evn && view_mode) { struct { int fd; short ev, rev; } pf = { fd, 1, 0 }; if (sc6(SYS_poll, (i64)&pf, 1, 150, 0, 0, 0) == 0) { view_check(); continue; } }
        if (evn) mcpy(e, evq[--evn], 32); else if (rd_all(e, 32)) xexit(0);                 /* the X server is gone: so are we */
        u32 t = e[0] & 0x7F;
        if (t == 20) manage(get32(e + 8));                                                    /* MapRequest */
        else if (t == 23) {                                                                   /* ConfigureRequest */
            u32 w = get32(e + 8), m = get16(e + 26);
            if (is_main(w)) fill_screen(w);
            else { i64 v[7] = { (short)get16(e + 16), (short)get16(e + 18), (i64)get16(e + 20), (i64)get16(e + 22), (i64)get16(e + 24), 0, 0 }; configure(w, m & 31, v); }
        } else if (t == 22 && get32(e + 8) == root) {                                         /* the screen has a new size */
            sw = get16(e + 20); sh = get16(e + 22);
            for (int i = 0; i < nmain; ++i) fill_screen(mainw[i]);
        } else if (t == 17 || t == 18) forget(get32(e + 8));                                  /* DestroyNotify, UnmapNotify */
        else if (t == 33 && get32(e + 8) == A_state && e[1] == 32) {                          /* _NET_WM_STATE request from a program (Firefox: F11) */
            u32 w = get32(e + 4), act = get32(e + 12), a1 = get32(e + 16), a2 = get32(e + 20); int k = is_main(w);
            if (k && (a1 == A_fs || a2 == A_fs)) { int on = act == 2 ? !fsw[k - 1] : (int)act; fsw[k - 1] = (u8)on; set_state(w, on); }
        }
    }
    return 0;
}

__asm__(".text\n.global _start\n_start:\n xor %rbp,%rbp\n mov %rsp,%rdi\n and $-16,%rsp\n call start_c\n");
void start_c(long *sp) { int argc = (int)sp[0]; char **argv = (char **)(sp + 1); main_c(argc, argv); xexit(0); }
