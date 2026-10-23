/* cosmsgd core: protocol and state. See core.h. The behaviour follows tools/cosmsg_server.py command by command. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "bearssl.h"
#include "core.h"
#include "../messenger/vault_crypto.inc"          /* scrypt, shared with the app lock */

#define MAX_TEXT 600            /* characters */
#define MAX_BACKLOG 2000        /* undelivered events one user can have waiting */
#define MAX_BACKLOG_PAIR 400    /* ... from one single sender */
#define MAX_GKEY_WAITING 64
#define KEYPUT_PER_HOUR 12
#define MAX_E2E 3200            /* characters of one end-to-end envelope (never cut) */
#define E2E_ANN 3300            /* an encrypted announcement fits */
#define E2E_B64LEN 263          /* public key bundle: 197 bytes in base64url */
#define MAX_NAME 23
#define MAX_BIO 120
#define MAX_LINE 16384
#define MAX_CONNS 48
#define MAX_USERS 4000
#define OUT_LIMIT (1u << 20)    /* a client that does not read 1 MB of output is dropped */
#define IDLE_MS 120000
#define REPORT_LIMIT 5
#define MSG_TOTAL_MAX 12000     /* undelivered events on the whole server (a group message is one event per member) */
#define SENDER_TOTAL_MAX 1200   /* undelivered events one sender may have waiting over all recipients */
#define BACKLOG_BYTES_MAX (320u * 1024)   /* text bytes waiting for one user: a login replay always fits OUT_LIMIT */
#define BACKLOG_TTL (14u * 86400)
#define HASH_PER_CONN 30        /* password hashes (scrypt, 16 MiB and tens of ms each) one connection may cause per minute ... */
#define HASH_PER_MIN 120        /* ... and the whole server */
#define LOGIN_MS 20000          /* a connection that has not logged in or registered after this long is hung up */
#define REG_PER_IP 20           /* registrations per hour from one address (loopback is exempt) */
#define DL_MEM_MAX (64u * 1024 * 1024)   /* file data held by downloads in progress, over all connections */
static const char INVITE_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

/* ------------------------------------------------------------------ state */
typedef struct { char id[12], tok[72]; char name[96], handle[24], bio[512], avatar[20], invite[12], e2ekey[E2E_B64LEN + 1]; uint8_t kp_cnt; uint32_t kp_win; uint8_t disc, hide, restricted; uint32_t created, last_seen; } user_t;
typedef struct { int a, b; } pair_t;
typedef struct { int frm, to; char intro[512]; } req_t;
typedef struct { uint32_t seq, ts; int frm, to; uint8_t kind; char msgid[28]; char *text; char gid[12]; uint32_t tlen; } msg_t;     /* gid: a group message (to = the member) */
typedef struct { int reader, writer; uint32_t upto; char gid[12]; } read_t;       /* gid empty: a 1:1 chat */
typedef struct { char gid[12], name[100], pw[100]; int owner; uint32_t created; int gfails; uint32_t gwin, glocked; char ann[E2E_ANN + 8]; int ann_by; uint32_t ann_ts; } group_t;
typedef struct { char gid[12]; int uid; uint32_t joined; uint8_t role; } gmem_t;
typedef struct { uint32_t id, ts; char gid[12]; int author; char *text; } note_t;
static group_t *groups; static int ngroups, capgroups;
static gmem_t *gmems; static int ngmems, capgmems;
static note_t *notes; static int nnotes, capnotes; static uint32_t next_note = 1;
typedef struct { int reporter, target; uint32_t ts; char reason[256]; char *evidence; } report_t;

static user_t *users; static int nusers;
static pair_t *friends; static int nfriends, capfriends;
static pair_t *blocks; static int nblocks, capblocks;
static req_t *reqs; static int nreqs, capreqs;
static msg_t *msgs; static int nmsgs, capmsgs; static uint32_t next_seq = 1;
static read_t *reads; static int nreads, capreads;
static report_t *reports; static int nreports, capreports;
typedef struct { char fid[20]; int owner; char name[88]; uint32_t size, created; uint8_t done, pub; } file_t;
typedef struct { int file, uid; } acl_t;
static file_t *files; static int nfiles, capfiles;
static acl_t *acls; static int nacls, capacls;
static size_t upload_mem;                                   /* bytes held by uploads in progress, over all connections */
static size_t dl_mem;                                       /* bytes held by downloads in progress, over all connections */
#define MAX_FILE (10u * 1024 * 1024)
#define QUOTA (300u * 1024 * 1024)
#define MAX_AVATAR (256u * 1024)
#define CHUNK 6144
#define FILE_TTL (14u * 86400)
#define UPLOAD_MEM_MAX (48u * 1024 * 1024)
static bool dirty; static uint64_t dirty_since;
typedef struct { char sub[64]; int uid; char email[96]; uint32_t ts; } gacc_t;
static gacc_t *gaccs; static int ngaccs, capgaccs;
static uint32_t conn_gen;

struct core_conn {
    bool used; int uid;                       /* user index, -1 before login */
    char *in; size_t in_len; bool in_skip;    /* line assembly; in_skip: the current line is too long and is being thrown away */
    char *out; size_t out_len, out_cap, out_pos;
    uint64_t last_rx, window; int count;
    bool close_after;                         /* hang up when the output is drained */
    uint64_t opened, hwin; int hcount; uint32_t ip;     /* connect time, window of password hashes made for this connection, peer address (0: unknown) */
    uint32_t gen;                              /* identifies this connection to outstanding Google jobs */
    struct { bool active; int mode; char name[96], dev[256]; int interval; uint64_t exp, next; int job; char idtok[2048]; bool jwks_retry; } gf;     /* the Google sign-in in progress (mode 1 new device, 2 link) */
    struct { char fid[20]; unsigned char *data; size_t got, size; int next; } up[2];          /* uploads in progress (kept in memory until FEND) */
    struct { unsigned char *data; size_t len, off; int idx; char fid[20]; } dl[4];        /* downloads being streamed */
};
static core_conn_t conns[MAX_CONNS];

static bool loading, save_failed;           /* loading: the state file is being read; save_failed: this save ran out of memory and must not replace the file */
static void oom_stop(void);          /* saves the state and exits: never write past a table (the watchdog of the host starts the server again) */
#define GROW(arr, n, cap) do { if ((n) >= (cap)) { int nc_ = (cap) ? (cap) * 2 : 64; void *p_ = realloc((arr), sizeof *(arr) * (size_t)nc_); if (!p_) { oom_stop(); } (arr) = p_; (cap) = nc_; } } while (0)
static int cap_users_;

/* ------------------------------------------------------------------ small helpers */
static void mark_dirty(void) { if (!dirty) { dirty = true; dirty_since = plat_ms(); } }

static void hexs(char *out, const unsigned char *b, size_t n) { static const char *h = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; } out[2 * n] = 0; }
static void tok_hash(const char *token, char *out /* >= 67 */) {
    br_sha256_context sc; unsigned char d[32]; br_sha256_init(&sc); br_sha256_update(&sc, token, strlen(token)); br_sha256_out(&sc, d);
    out[0] = 'h'; out[1] = ':'; hexs(out + 2, d, 32);
}
static bool ct_eq(const char *a, const char *b) { size_t la = strlen(a), lb = strlen(b); unsigned r = (unsigned)(la ^ lb); for (size_t i = 0; i < la && i < lb; i++) r |= (unsigned char)(a[i] ^ b[i]); return r == 0; }
static uint32_t rnd_below(uint32_t n) { uint32_t v; do { plat_random(&v, 4); } while (v >= (0xFFFFFFFFu / n) * n); return v % n; }

/* Python's str.strip() for the whitespace that matters here (ASCII and U+3000), in place */
static char *strip(char *s) {
    for (;;) { if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++; else if ((unsigned char)s[0] == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80) s += 3; else break; }
    size_t n = strlen(s);
    for (;;) {
        if (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
        else if (n >= 3 && (unsigned char)s[n - 3] == 0xE3 && (unsigned char)s[n - 2] == 0x80 && (unsigned char)s[n - 1] == 0x80) n -= 3;
        else break;
    }
    s[n] = 0; return s;
}
/* keep at most maxc characters (UTF-8) */
static void cut_chars(char *s, int maxc) {
    int c = 0; for (char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) { if (c == maxc) { *p = 0; return; } c++; }
}
static void lower_ascii(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }
static void copyz(char *d, size_t cap, const char *s) { size_t n = strlen(s); if (n >= cap) { n = cap - 1; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; } memcpy(d, s, n); d[n] = 0; }
static bool valid_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) p++;
        else if (*p >= 0xC2 && *p <= 0xDF && (p[1] & 0xC0) == 0x80) p += 2;
        else if (*p >= 0xE0 && *p <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) p += 3;
        else if (*p >= 0xF0 && *p <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) p += 4;
        else return false;
    }
    return true;
}

static int find_user(const char *id) { for (int i = 0; i < nusers; i++) if (strcmp(users[i].id, id) == 0) return i; return -1; }
static bool pair_has(const pair_t *a, int n, int x, int y) { for (int i = 0; i < n; i++) if (a[i].a == x && a[i].b == y) return true; return false; }
static bool are_friends(int a, int b) { return pair_has(friends, nfriends, a, b); }
static bool is_blocked(int by, int who) { return pair_has(blocks, nblocks, by, who); }
static void pair_del(pair_t *a, int *n, int x, int y) { for (int i = 0; i < *n; i++) if (a[i].a == x && a[i].b == y) { a[i] = a[--*n]; return; } }
static void friend_add(int a, int b) { if (!are_friends(a, b)) { GROW(friends, nfriends, capfriends); friends[nfriends++] = (pair_t){ a, b }; } }
static void friend_del(int a, int b) { pair_del(friends, &nfriends, a, b); pair_del(friends, &nfriends, b, a); }
static int find_req(int frm, int to) { for (int i = 0; i < nreqs; i++) if (reqs[i].frm == frm && reqs[i].to == to) return i; return -1; }
static void req_del(int frm, int to) { int i = find_req(frm, to); if (i >= 0) reqs[i] = reqs[--nreqs]; }

/* ------------------------------------------------------------------ base64 and files */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const unsigned char *in, size_t n, char *out) {
    size_t o = 0; size_t i = 0;
    for (; i + 2 < n; i += 3) { unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2]; out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63]; out[o++] = B64[(v >> 6) & 63]; out[o++] = B64[v & 63]; }
    if (n - i == 1) { unsigned v = (unsigned)in[i] << 16; out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63]; out[o++] = '='; out[o++] = '='; }
    else if (n - i == 2) { unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8; out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63]; out[o++] = B64[(v >> 6) & 63]; out[o++] = '='; }
    out[o] = 0; return o;
}
/* strict decoding (like Python's validate=True): returns the byte count or -1 */
static long b64_decode(const char *in, unsigned char *out, size_t cap) {
    size_t L = strlen(in); if (L % 4) return -1;
    size_t o = 0;
    for (size_t i = 0; i < L; i += 4) {
        int v[4]; int pad = 0;
        for (int k = 0; k < 4; k++) {
            char ch = in[i + k]; const char *p;
            if (ch == '=') { if (i + 4 != L || k < 2) return -1; v[k] = 0; pad++; }
            else { if (pad) return -1; p = strchr(B64, ch); if (!p || !ch) return -1; v[k] = (int)(p - B64); }
        }
        unsigned x = (unsigned)v[0] << 18 | (unsigned)v[1] << 12 | (unsigned)v[2] << 6 | (unsigned)v[3];
        if (o + 3 - (size_t)pad > cap) return -1;
        out[o++] = (unsigned char)(x >> 16); if (pad < 2) out[o++] = (unsigned char)(x >> 8); if (pad < 1) out[o++] = (unsigned char)x;
    }
    return (long)o;
}
static int find_file(const char *fid) { for (int i = 0; i < nfiles; i++) if (!strcmp(files[i].fid, fid)) return i; return -1; }
static bool acl_has(int fi, int uid) { for (int i = 0; i < nacls; i++) if (acls[i].file == fi && acls[i].uid == uid) return true; return false; }
static size_t used_bytes(int uid) { size_t t = 0; for (int i = 0; i < nfiles; i++) if (files[i].owner == uid) t += files[i].size; return t; }
static void file_remove_at(int fi) {
    plat_blob_delete(files[fi].fid);
    for (int i = 0; i < nacls;) { if (acls[i].file == fi) acls[i] = acls[--nacls]; else i++; }
    int last = nfiles - 1; if (fi != last) { files[fi] = files[last]; for (int i = 0; i < nacls; i++) if (acls[i].file == last) acls[i].file = fi; }
    nfiles--;
}

static int up_find(core_conn_t *c, const char *fid) { for (int i = 0; i < 2; i++) if (c->up[i].data && !strcmp(c->up[i].fid, fid)) return i; return -1; }
static void up_drop(core_conn_t *c, int slot, bool remove_file) {            /* ends an upload; the half-made file entry goes too unless it was finished */
    int fi = find_file(c->up[slot].fid);
    free(c->up[slot].data); upload_mem -= c->up[slot].size; memset(&c->up[slot], 0, sizeof c->up[slot]);
    if (remove_file && fi >= 0) file_remove_at(fi);
}

/* ------------------------------------------------------------------ output */
static void out_raw(core_conn_t *c, const char *s, size_t n) {
    if (!c->used || c->close_after) return;
    if (c->out_len - c->out_pos + n > OUT_LIMIT) { c->close_after = true; return; }
    if (c->out_pos && c->out_pos == c->out_len) c->out_pos = c->out_len = 0;
    if (c->out_pos && c->out_len + n > c->out_cap) { memmove(c->out, c->out + c->out_pos, c->out_len - c->out_pos); c->out_len -= c->out_pos; c->out_pos = 0; }   /* reuse the sent part before growing */
    if (c->out_len + n > c->out_cap) {
        size_t nc = c->out_cap ? c->out_cap : 4096; while (nc < c->out_len + n) nc *= 2;
        char *p = realloc(c->out, nc); if (!p) { c->close_after = true; return; }
        c->out = p; c->out_cap = nc;
    }
    memcpy(c->out + c->out_len, s, n); c->out_len += n;
}
static void out_frame_v(core_conn_t *c, const char *first, va_list ap) {
    char buf[4096], *b = buf; size_t cap = sizeof buf, o = 0; const char *f = first; bool firstf = true;
    for (; f; f = va_arg(ap, const char *)) {
        size_t need = o + 2 * strlen(f) + 2;
        if (need > cap) { size_t nc = need * 2; char *nb = malloc(nc); if (!nb) break; memcpy(nb, b, o); if (b != buf) free(b); b = nb; cap = nc; }
        if (!firstf) b[o++] = '\t'; firstf = false;
        for (const char *p = f; *p; p++) {
            if (*p == '\\') { b[o++] = '\\'; b[o++] = '\\'; } else if (*p == '\t') { b[o++] = '\\'; b[o++] = 't'; } else if (*p == '\n') { b[o++] = '\\'; b[o++] = 'n'; } else b[o++] = *p;
        }
    }
    if (o + 1 > cap) { char *nb = realloc(b == buf ? NULL : b, o + 1); if (nb) { if (b == buf) memcpy(nb, buf, o); b = nb; } }
    b[o++] = '\n'; out_raw(c, b, o); if (b != buf) free(b);
}
/* emit(conn, "FIELD", "field", ..., NULL) */
static void emit(core_conn_t *c, const char *first, ...) { va_list ap; va_start(ap, first); out_frame_v(c, first, ap); va_end(ap); }
static void push(int uid, const char *first, ...) {
    for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used && conns[i].uid == uid) { va_list ap; va_start(ap, first); out_frame_v(&conns[i], first, ap); va_end(ap); }
}
static const char *num(char *buf, unsigned long long v) { snprintf(buf, 24, "%llu", v); return buf; }

