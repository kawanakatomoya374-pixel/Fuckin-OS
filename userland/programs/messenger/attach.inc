/* attach.inc - attachments for C-OS Message (included by main.c after the relay-server code).
 *
 * A file travels through the relay server: FUP/FCH/FEND to upload, FGET/FDATA to download (see tools/cosmsg_server.py).
 * The message that announces it has the text "\x01F|<fid>|<size>|<name>"; while an upload is running the sender's own
 * message reads "\x01U|0|<size>|<name>". Downloaded and uploaded files are cached in /messenger/files/<fid>.
 * Images (PNG, JPEG, BMP, GIF, WebP) get a thumbnail in the chat and a full-size viewer; MP3/Ogg/WAV play in the chat;
 * text files open in a viewer; everything can be saved to /downloads. Files are chosen with the "+" picker, drag and
 * drop onto the window, or /send <path>. */
#include "cosaudiodec.h"
#include "../../third_party/stb_image.h"

extern bool cos_webp_decode(const uint8_t *data, uint64_t size, uint8_t *out_bgra, uint64_t max_w, uint64_t max_h, uint64_t *out_w, uint64_t *out_h);
void serial_puts(const char *s) { cos_puts(s); }     /* the WebP decoder's only kernel dependency */

#define ATT_MAX       ((10u << 20) - 65536u)      /* the server's limit is 10 MB: leave room for the 16 bytes per 4 KB that encryption adds */
#define ATT_CHUNK     6144
#define ATT_AUTO_IMG  (2u << 20)
#define FILES_DIR     DIR "/files"
enum { AS_NEW = 0, AS_PLAIN, AS_REMOTE, AS_LOADING, AS_READY, AS_UPLOADING, AS_FAILED };
enum { AK_FILE = 0, AK_IMAGE, AK_AUDIO, AK_TEXT };

typedef struct { char fid[24]; uint32_t size; char name[96]; int kind; bool local; bool enc; uint8_t key[32]; } att_t;      /* enc: sealed on the server, key from the (end-to-end encrypted) announcement */

/* ---------------------------------------------------------------- helpers */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t *s, size_t n, char *o) {
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        o[k++] = B64[v >> 18]; o[k++] = B64[(v >> 12) & 63];
        o[k++] = i + 1 < n ? B64[(v >> 6) & 63] : '='; o[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    o[k] = 0; return k;
}
static int b64_val(char c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; }
static long b64_decode(const char *s, uint8_t *o, size_t cap) {
    size_t k = 0; uint32_t acc = 0; int bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64_val(*s); if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (k >= cap) return -1; o[k++] = (uint8_t)(acc >> bits); }
    }
    return (long)k;
}

static bool ext_is(const char *name, const char *list) {         /* list: ".png .jpg ..." */
    const char *dot = strrchr(name, '.'); if (!dot) return false;
    char e[12]; size_t n = strlen(dot); if (n >= sizeof e) return false;
    for (size_t i = 0; i <= n; i++) e[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
    size_t el = strlen(e);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *q = p; while (*q && *q != ' ') q++;
        if ((size_t)(q - p) == el && memcmp(p, e, el) == 0) return true;
        p = q;
    }
    return false;
}
static int kind_of(const char *name) {
    if (ext_is(name, ".png .jpg .jpeg .bmp .gif .webp")) return AK_IMAGE;
    if (ext_is(name, ".mp3 .ogg .wav")) return AK_AUDIO;
    if (ext_is(name, ".txt .md .log .csv .json .xml .c .h .cpp .py .js .html .css .ini .cfg .yml .yaml .sh .tsv")) return AK_TEXT;
    return AK_FILE;
}
static void clean_name(char *d, size_t cap, const char *s) {
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) { unsigned char c = (unsigned char)*s; d[k++] = (c < 32 || c == '/' || c == '\\' || c == '|') ? '_' : (char)c; }
    d[k] = 0;
    while (k > 0 && ((unsigned char)d[k - 1] & 0xC0) == 0x80 && k == cap - 1) k--;       /* never end on half a character */
    if (!d[0]) snprintf(d, cap, "file");
}

static bool att_parse(const char *t, att_t *a) {
    if (t[0] != 1 || (t[1] != 'F' && t[1] != 'U') || t[2] != '|') return false;
    a->local = t[1] == 'U';
    const char *p = t + 3, *e = strchr(p, '|');
    if (!e || e - p >= (int)sizeof a->fid || e == p) return false;
    memcpy(a->fid, p, (size_t)(e - p)); a->fid[e - p] = 0;
    p = e + 1; a->size = (uint32_t)strtoul(p, NULL, 10);
    e = strchr(p, '|'); if (!e) return false;
    a->enc = false;
    const char *kb = strchr(e + 1, '|');                           /* "...|name|<key>": the file key (43 base64url characters) */
    if (kb && strlen(kb + 1) == 43 && e2e_b64dec(kb + 1, 43, a->key, 32) == 32) { a->enc = true; char nm[96]; size_t nl = (size_t)(kb - (e + 1)); if (nl >= sizeof nm) nl = sizeof nm - 1; memcpy(nm, e + 1, nl); nm[nl] = 0; clean_name(a->name, sizeof a->name, nm); }
    else clean_name(a->name, sizeof a->name, e + 1);
    a->kind = kind_of(a->name);
    return true;
}
static void att_path(char *o, size_t cap, const char *fid) { snprintf(o, cap, FILES_DIR "/%s", fid); }
static bool fid_ok(const char *fid) { for (const char *p = fid; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false; return fid[0] != 0; }

static void fmt_size(char *o, size_t cap, uint32_t n) {
    if (n >= (1u << 20)) snprintf(o, cap, "%u.%u MB", n >> 20, (n & 0xFFFFF) * 10 >> 20);
    else if (n >= 1024) snprintf(o, cap, "%u KB", n >> 10);
    else snprintf(o, cap, "%u B", n);
}

static uint8_t *read_all(const char *path, size_t *len) {
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size > ATT_MAX + 1024) return NULL;
    uint8_t *b = (uint8_t *)malloc((size_t)st.size);
    if (!b) return NULL;
    ssize_t got = vread(path, b, (size_t)st.size);                 /* the cache is encrypted by the app lock when it is on */
    if (got <= 0) { free(b); return NULL; }
    *len = (size_t)got; return b;
}

