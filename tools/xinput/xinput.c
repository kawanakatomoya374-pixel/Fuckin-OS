/* cos-xinput - gives the X server keyboard and mouse input through the XTEST extension: raw X11 wire protocol, no libraries (built freestanding like cos-wm).
 *
 * With the shared-memory display path linuxgui.c-os talks to a plain Xvfb, which has no VNC server to take input from. This helper listens on 127.0.0.1:5902 and takes the
 * RFB-format messages linuxgui already makes (KeyEvent: 8 bytes, type 4; PointerEvent: 6 bytes, type 5) and replays them as XTEST FakeInput requests. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long u64; typedef long i64;

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f; i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_read 0
#define SYS_write 1
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_close 3
#define SYS_bind 49
#define SYS_listen 50
#define SYS_exit 60
static void xexit(int c) { sc6(SYS_exit, c, 0, 0, 0, 0, 0); for (;;) {} }

static int fd = -1; static u16 seq;
static u32 root;
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 get32(const u8 *p) { return get16(p) | get16(p + 2) << 16; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void mcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void mzero(void *d, u64 n) { u8 *a = d; while (n--) *a++ = 0; }
static int wr_fd(int f, const void *b, u64 n) { const u8 *p = b; while (n) { i64 w = sc6(SYS_write, f, (i64)p, (i64)n, 0, 0, 0); if (w <= 0) return -1; p += w; n -= (u64)w; } return 0; }
static int rd_fd(int f, void *b, u64 n) { u8 *p = b; while (n) { i64 r = sc6(SYS_read, f, (i64)p, (i64)n, 0, 0, 0); if (r <= 0) return -1; p += r; n -= (u64)r; } return 0; }
static int wr_all(const void *b, u64 n) { return wr_fd(fd, b, n); }
static int rd_all(void *b, u64 n) { return rd_fd(fd, b, n); }

static int reply(u8 *rep, u8 *extra, u32 cap, u32 *extra_len) {                 /* the next reply (nothing else is requested, so no events or errors are expected before it) */
    for (;;) {
        u8 b[32]; if (rd_all(b, 32)) return -1;
        if (b[0] == 0) return -2;
        if (b[0] == 1) {
            u32 ex = get32(b + 4) * 4; if (extra_len) *extra_len = ex;
            if (ex) { if (ex > cap) return -3; if (rd_all(extra, ex)) return -1; }
            mcpy(rep, b, 32); return 0;
        }
    }
}
static void req(u8 op, u8 data, const u8 *body, u32 body_bytes_padded) {
    u8 h[4]; h[0] = op; h[1] = data; put16(h + 2, 1 + body_bytes_padded / 4);
    wr_all(h, 4); if (body_bytes_padded) wr_all(body, body_bytes_padded); seq++;
}

static void logx(const char *tag, u32 a, u32 b) {            /* a line in the log (stderr): the first events only */
    static int n; if (n++ > 60) return;
    char l[48]; int k = 0; while (*tag && k < 8) l[k++] = *tag++; l[k++] = ' ';
    for (int sh = 28; sh >= 0; sh -= 4) l[k++] = "0123456789abcdef"[(a >> sh) & 15]; l[k++] = ' ';
    for (int sh = 28; sh >= 0; sh -= 4) l[k++] = "0123456789abcdef"[(b >> sh) & 15]; l[k++] = '\n';
    sc6(SYS_write, 2, (i64)l, k, 0, 0, 0);
}
static u8 xtest, min_kc, max_kc, kspk;
static u32 keymap[256][8];
static void fake(u8 type, u8 detail, int x, int y) {
    u8 b[32]; mzero(b, sizeof b); b[0] = type; b[1] = detail; put32(b + 8, type == 6 ? root : 0); put16(b + 20, (u32)x); put16(b + 22, (u32)y);
    req(xtest, 2, b, 32);
}
static u8 shift_kc;
static int pressed_n; static struct { u32 sym; u8 kc; u8 tmp_shift; } pressed[24];
static int shift_held;