/* ------------------------------------------------------------------ persistence */
static void esc_to(FILE *unused, const char *s, char **o, size_t *n, size_t *cap) {
    (void)unused;
    size_t need = *n + 2 * strlen(s) + 2;
    if (need > *cap) { size_t nc = need * 2; char *p = realloc(*o, nc); if (!p) { save_failed = true; return; } *o = p; *cap = nc; }
    for (; *s; s++) { if (*s == '\\') { (*o)[(*n)++] = '\\'; (*o)[(*n)++] = '\\'; } else if (*s == '\t') { (*o)[(*n)++] = '\\'; (*o)[(*n)++] = 't'; } else if (*s == '\n') { (*o)[(*n)++] = '\\'; (*o)[(*n)++] = 'n'; } else (*o)[(*n)++] = *s; }
}
static void line_out(char **o, size_t *n, size_t *cap, int nf, ...) {
    va_list ap; va_start(ap, nf);
    for (int i = 0; i < nf; i++) { if (i) { if (*n + 2 > *cap) { char *p = realloc(*o, *cap * 2 + 64); if (!p) { save_failed = true; break; } *o = p; *cap = *cap * 2 + 64; } (*o)[(*n)++] = '\t'; } esc_to(NULL, va_arg(ap, const char *), o, n, cap); }
    va_end(ap);
    if (*n + 2 > *cap) { char *p = realloc(*o, *cap * 2 + 64); if (!p) { save_failed = true; return; } *o = p; *cap = *cap * 2 + 64; }
    (*o)[(*n)++] = '\n';
}
static void save_state(void) {
    size_t n = 0, cap = 1 << 16; char *o = malloc(cap); if (!o) { plat_log("[COSMSGD] no memory to save the state - keeping the old file\n"); return; }
    save_failed = false;
    char a[24], b[24], c2[24], d[24], e[24], f[24], g[24];
    line_out(&o, &n, &cap, 2, "V", "1");
    line_out(&o, &n, &cap, 2, "S", num(a, next_seq));
    for (int i = 0; i < nusers; i++) { user_t *u = &users[i];
        line_out(&o, &n, &cap, 13, "U", u->id, u->tok, u->name, u->handle, u->bio, num(a, u->disc), num(b, u->hide), num(c2, u->restricted), u->avatar, u->invite, num(d, u->created), num(e, u->last_seen)); }
    for (int i = 0; i < nusers; i++) if (users[i].e2ekey[0]) line_out(&o, &n, &cap, 3, "K", users[i].id, users[i].e2ekey);
    for (int i = 0; i < nfriends; i++) line_out(&o, &n, &cap, 3, "F", users[friends[i].a].id, users[friends[i].b].id);
    for (int i = 0; i < nblocks; i++) line_out(&o, &n, &cap, 3, "B", users[blocks[i].a].id, users[blocks[i].b].id);
    for (int i = 0; i < nreqs; i++) line_out(&o, &n, &cap, 4, "R", users[reqs[i].frm].id, users[reqs[i].to].id, reqs[i].intro);
    for (int i = 0; i < nmsgs; i++) line_out(&o, &n, &cap, 9, "M", num(a, msgs[i].seq), users[msgs[i].frm].id, users[msgs[i].to].id, num(b, msgs[i].ts), num(c2, msgs[i].kind), msgs[i].msgid, msgs[i].text, msgs[i].gid);
    for (int i = 0; i < nreads; i++) line_out(&o, &n, &cap, 5, "D", users[reads[i].reader].id, users[reads[i].writer].id, num(a, reads[i].upto), reads[i].gid);
    for (int i = 0; i < ngroups; i++) { group_t *g = &groups[i]; line_out(&o, &n, &cap, 10, "G", g->gid, g->name, users[g->owner].id, g->pw, num(a, g->created), g->ann, g->ann_by >= 0 ? users[g->ann_by].id : "", num(b, g->ann_ts), num(c2, g->glocked)); }
    for (int i = 0; i < ngmems; i++) line_out(&o, &n, &cap, 5, "N", gmems[i].gid, users[gmems[i].uid].id, num(a, gmems[i].joined), num(b, gmems[i].role));
    for (int i = 0; i < nnotes; i++) line_out(&o, &n, &cap, 6, "T", num(a, notes[i].id), notes[i].gid, users[notes[i].author].id, num(b, notes[i].ts), notes[i].text);
    for (int i = 0; i < nfiles; i++) if (files[i].done) line_out(&o, &n, &cap, 7, "L", files[i].fid, users[files[i].owner].id, files[i].name, num(a, files[i].size), num(b, files[i].created), num(c2, files[i].pub));
    for (int i = 0; i < nacls; i++) if (files[acls[i].file].done) line_out(&o, &n, &cap, 3, "A", files[acls[i].file].fid, users[acls[i].uid].id);
    for (int i = 0; i < ngaccs; i++) line_out(&o, &n, &cap, 5, "Y", gaccs[i].sub, users[gaccs[i].uid].id, gaccs[i].email, num(a, gaccs[i].ts));
    for (int i = 0; i < nreports; i++) line_out(&o, &n, &cap, 6, "P", users[reports[i].reporter].id, users[reports[i].target].id, num(a, reports[i].ts), reports[i].reason, reports[i].evidence ? reports[i].evidence : "");
    (void)f; (void)g;
    if (save_failed) plat_log("[COSMSGD] out of memory while saving - the old state file is kept\n");
    else if (plat_save(o, n)) dirty = false; else plat_log("[COSMSGD] cannot save the state\n");
    free(o);
}
static void oom_stop(void) {
    if (loading) { plat_log("[COSMSGD] out of memory while reading the state - stopping without touching the state file\n"); exit(1); }   /* saving now would replace the file with the part read so far */
    plat_log("[COSMSGD] out of memory - saving the state and stopping instead of writing past a table\n"); save_state(); exit(1);
}
static char *tab_split(char *l, char **f, int max) {      /* splits a line on TAB and unescapes the fields in place; returns the number of fields via f[max] hack */
    int n = 0; char *p = l; f[n++] = p;
    for (; *p; p++) if (*p == '\t' && n < max) { *p = 0; f[n++] = p + 1; }
    for (int i = 0; i < n; i++) { char *r = f[i], *w = f[i]; while (*r) { if (*r == '\\' && r[1]) { r++; *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r; r++; } else *w++ = *r++; } *w = 0; }
    f[n] = NULL; return NULL;
}
static void load_state(void) {
    size_t len; char *buf = plat_load(&len); if (!buf) return;
    loading = true;
    char *l = buf; int ok = 0;
    while (l < buf + len) {
        char *e = memchr(l, '\n', (size_t)(buf + len - l)); if (!e) break; *e = 0;
        char *f[16]; tab_split(l, f, 15); int n = 0; while (f[n]) n++;
        if (f[0][0] == 'V') ok = 1;
        else if (f[0][0] == 'S' && n >= 2) next_seq = (uint32_t)strtoul(f[1], NULL, 10);
        else if (f[0][0] == 'U' && n >= 13 && nusers < MAX_USERS) {
            GROW(users, nusers, cap_users_); user_t *u = &users[nusers++]; memset(u, 0, sizeof *u);
            copyz(u->id, sizeof u->id, f[1]); copyz(u->tok, sizeof u->tok, f[2]); copyz(u->name, sizeof u->name, f[3]); copyz(u->handle, sizeof u->handle, f[4]); copyz(u->bio, sizeof u->bio, f[5]);
            u->disc = (uint8_t)atoi(f[6]); u->hide = (uint8_t)atoi(f[7]); u->restricted = (uint8_t)atoi(f[8]); copyz(u->avatar, sizeof u->avatar, f[9]); copyz(u->invite, sizeof u->invite, f[10]);
            u->created = (uint32_t)strtoul(f[11], NULL, 10); u->last_seen = (uint32_t)strtoul(f[12], NULL, 10);
        } else if ((f[0][0] == 'F' || f[0][0] == 'B') && n >= 3) {
            int a = find_user(f[1]), b = find_user(f[2]); if (a < 0 || b < 0) { l = e + 1; continue; }
            if (f[0][0] == 'F') { GROW(friends, nfriends, capfriends); friends[nfriends++] = (pair_t){ a, b }; } else { GROW(blocks, nblocks, capblocks); blocks[nblocks++] = (pair_t){ a, b }; }
        } else if (f[0][0] == 'K' && n >= 3) {
            int a = find_user(f[1]); if (a >= 0) copyz(users[a].e2ekey, sizeof users[a].e2ekey, f[2]);
        } else if (f[0][0] == 'R' && n >= 4) {
            int a = find_user(f[1]), b = find_user(f[2]); if (a >= 0 && b >= 0) { GROW(reqs, nreqs, capreqs); req_t *r = &reqs[nreqs++]; r->frm = a; r->to = b; copyz(r->intro, sizeof r->intro, f[3]); }
        } else if (f[0][0] == 'M' && n >= 8) {
            int a = find_user(f[2]), b = find_user(f[3]); if (a >= 0 && b >= 0) { GROW(msgs, nmsgs, capmsgs); msg_t *m = &msgs[nmsgs++]; m->seq = (uint32_t)strtoul(f[1], NULL, 10); m->frm = a; m->to = b; m->ts = (uint32_t)strtoul(f[4], NULL, 10); m->kind = (uint8_t)atoi(f[5]); copyz(m->msgid, sizeof m->msgid, f[6]); m->text = strdup(f[7]); if (!m->text) oom_stop(); m->tlen = (uint32_t)strlen(m->text); m->gid[0] = 0; if (n >= 9) copyz(m->gid, sizeof m->gid, f[8]); }
        } else if (f[0][0] == 'D' && n >= 4) {
            int a = find_user(f[1]), b = find_user(f[2]); if (a >= 0 && b >= 0) { GROW(reads, nreads, capreads); read_t *r = &reads[nreads++]; memset(r, 0, sizeof *r); r->reader = a; r->writer = b; r->upto = (uint32_t)strtoul(f[3], NULL, 10); if (n >= 5) copyz(r->gid, sizeof r->gid, f[4]); }
        } else if (f[0][0] == 'G' && n >= 10) {
            int ow = find_user(f[3]); if (ow >= 0) { GROW(groups, ngroups, capgroups); group_t *g = &groups[ngroups++]; memset(g, 0, sizeof *g); copyz(g->gid, sizeof g->gid, f[1]); copyz(g->name, sizeof g->name, f[2]); g->owner = ow; copyz(g->pw, sizeof g->pw, f[4]); g->created = (uint32_t)strtoul(f[5], NULL, 10); copyz(g->ann, sizeof g->ann, f[6]); g->ann_by = f[7][0] ? find_user(f[7]) : -1; g->ann_ts = (uint32_t)strtoul(f[8], NULL, 10); g->glocked = (uint32_t)strtoul(f[9], NULL, 10); }
        } else if (f[0][0] == 'N' && n >= 5) {
            int u = find_user(f[2]); if (u >= 0) { GROW(gmems, ngmems, capgmems); gmem_t *m = &gmems[ngmems++]; memset(m, 0, sizeof *m); copyz(m->gid, sizeof m->gid, f[1]); m->uid = u; m->joined = (uint32_t)strtoul(f[3], NULL, 10); m->role = (uint8_t)atoi(f[4]); }
        } else if (f[0][0] == 'T' && n >= 6) {
            int u = find_user(f[3]); if (u >= 0) { GROW(notes, nnotes, capnotes); note_t *t = &notes[nnotes++]; t->id = (uint32_t)strtoul(f[1], NULL, 10); copyz(t->gid, sizeof t->gid, f[2]); t->author = u; t->ts = (uint32_t)strtoul(f[4], NULL, 10); t->text = strdup(f[5]); if (t->id >= next_note) next_note = t->id + 1; }
        } else if (f[0][0] == 'Y' && n >= 5) {
            int u = find_user(f[2]); if (u >= 0) { GROW(gaccs, ngaccs, capgaccs); gacc_t *g = &gaccs[ngaccs++]; copyz(g->sub, sizeof g->sub, f[1]); g->uid = u; copyz(g->email, sizeof g->email, f[3]); g->ts = (uint32_t)strtoul(f[4], NULL, 10); }
        } else if (f[0][0] == 'L' && n >= 7) {
            int ow = find_user(f[2]); if (ow >= 0) { GROW(files, nfiles, capfiles); file_t *fl = &files[nfiles++]; memset(fl, 0, sizeof *fl); copyz(fl->fid, sizeof fl->fid, f[1]); fl->owner = ow; copyz(fl->name, sizeof fl->name, f[3]); fl->size = (uint32_t)strtoul(f[4], NULL, 10); fl->created = (uint32_t)strtoul(f[5], NULL, 10); fl->pub = (uint8_t)atoi(f[6]); fl->done = 1; }
        } else if (f[0][0] == 'A' && n >= 3) {
            int fi = find_file(f[1]), u = find_user(f[2]); if (fi >= 0 && u >= 0) { GROW(acls, nacls, capacls); acls[nacls++] = (acl_t){ fi, u }; }
        } else if (f[0][0] == 'P' && n >= 6) {
            int a = find_user(f[1]), b = find_user(f[2]); if (a >= 0 && b >= 0) { GROW(reports, nreports, capreports); report_t *r = &reports[nreports++]; r->reporter = a; r->target = b; r->ts = (uint32_t)strtoul(f[3], NULL, 10); copyz(r->reason, sizeof r->reason, f[4]); r->evidence = strdup(f[5]); }
        }
        l = e + 1;
    }
    free(buf); loading = false;
    char lb[80]; snprintf(lb, sizeof lb, "[COSMSGD] state %s: %d users, %d friendships, %d queued events\n", ok ? "loaded" : "empty", nusers, nfriends / 2, nmsgs); plat_log(lb);
}