/* ---------------------------------------------------------------- image decoding */
static bool webp_dims(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h) {
    if (n < 30 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4)) return false;
    size_t off = 12;
    while (off + 8 <= n) {
        uint32_t cs = (uint32_t)d[off + 4] | (uint32_t)d[off + 5] << 8 | (uint32_t)d[off + 6] << 16 | (uint32_t)d[off + 7] << 24;
        const uint8_t *c = d + off + 8;
        if (!memcmp(d + off, "VP8X", 4) && off + 8 + 10 <= n) { *w = 1 + ((uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16); *h = 1 + ((uint32_t)c[7] | (uint32_t)c[8] << 8 | (uint32_t)c[9] << 16); return true; }
        if (!memcmp(d + off, "VP8 ", 4) && off + 8 + 10 <= n) { *w = ((uint32_t)c[6] | (uint32_t)c[7] << 8) & 0x3FFF; *h = ((uint32_t)c[8] | (uint32_t)c[9] << 8) & 0x3FFF; return true; }
        if (!memcmp(d + off, "VP8L", 4) && off + 8 + 5 <= n && c[0] == 0x2F) { uint32_t b = (uint32_t)c[1] | (uint32_t)c[2] << 8 | (uint32_t)c[3] << 16 | (uint32_t)c[4] << 24; *w = 1 + (b & 0x3FFF); *h = 1 + ((b >> 14) & 0x3FFF); return true; }
        if (!memcmp(d + off, "ANMF", 4) && off + 8 + 12 <= n) { *w = 1 + ((uint32_t)c[6] | (uint32_t)c[7] << 8 | (uint32_t)c[8] << 16); *h = 1 + ((uint32_t)c[9] | (uint32_t)c[10] << 8 | (uint32_t)c[11] << 16); return true; }
        off += 8 + cs + (cs & 1);
    }
    return false;
}

/* Decodes any supported image to 0xAARRGGBB pixels (malloc'ed). */
static uint32_t *img_decode(const uint8_t *d, size_t n, int *ow, int *oh) {
    uint32_t w = 0, h = 0;
    if (n > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
        if (!webp_dims(d, n, &w, &h) || w == 0 || h == 0 || w > 4096 || h > 4096) return NULL;
        uint8_t *bgra = (uint8_t *)malloc((size_t)w * h * 4);
        if (!bgra) return NULL;
        uint64_t rw = 0, rh = 0;
        if (!cos_webp_decode(d, n, bgra, w, h, &rw, &rh) || !rw || !rh) { free(bgra); return NULL; }
        uint32_t *px = (uint32_t *)malloc((size_t)rw * rh * 4);
        if (px) for (uint64_t i = 0; i < rw * rh; i++) px[i] = (uint32_t)bgra[i * 4 + 3] << 24 | (uint32_t)bgra[i * 4 + 2] << 16 | (uint32_t)bgra[i * 4 + 1] << 8 | bgra[i * 4];
        free(bgra);
        *ow = (int)rw; *oh = (int)rh; return px;
    }
    int iw, ih, comp;
    if (!stbi_info_from_memory(d, (int)n, &iw, &ih, &comp) || iw <= 0 || ih <= 0 || (long)iw * ih > 40L * 1000 * 1000) return NULL;
    uint8_t *rgba = stbi_load_from_memory(d, (int)n, &iw, &ih, &comp, 4);
    if (!rgba) return NULL;
    uint32_t *px = (uint32_t *)malloc((size_t)iw * ih * 4);
    if (px) for (long i = 0; i < (long)iw * ih; i++) px[i] = (uint32_t)rgba[i * 4 + 3] << 24 | (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
    stbi_image_free(rgba);
    *ow = iw; *oh = ih; return px;
}

/* area-average scale of ARGB pixels onto `bg`, producing opaque 0x00RRGGBB */
static uint32_t *img_scale(const uint32_t *src, int sw, int sh, int dw, int dh, uint32_t bg) {
    uint32_t *o = (uint32_t *)malloc((size_t)dw * dh * 4);
    if (!o) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh); if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw); if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            int stepy = (y1 - y0) > 4 ? (y1 - y0) / 4 : 1, stepx = (x1 - x0) > 4 ? (x1 - x0) / 4 : 1;
            for (int yy = y0; yy < y1; yy += stepy) for (int xx = x0; xx < x1; xx += stepx) {
                uint32_t p = src[(long)yy * sw + xx]; unsigned a = p >> 24;
                unsigned pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
                r += (pr * a + ((bg >> 16) & 255) * (255 - a)) / 255; g += (pg * a + ((bg >> 8) & 255) * (255 - a)) / 255; b += (pb * a + (bg & 255) * (255 - a)) / 255; cnt++;
            }
            o[(long)y * dw + x] = (r / cnt) << 16 | (g / cnt) << 8 | (b / cnt);
        }
    }
    return o;
}

/* ---------------------------------------------------------------- thumbnails (a small cache) */
#define THUMB_MAX_W 260
#define THUMB_MAX_H 220
static uint32_t *thumb_ring[24]; static int thumb_next; static int thumb_budget;

static void thumb_free(uint32_t *p) {
    if (!p) return;
    for (int i = 0; i < 24; i++) if (thumb_ring[i] == p) thumb_ring[i] = NULL;
    for (int i = 0; i < nct; i++) for (int j = 0; j < ct[i].n; j++) if (ct[i].m[j].thumb == p) ct[i].m[j].thumb = NULL;
    free(p);
}
/* false = no preview possible */
static bool att_thumb(msg_t *m, const att_t *a) {
    if (m->thumb) return true;
    if (m->tw < 0 || thumb_budget <= 0) return false;
    thumb_budget--;
    char path[96]; att_path(path, sizeof path, a->fid);
    size_t n; uint8_t *d = read_all(path, &n);
    int w = 0, h = 0; uint32_t *px = d ? img_decode(d, n, &w, &h) : NULL;
    free(d);
    if (!px) { m->tw = -1; return false; }
    int tw = w, th = h;
    if (tw > THUMB_MAX_W) { th = th * THUMB_MAX_W / tw; tw = THUMB_MAX_W; }
    if (th > THUMB_MAX_H) { tw = tw * THUMB_MAX_H / th; th = THUMB_MAX_H; }
    if (tw < 1) tw = 1; if (th < 1) th = 1;
    uint32_t *t = img_scale(px, w, h, tw, th, m->mine ? MINE : THEIRS);
    free(px);
    if (!t) return false;
    if (thumb_ring[thumb_next]) thumb_free(thumb_ring[thumb_next]);
    thumb_ring[thumb_next] = t; thumb_next = (thumb_next + 1) % 24;
    m->thumb = t; m->tw = (int16_t)tw; m->th = (int16_t)th;
    return true;
}

/* ---------------------------------------------------------------- save to /downloads */
static bool att_save(const att_t *a) {
    char path[96]; att_path(path, sizeof path, a->fid);
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) { set_status(T("Download the file first")); return false; }
    cos_mkdir("/downloads");
    char dst[200], base[96], ext[16] = "";
    snprintf(base, sizeof base, "%s", a->name);
    char *dot = strrchr(base, '.');
    if (dot && strlen(dot) < sizeof ext) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    snprintf(dst, sizeof dst, "/downloads/%s%s", base, ext);
    for (int i = 1; i < 100; i++) { cos_stat_t st; if (cos_stat(dst, &st) != 0) break; snprintf(dst, sizeof dst, "/downloads/%s (%d)%s", base, i, ext); }
    bool ok = cos_write_file(dst, d, n) == (ssize_t)n;
    free(d);
    char b[240]; snprintf(b, sizeof b, ok ? T("Saved to %s") : T("Could not save %s"), dst);
    set_status(b);
    return ok;
}