static int find_key(u32 sym, int *col) {
    for (int c = 0; c < 2; ++c) for (int k = min_kc; k <= max_kc; ++k) if (keymap[k][c] == sym) { *col = c; return k; }
    return 0;
}
static u8 spare_kc;
static void key_event(u32 sym, int down) {
    int idx = -1; for (int i = 0; i < pressed_n; ++i) if (pressed[i].sym == sym) { idx = i; break; }
    if (!down) {
        if (idx < 0) return;
        fake(3, pressed[idx].kc, 0, 0);
        if (pressed[idx].tmp_shift) fake(3, shift_kc, 0, 0);
        pressed[idx] = pressed[--pressed_n]; return;
    }
    if (idx >= 0) { fake(3, pressed[idx].kc, 0, 0); fake(2, pressed[idx].kc, 0, 0); return; }          /* auto-repeat: another press */
    if (pressed_n >= 24) return;
    int col = 0; u32 s = sym; u8 kc = (u8)find_key(s, &col); u8 tmp = 0; logx("kc", kc, (u32)col);
    if (!kc && sym >= 0x01000000u) s = sym;                                                           /* Unicode key symbol: use the spare key code */
    if (!kc) {
        if (!spare_kc) return;
        u8 b[8 + 4 * 8]; mzero(b, sizeof b); b[0] = spare_kc; b[1] = kspk; for (int i = 0; i < kspk; ++i) put32(b + 4 + 4 * i, i == 0 ? s : 0);
        req(100, 1, b, 4 + 4 * (u32)kspk); kc = spare_kc; keymap[kc][0] = s; col = 0;
    }
    if (sym == 0xFFE1u || sym == 0xFFE2u) shift_held = 1;
    if (col == 1 && !shift_held) { fake(2, shift_kc, 0, 0); tmp = 1; }
    fake(2, kc, 0, 0); logx("sent", kc, 0);
    pressed[pressed_n].sym = sym; pressed[pressed_n].kc = kc; pressed[pressed_n].tmp_shift = tmp; pressed_n++;
}
static void key_up_shift(u32 sym) { if (sym == 0xFFE1u || sym == 0xFFE2u) shift_held = 0; }

