/* cos-xdrop - drops a file on a window of the X server (Xtigervnc) the way a file manager does: the XDND protocol (version 5), over the raw X11 wire protocol.
 *
 *   cos-xdrop X Y PATH...      (X, Y: where on the X screen; PATH: absolute Linux paths)
 *
 * linuxgui.c-os (the viewer of the Linux GUI window) starts it when a file is dropped on the window: the program under the point (Firefox, with its upload area)
 * gets a normal file drop, as if it came from another X program. No libraries: it is built freestanding (no libc, no Xlib) - raw system calls and the X11 protocol -
 * so it runs in any root file system and weighs a few KiB. Exit status 0: the target took the drop.
 * Steps: connect to /tmp/.X11-unix/X1 -> a source window -> own the selection XdndSelection -> find the top window under the point that has the property XdndAware ->
 * XdndEnter, XdndPosition (twice), wait for XdndStatus -> XdndDrop -> answer the target's request for the selection with the text/uri-list -> XdndFinished. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long u64; typedef long i64;

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f; i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_read 0
#define SYS_write 1
#define SYS_poll 7
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
static void send_client_message(u32 dest, u32 type, u32 d0, u32 d1, u32 d2, u32 d3, u32 d4) {
    u8 b[40]; mzero(b, sizeof b); put32(b, dest); put32(b + 4, 0);
    u8 *e = b + 8; e[0] = 33; e[1] = 32; put32(e + 4, dest); put32(e + 8, type); put32(e + 12, d0); put32(e + 16, d1); put32(e + 20, d2); put32(e + 24, d3); put32(e + 28, d4);
    req(25, 0, b, 40);
}

static int dbg_status_try, dbg_fin_try;                         /* diagnostics: which round of the waits brought XdndStatus and the end of the drop */
static u32 aEnter, aPos, aStatus, aDrop, aFinished, aSel, aAware, aCopy, aUri;
static void init_x(void) {
    /* connect (once), make the source window, look up the atoms */
    struct { u16 fam; char path[108]; } sa; mzero(&sa, sizeof sa); sa.fam = 1; { const char *p = "/tmp/.X11-unix/X1"; mcpy(sa.path, p, (u64)slen(p) + 1); }
    fd = (int)sc6(SYS_socket, 1, 1, 0, 0, 0, 0); if (fd < 0) xexit(3);
    if (sc6(SYS_connect, fd, (i64)&sa, 2 + slen("/tmp/.X11-unix/X1") + 1, 0, 0, 0) < 0) xexit(4);
    { u8 s[12]; mzero(s, sizeof s); s[0] = 'l'; put16(s + 2, 11); if (wr_all(s, 12)) xexit(5); }
    u8 hd[8]; if (rd_all(hd, 8) || hd[0] != 1) xexit(6);
    static u8 setup[262144]; u32 sl = get16(hd + 6) * 4; if (sl > sizeof setup || rd_all(setup, sl)) xexit(7);
    my_id_base = get32(setup + 4); my_id_mask = get32(setup + 8);
    { u32 vl = get16(setup + 16), nf = setup[21]; root = get32(setup + 32 + ((vl + 3) & ~3u) + 8 * nf); }
    win = my_id_base | (my_id_mask & 1u ? 1u : (my_id_mask & -my_id_mask));
    /* a source window (input only, 1x1) */
    { u8 b[28]; mzero(b, sizeof b); put32(b, win); put32(b + 4, root); put16(b + 12, 1); put16(b + 14, 1); put16(b + 16, 0); put16(b + 18, 2); req(1, 0, b, 28); }
    aEnter = intern("XdndEnter"); aPos = intern("XdndPosition"); aStatus = intern("XdndStatus"); aDrop = intern("XdndDrop"); aFinished = intern("XdndFinished");
    aSel = intern("XdndSelection"); aAware = intern("XdndAware"); aCopy = intern("XdndActionCopy"); aUri = intern("text/uri-list");
}