/* ---------------------------------------------------------------- uploads and downloads */
static struct { bool active; int state; uint8_t *data, *plain; char fname[96], fkey[48]; size_t size, plain_n, sent; int idx; char fid[24]; char uid[12]; uint32_t msgid; char name[96]; uint64_t t; bool avatar; } up;
typedef struct { bool active, avatar, enc; uint8_t key[32]; uint32_t plain; char fid[24]; uint32_t size, got; int next; uint8_t *buf; } dl_t;
static dl_t dls[3];

static msg_t *att_find_msg(const char *fid, contact_t **kout) {
    for (int i = 0; i < nct; i++) for (int j = ct[i].n - 1; j >= 0; j--) {
        const char *t = ct[i].m[j].text;
        if (t[0] == 1 && t[1] == 'F' && !strncmp(t + 3, fid, strlen(fid)) && t[3 + strlen(fid)] == '|') { if (kout) *kout = &ct[i]; return &ct[i].m[j]; }
    }
    return NULL;
}
static void att_mark(const char *fid, int state) {
    for (int i = 0; i < nct; i++) for (int j = 0; j < ct[i].n; j++) {
        const char *t = ct[i].m[j].text;
        if (t[0] == 1 && t[1] == 'F' && !strncmp(t + 3, fid, strlen(fid)) && t[3 + strlen(fid)] == '|') { ct[i].m[j].att = (uint8_t)state; if (state == AS_READY) thumb_free(ct[i].m[j].thumb); }
    }
}
static dl_t *dl_find(const char *fid) { for (int i = 0; i < 3; i++) if (dls[i].active && !strcmp(dls[i].fid, fid)) return &dls[i]; return NULL; }
static void dl_fail(dl_t *d, const char *why) { free(d->buf); d->buf = NULL; d->active = false; if (!d->avatar) att_mark(d->fid, AS_REMOTE); else av_changed(); if (why) set_status(why); dirty = true; }

static void att_fail_upload(const char *why) {
    if (!up.active) return;
    contact_t *k = NULL; msg_t *m = up.avatar ? NULL : find_srv_msg(up.msgid, up.uid, &k);
    if (m) { m->state = ST_FAILED; m->att = AS_FAILED; if (k) save_chat(k); }
    e2e_wipe(up.fkey, sizeof up.fkey); if (up.plain) e2e_wipe(up.plain, up.plain_n); free(up.data); up.data = NULL; free(up.plain); up.plain = NULL; up.active = false;
    if (why) set_status(why);
    dirty = true;
}
static void att_on_disconnect(void) {
    att_fail_upload(T("Upload interrupted - the connection was lost"));
    for (int i = 0; i < 3; i++) if (dls[i].active) dl_fail(&dls[i], NULL);
}

/* Uploads a profile photo (no chat message): once the server has it, AVATAR <fid> makes it public. */
static bool att_upload_avatar(uint8_t *data, size_t n) {
    if (srv_state != SV_ONLINE) { free(data); set_status(T("Not connected to the server")); return false; }
    if (up.active) { free(data); set_status(T("Another upload is still running")); return false; }
    memset(&up, 0, sizeof up);
    up.active = true; up.avatar = true; up.data = data; up.size = n; up.t = cos_time_ms();
    snprintf(up.name, sizeof up.name, "avatar.bmp");
    return true;
}

static void att_send_file(contact_t *k, const char *path) {
    if (!k->uid[0]) { set_status(T("Files can only be sent to friends made through the server")); return; }
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to the server")); return; }
    if (up.active) { set_status(T("Another upload is still running")); return; }
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.is_dir) { set_status(T("No such file")); return; }
    if (st.size == 0 || st.size > ATT_MAX) { set_status(T("Files must be between 1 byte and 10 MB")); return; }
    if (is_group(k) && e2e_req && (!e2e_ready || k->ngm == 0)) { set_status(e2e_ready ? T("Cannot send a file yet: the member list has not arrived") : T("Cannot send a file: encryption is not available")); return; }
    if (!is_group(k) && e2e_req && !(e2e_ready && KP(k)->has_kb && !KP(k)->kchanged)) { set_status(KP(k)->kchanged ? T("This friend's security key changed - check Actions > Security first") : T("Cannot send a file yet: no encryption key for this friend (yet)")); return; }
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) { set_status(T("Cannot read that file")); return; }
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char name[96]; clean_name(name, sizeof name, base);
    uint8_t *plain = NULL; size_t plain_n = 0; uint8_t fkey[32]; bool seal = e2e_ready && (is_group(k) ? k->ngm > 0 : (KP(k)->has_kb && !KP(k)->kchanged));
    if (seal) {                                                      /* encrypt the file here: the server only gets sealed segments */
        size_t cs = e2e_file_cipher_size(n); uint8_t *cb = (uint8_t *)malloc(cs); if (!cb) { free(d); set_status(T("Out of memory")); return; }
        e2e_rand(fkey, 32);
        for (size_t off = 0, idx = 0, co = 0; off < n || (n == 0 && idx == 0); idx++) { size_t l = n - off < E2E_SEG ? n - off : E2E_SEG; memcpy(cb + co, d + off, l); e2e_file_seal(fkey, idx, off + l >= n, cb + co, l); co += l + 16; off += l; if (n == 0) break; }
        plain = d; plain_n = n; d = cb; n = cs;
    }
    char text[160]; snprintf(text, sizeof text, "\x01U|0|%u|%s", (unsigned)(seal ? plain_n : n), name);
    msg_t *m = add_msg(k, true, (uint32_t)cos_time_unix(), ST_SENDING, ++next_id, text);
    if (!m) { free(d); return; }
    m->att = AS_UPLOADING;
    { char lb[160]; snprintf(lb, sizeof lb, "[MESSENGER] upload start %s (%u bytes)\n", name, (unsigned)n); cos_puts(lb); }
    memset(&up, 0, sizeof up);
    up.active = true; up.data = d; up.size = n; up.msgid = m->id; up.t = cos_time_ms(); up.plain = plain; up.plain_n = plain_n;
    snprintf(up.uid, sizeof up.uid, "%s", k->uid); snprintf(up.name, sizeof up.name, "%s", seal ? "e2e.bin" : name);    /* the server never sees the file name of an encrypted file */
    if (seal) { char kb[48]; e2e_b64enc(fkey, 32, kb); snprintf(up.fname, sizeof up.fname, "%s", name); snprintf(up.fkey, sizeof up.fkey, "%s", kb); e2e_wipe(fkey, 32); }
    else { up.fname[0] = 0; up.fkey[0] = 0; }
    scroll_px = 0; dirty = true;
}

static bool tls_room(size_t n) {          /* is there room in the TLS send buffer for n more bytes? (flushes first) */
    if (!tls_pump()) return false;
    size_t len = 0; unsigned char *b = br_ssl_engine_sendapp_buf(&tls_cc.eng, &len); (void)b;
    return len >= n;
}