/* ------------------------------------------------------------------ presence and login */
static bool is_online(int uid) { for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used && conns[i].uid == uid) return true; return false; }
static void pres_state(int uid, char *st, char *ts) {
    user_t *u = &users[uid];
    if (u->hide) { strcpy(st, "2"); strcpy(ts, "0"); return; }
    if (is_online(uid)) { strcpy(st, "1"); num(ts, plat_unix()); return; }
    strcpy(st, "0"); num(ts, u->last_seen);
}
static void broadcast_presence(int uid) {
    char st[24], ts[24]; pres_state(uid, st, ts);
    for (int i = 0; i < nfriends; i++) if (friends[i].a == uid) push(friends[i].b, "PRES", users[uid].id, st, ts, NULL);
}
static void friend_frame(core_conn_t *c, int of) { user_t *u = &users[of]; emit(c, "FRIEND", u->id, u->name, u->handle, u->avatar, NULL); }
static void push_friend_info(int uid) {
    for (int i = 0; i < nfriends; i++) if (friends[i].a == uid) push(friends[i].b, "FRIEND", users[uid].id, users[uid].name, users[uid].handle, users[uid].avatar, NULL);
}
static void event_frame(core_conn_t *c, const msg_t *m) {
    char sq[24], ts[24]; num(sq, m->seq); num(ts, m->ts); const char *from = users[m->frm].id;
    if (m->gid[0]) {
        if (m->kind == 3) emit(c, "GKEY_IN", sq, m->gid, from, ts, m->msgid, m->text, NULL);
        else if (m->kind == 1) emit(c, "GEDIT_IN", sq, m->gid, from, ts, m->msgid, m->text, NULL);
        else if (m->kind == 2) emit(c, "GDEL_IN", sq, m->gid, from, ts, m->msgid, NULL);
        else emit(c, "GMSG", sq, m->gid, from, ts, m->text, m->msgid, NULL);
        return;
    }
    if (m->kind == 1) emit(c, "EDIT_IN", sq, from, ts, m->msgid, m->text, NULL);
    else if (m->kind == 2) emit(c, "DEL_IN", sq, from, ts, m->msgid, NULL);
    else emit(c, "MSG", sq, from, ts, m->text, m->msgid, NULL);
}
/* ------------------------------------------------------------------ groups */
#define MAX_GNAME 30
#define GROUP_MEMBERS 200
#define GROUPS_PER_USER 30
#define USER_TRIES 5
#define USER_LOCK 600
#define GROUP_TRIES 40
#define GROUP_WINDOW 3600
#define GROUP_LOCK 1800
#define ANY_TRIES 25
#define ANY_LOCK 3600
static int find_group(const char *gid) { for (int i = 0; i < ngroups; i++) if (!strcmp(groups[i].gid, gid)) return i; return -1; }
static int gm_find(const char *gid, int uid) { for (int i = 0; i < ngmems; i++) if (gmems[i].uid == uid && !strcmp(gmems[i].gid, gid)) return i; return -1; }
static int gm_count(const char *gid) { int k = 0; for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) k++; return k; }
static int gm_of_user(int uid) { int k = 0; for (int i = 0; i < ngmems; i++) if (gmems[i].uid == uid) k++; return k; }
/* the member's rank: 2 owner, 1 sub-admin, 0 member, -1 not in the group */
static int role_of(const char *gid, int uid) {
    int g = find_group(gid); if (g >= 0 && groups[g].owner == uid) return 2;
    int m = gm_find(gid, uid); return m >= 0 ? gmems[m].role : -1;
}
static void member_frame(core_conn_t *c, const char *gid, int uid) { char r[8]; snprintf(r, sizeof r, "%d", role_of(gid, uid)); emit(c, "GMEMBER", gid, users[uid].id, users[uid].name, r, NULL); }
static void group_state(core_conn_t *c, const char *gid) {
    int g = find_group(gid); if (g < 0) return;
    emit(c, "GROUP", gid, groups[g].name, users[groups[g].owner].id, NULL);
    for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) member_frame(c, gid, gmems[i].uid);
    if (groups[g].ann[0]) { char ts[24]; emit(c, "GANN", gid, groups[g].ann, groups[g].ann_by >= 0 ? users[groups[g].ann_by].id : "", num(ts, groups[g].ann_ts), NULL); }
}
static void push_group_state(const char *gid, int to /* -1: every member */) {
    for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && (to < 0 || gmems[i].uid == to))
        for (int k = 0; k < MAX_CONNS; k++) if (conns[k].used && conns[k].uid == gmems[i].uid) group_state(&conns[k], gid);
}
static void push_member(const char *gid, int who) {
    char r[8]; snprintf(r, sizeof r, "%d", role_of(gid, who));
    for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) push(gmems[i].uid, "GMEMBER", gid, users[who].id, users[who].name, r, NULL);
}
static bool backlog_full(int frm, int to);
static void queue_group(int frm, const char *gid, const char *msgid, const char *text, int kind) {
    uint32_t ts = (uint32_t)plat_unix();
    for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && gmems[i].uid != frm && !backlog_full(frm, gmems[i].uid)) {
        GROW(msgs, nmsgs, capmsgs); char *t = strdup(text); if (!t) return;
        msg_t *m = &msgs[nmsgs++]; memset(m, 0, sizeof *m); m->seq = next_seq++; m->ts = ts; m->frm = frm; m->to = gmems[i].uid; m->kind = (uint8_t)kind; copyz(m->msgid, sizeof m->msgid, msgid); m->text = t; m->tlen = (uint32_t)strlen(t); copyz(m->gid, sizeof m->gid, gid);
        for (int k = 0; k < MAX_CONNS; k++) if (conns[k].used && conns[k].uid == m->to) event_frame(&conns[k], m);
    }
    mark_dirty();
}
static void drop_member(const char *gid_in, int uid) {
    char gid[12]; copyz(gid, sizeof gid, gid_in);
    int mi = gm_find(gid, uid); if (mi >= 0) { memmove(&gmems[mi], &gmems[mi + 1], sizeof(gmem_t) * (size_t)(ngmems - mi - 1)); ngmems--; }
    for (int i = 0; i < nmsgs;) { if (msgs[i].to == uid && !strcmp(msgs[i].gid, gid)) { free(msgs[i].text); memmove(&msgs[i], &msgs[i + 1], sizeof(msg_t) * (size_t)(nmsgs - i - 1)); nmsgs--; } else i++; }
    int g = find_group(gid); int rest = gm_count(gid); bool was_owner = g >= 0 && groups[g].owner == uid;
    if (g >= 0 && !rest) {
        for (int i = 0; i < nmsgs;) { if (!strcmp(msgs[i].gid, gid)) { free(msgs[i].text); memmove(&msgs[i], &msgs[i + 1], sizeof(msg_t) * (size_t)(nmsgs - i - 1)); nmsgs--; } else i++; }
        for (int i = 0; i < nnotes;) { if (!strcmp(notes[i].gid, gid)) { free(notes[i].text); memmove(&notes[i], &notes[i + 1], sizeof(note_t) * (size_t)(nnotes - i - 1)); nnotes--; } else i++; }
        for (int i = 0; i < nreads;) { if (!strcmp(reads[i].gid, gid)) reads[i] = reads[--nreads]; else i++; }
        memmove(&groups[g], &groups[g + 1], sizeof(group_t) * (size_t)(ngroups - g - 1)); ngroups--;
    } else if (was_owner) {                                    /* the first sub-admin, or else the oldest member, takes over */
        int pick = -1; for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && gmems[i].role == 1) { pick = i; break; }
        if (pick < 0) for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) { pick = i; break; }
        groups[g].owner = gmems[pick].uid; gmems[pick].role = 0;
    }
    mark_dirty();
    push(uid, "GROUP_DEL", gid, NULL);
    for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) push(gmems[i].uid, "GMEMBER_DEL", gid, users[uid].id, NULL);
    if (rest && was_owner) push_group_state(gid, -1);
}

/* 6-digit group passwords: weak ones are refused, stored as "salt:scrypt(N=2^14,r=8,p=1)" like the Python server */
static bool pw_ok_format(const char *pw) { if (strlen(pw) != 6) return false; for (int i = 0; i < 6; i++) if (pw[i] < '0' || pw[i] > '9') return false; return true; }
static bool weak_pw(const char *pw) {
    int d[6]; for (int i = 0; i < 6; i++) d[i] = pw[i] - '0';
    bool same = true, up = true, down = true;
    for (int i = 0; i < 5; i++) { if (d[i + 1] != d[i]) same = false; if (d[i + 1] - d[i] != 1) up = false; if (d[i + 1] - d[i] != -1) down = false; }
    return same || up || down || !strcmp(pw, "123123") || !strcmp(pw, "121212") || !strcmp(pw, "112233") || !strcmp(pw, "012345") || !strcmp(pw, "654321");
}
static void gen_pw(char *out) { for (;;) { snprintf(out, 8, "%06u", rnd_below(1000000)); if (!weak_pw(out)) return; } }
/* scrypt takes 16 MiB and tens of milliseconds and runs on the one thread that serves everybody: a budget per connection and one for the server */
static bool hash_allowed(core_conn_t *c) {
    static uint64_t gwin; static int gcount; uint64_t now = plat_ms();
    if (now - c->hwin > 60000) { c->hwin = now; c->hcount = 0; }
    if (now - gwin > 60000) { gwin = now; gcount = 0; }
    if (c->hcount >= HASH_PER_CONN || gcount >= HASH_PER_MIN) return false;
    c->hcount++; gcount++; return true;
}
static bool hash_pw(const char *pw, char *out /* >= 100 */) {
    unsigned char salt[16], h[32]; plat_random(salt, sizeof salt);
    if (scrypt_kdf((const uint8_t *)pw, strlen(pw), salt, 16, 1u << 14, 8, 1, h, 32) != 0) { out[0] = 0; return false; }   /* (no memory: never store stack bytes as a hash) */
    hexs(out, salt, 16); out[32] = ':'; hexs(out + 33, h, 32); return true;
}
static int hexval(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static bool check_pw(const char *pw, const char *stored) {
    if (strlen(stored) != 32 + 1 + 64 || stored[32] != ':') return false;
    unsigned char salt[16], want[32], got[32];
    for (int i = 0; i < 16; i++) { int a = hexval(stored[2 * i]), b = hexval(stored[2 * i + 1]); if (a < 0 || b < 0) return false; salt[i] = (unsigned char)(a << 4 | b); }
    for (int i = 0; i < 32; i++) { int a = hexval(stored[33 + 2 * i]), b = hexval(stored[34 + 2 * i]); if (a < 0 || b < 0) return false; want[i] = (unsigned char)(a << 4 | b); }
    if (scrypt_kdf((const uint8_t *)pw, strlen(pw), salt, 16, 1u << 14, 8, 1, got, 32) != 0) return false;
    unsigned r = 0; for (int i = 0; i < 32; i++) r |= (unsigned)(got[i] ^ want[i]); return r == 0;
}
/* wrong-guess bookkeeping (memory only): per user and group, and per user over all groups */
static struct { int uid; char gid[12]; int fails; uint64_t win, locked; } tries[256]; static int ntries;
static struct { int uid; int fails; uint64_t win, locked; } any_tries[256]; static int nany;
static uint64_t now_s(void) { return plat_unix(); }
static int tries_slot(int uid, const char *gid) { for (int i = 0; i < ntries; i++) if (tries[i].uid == uid && !strcmp(tries[i].gid, gid)) return i; if (ntries == 256) { memmove(&tries[0], &tries[1], sizeof tries[0] * 255); ntries--; } int i = ntries++; tries[i].uid = uid; copyz(tries[i].gid, sizeof tries[i].gid, gid); tries[i].fails = 0; tries[i].win = now_s(); tries[i].locked = 0; return i; }
static int any_slot(int uid) { for (int i = 0; i < nany; i++) if (any_tries[i].uid == uid) return i; if (nany == 256) { memmove(&any_tries[0], &any_tries[1], sizeof any_tries[0] * 255); nany--; } int i = nany++; any_tries[i].uid = uid; any_tries[i].fails = 0; any_tries[i].win = now_s(); any_tries[i].locked = 0; return i; }
/* NULL when the password is right, otherwise the error text. A wrong id and a wrong password look the same. */
static const char *verify_group_pw(int uid, const char *gid, const char *pw, char *errbuf) {
    uint64_t now = now_s(); int g = find_group(gid), ts = tries_slot(uid, gid), as = any_slot(uid);
    uint64_t left_t = tries[ts].locked > any_tries[as].locked ? tries[ts].locked : any_tries[as].locked; if (g >= 0 && groups[g].glocked > left_t) left_t = groups[g].glocked;
    if (left_t > now) { snprintf(errbuf, 100, "too many wrong passwords - try again in %d minutes", (int)((left_t - now) / 60 + 1)); return errbuf; }
    bool ok = false;
    if (pw_ok_format(pw)) { char dummy[100]; if (g < 0) hash_pw("000000", dummy); ok = g >= 0 && check_pw(pw, groups[g].pw); } else { char dummy[100]; hash_pw("000000", dummy); }
    if (ok) { tries[ts].fails = 0; return NULL; }
    tries[ts].fails++; if (tries[ts].fails >= USER_TRIES) { tries[ts].fails = 0; tries[ts].locked = now + USER_LOCK; }
    if (now - any_tries[as].win > 3600) { any_tries[as].fails = 0; any_tries[as].win = now; }
    any_tries[as].fails++; if (any_tries[as].fails >= ANY_TRIES) { any_tries[as].fails = 0; any_tries[as].locked = now + ANY_LOCK; }
    if (g >= 0) {
        if (now - groups[g].gwin <= GROUP_WINDOW) groups[g].gfails++; else { groups[g].gfails = 1; groups[g].gwin = (uint32_t)now; }
        if (groups[g].gfails >= GROUP_TRIES) { groups[g].glocked = (uint32_t)(now + GROUP_LOCK); groups[g].gfails = 0; }
        mark_dirty();
    }
    return "wrong group ID or password";
}

static void google_status(core_conn_t *c);
static void on_login(core_conn_t *c, int uid) {
    bool first = !is_online(uid); c->uid = uid;
    if (first) broadcast_presence(uid);
    char st[24], ts[24], a[24], b[24];
    for (int i = 0; i < nfriends; i++) if (friends[i].a == uid) { int o = friends[i].b; friend_frame(c, o); pres_state(o, st, ts); emit(c, "PRES", users[o].id, st, ts, NULL); }
    for (int i = 0; i < nreqs; i++) if (reqs[i].to == uid) { user_t *f = &users[reqs[i].frm]; emit(c, "FREQ_IN", f->id, f->name, reqs[i].intro, f->handle, f->avatar, NULL); }
    for (int i = 0; i < ngmems; i++) if (gmems[i].uid == uid) group_state(c, gmems[i].gid);
    for (int i = 0; i < nreads; i++) if (reads[i].writer == uid) {
        if (reads[i].gid[0]) emit(c, "GREAD_IN", reads[i].gid, users[reads[i].reader].id, num(a, reads[i].upto), NULL);
        else emit(c, "READ_IN", users[reads[i].reader].id, num(a, reads[i].upto), NULL);
    }
    /* undelivered events, oldest first (the array is in sequence order) */
    for (int i = 0; i < nmsgs; i++) if (msgs[i].to == uid) event_frame(c, &msgs[i]);
    (void)b;
    google_status(c);
    emit(c, "SYNCED", NULL);
}

static bool share_group(int a, int b) {
    for (int i = 0; i < ngmems; i++) if (gmems[i].uid == a) for (int j = 0; j < ngmems; j++) if (gmems[j].uid == b && !strcmp(gmems[i].gid, gmems[j].gid)) return true;
    return false;
}
static bool backlog_full(int frm, int to) {          /* frm < 0: only the totals of the recipient count */
    if (nmsgs >= MSG_TOTAL_MAX) return true;             /* the whole server: group fan-out multiplies one send by the member count */
    int all = 0, pair = 0, sender = 0; size_t bytes = 0;
    for (int i = 0; i < nmsgs; i++) {
        if (msgs[i].to == to) { all++; bytes += msgs[i].tlen; if (frm >= 0 && msgs[i].frm == frm) pair++; }
        if (frm >= 0 && msgs[i].frm == frm) sender++;
    }
    return all >= MAX_BACKLOG || pair >= MAX_BACKLOG_PAIR || sender >= SENDER_TOTAL_MAX || bytes >= BACKLOG_BYTES_MAX;
}
static void queue_msg(int frm, int to, const char *msgid, const char *text, int kind) {
    GROW(msgs, nmsgs, capmsgs); msg_t *m = &msgs[nmsgs]; char *t = strdup(text); if (!t) return;
    nmsgs++; memset(m, 0, sizeof *m); m->seq = next_seq++; m->ts = (uint32_t)plat_unix(); m->frm = frm; m->to = to; m->kind = (uint8_t)kind; copyz(m->msgid, sizeof m->msgid, msgid); m->text = t; m->tlen = (uint32_t)strlen(t);
    mark_dirty();
    for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used && conns[i].uid == to) event_frame(&conns[i], m);
}