static int do_drop(int argc, char **argv) {
    if (argc < 4) return 2;
    dbg_status_try = dbg_fin_try = 0;
    int px = 0, py = 0; for (const char *s = argv[1]; *s >= '0' && *s <= '9'; ++s) px = px * 10 + (*s - '0'); for (const char *s = argv[2]; *s >= '0' && *s <= '9'; ++s) py = py * 10 + (*s - '0');
    /* the data: a text/uri-list of the paths */
    static char uris[8192]; int ul = 0; evn = 0;
    for (int a = 3; a < argc; ++a) {
        const char *s = argv[a]; const char *pre = "file://"; for (int i = 0; pre[i] && ul < (int)sizeof uris - 8; ++i) uris[ul++] = pre[i];
        for (; *s && ul < (int)sizeof uris - 8; ++s) { u8 c = (u8)*s; int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
            if (ok) uris[ul++] = (char)c; else { static const char hx[] = "0123456789ABCDEF"; uris[ul++] = '%'; uris[ul++] = hx[c >> 4]; uris[ul++] = hx[c & 15]; } }
        uris[ul++] = '\r'; uris[ul++] = '\n';
    }
    /* own the selection */
    { u8 b[12]; put32(b, win); put32(b + 4, aSel); put32(b + 8, 0); req(22, 0, b, 12); }
    /* the window under the point that understands XDND: the children of the root, from the top */
    static u8 tree[16384]; u8 rep[32]; u32 tl = 0;
    { u8 b[4]; put32(b, root); req(15, 0, b, 4); }
    if (get_reply(seq, rep, tree, sizeof tree, &tl)) return 8;
    u32 nch = get16(rep + 16), target = 0, ver = 0;
    for (int i = (int)nch - 1; i >= 0 && !target; --i) {
        u32 w = get32(tree + 4 * i); u8 at[44], ex[64];
        { u8 b[4]; put32(b, w); req(3, 0, b, 4); } u32 el = 0; u8 ar[32];
        if (get_reply(seq, ar, ex, sizeof ex, &el) || el < 12) continue; (void)at;
        if (ar[26] != 2) continue;                                                   /* viewable? (map state: reply byte 26) */
        { u8 b[4]; put32(b, w); req(14, 0, b, 4); } u8 gr[32];
        if (get_reply(seq, gr, 0, 0, 0)) continue;
        int gx = (short)get16(gr + 12), gy = (short)get16(gr + 14), gw = (int)get16(gr + 16), gh = (int)get16(gr + 18);
        if (px < gx || py < gy || px >= gx + gw || py >= gy + gh) continue;
        { u8 b[20]; put32(b, w); put32(b + 4, aAware); put32(b + 8, 0); put32(b + 12, 0); put32(b + 16, 4); req(20, 0, b, 20); }
        u8 pr[32], pd[64]; u32 pl = 0; if (get_reply(seq, pr, pd, sizeof pd, &pl)) continue;
        if (get32(pr + 16) >= 1 && pl >= 4) { target = w; ver = get32(pd); }
    }
    if (!target) return 9;
    if (ver > 5) ver = 5;
    send_client_message(target, aEnter, win, ver << 24, aUri, 0, 0);
    u32 posd = ((u32)px << 16) | (u32)py;
    send_client_message(target, aPos, win, 0, posd, 0, aCopy);
    /* wait for the status */
    int accepted = 0;
    for (int tries = 0; tries < 4 && !accepted; ++tries) {          /* Firefox does not always answer XdndStatus: the drop is sent after at most 0.6 s */
        u8 e[32]; int have = 0;
        if (evn) { mcpy(e, evq[--evn], 32); have = 1; }
        else if (wait_readable(150) && !rd_all(e, 32)) have = 1;
        if (have && e[0] == 33 && get32(e + 8) == aStatus) { accepted = (get32(e + 16) & 1) != 0; dbg_status_try = tries + 1; }
        else if (!have && tries == 1) send_client_message(target, aPos, win, 0, posd, 0, aCopy);        /* a program that stays silent gets one more position message */
    }
    send_client_message(target, aDrop, win, 0, 0, 0, 0);
    /* serve the selection request, then wait for XdndFinished */
    int served = 0, finished = 0;
    for (int tries = 0; tries < 40 && !finished; ++tries) {
        u8 e[32]; int have = 0;
        if (evn) { mcpy(e, evq[--evn], 32); have = 1; }
        else if (wait_readable(150) && !rd_all(e, 32)) have = 1;
        if (!have) { if (served) break; continue; }
        if ((e[0] & 0x7F) == 30) {                                                  /* SelectionRequest */
            u32 requestor = get32(e + 12), selection = get32(e + 16), target_atom = get32(e + 20), property = get32(e + 24), time = get32(e + 4);
            if (!property) property = target_atom;
            static u8 cp[8300]; mzero(cp, 24); put32(cp, requestor); put32(cp + 4, property); put32(cp + 8, aUri); cp[12] = 8; put32(cp + 16, (u32)ul); mcpy(cp + 20, uris, (u64)ul);
            { u32 padded = (u32)((ul + 3) & ~3); u8 h[4]; h[0] = 18; h[1] = 0; put16(h + 2, 1 + (20 + padded) / 4); wr_all(h, 4); wr_all(cp, 20 + padded); seq++; }
            { u8 b[40]; mzero(b, sizeof b); put32(b, requestor); put32(b + 4, 0); u8 *n = b + 8; n[0] = 31; put32(n + 4, time); put32(n + 8, requestor); put32(n + 12, selection); put32(n + 16, target_atom); put32(n + 20, property); req(25, 0, b, 40); }
            served = 1;
        } else if (e[0] == 33 && get32(e + 8) == aFinished) { finished = 1; dbg_fin_try = tries + 1; }
    }
    return served ? 0 : 10;
}