static void att_upload_pump(void) {
    if (!up.active || srv_state != SV_ONLINE) return;
    contact_t *k = NULL; msg_t *m = up.avatar ? NULL : find_srv_msg(up.msgid, up.uid, &k);
    if (!m && !up.avatar) { att_fail_upload(NULL); return; }
    char nb[24];
    if (up.state == 0) { snprintf(nb, sizeof nb, "%u", (unsigned)up.size); if (srv_tx("FUP", nb, up.name, NULL)) { up.state = 1; up.t = cos_time_ms(); } return; }
    if ((up.state == 1 || up.state == 3) && since(up.t) > 30000) { att_fail_upload(T("The server did not answer the upload")); return; }
    if (up.state == 2) {
        for (int c = 0; c < 3 && up.sent < up.size; c++) {
            if (!tls_room(ATT_CHUNK * 4 / 3 + 128)) return;
            size_t take = up.size - up.sent < ATT_CHUNK ? up.size - up.sent : ATT_CHUNK;
            char b64[ATT_CHUNK * 4 / 3 + 8]; b64_encode(up.data + up.sent, take, b64);
            snprintf(nb, sizeof nb, "%d", up.idx);
            if (!srv_tx("FCH", up.fid, nb, b64)) return;
            up.sent += take; up.idx++;
            if (m) m->apct = (uint8_t)(up.sent * 100 / up.size);
            dirty = true;
        }
        if (up.sent >= up.size) { srv_tx("FEND", up.fid, NULL, NULL); up.state = 3; up.t = cos_time_ms(); cos_puts("[MESSENGER] upload: all chunks sent, FEND\n"); }
    }
}

/* Fetches a profile photo (size unknown, at most 256 KB). */
static bool att_fetch_avatar(const char *fid) {
    if (!fid_ok(fid) || srv_state != SV_ONLINE) return false;
    if (dl_find(fid)) return true;
    dl_t *d = NULL; for (int i = 0; i < 3; i++) if (!dls[i].active) { d = &dls[i]; break; }
    if (!d) return false;
    d->buf = (uint8_t *)malloc(256 * 1024);
    if (!d->buf) return false;
    d->active = true; d->avatar = true; snprintf(d->fid, sizeof d->fid, "%s", fid); d->size = 256 * 1024; d->got = 0; d->next = 0;
    srv_tx("FGET", fid, NULL, NULL);
    return true;
}

static bool att_download(const att_t *a) {
    if (!fid_ok(a->fid) || a->size == 0 || a->size > ATT_MAX) return false;
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to the server")); return false; }
    if (dl_find(a->fid)) return true;
    dl_t *d = NULL; for (int i = 0; i < 3; i++) if (!dls[i].active) { d = &dls[i]; break; }
    if (!d) return false;
    uint32_t wire_size = a->enc ? (uint32_t)e2e_file_cipher_size(a->size) : a->size;
    d->buf = (uint8_t *)malloc(wire_size);
    if (!d->buf) return false;
    d->active = true; d->avatar = false; snprintf(d->fid, sizeof d->fid, "%s", a->fid); d->size = wire_size; d->got = 0; d->next = 0;
    d->enc = a->enc; d->plain = a->size; if (a->enc) memcpy(d->key, a->key, 32);
    srv_tx("FGET", a->fid, NULL, NULL);
    att_mark(a->fid, AS_LOADING);
    dirty = true;
    return true;
}

/* frames from the server; true when the frame was an attachment frame */
static bool att_srv_frame(char **f, int n) {
    const char *cmd = f[0];
    if (!strcmp(cmd, "FUP_OK") && n >= 2) {
        if (up.active && up.state == 1) { snprintf(up.fid, sizeof up.fid, "%s", f[1]); up.state = 2; cos_puts("[MESSENGER] upload: FUP_OK\n"); }
        return true;
    }
    if (!strcmp(cmd, "FEND_OK") && n >= 2) {
        if (up.active && up.state == 3 && !strcmp(f[1], up.fid)) {
            cos_puts("[MESSENGER] upload: FEND_OK, announcing\n");
            contact_t *k = NULL; msg_t *m = up.avatar ? NULL : find_srv_msg(up.msgid, up.uid, &k);
            cos_mkdir(FILES_DIR);
            char path[96]; att_path(path, sizeof path, up.fid);
            vwrite(path, up.plain ? up.plain : up.data, up.plain ? up.plain_n : up.size);
            if (up.avatar) { srv_tx("AVATAR", up.fid, NULL, NULL); prof_avatar_uploaded(up.fid); }
            if (m) {
                char text[260];
                if (up.fkey[0]) snprintf(text, sizeof text, "\x01" "F|%s|%u|%s|%s", up.fid, (unsigned)up.plain_n, up.fname, up.fkey);
                else snprintf(text, sizeof text, "\x01" "F|%s|%u|%s", up.fid, (unsigned)up.size, up.name);
                free(m->text); m->text = strdup(text); m->att = AS_READY; m->apct = 100;
                srv_send_msg(k, m); save_chat(k);
            }
            e2e_wipe(up.fkey, sizeof up.fkey); if (up.plain) e2e_wipe(up.plain, up.plain_n); free(up.data); up.data = NULL; free(up.plain); up.plain = NULL; up.active = false; dirty = true;
        }
        return true;
    }
    if (!strcmp(cmd, "FERR")) {
        const char *why = n >= 3 ? f[2] : T("file transfer failed");
        if (up.active && (n < 2 || !f[1][0] || !strcmp(f[1], up.fid))) att_fail_upload(why);
        else if (n >= 2) { dl_t *d = dl_find(f[1]); if (d) dl_fail(d, why); }
        return true;
    }
    if (!strcmp(cmd, "FDATA") && n >= 5) {
        dl_t *d = dl_find(f[1]);
        if (!d) return true;
        if ((int)strtol(f[2], NULL, 10) != d->next) { dl_fail(d, T("Download out of order")); return true; }
        long got = b64_decode(f[4], d->buf + d->got, d->size - d->got);
        if (got < 0) { dl_fail(d, T("Corrupt download")); return true; }
        d->got += (uint32_t)got; d->next++; dirty = true;
        if (f[3][0] == '1') {
            if (!d->avatar && d->got != d->size) { dl_fail(d, T("Download is incomplete")); return true; }
            cos_mkdir(FILES_DIR);
            char path[96]; att_path(path, sizeof path, d->fid);
            uint32_t outn = d->got;
            if (d->enc) {                                                 /* open the sealed segments: every one is checked, a missing or moved one fails */
                size_t co = 0, po = 0; bool good = d->got == e2e_file_cipher_size(d->plain);
                for (size_t idx = 0; good && co < d->got; idx++) { size_t cl = d->got - co < E2E_SEG + 16 ? d->got - co : E2E_SEG + 16; bool last = co + cl >= d->got;
                    if (cl < 16 || !e2e_file_open(d->key, idx, last, d->buf + co, cl)) { good = false; break; } memmove(d->buf + po, d->buf + co, cl - 16); po += cl - 16; co += cl; }
                if (!good || po != d->plain) { dl_fail(d, T("The file is damaged or was tampered with")); return true; }
                outn = (uint32_t)po; e2e_wipe(d->key, 32);
            }
            vwrite(path, d->buf, outn);
            free(d->buf); d->buf = NULL; d->active = false;
            if (d->avatar) av_changed(); else att_mark(d->fid, AS_READY);
        }
        return true;
    }
    return false;
}