static void new_invite(int uid) {
    for (;;) {
        char code[10]; for (int i = 0; i < 8; i++) code[i] = INVITE_ALPHABET[rnd_below(31)]; code[8] = 0;
        bool used = false; for (int i = 0; i < nusers; i++) if (strcmp(users[i].invite, code) == 0) used = true;
        if (!used) { strcpy(users[uid].invite, code); mark_dirty(); return; }
    }
}
static void new_token_for(int uid, char *token /* >= 25 */) {
    unsigned char r[12]; plat_random(r, sizeof r); hexs(token, r, 12); tok_hash(token, users[uid].tok); mark_dirty();
}

static struct { char id[12]; int fails; uint64_t since; } login_fail[256]; static int nlogin_fail;
static int lf_find(const char *id) { for (int i = 0; i < nlogin_fail; i++) if (strcmp(login_fail[i].id, id) == 0) return i; return -1; }

/* ------------------------------------------------------------------ commands */
static char sq_[40]; static int sq_len;
static int search_rank(const user_t *u) {                  /* 2 handle == q, 1 handle starts with q, 0 otherwise (sqlite: ORDER BY (handle=?) DESC, handle LIKE q% DESC) */
    char h[24]; copyz(h, sizeof h, u->handle); lower_ascii(h);
    if (h[0] && strcmp(h, sq_) == 0) return 2;
    if (h[0] && strncmp(h, sq_, (size_t)sq_len) == 0) return 1;
    return 0;
}
static int cmp_users(const void *x, const void *y) {
    const user_t *a = &users[*(const int *)x], *b = &users[*(const int *)y];
    int ra = search_rank(a), rb = search_rank(b);
    if (ra != rb) return rb - ra;
    int pa = ra >= 1, pb = rb >= 1; if (pa != pb) return pb - pa;
    return strcmp(a->name, b->name);
}
static bool contains_ci(const char *hay, const char *needle) {        /* ASCII case-insensitive substring (sqlite LIKE) */
    size_t nl = strlen(needle); if (!nl) return true;
    for (const char *p = hay; *p; p++) { size_t i = 0; while (i < nl && p[i] && ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]) == needle[i]) i++; if (i == nl) return true; }
    return false;
}
static bool starts_ci(const char *s, const char *pre) { size_t n = strlen(pre); for (size_t i = 0; i < n; i++) { char ch = s[i]; if (!ch) return false; if (ch >= 'A' && ch <= 'Z') ch += 32; if (ch != pre[i]) return false; } return true; }

static void do_command(core_conn_t *c, char **f, int n);

static void cmd_friend_request(core_conn_t *c, int uid, const char *to_id, const char *intro_in) {
    int to = find_user(to_id); char intro[512]; copyz(intro, sizeof intro, intro_in); cut_chars(intro, 120);
    if (to < 0 || to == uid || is_blocked(to, uid)) { emit(c, "ERR", "cannot send a request to that user", NULL); return; }
    if (users[uid].restricted) { emit(c, "ERR", "your account is restricted after reports from other users", NULL); return; }
    if (are_friends(uid, to)) { emit(c, "ERR", "already friends", NULL); return; }
    if (find_req(to, uid) >= 0) { char *ff[2] = { "FACC", (char *)to_id }; do_command(c, ff, 2); return; }
    int pend = 0; for (int i = 0; i < nreqs; i++) if (reqs[i].frm == uid) pend++;
    if (pend >= 50) { emit(c, "ERR", "too many pending requests", NULL); return; }
    int ri = find_req(uid, to); if (ri < 0) { GROW(reqs, nreqs, capreqs); ri = nreqs++; }
    reqs[ri].frm = uid; reqs[ri].to = to; copyz(reqs[ri].intro, sizeof reqs[ri].intro, intro); mark_dirty();
    emit(c, "OK_FREQ", to_id, NULL);
    push(to, "FREQ_IN", users[uid].id, users[uid].name, intro, users[uid].handle, users[uid].avatar, NULL);
}

static bool is_group_cmd(const char *c) {
    static const char *const L[] = { "GNEW", "GJOIN", "GLEAVE", "GINFO", "GSEND", "GEDIT", "GDEL", "GPASS", "GKICK", "GRENAME", "GROLE", "GOWNER", "GANN", "GNOTE", "GNOTES", "GNOTEDEL", "GREAD", "GTYPING", "GKEY" };
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) if (!strcmp(c, L[i])) return true;
    return false;
}
static void group_command(core_conn_t *c, char **f, int n) {
    int uid = c->uid; const char *cmd = f[0]; char a[24], b[24];
    const char *gid = (!strcmp(cmd, "GSEND") || !strcmp(cmd, "GEDIT") || !strcmp(cmd, "GDEL")) && n > 2 ? f[2] : (n > 1 ? f[1] : "");
    int g = find_group(gid); bool is_member = gm_find(gid, uid) >= 0; bool is_owner = g >= 0 && groups[g].owner == uid; int myrole = role_of(gid, uid);
    if (!strcmp(cmd, "GNEW") && n >= 2) {
        char name[200]; copyz(name, sizeof name, f[1]); char *nm = strip(name); cut_chars(nm, MAX_GNAME);
        char pwb[200]; copyz(pwb, sizeof pwb, n > 2 ? f[2] : ""); char *pw = strip(pwb);
        if (!*nm) { emit(c, "ERR", "give the group a name", NULL); return; }
        if (gm_of_user(uid) >= GROUPS_PER_USER) { emit(c, "ERR", "you are in too many groups", NULL); return; }
        if (*pw && (!pw_ok_format(pw) || weak_pw(pw))) { emit(c, "ERR", "the password must be 6 digits and not too simple (not 123456, 111111, ...)", NULL); return; }
        char gen[8]; if (!*pw) { gen_pw(gen); pw = gen; }
        if (!hash_allowed(c)) { emit(c, "ERR", "too many password operations - wait a minute", NULL); return; }
        char ngid[12]; for (;;) { snprintf(ngid, sizeof ngid, "g%u", 10000000u + rnd_below(90000000u)); if (find_group(ngid) < 0) break; }
        GROW(groups, ngroups, capgroups); group_t *gr = &groups[ngroups++]; memset(gr, 0, sizeof *gr);
        copyz(gr->gid, sizeof gr->gid, ngid); copyz(gr->name, sizeof gr->name, nm); gr->owner = uid; gr->created = (uint32_t)plat_unix(); gr->ann_by = -1;
        if (!hash_pw(pw, gr->pw)) { ngroups--; emit(c, "ERR", "the server is out of memory - try again later", NULL); return; }
        GROW(gmems, ngmems, capgmems); gmem_t *m = &gmems[ngmems++]; memset(m, 0, sizeof *m); copyz(m->gid, sizeof m->gid, ngid); m->uid = uid; m->joined = (uint32_t)plat_unix();
        mark_dirty(); emit(c, "OK_GNEW", ngid, nm, pw, NULL); push_group_state(ngid, -1);
    } else if (!strcmp(cmd, "GJOIN") && n >= 3) {
        char pwb[200], eb[110]; copyz(pwb, sizeof pwb, f[2]);
        if (!hash_allowed(c)) { emit(c, "ERR", "too many password operations - wait a minute", NULL); return; }
        const char *err = verify_group_pw(uid, gid, strip(pwb), eb);
        if (err) { emit(c, "ERR", err, NULL); return; }
        if (is_member) { emit(c, "ERR", "you are already in this group", NULL); return; }
        if (gm_count(gid) >= GROUP_MEMBERS) { emit(c, "ERR", "this group is full", NULL); return; }
        if (gm_of_user(uid) >= GROUPS_PER_USER) { emit(c, "ERR", "you are in too many groups", NULL); return; }
        GROW(gmems, ngmems, capgmems); gmem_t *m = &gmems[ngmems++]; memset(m, 0, sizeof *m); copyz(m->gid, sizeof m->gid, gid); m->uid = uid; m->joined = (uint32_t)plat_unix(); mark_dirty();
        emit(c, "OK_GJOIN", gid, groups[find_group(gid)].name, NULL); push_group_state(gid, uid);
        for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && gmems[i].uid != uid) push(gmems[i].uid, "GMEMBER", gid, users[uid].id, users[uid].name, "0", NULL);
    } else if (!is_member) {
        emit(c, "ERR", "you are not in that group", NULL);
    } else if (!strcmp(cmd, "GLEAVE")) {
        drop_member(gid, uid); emit(c, "OK_GLEAVE", gid, NULL);
    } else if (!strcmp(cmd, "GINFO")) {
        group_state(c, gid);
    } else if (!strcmp(cmd, "GKEY") && n >= 5) {                       /* GKEY gid uid epoch wrapped: one member gets the group key of an epoch, wrapped for them */
        int to = find_user(f[2]);
        if (to < 0 || to == uid || gm_find(gid, to) < 0) { emit(c, "ERR", "not a member of that group", NULL); return; }
        if (f[4][0] != '\x01' || f[4][1] != 'E' || strlen(f[4]) > MAX_E2E || strlen(f[3]) > 12) { emit(c, "ERR", "bad group key", NULL); return; }
        { int kw = 0; for (int i = 0; i < nmsgs; i++) if (msgs[i].to == to && msgs[i].kind == 3) kw++; if (kw >= MAX_GKEY_WAITING || backlog_full(-1, to)) { emit(c, "ERR", "that member has too many keys waiting", NULL); return; } }
        for (int i = 0; i < nmsgs;) { if (msgs[i].frm == uid && msgs[i].to == to && msgs[i].kind == 3 && !strcmp(msgs[i].gid, gid) && !strcmp(msgs[i].msgid, f[3])) { free(msgs[i].text); memmove(&msgs[i], &msgs[i + 1], sizeof(msg_t) * (size_t)(nmsgs - i - 1)); nmsgs--; } else i++; }
        GROW(msgs, nmsgs, capmsgs); msg_t *m = &msgs[nmsgs]; char *t = strdup(f[4]); if (!t) return; nmsgs++;
        memset(m, 0, sizeof *m); m->seq = next_seq++; m->ts = (uint32_t)plat_unix(); m->frm = uid; m->to = to; m->kind = 3; copyz(m->msgid, sizeof m->msgid, f[3]); m->text = t; m->tlen = (uint32_t)strlen(t); copyz(m->gid, sizeof m->gid, gid);
        mark_dirty(); for (int k = 0; k < MAX_CONNS; k++) if (conns[k].used && conns[k].uid == to) event_frame(&conns[k], m);
        emit(c, "OK_GKEY", gid, users[to].id, f[3], NULL);
    } else if (!strcmp(cmd, "GSEND") && n >= 4) {
        bool genc = f[3][0] == '\x01' && f[3][1] == 'G'; char reqe[8]; char fidg[24] = "";
        if (genc && strlen(f[3]) > MAX_E2E) { emit(c, "NOSEND", f[1], "too long", NULL); return; }
        if (!genc && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { emit(c, "NOSEND", f[1], "encryption required", NULL); return; }
        char *text = malloc(strlen(f[3]) + 1); if (!text) return; strcpy(text, f[3]); if (!genc) cut_chars(text, MAX_TEXT);
        if (genc && n >= 5) copyz(fidg, sizeof fidg, f[4]);
        else if (!strncmp(text, "\x01" "F|", 3)) {
            const char *p = text + 3, *q = strchr(p, '|'); int bars = 0; for (const char *z = text; *z; z++) if (*z == '|') bars++;
            if (q && bars >= 3 && (size_t)(q - p) < sizeof fidg) { memcpy(fidg, p, (size_t)(q - p)); fidg[q - p] = 0; }
        }
        if (fidg[0] || (!genc && !strncmp(text, "\x01" "F|", 3))) {
            int fi = find_file(fidg);
            if (fi < 0 || files[fi].owner != uid || !files[fi].done) { free(text); emit(c, "NOSEND", f[1], "file not uploaded", NULL); return; }
            for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && !acl_has(fi, gmems[i].uid)) { GROW(acls, nacls, capacls); acls[nacls++] = (acl_t){ fi, gmems[i].uid }; }
        }
        queue_group(uid, gid, f[1], text, 0); free(text); emit(c, "GSENT", f[1], NULL);
    } else if (!strcmp(cmd, "GEDIT") && n >= 4) {
        bool genc = f[3][0] == '\x01' && f[3][1] == 'G'; char reqe[8];
        if (genc && strlen(f[3]) > MAX_E2E) { emit(c, "NOSEND", f[1], "too long", NULL); return; }
        if (!genc && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { emit(c, "NOSEND", f[1], "encryption required", NULL); return; }
        char *text = malloc(strlen(f[3]) + 1); if (!text) return; strcpy(text, f[3]); if (!genc) cut_chars(text, MAX_TEXT);
        queue_group(uid, gid, f[1], text, 1); free(text); emit(c, "OK_GEDIT", f[1], NULL);
    } else if (!strcmp(cmd, "GDEL") && n >= 3) {
        queue_group(uid, gid, f[1], "", 2); emit(c, "OK_GDEL", f[1], NULL);
    } else if (!strcmp(cmd, "GNOTE") && n >= 3) {
        bool genc = f[2][0] == '\x01' && f[2][1] == 'G'; char reqe[8];
        if (genc && strlen(f[2]) > MAX_E2E) { emit(c, "ERR", "note too long", NULL); return; }
        if (!genc && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { emit(c, "ERR", "encryption required", NULL); return; }
        char *t = malloc(strlen(f[2]) + 1); if (!t) return; strcpy(t, f[2]); char *s = genc ? t : strip(t); if (!genc) cut_chars(s, MAX_TEXT); if (!*s) { free(t); return; }
        int cnt = 0; for (int i = 0; i < nnotes; i++) if (!strcmp(notes[i].gid, gid)) cnt++;
        if (cnt >= 100) { free(t); emit(c, "ERR", "this group already has 100 notes - delete one first", NULL); return; }
        GROW(notes, nnotes, capnotes); note_t *nt = &notes[nnotes++]; nt->id = next_note++; copyz(nt->gid, sizeof nt->gid, gid); nt->author = uid; nt->ts = (uint32_t)plat_unix(); nt->text = strdup(s); free(t); mark_dirty();
        emit(c, "OK_GNOTE", gid, num(a, nt->id), NULL);
        for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) push(gmems[i].uid, "GNOTE", gid, a, users[uid].id, num(b, nt->ts), nt->text, NULL);
    } else if (!strcmp(cmd, "GNOTES")) {
        for (int i = 0; i < nnotes; i++) if (!strcmp(notes[i].gid, gid)) emit(c, "GNOTE", gid, num(a, notes[i].id), users[notes[i].author].id, num(b, notes[i].ts), notes[i].text, NULL);
        emit(c, "GNOTES_END", gid, NULL);
    } else if (!strcmp(cmd, "GNOTEDEL") && n >= 3) {
        unsigned long id = strtoul(f[2], NULL, 10); int ni = -1; for (int i = 0; i < nnotes; i++) if (notes[i].id == id && !strcmp(notes[i].gid, gid)) ni = i;
        if (ni < 0) { emit(c, "ERR", "no such note", NULL); return; }
        if (notes[ni].author != uid && myrole < 1) { emit(c, "ERR", "only the author or an admin can delete a note", NULL); return; }
        free(notes[ni].text); memmove(&notes[ni], &notes[ni + 1], sizeof(note_t) * (size_t)(nnotes - ni - 1)); nnotes--; mark_dirty();
        emit(c, "OK_GNOTEDEL", gid, f[2], NULL);
        for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) push(gmems[i].uid, "GNOTE_DEL", gid, f[2], NULL);
    } else if (!strcmp(cmd, "GTYPING")) {
        for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && gmems[i].uid != uid) push(gmems[i].uid, "GTYPING_IN", gid, users[uid].id, NULL);
    } else if (!strcmp(cmd, "GREAD") && n >= 4) {
        int w = find_user(f[2]); bool digits = f[3][0] != 0; for (const char *p = f[3]; *p; p++) if (*p < '0' || *p > '9') digits = false;
        if (w >= 0 && gm_find(gid, w) >= 0 && digits) {
            int i = 0; for (; i < nreads; i++) if (reads[i].reader == uid && reads[i].writer == w && !strcmp(reads[i].gid, gid)) break;
            if (i == nreads) { GROW(reads, nreads, capreads); nreads++; }
            reads[i].reader = uid; reads[i].writer = w; reads[i].upto = (uint32_t)strtoul(f[3], NULL, 10); copyz(reads[i].gid, sizeof reads[i].gid, gid); mark_dirty();
            push(w, "GREAD_IN", gid, users[uid].id, f[3], NULL);
        }
    } else if ((!strcmp(cmd, "GANN") || !strcmp(cmd, "GKICK") || !strcmp(cmd, "GRENAME")) && myrole < 1) {
        emit(c, "ERR", "only the group owner or a sub-admin can do that", NULL);
    } else if (!strcmp(cmd, "GANN")) {
        const char *src = n > 2 ? f[2] : ""; bool genc = src[0] == '\x01' && src[1] == 'G'; char reqe[8]; char *t = NULL, *s;
        if (genc) { if (strlen(src) > MAX_E2E) { emit(c, "ERR", "announcement too long", NULL); return; } t = strdup(src); s = t; }
        else { t = malloc(strlen(src) + 1); if (t) strcpy(t, src); s = t ? strip(t) : NULL; if (s) cut_chars(s, 300); }
        if (!s) { free(t); return; }
        if (!genc && *s && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { free(t); emit(c, "ERR", "encryption required", NULL); return; }
        copyz(groups[g].ann, sizeof groups[g].ann, s); groups[g].ann_by = uid; groups[g].ann_ts = (uint32_t)plat_unix(); mark_dirty();
        emit(c, "OK_GANN", gid, NULL);
        for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid)) push(gmems[i].uid, "GANN", gid, s, users[uid].id, num(a, groups[g].ann_ts), NULL);
        free(t);
    } else if (!strcmp(cmd, "GKICK") && n >= 3) {
        int t = find_user(f[2]);
        if (t < 0 || t == uid || gm_find(gid, t) < 0) { emit(c, "ERR", "no such member", NULL); return; }
        if (!is_owner && role_of(gid, t) >= 1) { emit(c, "ERR", "only the owner can remove an admin", NULL); return; }
        drop_member(gid, t); emit(c, "OK_GKICK", gid, f[2], NULL);
    } else if (!strcmp(cmd, "GRENAME") && n >= 3) {
        char t[200]; copyz(t, sizeof t, f[2]); char *s = strip(t); cut_chars(s, MAX_GNAME);
        if (!*s) { emit(c, "ERR", "give the group a name", NULL); return; }
        copyz(groups[g].name, sizeof groups[g].name, s); mark_dirty(); push_group_state(gid, -1);
    } else if (!is_owner) {
        emit(c, "ERR", "only the group owner can do that", NULL);
    } else if (!strcmp(cmd, "GPASS")) {
        char pwb[200]; copyz(pwb, sizeof pwb, n > 2 ? f[2] : ""); char *pw = strip(pwb); char gen[8];
        if (*pw && (!pw_ok_format(pw) || weak_pw(pw))) { emit(c, "ERR", "the password must be 6 digits and not too simple (not 123456, 111111, ...)", NULL); return; }
        if (!*pw) { gen_pw(gen); pw = gen; }
        if (!hash_allowed(c)) { emit(c, "ERR", "too many password operations - wait a minute", NULL); return; }
        { char nh[100]; if (!hash_pw(pw, nh)) { emit(c, "ERR", "the server is out of memory - try again later", NULL); return; } copyz(groups[g].pw, sizeof groups[g].pw, nh); }
        groups[g].gfails = 0; groups[g].glocked = 0; mark_dirty(); emit(c, "OK_GPASS", gid, pw, NULL);
    } else if (!strcmp(cmd, "GROLE") && n >= 4) {
        int t = find_user(f[2]); bool valid = (!strcmp(f[3], "0") || !strcmp(f[3], "1"));
        if (t < 0 || t == uid || gm_find(gid, t) < 0 || !valid) { emit(c, "ERR", "no such member", NULL); return; }
        if (f[3][0] == '1') { int k = 0; for (int i = 0; i < ngmems; i++) if (!strcmp(gmems[i].gid, gid) && gmems[i].role == 1) k++; if (k >= 10) { emit(c, "ERR", "a group can have at most 10 sub-admins", NULL); return; } }
        gmems[gm_find(gid, t)].role = (uint8_t)(f[3][0] - '0'); mark_dirty(); emit(c, "OK_GROLE", gid, f[2], f[3], NULL); push_member(gid, t);
    } else if (!strcmp(cmd, "GOWNER") && n >= 3) {
        int t = find_user(f[2]);
        if (t < 0 || t == uid || gm_find(gid, t) < 0) { emit(c, "ERR", "no such member", NULL); return; }
        groups[g].owner = t; gmems[gm_find(gid, t)].role = 0; gmems[gm_find(gid, uid)].role = 1; mark_dirty();
        emit(c, "OK_GOWNER", gid, f[2], NULL); push_group_state(gid, -1);
    } else emit(c, "ERR", "unknown command", NULL);
}