/* --serve: the viewer (linuxgui) asks for drops by writing "X Y PATH" into /tmp/cos-xdrop.req; this process stays connected to the X server, so a drop costs no process start and no connection set-up.
 * /tmp/cos-xdrop.alive says that the server runs (linuxgui starts a one-shot process when it is missing). */
#define SYS_open 2
#define SYS_close 3
#define SYS_unlink 87
static void serve(void) {
    { i64 f = sc6(SYS_open, (i64)"/tmp/cos-xdrop.alive", 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644, 0, 0, 0); if (f >= 0) { sc6(SYS_write, f, (i64)"1\n", 2, 0, 0, 0); sc6(SYS_close, f, 0, 0, 0, 0, 0); } }
    for (;;) {
        struct { int fd; short ev, rev; } pf = { fd, 1, 0 };
        sc6(SYS_poll, (i64)&pf, 1, 40, 0, 0, 0);                                 /* the X server's events are read when a drop needs them; sleep 40 ms */
        if (pf.rev & 1) { u8 e[32]; if (rd_all(e, 32)) xexit(0); }                /* events nobody asked for (the server also closes the connection): read and ignore */
        i64 f = sc6(SYS_open, (i64)"/tmp/cos-xdrop.req", 0, 0, 0, 0, 0); if (f < 0) continue;
        static char line[1200]; i64 got = sc6(SYS_read, f, (i64)line, sizeof line - 1, 0, 0, 0); sc6(SYS_close, f, 0, 0, 0, 0, 0);
        if (got <= 0) continue;                                                  /* (being written: try again) */
        line[got] = 0; if (line[got - 1] != '\n') continue;                       /* the writer ends the request with a newline */
        sc6(SYS_unlink, (i64)"/tmp/cos-xdrop.req", 0, 0, 0, 0, 0);
        line[got - 1] = 0;
        char *av[5]; int ac = 0; av[ac++] = (char *)"cos-xdrop"; char *p = line;
        for (int k = 0; k < 3 && *p; ++k) { av[ac++] = p; while (*p && *p != ' ') ++p; if (k < 2) { if (*p) *p++ = 0; } else break; }   /* X, Y, then the rest of the line is the path (it may hold spaces) */
        if (ac == 4) {
            int rc = do_drop(ac, av);
            i64 f2 = sc6(SYS_open, (i64)"/tmp/cos-xdrop.rc", 0x241, 0644, 0, 0, 0);        /* the result of the last drop: "0" = delivered (for diagnosing) */
            if (f2 >= 0) { char r[16] = { (char)('0' + rc % 10), ' ', (char)('0' + dbg_status_try / 10), (char)('0' + dbg_status_try % 10), ' ', (char)('0' + dbg_fin_try / 10), (char)('0' + dbg_fin_try % 10), '\n', 0 }; sc6(SYS_write, f2, (i64)r, 8, 0, 0, 0); sc6(SYS_close, f2, 0, 0, 0, 0, 0); }
        }
    }
}

int main_c(int argc, char **argv) {
    init_x();
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 's') { serve(); xexit(0); }
    xexit(do_drop(argc, argv));
    return 0;
}

__asm__(".text\n.global _start\n_start:\n xor %rbp,%rbp\n mov %rsp,%rdi\n and $-16,%rsp\n call start_c\n");
void start_c(long *sp) { int argc = (int)sp[0]; char **argv = (char **)(sp + 1); main_c(argc, argv); xexit(0); }