static void att_inspect(msg_t *m, att_t *a, bool *isatt) {
    *isatt = att_parse(m->text, a);
    if (m->att == AS_NEW) {
        if (!*isatt) m->att = AS_PLAIN;
        else if (a->local) m->att = AS_FAILED;
        else { cos_stat_t st; char p[96]; att_path(p, sizeof p, a->fid); m->att = (fid_ok(a->fid) && cos_stat(p, &st) == 0) ? AS_READY : AS_REMOTE; }
    }
    if (*isatt && m->att == AS_REMOTE && !m->mine && a->kind == AK_IMAGE && a->size <= ATT_AUTO_IMG && srv_state == SV_ONLINE && fid_ok(a->fid)) att_download(a);
}

/* ---------------------------------------------------------------- audio player */
static struct { char fid[24]; adec *d; uint8_t *data; bool playing, paused, opened, eof; uint32_t rate; double dur; uint64_t written; int16_t pcm[2048 * 2]; int len, off; } aud;

static void aud_stop(void) {
    if (aud.opened) { cos_audio_stop(); cos_audio_close(); }
    if (aud.d) adec_close(aud.d);
    free(aud.data);
    memset(&aud, 0, sizeof aud);
    dirty = true;
}
static double aud_pos(void) {
    if (!aud.opened || !aud.rate) return 0;
    cos_audio_status_t st; uint64_t pend = 0;
    if (cos_audio_status(&st) == 0) pend = st.pending_samples / 2;
    uint64_t w = aud.written; double p = (double)(w > pend ? w - pend : 0) / aud.rate;
    return p > aud.dur && aud.dur > 0 ? aud.dur : p;
}
static void aud_toggle(const att_t *a) {
    if (aud.d && !strcmp(aud.fid, a->fid)) {
        if (aud.paused) { aud.paused = false; aud.playing = true; }
        else { aud.paused = true; aud.playing = false; if (aud.opened) { cos_audio_stop(); uint64_t w = aud.written; (void)w; } }
        dirty = true; return;
    }
    aud_stop();
    char path[96]; att_path(path, sizeof path, a->fid);
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) { set_status(T("Download the file first")); return; }
    adec *dec = adec_open(d, n);
    if (!dec) { free(d); set_status(T("This audio file cannot be played (MP3, Ogg Vorbis and WAV, 8-48 kHz)")); return; }
    int r = cos_audio_open(adec_rate(dec), 2);
    if (r != 0) { adec_close(dec); free(d); set_status(r == -16 ? T("The audio device is in use by another program") : T("No audio device")); return; }
    aud.d = dec; aud.data = d; aud.rate = adec_rate(dec); aud.dur = adec_duration(dec); aud.opened = true; aud.playing = true;
    { char lb[100]; snprintf(lb, sizeof lb, "[MESSENGER] audio start %u Hz %.1f s\n", aud.rate, aud.dur); cos_puts(lb); }
    snprintf(aud.fid, sizeof aud.fid, "%s", a->fid);
    dirty = true;
}
static void aud_pump(void) {
    if (!aud.d || !aud.playing) return;
    for (int loop = 0; loop < 8; loop++) {
        if (aud.off >= aud.len) {
            if (aud.eof) break;
            int n = adec_read(aud.d, aud.pcm, 2048);
            if (n <= 0) { aud.eof = true; break; }
            aud.len = n * 2; aud.off = 0;
        }
        cos_audio_status_t st;
        if (cos_audio_status(&st) != 0 || st.free_samples < (uint64_t)(aud.len - aud.off)) return;
        int64_t acc = cos_audio_write(aud.pcm + aud.off, (uint64_t)(aud.len - aud.off));
        if (acc <= 0) return;
        aud.off += (int)acc; aud.written += (uint64_t)acc / 2;
    }
    if (aud.eof && aud.off >= aud.len) {
        cos_audio_status_t st;
        if (cos_audio_status(&st) != 0 || st.pending_samples == 0) { cos_puts("[MESSENGER] audio finished\n"); aud_stop(); }
    }
    dirty = true;
}

/* ---------------------------------------------------------------- viewers */
static struct { int kind; att_t a; uint32_t *pix; int w, h; char *text; int nlines, scroll, cap; const char **line; int *llen; } view;   /* kind 0 none, 1 image, 2 text */

static void view_close(void) {
    free(view.pix); free(view.text); free(view.line); free(view.llen);
    memset(&view, 0, sizeof view); dirty = true;
}
static void view_layout_text(void) {
    free(view.line); free(view.llen); view.line = NULL; view.llen = NULL; view.nlines = 0; view.cap = 0;
    int maxw = win_w - 80;
    for (char *p = view.text; *p; ) {
        char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        char saved = p[len]; p[len] = 0;
        int nl = len ? wrap(f_t, p, maxw) : 1;
        for (int l = 0; l < nl; l++) {
            if (view.nlines == view.cap) { int nc = view.cap ? view.cap * 2 : 256; if (nc > 40000) break; view.line = (const char **)realloc(view.line, sizeof(char *) * (size_t)nc); view.llen = (int *)realloc(view.llen, sizeof(int) * (size_t)nc); view.cap = nc; }
            view.line[view.nlines] = len ? ls_[l] : p; view.llen[view.nlines] = len ? ll_[l] : 0; view.nlines++;
        }
        p[len] = saved;                                   /* lines keep pointing into the text, which keeps its \n bytes */
        if (!e) break;
        p = e + 1;
    }
}
static void view_open(const att_t *a) {
    { char lb[140]; snprintf(lb, sizeof lb, "[MESSENGER] view open %s kind=%d\n", a->name, a->kind); cos_puts(lb); }
    char path[96]; att_path(path, sizeof path, a->fid);
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) { set_status(T("Download the file first")); return; }
    view_close();
    view.a = *a;
    if (a->kind == AK_IMAGE) {
        int w, h; uint32_t *px = img_decode(d, n, &w, &h);
        free(d);
        if (!px) { set_status(T("This image cannot be shown")); return; }
        int mw = win_w - 40, mh = win_h - 130, dw = w, dh = h;
        if (dw > mw) { dh = dh * mw / dw; dw = mw; }
        if (dh > mh) { dw = dw * mh / dh; dh = mh; }
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        view.pix = img_scale(px, w, h, dw, dh, 0x10131A); free(px);
        if (!view.pix) return;
        view.w = dw; view.h = dh; view.kind = 1;
    } else {
        size_t take = n > (256u << 10) ? (256u << 10) : n;
        view.text = (char *)malloc(take + 1);
        if (!view.text) { free(d); return; }
        for (size_t i = 0; i < take; i++) { unsigned char c = d[i]; view.text[i] = (c == '\r' || c == '\t') ? ' ' : (char)((c < 32 && c != '\n') ? ' ' : c); }
        view.text[take] = 0; free(d);
        view.kind = 2; view_layout_text();
        { char lb[100]; snprintf(lb, sizeof lb, "[MESSENGER] text viewer: %d lines\n", view.nlines); cos_puts(lb); }
    }
    dirty = true;
}