static void do_command(core_conn_t *c, char **f, int n) {
    int uid = c->uid; const char *cmd = f[0]; char a[24], b[24];
    if (!strcmp(cmd, "NAME") && n >= 2) {
        char name[96]; copyz(name, sizeof name, f[1]); char *s = strip(name); cut_chars(s, MAX_NAME);
        if (*s) { copyz(users[uid].name, sizeof users[uid].name, s); mark_dirty(); emit(c, "OK_NAME", s, NULL); push_friend_info(uid); }
    } else if (!strcmp(cmd, "HANDLE")) {
        char h[96]; copyz(h, sizeof h, n > 1 ? f[1] : ""); char *s = strip(h); lower_ascii(s); while (*s == '@') s++;
        size_t L = strlen(s); bool ok = true;
        for (size_t i = 0; i < L; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '.')) ok = false;
        if (L && (!ok || L < 3 || L > 20)) { emit(c, "ERR", "handle: 3-20 characters, a-z 0-9 _ .", NULL); return; }
        for (int i = 0; i < nusers; i++) if (i != uid && L && !strcmp(users[i].handle, s)) { emit(c, "ERR", "that handle is taken", NULL); return; }
        copyz(users[uid].handle, sizeof users[uid].handle, s); mark_dirty(); emit(c, "OK_HANDLE", s, NULL); push_friend_info(uid);
    } else if (!strcmp(cmd, "BIO")) {
        char t[512]; copyz(t, sizeof t, n > 1 ? f[1] : ""); char *s = strip(t); cut_chars(s, MAX_BIO);
        copyz(users[uid].bio, sizeof users[uid].bio, s); mark_dirty(); emit(c, "OK_BIO", NULL);
    } else if (!strcmp(cmd, "DISC") && n >= 2) {
        users[uid].disc = f[1][0] == '1' && !f[1][1]; mark_dirty(); emit(c, "OK_DISC", users[uid].disc ? "1" : "0", NULL);
    } else if (!strcmp(cmd, "AVATAR")) {
        const char *fid = n > 1 ? f[1] : ""; int fi = -1;
        if (*fid) {
            fi = find_file(fid);
            if (fi < 0 || files[fi].owner != uid || !files[fi].done || files[fi].size > MAX_AVATAR) { emit(c, "ERR", "avatar must be an uploaded image up to 256 KB", NULL); return; }
            files[fi].pub = 1;
        }
        copyz(users[uid].avatar, sizeof users[uid].avatar, fid); mark_dirty(); emit(c, "OK_AVATAR", fid, NULL); push_friend_info(uid);
    } else if (!strcmp(cmd, "ME")) {
        if (!users[uid].invite[0]) new_invite(uid);
        user_t *u = &users[uid]; emit(c, "ME", u->id, u->name, u->handle, u->bio, u->disc ? "1" : "0", u->avatar, u->invite, NULL);
    } else if (!strcmp(cmd, "INVITE_NEW")) {
        new_invite(uid); emit(c, "INVITE", users[uid].invite, NULL);
    } else if (!strcmp(cmd, "JOIN") && n >= 2) {
        char code[64]; int k = 0; for (const char *p = f[1]; *p && k < 60; p++) { if (*p == '-' || *p == ' ') continue; code[k++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p; } code[k] = 0;
        int t = -1; for (int i = 0; i < nusers; i++) if (users[i].invite[0] && !strcmp(users[i].invite, code)) t = i;
        if (t < 0) { emit(c, "ERR", "invalid invite code", NULL); return; }
        cmd_friend_request(c, uid, users[t].id, "Joined with your invite code");
    } else if (!strcmp(cmd, "SEARCH") && n >= 2) {
        char q[96]; copyz(q, sizeof q, f[1]); char *s = strip(q); lower_ascii(s); while (*s == '@') s++; cut_chars(s, 30);
        copyz(sq_, sizeof sq_, s); sq_len = (int)strlen(sq_);
        int *rows = malloc(sizeof(int) * (size_t)(nusers + 1)), nr = 0;
        if (rows) {
            int chars = 0; for (const char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) chars++;
            char like[40]; int k = 0; for (const char *p = s; *p && k < 38; p++) if (*p != '%') like[k++] = *p; like[k] = 0;
            if (chars >= 2) for (int i = 0; i < nusers; i++) {
                user_t *u = &users[i];
                if (!u->disc || u->restricted || i == uid || is_blocked(i, uid)) continue;
                char h[24]; copyz(h, sizeof h, u->handle);
                if (starts_ci(h, like) || contains_ci(u->name, like)) rows[nr++] = i;
            }
            qsort(rows, (size_t)nr, sizeof(int), cmp_users);
        }
        if (nr > 20) nr = 20;
        for (int i = 0; i < nr; i++) { user_t *u = &users[rows[i]]; emit(c, "RESULT", u->id, u->name, u->handle, u->bio, u->avatar, NULL); }
        emit(c, "RESULT_END", num(a, (unsigned)nr), NULL); free(rows);
    } else if (!strcmp(cmd, "PROFILE") && n >= 2) {
        int t = find_user(f[1]);
        if (t < 0 || is_blocked(t, uid)) { emit(c, "ERR", "no such user", NULL); return; }
        user_t *u = &users[t]; bool vis = t == uid || u->disc || are_friends(uid, t) || find_req(t, uid) >= 0;
        emit(c, "PROFILE", u->id, u->name, vis ? u->handle : "", vis ? u->bio : "", vis ? u->avatar : "", NULL);
    } else if (!strcmp(cmd, "FIND") && n >= 2) {
        int t = find_user(f[1]);
        if (t >= 0 && !is_blocked(t, uid)) emit(c, "FOUND", users[t].id, users[t].name, NULL); else emit(c, "ERR", "no such user", NULL);
    } else if (!strcmp(cmd, "FREQ") && n >= 2) {
        cmd_friend_request(c, uid, f[1], n > 2 ? f[2] : "");
    } else if (!strcmp(cmd, "FACC") && n >= 2) {
        int o = find_user(f[1]);
        if (o < 0 || find_req(o, uid) < 0) { emit(c, "ERR", "no such request", NULL); return; }
        req_del(o, uid); friend_add(uid, o); friend_add(o, uid); mark_dirty();
        push(uid, "FRIEND", users[o].id, users[o].name, users[o].handle, users[o].avatar, NULL);
        push(o, "FRIEND", users[uid].id, users[uid].name, users[uid].handle, users[uid].avatar, NULL);
        { char st[24], ts[24]; pres_state(o, st, ts); push(uid, "PRES", users[o].id, st, ts, NULL); pres_state(uid, st, ts); push(o, "PRES", users[uid].id, st, ts, NULL); }
    } else if (!strcmp(cmd, "FDEC") && n >= 2) {
        int o = find_user(f[1]); if (o >= 0) { req_del(o, uid); mark_dirty(); }
    } else if (!strcmp(cmd, "FDEL") && n >= 2) {
        int o = find_user(f[1]); if (o >= 0) friend_del(uid, o); mark_dirty();
        push(uid, "FRIEND_DEL", f[1], NULL);
        if (o >= 0) push(o, "FRIEND_DEL", users[uid].id, NULL);
    } else if (!strcmp(cmd, "TYPING") && n >= 2) {
        int o = find_user(f[1]); if (o >= 0 && are_friends(uid, o)) push(o, "TYPING_IN", users[uid].id, NULL);
    } else if (!strcmp(cmd, "SETPRES") && n >= 2) {
        int v = f[1][0] == '1' && !f[1][1]; users[uid].hide = v ? 0 : 1; mark_dirty(); emit(c, "OK_SETPRES", v ? "1" : "0", NULL); broadcast_presence(uid);
    } else if (!strcmp(cmd, "BLOCKS")) {
        int k = 0; for (int i = 0; i < nblocks; i++) if (blocks[i].a == uid) { emit(c, "BLOCKED", users[blocks[i].b].id, users[blocks[i].b].name, NULL); k++; }
        emit(c, "BLOCKS_END", num(a, (unsigned)k), NULL);
    } else if (!strcmp(cmd, "UNBLOCK") && n >= 2) {
        int o = find_user(f[1]); if (o >= 0) { pair_del(blocks, &nblocks, uid, o); mark_dirty(); } emit(c, "OK_UNBLOCK", f[1], NULL);
    } else if (!strcmp(cmd, "BLOCK") && n >= 2) {
        int o = find_user(f[1]);
        if (o >= 0) { if (!is_blocked(uid, o)) { GROW(blocks, nblocks, capblocks); blocks[nblocks++] = (pair_t){ uid, o }; } friend_del(uid, o); req_del(o, uid); mark_dirty(); }
        emit(c, "OK_BLOCK", f[1], NULL); push(uid, "FRIEND_DEL", f[1], NULL);
    } else if (!strcmp(cmd, "REPORT") && n >= 3) {
        int t = find_user(f[1]); uint32_t now = (uint32_t)plat_unix();
        if (t < 0 || t == uid) { emit(c, "ERR", "no such user", NULL); return; }
        int recent = 0; bool dup = false;
        for (int i = 0; i < nreports; i++) if (reports[i].reporter == uid) { if (reports[i].ts > now - 3600) recent++; if (reports[i].target == t && reports[i].ts > now - 86400) dup = true; }
        if (recent >= 5) { emit(c, "ERR", "too many reports - try again later", NULL); return; }
        if (dup) { emit(c, "ERR", "you already reported that user today", NULL); return; }
        char reason[256]; copyz(reason, sizeof reason, f[2]); char *rs = strip(reason); cut_chars(rs, 200);
        char ev[4100]; copyz(ev, sizeof ev, n > 3 ? f[3] : ""); cut_chars(ev, 1000);
        GROW(reports, nreports, capreports); report_t *r = &reports[nreports++]; r->reporter = uid; r->target = t; r->ts = now; copyz(r->reason, sizeof r->reason, rs); r->evidence = strdup(ev);
        int distinct = 0; for (int i = 0; i < nreports; i++) if (reports[i].target == t && reports[i].ts > now - 7 * 86400) { bool seen = false; for (int j = 0; j < i; j++) if (reports[j].target == t && reports[j].ts > now - 7 * 86400 && reports[j].reporter == reports[i].reporter) seen = true; if (!seen) distinct++; }
        if (distinct >= REPORT_LIMIT) users[t].restricted = 1;
        if (!is_blocked(uid, t)) { GROW(blocks, nblocks, capblocks); blocks[nblocks++] = (pair_t){ uid, t }; }
        friend_del(uid, t); mark_dirty(); emit(c, "OK_REPORT", f[1], NULL); push(uid, "FRIEND_DEL", f[1], NULL);
    } else if (!strcmp(cmd, "READ") && n >= 3) {
        int o = find_user(f[1]); bool digits = f[2][0] != 0; for (const char *p = f[2]; *p; p++) if (*p < '0' || *p > '9') digits = false;
        if (o >= 0 && digits && are_friends(uid, o)) {
            uint32_t up = (uint32_t)strtoul(f[2], NULL, 10); int i = 0; for (; i < nreads; i++) if (reads[i].reader == uid && reads[i].writer == o && !reads[i].gid[0]) break;
            if (i == nreads) { GROW(reads, nreads, capreads); nreads++; }
            reads[i] = (read_t){ uid, o, up }; mark_dirty(); push(o, "READ_IN", users[uid].id, f[2], NULL);
        }
    } else if (!strcmp(cmd, "KEYPUT") && n >= 2) {
        size_t l = strlen(f[1]); bool okb = l == E2E_B64LEN; for (size_t i = 0; okb && i < l; i++) { char ch = f[1][i]; okb = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'; }
        if (!okb) { emit(c, "ERR", "bad key bundle", NULL); return; }
        if (!strcmp(users[uid].e2ekey, f[1])) { emit(c, "OK_KEYPUT", NULL); return; }          /* the same bundle again: nothing to do */
        uint32_t nowt = (uint32_t)plat_unix(); if (nowt - users[uid].kp_win >= 3600) { users[uid].kp_win = nowt; users[uid].kp_cnt = 0; }
        if (users[uid].kp_cnt >= KEYPUT_PER_HOUR) { emit(c, "ERR", "too many key changes - try again later", NULL); return; }
        users[uid].kp_cnt++; copyz(users[uid].e2ekey, sizeof users[uid].e2ekey, f[1]); mark_dirty(); emit(c, "OK_KEYPUT", NULL);
    } else if (!strcmp(cmd, "KEYGET") && n >= 2) {
        int o = find_user(f[1]); if (o < 0 || (o != uid && !are_friends(uid, o) && !share_group(uid, o))) { emit(c, "ERR", "not friends", NULL); return; }
        emit(c, "KEY", users[o].id, users[o].e2ekey, NULL);
    } else if (!strcmp(cmd, "SEND") && n >= 4) {
        int to = find_user(f[2]); bool enc = f[3][0] == '\x01' && f[3][1] == 'E'; char reqe[8];
        if (to < 0 || !are_friends(uid, to)) { emit(c, "NOSEND", f[1], "not friends", NULL); return; }
        if (enc && strlen(f[3]) > MAX_E2E) { emit(c, "NOSEND", f[1], "too long", NULL); return; }
        if (!enc && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { emit(c, "NOSEND", f[1], "encryption required", NULL); return; }
        if (backlog_full(uid, to)) { emit(c, "NOSEND", f[1], "the recipient's mailbox is full", NULL); return; }
        char *text = malloc(strlen(f[3]) + 1); if (!text) return; strcpy(text, f[3]); if (!enc) cut_chars(text, MAX_TEXT);
        char fid[24] = "";
        if (enc && n >= 5) copyz(fid, sizeof fid, f[4]);
        else if (!strncmp(text, "\x01" "F|", 3)) {                      /* an attachment: only a file you uploaded, and its receiver may then download it */
            const char *p = text + 3, *q = strchr(p, '|'); int bars = 0; for (const char *z = text; *z; z++) if (*z == '|') bars++;
            if (q && bars >= 3 && (size_t)(q - p) < sizeof fid) { memcpy(fid, p, (size_t)(q - p)); fid[q - p] = 0; }
        }
        if (fid[0] || (!enc && !strncmp(text, "\x01" "F|", 3))) {
            int fi = find_file(fid);
            if (fi < 0 || files[fi].owner != uid || !files[fi].done) { free(text); emit(c, "NOSEND", f[1], "file not uploaded", NULL); return; }
            if (!acl_has(fi, to)) { GROW(acls, nacls, capacls); acls[nacls++] = (acl_t){ fi, to }; mark_dirty(); }
        }
        queue_msg(uid, to, f[1], text, 0); free(text); emit(c, "SENT", f[1], NULL);
    } else if ((!strcmp(cmd, "EDIT") && n >= 4) || (!strcmp(cmd, "DEL") && n >= 3)) {
        int to = find_user(f[2]);
        if (to < 0 || !are_friends(uid, to)) { emit(c, "NOSEND", f[1], "not friends", NULL); return; }
        if (backlog_full(uid, to)) { emit(c, "NOSEND", f[1], "the recipient's mailbox is full", NULL); return; }
        if (cmd[0] == 'E') {
            bool enc = f[3][0] == '\x01' && f[3][1] == 'E'; char reqe[8];
            if (enc && strlen(f[3]) > MAX_E2E) { emit(c, "NOSEND", f[1], "too long", NULL); return; }
            if (!enc && core_conf_get("require_e2e", reqe, sizeof reqe) && reqe[0] == '1') { emit(c, "NOSEND", f[1], "encryption required", NULL); return; }
            char *text = malloc(strlen(f[3]) + 1); if (!text) return; strcpy(text, f[3]); if (!enc) cut_chars(text, MAX_TEXT);
            queue_msg(uid, to, f[1], text, 1); free(text); emit(c, "OK_EDIT", f[1], NULL);
        } else { queue_msg(uid, to, f[1], "", 2); emit(c, "OK_DEL", f[1], NULL); }
    } else if (!strcmp(cmd, "GOT") && n >= 2) {
        char *end; unsigned long seq = strtoul(f[1], &end, 10); if (*end || !f[1][0]) return;
        for (int i = 0; i < nmsgs; i++) if (msgs[i].seq == seq && msgs[i].to == uid) {
            msg_t m = msgs[i]; memmove(&msgs[i], &msgs[i + 1], sizeof(msg_t) * (size_t)(nmsgs - i - 1)); nmsgs--; mark_dirty();
            if (m.kind == 0 && !m.gid[0]) push(m.frm, "DELIV", m.msgid, users[uid].id, NULL);
            free(m.text); break;
        }
    } else if (!strcmp(cmd, "FUP") && n >= 3) {
        unsigned long size = strtoul(f[1], NULL, 10); int slot = -1; char em[96];
        for (int i = 0; i < 2; i++) if (!c->up[i].data) { slot = i; break; }
        if (size < 1 || size > MAX_FILE) { snprintf(em, sizeof em, "file too large (max %u MB)", MAX_FILE >> 20); emit(c, "FERR", "", em, NULL); return; }
        if (slot < 0 || used_bytes(uid) + size > QUOTA || upload_mem + size > UPLOAD_MEM_MAX) { emit(c, "FERR", "", "upload quota exceeded", NULL); return; }
        unsigned char *buf = malloc(size); if (!buf) { emit(c, "FERR", "", "upload quota exceeded", NULL); return; }
        unsigned char r[8]; char fid[20]; plat_random(r, sizeof r); hexs(fid, r, 8);
        const char *base = f[2]; for (const char *p = f[2]; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        GROW(files, nfiles, capfiles); int fi = nfiles++; file_t *fl = &files[fi]; memset(fl, 0, sizeof *fl);
        copyz(fl->fid, sizeof fl->fid, fid); fl->owner = uid; copyz(fl->name, 81, *base ? base : "file"); fl->size = (uint32_t)size; fl->created = (uint32_t)plat_unix();
        copyz(c->up[slot].fid, sizeof c->up[slot].fid, fid); c->up[slot].data = buf; c->up[slot].got = 0; c->up[slot].size = size; c->up[slot].next = 0; upload_mem += size;
        emit(c, "FUP_OK", fid, NULL);
    } else if (!strcmp(cmd, "FCH") && n >= 4) {
        int slot = up_find(c, f[1]); if (slot < 0) return;
        unsigned char tmp[CHUNK + 8]; long len = b64_decode(f[3], tmp, sizeof tmp); int idx = atoi(f[2]);
        if (len <= 0 || len > CHUNK || idx != c->up[slot].next || c->up[slot].got + (size_t)len > c->up[slot].size || !f[2][0]) {
            char fid[20]; copyz(fid, sizeof fid, f[1]); up_drop(c, slot, true); emit(c, "FERR", fid, "bad chunk", NULL); return;
        }
        memcpy(c->up[slot].data + c->up[slot].got, tmp, (size_t)len); c->up[slot].got += (size_t)len; c->up[slot].next++;
    } else if (!strcmp(cmd, "FEND") && n >= 2) {
        int slot = up_find(c, f[1]); if (slot < 0) { emit(c, "FERR", f[1], "no such upload", NULL); return; }
        char fid[20]; copyz(fid, sizeof fid, f[1]); int fi = find_file(fid);
        bool ok = fi >= 0 && c->up[slot].got == c->up[slot].size && plat_blob_save(fid, c->up[slot].data, c->up[slot].size);
        up_drop(c, slot, !ok);
        if (!ok) { emit(c, "FERR", fid, "size mismatch", NULL); return; }
        files[find_file(fid)].done = 1; mark_dirty(); emit(c, "FEND_OK", fid, NULL);
        { char lb[80]; snprintf(lb, sizeof lb, "[COSMSGD] upload stored: %u bytes\n", files[find_file(fid)].size); plat_log(lb); }
    } else if (!strcmp(cmd, "FGET") && n >= 2) {
        int fi = find_file(f[1]);
        if (fi < 0 || !files[fi].done || !(files[fi].pub || files[fi].owner == uid || acl_has(fi, uid))) { emit(c, "FERR", f[1], "no such file", NULL); return; }
        int slot = -1; for (int i = 0; i < 4; i++) if (!c->dl[i].data) { slot = i; break; }
        if (slot >= 0 && dl_mem + files[fi].size > DL_MEM_MAX) { emit(c, "FERR", f[1], "the server is busy - try again in a moment", NULL); return; }
        size_t len = 0; unsigned char *data = slot >= 0 ? plat_blob_load(files[fi].fid, &len) : NULL;
        if (!data || !len) { free(data); emit(c, "FERR", f[1], "read failed", NULL); return; }
        { char lb[80]; snprintf(lb, sizeof lb, "[COSMSGD] download: %u bytes\n", (unsigned)len); plat_log(lb); }
        dl_mem += len; c->dl[slot].data = data; c->dl[slot].len = len; c->dl[slot].off = 0; c->dl[slot].idx = 0; copyz(c->dl[slot].fid, sizeof c->dl[slot].fid, files[fi].fid);
    } else if (is_group_cmd(cmd)) {
        group_command(c, f, n);
    } else {
        emit(c, "ERR", "unknown command", NULL);
    }
    (void)b;
}

/* ------------------------------------------------------------------ Google sign-in (OAuth 2.0 device flow; see tools/cosmsg_google.py)
 * The user types a short code at google.com/device; this server asks Google for the result, so neither the Google password nor
 * any Google token ever reaches the client. The ID token is checked like OpenID Connect Core 3.1.3.7 requires: RS256 signature
 * against Google's published keys, issuer, audience (our client id), expiry, verified e-mail and an allowed domain. The stable
 * `sub` claim identifies the person. */
typedef struct { char client_id[160], secret[160], domains[160], dev_host[64], tok_host[64], jwks_host[64]; int port; bool require; } gconf_t;
/* google.conf is read at most every second, not for every line a client sends */
static char *conf_cache; static uint64_t conf_at; static bool conf_have;
static char *conf_text(void) {
    uint64_t now = plat_ms();
    if (!conf_at || now - conf_at > 1000) { free(conf_cache); size_t len; conf_cache = plat_conf_load(&len); conf_have = conf_cache != NULL; conf_at = now ? now : 1; }
    return conf_have ? conf_cache : NULL;
}
bool core_conf_get(const char *key, char *out, size_t cap) {
    char *t0 = conf_text(); if (!t0) return false;
    char *t = strdup(t0); if (!t) return false;
    size_t kl = strlen(key); bool found = false;
    for (char *l = t; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        if (!strncmp(l, key, kl) && l[kl] == '=') { copyz(out, cap, l + kl + 1); size_t n = strlen(out); while (n && (out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0; found = true; }
        if (!e) break; l = e + 1;
    }
    free(t); return found;
}
static bool gconf_load(gconf_t *g) {
    memset(g, 0, sizeof *g); char b[160];
    if (!core_conf_get("client_id", g->client_id, sizeof g->client_id) || !core_conf_get("client_secret", g->secret, sizeof g->secret) || !g->client_id[0] || !g->secret[0]) return false;
    if (!core_conf_get("domains", g->domains, sizeof g->domains) || !g->domains[0]) strcpy(g->domains, "gmail.com,googlemail.com");
    if (!core_conf_get("device_host", g->dev_host, sizeof g->dev_host)) strcpy(g->dev_host, "oauth2.googleapis.com");
    if (!core_conf_get("token_host", g->tok_host, sizeof g->tok_host)) strcpy(g->tok_host, "oauth2.googleapis.com");
    if (!core_conf_get("jwks_host", g->jwks_host, sizeof g->jwks_host)) strcpy(g->jwks_host, "www.googleapis.com");
    g->port = core_conf_get("port", b, sizeof b) ? atoi(b) : 443; if (g->port <= 0) g->port = 443;
    g->require = core_conf_get("require", b, sizeof b) && b[0] == '1';
    return true;
}

/* --- a very small JSON reader: enough for Google's flat answers, the JWKS and the ID token's claims --- */
static const char *json_find(const char *j, const char *key) {      /* the value that follows "key": at any depth (first match) */
    char pat[80]; snprintf(pat, sizeof pat, "\"%s\"", key); size_t pl = strlen(pat);
    for (const char *p = j; (p = strstr(p, pat)); p += pl) {
        const char *q = p + pl; while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') continue; q++; while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        return q;
    }
    return NULL;
}
static bool json_str(const char *j, const char *key, char *out, size_t cap) {      /* a string or a bare number/true/false as text */
    const char *v = json_find(j, key); if (!v) return false; size_t o = 0;
    if (*v == '"') {
        for (v++; *v && *v != '"' && o + 1 < cap; v++) {
            if (*v == '\\' && v[1]) { v++; out[o++] = *v == 'n' ? '\n' : *v == 't' ? '\t' : *v; } else out[o++] = *v;
        }
    } else { while (*v && *v != ',' && *v != '}' && *v != ' ' && *v != '\n' && *v != '\r' && o + 1 < cap) out[o++] = *v++; }
    out[o] = 0; return true;
}
static long b64url_decode(const char *in, unsigned char *out, size_t cap) {
    char tmp[3100]; size_t L = strlen(in); if (L > 3000) return -1;
    size_t k = 0; for (size_t i = 0; i < L; i++) tmp[k++] = in[i] == '-' ? '+' : in[i] == '_' ? '/' : in[i];
    while (k % 4) tmp[k++] = '='; tmp[k] = 0; return b64_decode(tmp, out, cap);
}
static char *jwks_cache; static uint64_t jwks_at;                 /* Google's published keys, refetched hourly */

static bool domain_allowed(const char *domains, const char *dom) {
    const char *p = domains; size_t dl = strlen(dom);
    while (*p) { const char *e = strchr(p, ','); size_t n = e ? (size_t)(e - p) : strlen(p); if (n == dl && !strncmp(p, dom, n)) return true; if (!e) break; p = e + 1; }
    return false;
}
/* NULL when the token is good (sub and email are filled in), otherwise the reason */
static const char *verify_id_token(const char *token, const gconf_t *g, char *sub, size_t subcap, char *email, size_t emailcap) {
    char h64[400], p64[1800], s64[700]; const char *d1 = strchr(token, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || strchr(d2 + 1, '.')) return "malformed token";
    if ((size_t)(d1 - token) >= sizeof h64 || (size_t)(d2 - d1 - 1) >= sizeof p64 || strlen(d2 + 1) >= sizeof s64) return "malformed token";
    memcpy(h64, token, (size_t)(d1 - token)); h64[d1 - token] = 0; memcpy(p64, d1 + 1, (size_t)(d2 - d1 - 1)); p64[d2 - d1 - 1] = 0; copyz(s64, sizeof s64, d2 + 1);
    unsigned char hdr[400], claims[1800], sig[600]; long hl = b64url_decode(h64, hdr, sizeof hdr - 1), cl = b64url_decode(p64, claims, sizeof claims - 1), sl = b64url_decode(s64, sig, sizeof sig);
    if (hl < 0 || cl < 0 || sl <= 0) return "malformed token";
    hdr[hl] = 0; claims[cl] = 0;
    char alg[16], kid[80]; if (!json_str((char *)hdr, "alg", alg, sizeof alg) || strcmp(alg, "RS256")) return "unsupported signature algorithm";
    if (!json_str((char *)hdr, "kid", kid, sizeof kid)) return "unknown signing key";
    if (!jwks_cache) return "unknown signing key";
    /* find the key with this kid: walk the JWKS objects */
    unsigned char nb[520], eb[16]; long nl = -1, el = -1; bool found = false;
    for (const char *p = jwks_cache; (p = strchr(p, '{')); p++) {
        const char *e = strchr(p, '}'); if (!e) break;
        size_t ol = (size_t)(e - p) + 1; char *obj = malloc(ol + 1); if (!obj) break; memcpy(obj, p, ol); obj[ol] = 0;
        char okid[80], kty[16], n64[760], e64[24];
        if (json_str(obj, "kid", okid, sizeof okid) && !strcmp(okid, kid) && json_str(obj, "kty", kty, sizeof kty) && !strcmp(kty, "RSA") && json_str(obj, "n", n64, sizeof n64) && json_str(obj, "e", e64, sizeof e64)) {
            nl = b64url_decode(n64, nb, sizeof nb); el = b64url_decode(e64, eb, sizeof eb); found = nl > 0 && el > 0;
        }
        free(obj); if (found) break;
    }
    if (!found) return "unknown signing key";
    {   br_rsa_public_key pk = { nb, (size_t)nl, eb, (size_t)el }; unsigned char want[32], got[32]; br_sha256_context hc;
        br_sha256_init(&hc); br_sha256_update(&hc, token, (size_t)(d2 - token)); br_sha256_out(&hc, want);
        if (!br_rsa_i31_pkcs1_vrfy(sig, (size_t)sl, BR_HASH_OID_SHA256, 32, &pk, got) || memcmp(got, want, 32) != 0) return "bad signature"; }
    char iss[80], aud[200], tmp[40], ev[16]; uint64_t now = plat_unix();
    if (!json_str((char *)claims, "iss", iss, sizeof iss) || (strcmp(iss, "https://accounts.google.com") && strcmp(iss, "accounts.google.com"))) return "wrong issuer";
    const char *av = json_find((char *)claims, "aud"); bool aud_ok = false;
    if (av && *av == '[') { char pat[200]; snprintf(pat, sizeof pat, "\"%s\"", g->client_id); const char *close = strchr(av, ']'); const char *hit = strstr(av, pat); aud_ok = hit && close && hit < close; }
    else if (json_str((char *)claims, "aud", aud, sizeof aud)) aud_ok = !strcmp(aud, g->client_id);
    if (!aud_ok) return "wrong audience";
    if (!json_str((char *)claims, "exp", tmp, sizeof tmp) || (uint64_t)atoll(tmp) <= now) return "token expired";
    if (json_str((char *)claims, "iat", tmp, sizeof tmp) && (uint64_t)atoll(tmp) > now + 300) return "token issued in the future";
    if (!json_str((char *)claims, "sub", sub, subcap) || !sub[0]) return "no subject";
    if (!json_str((char *)claims, "email_verified", ev, sizeof ev) || strcmp(ev, "true")) return "e-mail address not verified";
    if (!json_str((char *)claims, "email", email, emailcap)) return "e-mail address not verified";
    lower_ascii(email); const char *at = strrchr(email, '@');
    if (!at || !domain_allowed(g->domains, at + 1)) return "only gmail.com / googlemail.com addresses can sign in";
    return NULL;
}
static void mask_email(const char *em, char *out, size_t cap) { const char *at = strrchr(em, '@'); snprintf(out, cap, "%.1s***@%s", em, at ? at + 1 : ""); }
static int gacc_of_uid(int uid) { for (int i = 0; i < ngaccs; i++) if (gaccs[i].uid == uid) return i; return -1; }
static int gacc_of_sub(const char *sub) { for (int i = 0; i < ngaccs; i++) if (!strcmp(gaccs[i].sub, sub)) return i; return -1; }
static void google_status(core_conn_t *c) {
    gconf_t g; bool conf = gconf_load(&g); int a = c->uid >= 0 ? gacc_of_uid(c->uid) : -1; char mk[120] = "";
    if (a >= 0) mask_email(gaccs[a].email, mk, sizeof mk);
    emit(c, "GOOGLE_STATUS", a >= 0 ? "1" : "0", mk, conf && g.require ? "1" : "0", conf ? "1" : "0", NULL);
}

/* outstanding requests to Google */
static struct { int id; int conn; uint32_t gen; int kind; } gjobs[8];       /* kind 1 device code, 2 token poll, 3 key set */
static core_conn_t *gjob_conn(int slot) { core_conn_t *c = &conns[gjobs[slot].conn]; return c->used && c->gen == gjobs[slot].gen ? c : NULL; }
static bool gjob_start(core_conn_t *c, int kind, bool post, const char *host, int port, const char *path, const char *body) {
    int slot = -1; for (int i = 0; i < 8; i++) if (!gjobs[i].id) { slot = i; break; }
    int id = slot >= 0 ? plat_http(post, host, port, path, body) : 0;
    if (!id) return false;
    gjobs[slot].id = id; gjobs[slot].conn = (int)(c - conns); gjobs[slot].gen = c->gen; gjobs[slot].kind = kind; return true;
}
static void url_enc(const char *s, char *out, size_t cap) { size_t o = 0; for (; *s && o + 4 < cap; s++) { if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.' || *s == '~') out[o++] = *s; else o += (size_t)snprintf(out + o, 4, "%%%02X", (unsigned char)*s); } out[o] = 0; }

static void google_start(core_conn_t *c, int mode, const char *name) {
    gconf_t g; if (!gconf_load(&g)) { emit(c, "ERR", "this server has no Google sign-in", NULL); return; }
    char cid[400], body[600]; url_enc(g.client_id, cid, sizeof cid); snprintf(body, sizeof body, "client_id=%s&scope=openid%%20email", cid);
    memset(&c->gf, 0, sizeof c->gf); c->gf.mode = mode; copyz(c->gf.name, sizeof c->gf.name, name);
    if (!gjob_start(c, 1, true, g.dev_host, g.port, "/device/code", body)) emit(c, "ERR", "Google sign-in is busy - try again in a moment", NULL);
}

static void google_finish(core_conn_t *c, const char *sub, const char *email) {            /* the verified person: link, or sign in / register */
    uint64_t now = plat_unix(); int mode = c->gf.mode; char name[96]; copyz(name, sizeof name, c->gf.name); char mk[120]; mask_email(email, mk, sizeof mk);
    memset(&c->gf, 0, sizeof c->gf);
    int row = gacc_of_sub(sub);
    if (mode == 2) {
        int uid = c->uid;
        if (row >= 0 && gaccs[row].uid != uid) { emit(c, "ERR", "that Google account is already linked to another C-OS account", NULL); return; }
        if (gacc_of_uid(uid) >= 0 && row < 0) { emit(c, "ERR", "this account is already linked to a different Google account", NULL); return; }
        if (row < 0) { GROW(gaccs, ngaccs, capgaccs); row = ngaccs++; }
        copyz(gaccs[row].sub, sizeof gaccs[row].sub, sub); gaccs[row].uid = uid; copyz(gaccs[row].email, sizeof gaccs[row].email, email); gaccs[row].ts = (uint32_t)now; mark_dirty();
        emit(c, "GOOGLE_OK", mk, NULL); return;
    }
    int ui; char token[28];
    if (row >= 0) {                                       /* the same person on a new device: same account, new token, old tokens die */
        ui = gaccs[row].uid; new_token_for(ui, token); copyz(gaccs[row].email, sizeof gaccs[row].email, email); gaccs[row].ts = (uint32_t)now;
    } else {
        if (nusers >= MAX_USERS) { emit(c, "ERR", "this server is full", NULL); return; }
        char id[12]; for (;;) { snprintf(id, sizeof id, "%u", 10000000u + rnd_below(90000000u)); if (find_user(id) < 0) break; }
        GROW(users, nusers, cap_users_); ui = nusers++; user_t *u = &users[ui]; memset(u, 0, sizeof *u);
        char nm[96]; copyz(nm, sizeof nm, name); char *s = strip(nm); cut_chars(s, MAX_NAME); if (!*s) s = "user";
        copyz(u->id, sizeof u->id, id); copyz(u->name, sizeof u->name, s); u->created = (uint32_t)now; new_token_for(ui, token);
        GROW(gaccs, ngaccs, capgaccs); row = ngaccs++; copyz(gaccs[row].sub, sizeof gaccs[row].sub, sub); gaccs[row].uid = ui; copyz(gaccs[row].email, sizeof gaccs[row].email, email); gaccs[row].ts = (uint32_t)now;
    }
    mark_dirty();
    emit(c, "OK_REG", users[ui].id, token, users[ui].name, NULL); emit(c, "GOOGLE_OK", mk, NULL); on_login(c, ui);
}

static void google_check_token(core_conn_t *c) {                    /* c->gf.idtok holds an ID token and the key set is available */
    gconf_t g; if (!gconf_load(&g)) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "this server has no Google sign-in", NULL); return; }
    char sub[80], email[100]; const char *err = verify_id_token(c->gf.idtok, &g, sub, sizeof sub, email, sizeof email);
    if (err && !strcmp(err, "unknown signing key") && !c->gf.jwks_retry) {                 /* Google rotated its keys: fetch them once more */
        c->gf.jwks_retry = true;
        if (gjob_start(c, 3, false, g.jwks_host, g.port, "/oauth2/v3/certs", NULL)) return;
    }
    if (err) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", err, NULL); return; }
    google_finish(c, sub, email);
}

void core_http_done(int job, int status, const char *body, size_t len) {
    (void)len; int slot = -1; for (int i = 0; i < 8; i++) if (gjobs[i].id == job) slot = i;
    if (slot < 0) return;
    core_conn_t *c = gjob_conn(slot); int kind = gjobs[slot].kind; gjobs[slot].id = 0;
    if (!c) return;                                                   /* the person went away */
    gconf_t g; if (!gconf_load(&g)) { emit(c, "ERR", "this server has no Google sign-in", NULL); return; }
    char tmp[400];
    if (kind == 1) {
        char dev[256], uc[64], url[160], ex[16], iv[16];
        if (status != 200 || !json_str(body, "device_code", dev, sizeof dev) || !json_str(body, "user_code", uc, sizeof uc)) {
            char em[200]; if (!status) snprintf(em, sizeof em, "cannot reach Google"); else { char er[60] = ""; json_str(body, "error", er, sizeof er); snprintf(em, sizeof em, "Google refused to start a sign-in (%s)", er[0] ? er : "error"); }
            memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", em, NULL); return;
        }
        if (!json_str(body, "verification_url", url, sizeof url) && !json_str(body, "verification_uri", url, sizeof url)) strcpy(url, "https://www.google.com/device");
        int expires = json_str(body, "expires_in", ex, sizeof ex) ? atoi(ex) : 600; int interval = json_str(body, "interval", iv, sizeof iv) ? atoi(iv) : 5; if (interval < 3) interval = 3;
        uint64_t now = plat_unix(); copyz(c->gf.dev, sizeof c->gf.dev, dev); c->gf.interval = interval; c->gf.exp = now + (uint64_t)expires; c->gf.next = now + (uint64_t)interval; c->gf.active = true;
        char e2[16], i2[16]; snprintf(e2, sizeof e2, "%d", expires); snprintf(i2, sizeof i2, "%d", interval); emit(c, "GOOGLE_CODE", uc, url, e2, i2, NULL);
    } else if (kind == 2) {
        char er[64] = "", idt[2048];
        if (status != 200 && !status) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "cannot reach Google", NULL); return; }
        if (json_str(body, "error", er, sizeof er)) {
            if (!strcmp(er, "authorization_pending")) { emit(c, "GOOGLE_PENDING", NULL); return; }
            if (!strcmp(er, "slow_down")) { c->gf.interval += 5; emit(c, "GOOGLE_PENDING", NULL); return; }
            if (!strcmp(er, "access_denied")) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "the Google sign-in was cancelled", NULL); return; }
            if (!strcmp(er, "expired_token") || !strcmp(er, "invalid_grant")) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "the code expired - start again", NULL); return; }
        }
        if (status != 200 || !json_str(body, "id_token", idt, sizeof idt)) { snprintf(tmp, sizeof tmp, "Google sign-in failed (%s)", er[0] ? er : "error"); memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", tmp, NULL); return; }
        copyz(c->gf.idtok, sizeof c->gf.idtok, idt); c->gf.jwks_retry = false;
        if (!jwks_cache || plat_ms() - jwks_at > 3600000) { if (gjob_start(c, 3, false, g.jwks_host, g.port, "/oauth2/v3/certs", NULL)) return; memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "Google sign-in is busy - try again in a moment", NULL); return; }
        google_check_token(c);
    } else if (kind == 3) {
        if (status == 200 && strstr(body, "\"keys\"")) { free(jwks_cache); jwks_cache = strdup(body); jwks_at = plat_ms(); }
        else if (!jwks_cache) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "cannot fetch Google's keys", NULL); return; }
        if (c->gf.idtok[0]) google_check_token(c);
    }
}
static void google_poll(core_conn_t *c) {
    if (!c->gf.active) { emit(c, "ERR", "no Google sign-in in progress", NULL); return; }
    uint64_t now = plat_unix(); gconf_t g;
    if (now > c->gf.exp) { memset(&c->gf, 0, sizeof c->gf); emit(c, "ERR", "the code expired - start again", NULL); return; }
    if (now < c->gf.next) { emit(c, "GOOGLE_PENDING", NULL); return; }                // asked too early: do not bother Google
    if (c->gf.job || c->gf.idtok[0]) { emit(c, "GOOGLE_PENDING", NULL); return; }
    if (!gconf_load(&g)) { emit(c, "ERR", "this server has no Google sign-in", NULL); return; }
    c->gf.next = now + (uint64_t)c->gf.interval;
    char cid[400], sec[400], dev[600], body[1600]; url_enc(g.client_id, cid, sizeof cid); url_enc(g.secret, sec, sizeof sec); url_enc(c->gf.dev, dev, sizeof dev);
    snprintf(body, sizeof body, "client_id=%s&client_secret=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code", cid, sec, dev);
    if (!gjob_start(c, 2, true, g.tok_host, g.port, "/token", body)) emit(c, "GOOGLE_PENDING", NULL);
}