int main_c(int argc, char **argv) {
    (void)argc; (void)argv;
    struct { u16 fam; char path[108]; } sa; mzero(&sa, sizeof sa); sa.fam = 1; { const char *p = "/tmp/.X11-unix/X1"; mcpy(sa.path, p, (u64)slen(p) + 1); }
    fd = (int)sc6(SYS_socket, 1, 1, 0, 0, 0, 0); if (fd < 0) xexit(3);
    if (sc6(SYS_connect, fd, (i64)&sa, 2 + slen("/tmp/.X11-unix/X1") + 1, 0, 0, 0) < 0) xexit(4);
    { u8 s[12]; mzero(s, sizeof s); s[0] = 'l'; put16(s + 2, 11); if (wr_all(s, 12)) xexit(5); }
    u8 hd[8]; if (rd_all(hd, 8) || hd[0] != 1) xexit(6);
    static u8 setup[262144]; u32 sl = get16(hd + 6) * 4; if (sl > sizeof setup || rd_all(setup, sl)) xexit(7);
    min_kc = setup[26]; max_kc = setup[27];
    { u32 vl = get16(setup + 16), nf = setup[21]; u32 sb = 32 + ((vl + 3) & ~3u) + 8 * nf; root = get32(setup + sb); }
    u8 rep[32];
    { u8 b[16]; mzero(b, sizeof b); put16(b, 5); mcpy(b + 4, "XTEST", 5); req(98, 0, b, 12); if (reply(rep, 0, 0, 0) || !rep[8]) xexit(8); xtest = rep[9]; }
    {   /* the keyboard map */
        u8 b[4]; b[0] = min_kc; b[1] = (u8)(max_kc - min_kc + 1); b[2] = b[3] = 0; req(101, 0, b, 4);
        static u8 km[256 * 8 * 4 + 64]; u32 el = 0; if (reply(rep, km, sizeof km, &el)) xexit(9);
        kspk = rep[1]; if (kspk > 8) kspk = 8;
        u32 stride = rep[1]; for (int k = min_kc; k <= max_kc; ++k) for (u32 c = 0; c < kspk; ++c) { u32 o = ((u32)(k - min_kc) * stride + c) * 4; if (o + 4 <= el) keymap[k][c] = get32(km + o); }
        for (int k = min_kc; k <= max_kc; ++k) if (keymap[k][0] == 0xFFE1u) { shift_kc = (u8)k; break; }
        for (int k = max_kc; k >= min_kc; --k) { int empty = 1; for (u32 c = 0; c < kspk; ++c) if (keymap[k][c]) empty = 0; if (empty) { spare_kc = (u8)k; break; } }
        if (!spare_kc) spare_kc = max_kc;
    }
    /* the listening socket for linuxgui */
    int ls = (int)sc6(SYS_socket, 2, 1, 0, 0, 0, 0); if (ls < 0) xexit(10);
    { int one = 1; sc6(54 /* setsockopt */, ls, 1, 2 /* SO_REUSEADDR */, (i64)&one, 4, 0); }
    struct { u16 fam; u8 port[2]; u8 addr[4]; u8 pad[8]; } in; mzero(&in, sizeof in); in.fam = 2; in.port[0] = 0x17; in.port[1] = 0x0E; in.addr[0] = 127; in.addr[3] = 1;      /* 5902 */
    if (sc6(SYS_bind, ls, (i64)&in, 16, 0, 0, 0) < 0) xexit(11);
    if (sc6(SYS_listen, ls, 1, 0, 0, 0, 0) < 0) xexit(12);
    for (;;) {
        int c = (int)sc6(SYS_accept, ls, 0, 0, 0, 0, 0); if (c < 0) continue;
        int px = 0, py = 0; u8 mask = 0;
        for (;;) {
            u8 t; if (rd_fd(c, &t, 1)) { logx("eof", 0, 0); break; }
            if (t == 4) {
                u8 m[7]; if (rd_fd(c, m, 7)) break; u32 sym = (u32)m[3] << 24 | (u32)m[4] << 16 | (u32)m[5] << 8 | m[6];
                logx("key", sym, m[0]); key_event(sym, m[0]); if (!m[0]) key_up_shift(sym);
            } else if (t == 5) {
                u8 m[5]; if (rd_fd(c, m, 5)) break; u8 nm = m[0]; px = m[1] << 8 | m[2]; py = m[3] << 8 | m[4];
                fake(6, 0, px, py); if (nm != mask) logx("btn", nm, (u32)px << 16 | (u32)py);
                for (int bit = 0; bit < 7; ++bit) if ((nm ^ mask) & (1 << bit)) fake((nm & (1 << bit)) ? 4 : 5, (u8)(bit + 1), px, py);
                mask = nm;
            } else break;                                                  /* anything else is not ours */
        }
        for (int i = 0; i < pressed_n; ++i) { fake(3, pressed[i].kc, 0, 0); if (pressed[i].tmp_shift) fake(3, shift_kc, 0, 0); }      /* nothing stays pressed */
        pressed_n = 0; shift_held = 0;
        for (int bit = 0; bit < 7; ++bit) if (mask & (1 << bit)) fake(5, (u8)(bit + 1), px, py);
        sc6(SYS_close, c, 0, 0, 0, 0, 0);
    }
    return 0;
}

__asm__(".text\n.global _start\n_start:\n xor %rbp,%rbp\n mov %rsp,%rdi\n and $-16,%rsp\n call start_c\n");
void start_c(long *sp) { int argc = (int)sp[0]; char **argv = (char **)(sp + 1); main_c(argc, argv); xexit(0); }