#define VIEW_BAR 48
static void view_draw(void) {
    if (!view.kind) return;
    cui_fill(&c, 0, 0, win_w, win_h, 0x0B0D12);
    cui_fill(&c, 0, 0, win_w, VIEW_BAR, PANEL);
    cui_text_fit(&c, f_b, 16, 14, win_w - 220, view.a.name, TEXT);
    cui_round_rect(&c, win_w - 190, 9, 84, 30, 8, MINE);   cui_text_center(&c, f_s, win_w - 190, 9, 84, 30, T("Save"), 0xFFFFFF);
    cui_round_rect(&c, win_w - 98, 9, 84, 30, 8, 0x4A5068); cui_text_center(&c, f_s, win_w - 98, 9, 84, 30, T("Close"), 0xFFFFFF);
    if (view.kind == 1) cui_blit(&c, (win_w - view.w) / 2, VIEW_BAR + (win_h - VIEW_BAR - view.h) / 2, view.pix, view.w, view.h, view.w);
    else {
        int rows = (win_h - VIEW_BAR - 20) / lh;
        cui_clip(&c, 0, VIEW_BAR, win_w, win_h - VIEW_BAR);
        for (int i = 0; i < rows && view.scroll + i < view.nlines; i++) draw_line(40, VIEW_BAR + 12 + i * lh, view.line[view.scroll + i], view.llen[view.scroll + i], TEXT);
        cui_unclip(&c);
        char pg[40]; snprintf(pg, sizeof pg, "%d / %d", view.scroll + 1, view.nlines);
        cui_text(&c, f_s, win_w - cui_text_width(f_s, pg) - 16, win_h - 20, pg, MUTED);
    }
}
static void view_click(int x, int y) {
    if (y < VIEW_BAR && x >= win_w - 190 && x < win_w - 106) att_save(&view.a);
    else if ((y < VIEW_BAR && x >= win_w - 98) || (view.kind == 1 && (y > VIEW_BAR))) view_close();
}
static void view_key(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ESC) { view_close(); return; }
    if (view.kind != 2) return;
    int rows = (win_h - VIEW_BAR - 20) / lh, maxs = view.nlines > rows ? view.nlines - rows : 0;
    if (ev->special == COS_KEY_DOWN) view.scroll++; else if (ev->special == COS_KEY_UP) view.scroll--;
    else if (ev->special == COS_KEY_PGDN) view.scroll += rows; else if (ev->special == COS_KEY_PGUP) view.scroll -= rows;
    else if (ev->special == COS_KEY_HOME) view.scroll = 0; else if (ev->special == COS_KEY_END) view.scroll = maxs;
    if (view.scroll < 0) view.scroll = 0; if (view.scroll > maxs) view.scroll = maxs;
    dirty = true;
}

/* ---------------------------------------------------------------- file picker */
static void stage_add(const char *path);
typedef struct { char name[96]; bool dir; uint32_t size; } pk_ent;
static struct { bool open, avatar; char path[200]; pk_ent *e; int n, scroll; } pk;
#define PK_W 560
#define PK_H 440
#define PK_ROW 28

static int pk_cmp(const void *a, const void *b) {
    const pk_ent *x = (const pk_ent *)a, *y = (const pk_ent *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void pk_load(const char *path) {
    free(pk.e); pk.e = (pk_ent *)calloc(512, sizeof(pk_ent)); pk.n = 0; pk.scroll = 0;
    snprintf(pk.path, sizeof pk.path, "%s", path);
    int fd = cos_opendir(path);
    if (fd < 0) return;
    cos_dirent_t de;
    while (pk.n < 512 && cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.') continue;
        pk_ent *e = &pk.e[pk.n++];
        scopy(e->name, sizeof e->name, de.name); e->dir = de.is_dir != 0; e->size = (uint32_t)(de.size > 0xFFFFFFFFu ? 0xFFFFFFFFu : de.size);
    }
    cos_close(fd);
    qsort(pk.e, (size_t)pk.n, sizeof(pk_ent), pk_cmp);
}
static void pk_show(void) { pk.open = true; pk_load(pk.path[0] ? pk.path : "/pictures"); dirty = true; }
static void pk_up(void) {
    char *s = strrchr(pk.path, '/');
    if (!s || s == pk.path) { pk_load("/"); return; }
    *s = 0; pk_load(pk.path);
}
static const char *const PK_ROOTS[] = { "/pictures", "/music", "/documents", "/downloads", "/desktop" };
#define PK_PX ((win_w - PK_W) / 2)
#define PK_PY ((win_h - PK_H) / 2)
static void pk_draw(void) {
    if (!pk.open) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 160);
    int x = PK_PX, y = PK_PY;
    cui_round_rect(&c, x, y, PK_W, PK_H, 12, PANEL);
    cui_text(&c, f_b, x + 16, y + 12, pk.avatar ? T("Choose a profile photo") : T("Send a file"), TEXT);
    cui_text_fit(&c, f_s, x + 120, y + 15, PK_W - 150, pk.path, MUTED);
    for (int i = 0; i < 5; i++) { cui_round_rect(&c, x + 16 + i * 104, y + 40, 98, 24, 8, 0x2B3550); cui_text_center(&c, f_s, x + 16 + i * 104, y + 40, 98, 24, PK_ROOTS[i] + 1, TEXT); }
    int rows = (PK_H - 110) / PK_ROW;
    cui_clip(&c, x, y + 72, PK_W, rows * PK_ROW);
    int total = pk.n + 1;
    for (int i = 0; i < rows && pk.scroll + i < total; i++) {
        int idx = pk.scroll + i, ry = y + 72 + i * PK_ROW;
        if (i % 2) cui_fill(&c, x + 8, ry, PK_W - 16, PK_ROW, 0x20293E);
        if (idx == 0) { cui_text(&c, f_t, x + 20, ry + 4, T("..  (up)"), ACCENT); continue; }
        pk_ent *e = &pk.e[idx - 1];
        cui_text_fit(&c, f_t, x + 20, ry + 4, PK_W - 150, e->name, e->dir ? ACCENT : TEXT);
        if (!e->dir) { char sz[24]; fmt_size(sz, sizeof sz, e->size); cui_text(&c, f_s, x + PK_W - 16 - cui_text_width(f_s, sz), ry + 7, sz, e->size > ATT_MAX ? BAD : MUTED); }
    }
    cui_unclip(&c);
    cui_text(&c, f_s, x + 16, y + PK_H - 26, T("Click a file to send it (max 10 MB).  Esc: cancel"), MUTED);
}
static void pk_click(int mx, int my) {
    int x = PK_PX, y = PK_PY;
    if (mx < x || mx >= x + PK_W || my < y || my >= y + PK_H) { pk.open = false; pk.avatar = false; dirty = true; return; }
    if (my >= y + 40 && my < y + 64) { int i = (mx - x - 16) / 104; if (i >= 0 && i < 5) pk_load(PK_ROOTS[i]); dirty = true; return; }
    int rows = (PK_H - 110) / PK_ROW;
    if (my < y + 72 || my >= y + 72 + rows * PK_ROW) return;
    int idx = pk.scroll + (my - y - 72) / PK_ROW;
    if (idx == 0) { pk_up(); dirty = true; return; }
    if (idx - 1 >= pk.n) return;
    pk_ent *e = &pk.e[idx - 1];
    char full[320]; snprintf(full, sizeof full, "%s%s%s", pk.path, strcmp(pk.path, "/") ? "/" : "", e->name);
    if (e->dir) { pk_load(full); dirty = true; return; }
    pk.open = false; dirty = true;
    if (pk.avatar) { pk.avatar = false; prof_set_photo(full); return; }
    if (cur >= 0) stage_add(full);
}
static void pk_key(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ESC) { pk.open = false; pk.avatar = false; dirty = true; return; }
    int rows = (PK_H - 110) / PK_ROW, maxs = pk.n + 1 > rows ? pk.n + 1 - rows : 0;
    if (ev->special == COS_KEY_DOWN) pk.scroll++; else if (ev->special == COS_KEY_UP) pk.scroll--;
    else if (ev->special == COS_KEY_PGDN) pk.scroll += rows; else if (ev->special == COS_KEY_PGUP) pk.scroll -= rows;
    if (pk.scroll < 0) pk.scroll = 0; if (pk.scroll > maxs) pk.scroll = maxs;
    dirty = true;
}