/* registrations per source address and hour (the table is small and old entries are reused; loopback and unknown addresses are exempt) */
static bool reg_allowed(uint32_t ip) {
    static struct { uint32_t ip; uint32_t win; int n; } t[64]; uint32_t now = (uint32_t)plat_unix();
    if (!ip || (ip >> 24) == 127) return true;
    int free_i = 0, oldest = 0;
    for (int i = 0; i < 64; i++) {
        if (t[i].ip == ip) { if (now - t[i].win > 3600) { t[i].win = now; t[i].n = 0; } return ++t[i].n <= REG_PER_IP; }
        if (t[i].win < t[oldest].win) oldest = i;
        if (!t[i].ip) free_i = i;
    }
    int k = t[free_i].ip ? oldest : free_i; t[k].ip = ip; t[k].win = now; t[k].n = 1; return true;
}
static bool handle_line(core_conn_t *c, char *line) {
    uint64_t now = plat_ms();
    if (now - c->window > 5000) { c->window = now; c->count = 0; }
    bool fch = !strncmp(line, "FCH\t", 4) && c->uid >= 0 && (c->up[0].data || c->up[1].data);   /* upload chunks are free, but only while an upload is open */
    if (!fch && ++c->count > 40) return false;                       /* flood */
    char *f[16]; int n = 0;
    { char *p = line; f[n++] = p; for (; *p; p++) if (*p == '\t' && n < 15) { *p = 0; f[n++] = p + 1; } }
    for (int i = 0; i < n; i++) { char *r = f[i], *w = f[i]; while (*r) { if (*r == '\\' && r[1]) { r++; *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r; r++; } else *w++ = *r++; } *w = 0; }
    const char *cmd = f[0];
    if (!strcmp(cmd, "PING")) { emit(c, "PONG", NULL); return true; }
    if (c->uid < 0) {
        gconf_t gc; bool gconf = gconf_load(&gc);
        if (!strcmp(cmd, "REG") && n >= 2 && gconf && gc.require) emit(c, "NEED_GOOGLE", NULL);
        else if (!strcmp(cmd, "GREG")) google_start(c, 1, n > 1 ? f[1] : "");
        else if (!strcmp(cmd, "GPOLL")) google_poll(c);
        else if (!strcmp(cmd, "REG") && n >= 2) {
            char name[96]; copyz(name, sizeof name, f[1]); char *s = strip(name); cut_chars(s, MAX_NAME); if (!*s) s = "user";
            if (nusers >= MAX_USERS) { emit(c, "ERR", "this server is full", NULL); return true; }
            if (!reg_allowed(c->ip)) { emit(c, "ERR", "too many registrations from your address - try again later", NULL); return true; }
            char id[12]; for (;;) { snprintf(id, sizeof id, "%u", 10000000u + rnd_below(90000000u)); if (find_user(id) < 0) break; }
            GROW(users, nusers, cap_users_); int ui = nusers++; user_t *u = &users[ui]; memset(u, 0, sizeof *u);
            copyz(u->id, sizeof u->id, id); copyz(u->name, sizeof u->name, s); u->created = (uint32_t)plat_unix();
            char token[28]; new_token_for(ui, token); mark_dirty();
            emit(c, "OK_REG", id, token, u->name, NULL); on_login(c, ui);
        } else if (!strcmp(cmd, "LOGIN") && n >= 3) {
            uint64_t t = plat_unix(); int fi = lf_find(f[1]);
            if (fi >= 0 && login_fail[fi].fails >= 5 && t - login_fail[fi].since < 600) { emit(c, "ERR", "too many failed logins - wait a few minutes", NULL); c->close_after = true; return true; }
            int ui = find_user(f[1]); char h[80]; tok_hash(f[2], h);
            if (ui < 0 || !ct_eq(users[ui].tok, h)) {
                if (fi < 0 && nlogin_fail < 256) { fi = nlogin_fail++; copyz(login_fail[fi].id, sizeof login_fail[fi].id, f[1]); login_fail[fi].fails = 0; login_fail[fi].since = t; }
                if (fi >= 0) { if (t - login_fail[fi].since >= 600) { login_fail[fi].fails = 0; login_fail[fi].since = t; } login_fail[fi].fails++; }
                emit(c, "ERR", "bad id or token", NULL); c->close_after = true; return true;
            }
            if (fi >= 0) login_fail[fi] = login_fail[--nlogin_fail];
            emit(c, "OK_LOGIN", users[ui].id, users[ui].name, NULL); on_login(c, ui);
        } else emit(c, "ERR", "login first", NULL);
        return true;
    }
    if (!strcmp(cmd, "GLINK")) google_start(c, 2, "");
    else if (!strcmp(cmd, "GPOLL")) google_poll(c);
    else if (!strcmp(cmd, "GSTATUS")) google_status(c);
    else if (!strcmp(cmd, "LOGOUT_ALL")) { char token[28]; new_token_for(c->uid, token); emit(c, "OK_LOGOUT_ALL", token, NULL); }
    else if (!strcmp(cmd, "REG") || !strcmp(cmd, "LOGIN")) emit(c, "ERR", "unknown command", NULL);
    else if (({ gconf_t gq; gconf_load(&gq) && gq.require; }) && gacc_of_uid(c->uid) < 0 && strcmp(cmd, "NAME") && strcmp(cmd, "ME")) emit(c, "ERR", "sign in with Google first (/google)", NULL);
    else do_command(c, f, n);
    return true;
}