/* ---------------------------------------------------------------- staged attachments
 * The "+" button opens the File Manager in picker mode (/bin/files.c-os --pick RESULT): the files chosen there - and files dropped
 * on the window - are not sent at once but put in the message box as chips; Send (or Enter) sends them, then the text. */

static void stage_add(const char *path) {
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.is_dir) { set_status(T("No such file")); return; }
    if (st.size == 0 || st.size > ATT_MAX) { set_status(T("Files must be between 1 byte and 10 MB")); return; }
    for (int i = 0; i < nstg; i++) if (!strcmp(stg[i].path, path)) return;
    if (nstg >= STG_MAX) { set_status(T("At most 8 files at a time")); return; }
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(stg[nstg].path, sizeof stg[nstg].path, "%s", path);
    clean_name(stg[nstg].name, sizeof stg[nstg].name, base);
    stg[nstg].size = (uint32_t)st.size;
    nstg++; dirty = true;
}
static void stage_remove(int i) { if (i < 0 || i >= nstg) return; memmove(&stg[i], &stg[i + 1], (size_t)(nstg - i - 1) * sizeof stg[0]); nstg--; dirty = true; }

static void pick_launch(void) {
    if (cur < 0) { set_status(T("Select a friend first")); return; }
    if (pick_pid > 0) { set_status(T("The file picker is already open")); return; }
    cos_mkdir("/tmp"); cos_unlink(PICK_OUT);
    const char *av[] = { "files.c-os", "--pick", PICK_OUT, NULL };
    int64_t pid = cos_spawn_argv("/bin/files.c-os", av, NULL);
    if (pid <= 0) { pk_show(); return; }                                  /* no File Manager: the small built-in picker */
    pick_pid = pid;
}
static void pick_poll(void) {
    if (pick_pid <= 0) return;
    int stt;
    if (cos_waitpid(pick_pid, &stt, false) != pick_pid) return;           /* still open */
    pick_pid = 0;
    static char buf[32768];
    ssize_t n = cos_read_file(PICK_OUT, buf, sizeof buf - 1);
    cos_unlink(PICK_OUT);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        char *e = strchr(p, '\n'); if (e) *e = 0;
        if (*p) stage_add(p);
        if (!e) break;
        p = e + 1;
    }
}
static void sq_pump(void) {
    if (!sq.n && !sq.has_text) return;
    if (sq.ci < 0 || sq.ci >= nct) { sq.n = 0; sq.has_text = false; return; }
    if (up.active) return;
    if (sq.n) { att_send_file(&ct[sq.ci], sq.path[sq.head]); sq.head++; sq.n--; if (!sq.n) sq.head = 0; dirty = true; return; }
    send_text(&ct[sq.ci], sq.text); sq.has_text = false; dirty = true;
}

#define STG_CW 196
static int stg_y(void) { return win_h - INPUT_H - ((edit_msg >= 0 || reply_hdr[0]) ? 24 : 0) - STG_H; }
static void stg_chip(int i, int *x, int *y, int *w, int *h) { *x = LIST_W + 1 + 14 + i * (STG_CW + 8); *y = stg_y() + 6; *w = STG_CW; *h = STG_H - 12; }
static void stg_draw(void) {
    if (!nstg || cur < 0) return;
    int cx = LIST_W + 1, y = stg_y();
    cui_fill(&c, cx, y, win_w - cx, STG_H, PANEL); cui_fill(&c, cx, y, win_w - cx, 1, LINE);
    for (int i = 0; i < nstg; i++) {
        int x, yy, w, h; stg_chip(i, &x, &yy, &w, &h);
        if (x + w > win_w - 10) break;
        cui_round_rect(&c, x, yy, w, h, 8, 0x2B3550);
        int kd = kind_of(stg[i].name);
        uint32_t icon = kd == AK_IMAGE ? 0x4C9AFF : kd == AK_AUDIO ? 0xE5845A : kd == AK_TEXT ? 0x58D678 : 0xA0A8BC;
        cui_round_rect(&c, x + 5, yy + 4, h - 8, h - 8, 5, icon);
        cui_text_center(&c, f_s, x + 5, yy + 4, h - 8, h - 8, kd == AK_IMAGE ? "IMG" : kd == AK_AUDIO ? "SND" : kd == AK_TEXT ? "TXT" : "F", 0xFFFFFF);
        char sz[24]; fmt_size(sz, sizeof sz, stg[i].size);
        cui_text_fit(&c, f_s, x + h, yy + 2, w - h - 26, stg[i].name, TEXT);
        cui_text(&c, f_s, x + h, yy + h / 2 + 1, sz, MUTED);
        cui_text_center(&c, f_t, x + w - 22, yy, 20, h, "x", MUTED);
    }
}
static bool stg_click(int mx, int my) {
    if (!nstg || cur < 0 || mx <= LIST_W || my < stg_y() || my >= stg_y() + STG_H) return false;
    for (int i = 0; i < nstg; i++) {
        int x, y, w, h; stg_chip(i, &x, &y, &w, &h);
        if (mx >= x + w - 24 && mx < x + w && my >= y && my < y + h) { stage_remove(i); return true; }
    }
    return true;                                                           /* the strip swallows clicks that would hit the chat under it */
}