/* ------------------------------------------------------------------ connections */
void core_init(void) { load_state(); }

core_conn_t *core_open(void) {
    for (int i = 0; i < MAX_CONNS; i++) if (!conns[i].used) {
        core_conn_t *c = &conns[i]; memset(c, 0, sizeof *c); c->used = true; c->uid = -1; c->gen = ++conn_gen; c->last_rx = plat_ms(); c->window = plat_ms(); c->opened = c->last_rx;
        c->in = malloc(MAX_LINE + 2); if (!c->in) { c->used = false; return NULL; }
        return c;
    }
    return NULL;
}
void core_set_peer(core_conn_t *c, uint32_t ip) { if (c) c->ip = ip; }
bool core_conn_closing(core_conn_t *c) { return c && c->close_after; }
uint64_t core_conn_age_ms(core_conn_t *c) { return c ? plat_ms() - c->opened : 0; }
bool core_conn_loggedin(core_conn_t *c) { return c && c->uid >= 0; }
void core_close(core_conn_t *c) {
    if (!c || !c->used) return;
    int uid = c->uid;
    for (int i = 0; i < 2; i++) if (c->up[i].data) up_drop(c, i, true);        /* unfinished uploads are discarded */
    for (int i = 0; i < 4; i++) if (c->dl[i].data) { dl_mem -= c->dl[i].len; free(c->dl[i].data); }
    free(c->in); free(c->out); c->used = false; c->uid = -1;
    if (uid >= 0 && !is_online(uid)) { users[uid].last_seen = (uint32_t)plat_unix(); mark_dirty(); broadcast_presence(uid); }
}
bool core_feed(core_conn_t *c, const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        if (ch == '\n') {
            if (c->in_skip) { c->in_skip = false; c->in_len = 0; return false; }       /* a line over MAX_LINE: Python drops the connection */
            c->in[c->in_len] = 0; if (c->in_len && c->in[c->in_len - 1] == '\r') c->in[--c->in_len] = 0;
            c->in_len = 0;
            if (!valid_utf8(c->in)) { /* Python decodes with errors="replace": keep going with the raw bytes */ }
            c->last_rx = plat_ms();                                                      /* idle time counts whole lines: a trickle of bytes does not keep a connection */
            if (!handle_line(c, c->in)) return false;
            if (c->close_after) return true;
        } else if (c->in_len < MAX_LINE) c->in[c->in_len++] = ch;
        else c->in_skip = true;
    }
    return true;
}
/* streams file chunks while the output queue is short, so a 10 MB download never fills it */
static void pump_downloads(core_conn_t *c) {
    for (int i = 0; i < 4; i++) {
        while (c->dl[i].data && c->out_len - c->out_pos < 32768 && !c->close_after) {
            size_t left = c->dl[i].len - c->dl[i].off, k = left < CHUNK ? left : CHUNK; char b64[CHUNK * 4 / 3 + 8], ix[24];
            b64_encode(c->dl[i].data + c->dl[i].off, k, b64);
            bool last = k == left; emit(c, "FDATA", c->dl[i].fid, num(ix, (unsigned)c->dl[i].idx), last ? "1" : "0", b64, NULL);
            c->dl[i].off += k; c->dl[i].idx++;
            if (last) { dl_mem -= c->dl[i].len; free(c->dl[i].data); memset(&c->dl[i], 0, sizeof c->dl[i]); }
        }
    }
}
const char *core_out(core_conn_t *c, size_t *n) { pump_downloads(c); *n = c->out_len - c->out_pos; return c->out + c->out_pos; }
void core_out_ack(core_conn_t *c, size_t n) { c->out_pos += n; if (c->out_pos == c->out_len) c->out_pos = c->out_len = 0; }
bool core_wants_close(core_conn_t *c) { return c->close_after && c->out_pos == c->out_len; }