/* ---------------------------------------------------------------- bubbles in the chat */
#define CARD_W 280
#define CARD_H 62
/* Size of the bubble for an attachment message; false when the message is plain text. */
static bool att_measure(msg_t *m, int *bw, int *bh) {
    att_t a; bool is; att_inspect(m, &a, &is);
    if (!is) return false;
    if (a.kind == AK_IMAGE && m->att == AS_READY && m->tw >= 0 && (m->tw > 0 || att_thumb(m, &a))) { *bw = m->tw + 12; *bh = m->th + 12; }
    else { *bw = CARD_W; *bh = CARD_H; }
    return true;
}
/* Draws the content of an attachment bubble at (bx,top) and registers the clickable areas. */
static void att_draw(msg_t *m, int idx, int bx, int top, int bw, int bh) {
    att_t a; bool is; att_inspect(m, &a, &is);
    if (!is) return;
    uint32_t bg = m->mine ? MINE : THEIRS;
    cui_round_rect(&c, bx, top, bw, bh, 12, bg);
    if (a.kind == AK_IMAGE && m->att == AS_READY && m->tw > 0) {
        if (m->thumb || att_thumb(m, &a)) cui_blit(&c, bx + 6, top + 6, m->thumb, m->tw, m->th, m->tw);
        else { cui_fill(&c, bx + 6, top + 6, m->tw, m->th, 0x1A2036); dirty = true; }
        add_hit(idx, bx, top, bw, bh, 3);
        return;
    }
    char sz[24]; fmt_size(sz, sizeof sz, a.size);
    uint32_t icon = a.kind == AK_IMAGE ? 0x4C9AFF : a.kind == AK_AUDIO ? 0xE5845A : a.kind == AK_TEXT ? 0x58D678 : 0xA0A8BC;
    cui_text_fit(&c, f_t, bx + 62, top + 8, bw - 74, a.name, TEXT);
    if (a.kind == AK_AUDIO && m->att == AS_READY) {
        bool mine_playing = aud.d && !strcmp(aud.fid, a.fid);
        bool playing = mine_playing && aud.playing;
        cui_circle(&c, (float)(bx + 30), (float)(top + bh / 2), 20.0f, icon);
        if (playing) { cui_fill(&c, bx + 23, top + bh / 2 - 8, 5, 16, 0xFFFFFF); cui_fill(&c, bx + 32, top + bh / 2 - 8, 5, 16, 0xFFFFFF); }
        else cui_triangle(&c, (float)(bx + 24), (float)(top + bh / 2 - 9), (float)(bx + 24), (float)(top + bh / 2 + 9), (float)(bx + 38), (float)(top + bh / 2), 0xFFFFFF);
        add_hit(idx, bx + 8, top + bh / 2 - 22, 44, 44, 3);
        double pos = mine_playing ? aud_pos() : 0, dur = mine_playing ? aud.dur : 0;
        int px = bx + 62, pw = bw - 74;
        cui_fill(&c, px, top + 38, pw, 4, 0x1A2036);
        if (dur > 0) cui_fill(&c, px, top + 38, (int)(pw * (pos / dur > 1 ? 1 : pos / dur)), 4, icon);
        char tt[48]; snprintf(tt, sizeof tt, "%d:%02d%s%s", (int)pos / 60, (int)pos % 60, dur > 0 ? " / " : "", "");
        if (dur > 0) snprintf(tt, sizeof tt, "%d:%02d / %d:%02d   %s", (int)pos / 60, (int)pos % 60, (int)dur / 60, (int)dur % 60, sz); else snprintf(tt, sizeof tt, "%s", sz);
        cui_text(&c, f_s, px, top + 44, tt, MUTED);
        return;
    }
    cui_round_rect(&c, bx + 10, top + 12, 38, 38, 8, icon);
    cui_text_center(&c, f_s, bx + 10, top + 12, 38, 38, a.kind == AK_IMAGE ? "IMG" : a.kind == AK_AUDIO ? "SND" : a.kind == AK_TEXT ? "TXT" : "FILE", 0xFFFFFF);
    const char *label = NULL; uint32_t lc = 0x2F5C9E;
    if (m->att == AS_REMOTE) label = T("Download");
    else if (m->att == AS_READY) label = (a.kind == AK_IMAGE || a.kind == AK_TEXT) ? T("Open") : T("Save");
    if (m->att == AS_LOADING || m->att == AS_UPLOADING) {
        int pct = 0;
        if (m->att == AS_UPLOADING) pct = m->apct;
        else { dl_t *d = dl_find(a.fid); if (d && d->size) pct = (int)((uint64_t)d->got * 100 / d->size); }
        cui_fill(&c, bx + 62, top + 40, bw - 74, 5, 0x1A2036);
        cui_fill(&c, bx + 62, top + 40, (bw - 74) * pct / 100, 5, ACCENT);
        char tt[40]; snprintf(tt, sizeof tt, "%s  %d%%  %s", m->att == AS_UPLOADING ? T("Uploading") : T("Downloading"), pct, sz);
        cui_text(&c, f_s, bx + 62, top + 24, tt, MUTED);
    } else if (m->att == AS_FAILED) {
        cui_text(&c, f_s, bx + 62, top + 28, T("Transfer failed"), BAD);
    } else if (label) {
        cui_text(&c, f_s, bx + 62, top + 28, sz, MUTED);
        int lw = 76;
        cui_round_rect(&c, bx + bw - lw - 10, top + 28, lw, 24, 8, lc);
        cui_text_center(&c, f_s, bx + bw - lw - 10, top + 28, lw, 24, label, 0xFFFFFF);
        add_hit(idx, bx + bw - lw - 10, top + 28, lw, 24, 3);
        if (m->att == AS_READY && (a.kind == AK_IMAGE || a.kind == AK_TEXT)) {      /* a second button: save */
            cui_round_rect(&c, bx + bw - lw - 10 - 66, top + 28, 58, 24, 8, 0x3A4A70);
            cui_text_center(&c, f_s, bx + bw - lw - 10 - 66, top + 28, 58, 24, T("Save"), 0xFFFFFF);
            add_hit(idx, bx + bw - lw - 10 - 66, top + 28, 58, 24, 4);
        }
    }
}
/* click on an attachment action; kind 3 = primary, 4 = save */
static void att_action(contact_t *k, msg_t *m, int kind) {
    att_t a; bool is; att_inspect(m, &a, &is);
    if (!is) return;
    if (kind == 4) { att_save(&a); return; }
    if (m->att == AS_REMOTE) { if (!att_download(&a)) set_status(T("Cannot download right now")); return; }
    if (m->att != AS_READY) return;
    (void)k;
    if (a.kind == AK_AUDIO) aud_toggle(&a);
    else if (a.kind == AK_IMAGE || a.kind == AK_TEXT) view_open(&a);
    else att_save(&a);
}
/* the text shown in the contact list / notifications */
static const char *att_preview(const msg_t *m, char *buf, size_t cap) {
    att_t a;
    if (!att_parse(m->text, &a)) return msg_body(m->text);
    snprintf(buf, cap, "[%s] %s", a.kind == AK_IMAGE ? T("Image") : a.kind == AK_AUDIO ? T("Audio") : a.kind == AK_TEXT ? T("Text") : T("File"), a.name);
    return buf;
}

/* the app lock closes: drop a running transfer, its plain copy and file key, downloads in progress and the audio buffer */
static void up_wipe(void) {
    if (up.data) { memset(up.data, 0, up.size); free(up.data); }
    if (up.plain) { memset(up.plain, 0, up.plain_n); free(up.plain); }
    memset(&up, 0, sizeof up);
    for (int i = 0; i < 3; i++) { if (dls[i].buf) { memset(dls[i].buf, 0, dls[i].size); free(dls[i].buf); } memset(&dls[i], 0, sizeof dls[i]); }
    aud_stop();
}