void core_tick(void) {
    uint64_t now = plat_ms();
    if (dirty && now - dirty_since > 1500) save_state();
    static uint64_t last_purge; if (now - last_purge > 3600000 || !last_purge) {          /* attachments are kept for two weeks (profile photos stay) */
        last_purge = now; uint32_t old = (uint32_t)plat_unix() - FILE_TTL;
        for (int i = 0; i < nfiles;) { if (files[i].done && !files[i].pub && files[i].created < old) { file_remove_at(i); mark_dirty(); } else i++; }
    }
    static uint64_t last_expire; if (now - last_expire > 3600000 || !last_expire) {                  /* events nobody collected for two weeks go (as attachments do) */
        last_expire = now; uint32_t old = (uint32_t)plat_unix() - BACKLOG_TTL; int dropped = 0;
        for (int i = 0; i < nmsgs;) { if (msgs[i].ts && msgs[i].ts < old) { free(msgs[i].text); memmove(&msgs[i], &msgs[i + 1], sizeof(msg_t) * (size_t)(nmsgs - i - 1)); nmsgs--; dropped++; } else i++; }
        if (dropped) mark_dirty();
    }
    for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used) {
        if (now - conns[i].last_rx > IDLE_MS || (conns[i].uid < 0 && now - conns[i].opened > LOGIN_MS)) conns[i].close_after = true;
    }
}
