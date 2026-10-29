/* messenger.c-os - C-OS Message: a LINE-style chat between C-OS machines, as a ring-3 program.
 *
 * Transport: one UDP socket on port 5150. Every message carries an id and is acknowledged by the peer; the sender resends
 * it every RETRY_MS up to MAX_TRIES times, then marks it failed. The receiver ACKs duplicates too but shows them once.
 * Peers are identified by IPv4 address (contacts.txt: "<ip> <name>"). History is kept per contact in
 * /messenger/chat_<ip-hex>.txt, one line per message: "<mine>\t<ts>\t<state>\t<text>".
 * Internet mode (strangers): a relay server (tools/cosmsg_server.py, TCP 5151) gives every user a C-OS ID. Friends are made with
 * a request that the other side accepts; messages to friends are stored by the server until delivered. Server friends
 * are contacts with a uid (chat_u<uid>.txt); LAN contacts are keyed by IP.
 * Input commands: /server <host[:port]>  /friend <id> [hello]  /accept <id>  /decline <id>  /block <id>  /id
 *                 /add <ip> [name]  /name <my name>  /del  /scan  /help
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cos.h"
#include "cos_ui.h"
#include "cos_popup.h"
#include "bearssl.h"
#include <strings.h>
#include <ctype.h>
#include "cosime.h"
#include "e2e.inc"
#include "i18n.inc"
static void i18n_poll(bool force);

#define PORT        5150
#define DIR         "/messenger"
#define MAX_CONTACTS 128
#define MAX_MSGS    400
#define MAX_TEXT    600
#define RETRY_MS    1500
#define MAX_TRIES   6
#define LIST_W      250
#define HEAD_H      104
#define REQ_H       64
#define SRV_PORT    5151
#define IME_DIC     "/system/ime/cosime.dic"
#define IME_PREF    DIR "/ime.txt"
#define NAME_LEN    64
#define LAN_TEXT    1300        /* bytes of text in one UDP packet (an encrypted message is longer than the plain text) */
#define INPUT_H     56

#define BG      0x151A24
#define PANEL   0x1C2333
#define LINE    0x2B3550
#define TEXT    0xE4E9F2
#define MUTED   0x7C8AA5
#define ACCENT  0x4C9AFF
#define SELBG   0x26324D
#define MINE    0x2F7D4F
#define THEIRS  0x2A3350
#define WARN    0xF2B84B
#define BAD     0xE5645A

enum { T_MSG = 1, T_ACK = 2, T_HELLO = 3, T_HELLO_R = 4, T_EDIT = 5, T_DEL = 6, T_EVACK = 7, T_KEYREQ = 8, T_KEY = 9 };
enum { EV_NONE = 0, EV_EDIT = 1, EV_DEL = 2 };
#define MF_EDITED 1
#define MF_DELETED 2
#define MF_RSENT 4                      /* we told the sender we read this message */
enum { ST_SENDING = 0, ST_SENT = 1, ST_FAILED = 2, ST_RECV = 3, ST_DELIVERED = 4, ST_READ = 5 };
enum { SV_OFF = 0, SV_CONNECTING, SV_TLS, SV_LOGIN, SV_ONLINE };

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t type, pad[3];
    uint32_t id, ts;
    char name[24];
} hdr_t;

/* id: our own message id (mine); rid: the sender's id of a received message, which is what edit/delete events refer to. pend: an edit/delete the peer has not confirmed yet. */
typedef struct {
    uint32_t id, rid, ts, evts; uint8_t mine, state, tries, flags, pend; uint64_t sent_ms; char *text;      /* evts: the time of the newest edit or delete we accepted (an old one cannot be replayed over it) */
    uint8_t att, apct;                         /* attachment state (attach.inc) and transfer progress */
    char sid[12], who[16];                     /* group chats: who sent a received message (C-OS ID, and the name it had at the time) */
    uint32_t *thumb; int16_t tw, th;           /* decoded thumbnail (a weak pointer into the thumbnail cache) and its size */
} msg_t;
typedef struct {
    char name[NAME_LEN];
    uint32_t ip;
    char uid[12];                       /* non-empty: a friend made through the server (ip is 0) */
    char handle[24], avatar[24];        /* profile: @handle and the file id of the profile photo */
    struct gmem { char uid[12], name[24]; uint8_t role; uint32_t readup; } *gm; int ngm;   /* role 0 member, 1 sub-admin, 2 owner; readup: how far they have read our messages */
    uint32_t ann_t;                                            /* the signed time of the accepted announcement (an older genuine one is not taken back) */
    char ann[304], ann_by[12];                                 /* the pinned announcement */      /* group chats (uid starts with 'g'): the members the server told us about */
    char owner[12];
    msg_t *m; int n, cap;
    int unread;
    uint8_t pinned, muted, mention;                  /* pinned: on top of the list; muted: no chime and no count in the title; mention: somebody wrote @you */
    char *draft;                                     /* the half-written message of this chat */
    uint64_t typing_until; char typing_name[24];     /* someone is typing to us (server friends and groups) */
    uint8_t pres; uint32_t pres_ts;                  /* 0 unknown, 1 online, 2 offline since pres_ts, 3 hidden */
    uint32_t seen[16]; int nseen;
    bool loaded;
} contact_t;
/* End-to-end keys of one person (a friend or a fellow group member), looked up by C-OS ID: ks 0 unknown, 1 pinned, 2 verified by the user;
 * kchanged: a different identity key was seen (kb stays the pinned one, knb is the new one waiting for a decision). */
typedef struct { char uid[12]; uint8_t ks, kchanged, kchecked, kwait, has_kb; uint8_t kfp[32]; e2e_bundle_t kb, knb; uint64_t kfetch, kreq_ms; } pkey_t;
#define KR_MAX 2048
static pkey_t kr[KR_MAX]; static int nkr;
static pkey_t *pkr_find(const char *uid) {
    if (strlen(uid) >= sizeof kr[0].uid) return NULL;                          /* a longer ID can never be one of ours: never compare truncated */
    for (int i = 0; i < nkr; i++) if (!strcmp(kr[i].uid, uid)) return &kr[i];
    return NULL;
}
static pkey_t *pkr(const char *uid) {
    static pkey_t dummy; pkey_t *p = pkr_find(uid); if (p) return p;
    if (strlen(uid) < sizeof kr[0].uid) {
        int slot = nkr < KR_MAX ? nkr : -1;
        for (int i = 0; slot < 0 && i < nkr; i++) if (!kr[i].ks && !kr[i].kwait) slot = i;          /* full: reuse an entry that holds nothing worth keeping */
        if (slot >= 0) { if (slot == nkr) nkr++; memset(&kr[slot], 0, sizeof kr[0]); snprintf(kr[slot].uid, sizeof kr[slot].uid, "%s", uid); return &kr[slot]; }
    }
    memset(&dummy, 0, sizeof dummy); return &dummy;                           /* never shared state between two people */
}
/* the key of a contact: a friend by C-OS ID, a LAN contact by its IP address ("ip:xxxxxxxx") */
static const char *ckey(const contact_t *k) { static char b[4][16]; static int n; char *o = b[n++ & 3]; if (k->uid[0]) snprintf(o, 16, "%s", k->uid); else snprintf(o, 16, "ip:%08x", k->ip); return o; }
#define KP(k) pkr(ckey(k))

static cui_canvas c;
static cui_font *f_t, *f_s, *f_b, *f_h;
static int win_w, win_h, lh;
static int sock = -1;
static contact_t ct[MAX_CONTACTS]; static int nct, cur = -1;
static inline bool is_group(const contact_t *k) { return k->uid[0] == 'g'; }
static char my_name[NAME_LEN]; static uint32_t my_ip;
static char my_handle[24], my_bio[128], my_avatar[24], my_invite[16]; static bool my_disc;
static char in[MAX_TEXT]; static int in_len, in_pos;      /* the input line (UTF-8) and the caret's byte offset */
static cosime *ime; static bool ime_tried; static int64_t g_win; static int ime_popup_x;
static int sel_msg = -1, edit_msg = -1;          /* selected own message / the message being edited (indices into the open chat) */
typedef struct { int msg, x, y, w, h; int kind; } hit_t;     /* kind 0 bubble, 1 Edit, 2 Delete */
static hit_t hits[160]; static int nhits;
/* kinds: 0 bubble, 1 Edit, 2 Delete, 3/4 attachment, 5 Reply, 6 Copy, 7 Link */
static int div_idx = -1;                         /* the first unread message of the open chat ("ここから未読") */
static char reply_hdr[160]; static char reply_name[24], reply_snip[64];   /* the quote attached to the next message */
/* files chosen with "+" or dropped, waiting in the message box (attach.inc) */
#define STG_MAX  8
#define STG_H    40
#define PICK_OUT "/tmp/cos_msg_pick.txt"
static struct { char path[300]; char name[96]; uint32_t size; } stg[STG_MAX];
static int nstg;
static struct { int ci, n, head; char path[STG_MAX][300]; char text[MAX_TEXT]; bool has_text; } sq;      /* the send queue: uploads run one at a time */
static int64_t pick_pid;

/* A reply is "\x01Q" name "\x1f" snippet "\x02" body. Everything that shows or searches a message uses the body. */
static const char *msg_body(const char *t) {
    if (t[0] == 1 && t[1] == 'Q') { const char *s = strchr(t, 2); if (s) return s + 1; }
    return t;
}
static bool msg_quote(const char *t, char *name, size_t nn, char *snip, size_t ns) {
    if (t[0] != 1 || t[1] != 'Q') return false;
    const char *u = strchr(t + 2, 0x1f), *e = strchr(t, 2);
    if (!u || !e || u > e) return false;
    size_t a = (size_t)(u - (t + 2)), b = (size_t)(e - (u + 1));
    if (a >= nn) a = nn - 1;
    if (b >= ns) b = ns - 1;
    memcpy(name, t + 2, a); name[a] = 0; memcpy(snip, u + 1, b); snip[b] = 0;
    return true;
}
/* copy up to n bytes of one line, dropping control bytes and never cutting a UTF-8 character */
static void clean_snip(char *out, size_t cap, const char *src, int maxb) {
    size_t o = 0; const unsigned char *p = (const unsigned char *)src;
    for (; *p && (int)o < maxb && o + 1 < cap; p++) {
        if (*p < 32) { if ((*p == '\n' || *p == '\t') && o && out[o - 1] != ' ') out[o++] = ' '; continue; }
        out[o++] = (char)*p;
    }
    if (*p && (*p & 0xC0) == 0x80) { while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--; if (o > 0) o--; }   /* cut inside a character */
    out[o] = 0;
}

static bool first_url(const char *t, char *out, size_t cap) {
    const char *p = strstr(t, "https://"); if (!p) p = strstr(t, "http://"); if (!p) return false;
    size_t n = 0; while (p[n] && (unsigned char)p[n] > ' ' && n + 1 < cap) n++;
    memcpy(out, p, n); out[n] = 0; return true;
}
/* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2));
}
static void fmt_day(uint32_t ts, char *out, size_t n) {
    int64_t day = ts / 86400, today = (int64_t)(cos_time_unix() / 86400);
    if (day == today) { snprintf(out, n, "今日"); return; }
    if (day == today - 1) { snprintf(out, n, "昨日"); return; }
    int y, m, d; civil_from_days(day, &y, &m, &d); int ty, tm, td; civil_from_days(today, &ty, &tm, &td);
    static const char *const wd[7] = { "日", "月", "火", "水", "木", "金", "土" };
    if (y == ty) snprintf(out, n, "%d月%d日(%s)", m, d, wd[(day + 4) % 7]);
    else snprintf(out, n, "%d年%d月%d日(%s)", y, m, d, wd[(day + 4) % 7]);
}
static int scroll_px, list_scroll;
static bool dirty = true;
static char status[120]; static uint64_t status_until;
static uint32_t next_id;
static uint8_t *s_jp;

typedef struct { char uid[12], name[NAME_LEN], intro[124], handle[24], avatar[24]; } req_t;
static req_t reqs[16]; static int nreq;
static char srv_host[64], acc_id[12], acc_token[32]; static uint16_t srv_port = SRV_PORT;
static uint32_t last_seq;
static int srv_sd = -1, srv_state; static uint64_t srv_next_try, srv_last_tx, srv_last_rx;
static char srv_rx[32768]; static int srv_rx_len;

/* TLS: BearSSL's engine is pumped from the UI loop (no blocking callbacks). The server certificate is pinned
 * (SSH-style trust on first use): the SHA-256 of the leaf certificate is stored in account.txt, and a later change refuses
 * the connection until the user runs /repin. This suits a self-hosted relay with a self-signed certificate. */
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context dec;
    br_sha256_context sha;
    bool in_first, got_first;
} pin_x509;
static br_ssl_client_context tls_cc;
static br_x509_minimal_context tls_xc;
static pin_x509 tls_pin;
static unsigned char tls_iobuf[BR_SSL_BUFSIZE_BIDI];
static unsigned char srv_pin[32]; static bool srv_pin_set, pin_new, pin_mismatch;
static uint64_t tls_t0;

/* Copies src into dst (cap bytes) without leaving a half UTF-8 character at the end. */
static void scopy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) { n = cap - 1; while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--; }
    memcpy(dst, src, n); dst[n] = 0;
}
static void thumb_free(uint32_t *p);
static int64_t since(uint64_t t) { return (int64_t)(cos_time_ms() - t); }
static void set_status(const char *s) {
    snprintf(status, sizeof status, "%s", s); status_until = cos_time_ms() + 4000; dirty = true;
    char lb[160]; snprintf(lb, sizeof lb, "[MESSENGER] status: %s\n", status); cos_puts(lb);
}

static void load_jp_font(void) {
    cos_stat_t st;
    const char *fp = "/system/fonts/NotoSansJP-Regular.ttf";
    if (cos_stat(fp, &st) == 0 && st.size > 1000) {
        s_jp = (uint8_t *)malloc((size_t)st.size);
        if (s_jp && cos_read_file(fp, s_jp, (size_t)st.size) != (ssize_t)st.size) { free(s_jp); s_jp = NULL; }
    }
}
static cui_font *mkfont(int px, bool bold) {
    cui_font *f = bold ? cui_font_bold(px) : cui_font_default(px);
    if (f && s_jp) { cui_font *jp = cui_font_load(s_jp, px); if (jp) cui_font_set_fallback(f, jp); }
    return f;
}

/* ---------------------------------------------------------------- persistence */
static ssize_t vread(const char *path, void *buf, size_t cap);
static ssize_t vwrite(const char *path, const void *buf, size_t n);
static bool vault_locked(void);
static void vault_cmd(char *arg);

static void path_chat(char *out, size_t n, const contact_t *k) {
    if (k->uid[0]) snprintf(out, n, DIR "/chat_u%s.txt", k->uid); else snprintf(out, n, DIR "/chat_%08x.txt", k->ip);
}
static void save_account(void) {
    char hex[65] = "";
    if (srv_pin_set) for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", srv_pin[i]);
    char b[384]; int o = snprintf(b, sizeof b, "%s\n%u\n%s\n%s\n%u\n%s\n", srv_host, srv_port, acc_id, acc_token, last_seq, hex);
    vwrite(DIR "/account.txt", b, (size_t)o);
}
static void load_account(void) {
    char b[384]; cos_stat_t st;
    if (cos_stat(DIR "/account.txt", &st) != 0 || st.size == 0 || st.size >= sizeof b) return;
    ssize_t n = vread(DIR "/account.txt", b, (size_t)st.size); if (n <= 0) return;
    b[n] = 0;
    unsigned port = SRV_PORT, seq = 0; char *l[6] = {0}; int c = 0;
    for (char *p = b; *p && c < 6;) { l[c++] = p; char *e = strchr(p, '\n'); if (!e) break; *e = 0; p = e + 1; }
    if (c > 0) snprintf(srv_host, sizeof srv_host, "%s", l[0]);
    if (c > 1) sscanf(l[1], "%u", &port);
    if (c > 2) snprintf(acc_id, sizeof acc_id, "%s", l[2]);
    if (c > 3) snprintf(acc_token, sizeof acc_token, "%s", l[3]);
    if (c > 4) sscanf(l[4], "%u", &seq);
    if (c > 5 && strlen(l[5]) == 64) {
        srv_pin_set = true;
        for (int i = 0; i < 32; i++) { unsigned v = 0; sscanf(l[5] + i * 2, "%2x", &v); srv_pin[i] = (unsigned char)v; }
    }
    srv_port = (uint16_t)port; last_seq = seq;
}

static void save_contacts(void) {
    static char buf[MAX_CONTACTS * 160];
    size_t o = 0;
    o += (size_t)snprintf(buf + o, sizeof buf - o, "me %s\n", my_name);
    for (int i = 0; i < nct; i++)
        if (is_group(&ct[i])) o += (size_t)snprintf(buf + o, sizeof buf - o, "#%s %s\n", ct[i].uid, ct[i].name);
        else if (ct[i].uid[0]) o += (size_t)snprintf(buf + o, sizeof buf - o, "@%s %s\t%s\t%s\n", ct[i].uid, ct[i].name, ct[i].handle, ct[i].avatar);
        else o += (size_t)snprintf(buf + o, sizeof buf - o, "%u.%u.%u.%u %s\n", ct[i].ip >> 24, (ct[i].ip >> 16) & 255, (ct[i].ip >> 8) & 255,
                              ct[i].ip & 255, ct[i].name);
    vwrite(DIR "/contacts.txt", buf, o);
}

static void save_chat(contact_t *k) {
    size_t cap = 4096, o = 0;
    char *b = (char *)malloc(cap);
    if (!b) return;
    for (int i = 0; i < k->n; i++) {
        size_t need = strlen(k->m[i].text) + 160;
        if (o + need > cap) { while (o + need > cap) cap *= 2; char *nb = (char *)realloc(b, cap); if (!nb) { free(b); return; } b = nb; }
        msg_t *m = &k->m[i];
        if (is_group(k)) o += (size_t)snprintf(b + o, cap - o, "3\t%d\t%u\t%d\t%d\t%u\t%u\t%s\t%s\t%s\n", m->mine, m->ts, m->state, m->flags | (m->pend << 4), m->id, m->rid, m->sid, m->who, m->text);
        else o += (size_t)snprintf(b + o, cap - o, "2\t%d\t%u\t%d\t%d\t%u\t%u\t%s\n", m->mine, m->ts, m->state, m->flags | (m->pend << 4), m->id, m->rid, m->text);
    }
    char p[64]; path_chat(p, sizeof p, k);
    vwrite(p, b, o);
    free(b);
}

static void flat_text(char *s) { if (s) for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' '; }      /* the history file has one message per line: no sender can start a new record in it */
static msg_t *add_msg(contact_t *k, bool mine, uint32_t ts, int state, uint32_t id, const char *text) {
    if (k->n == MAX_MSGS) {                         /* drop the oldest */
        thumb_free(k->m[0].thumb);
        free(k->m[0].text);
        memmove(&k->m[0], &k->m[1], sizeof(msg_t) * (size_t)(MAX_MSGS - 1));
        k->n--;
    }
    if (k->n == k->cap) {
        int nc = k->cap ? k->cap * 2 : 32;
        msg_t *nm = (msg_t *)realloc(k->m, sizeof(msg_t) * (size_t)nc);
        if (!nm) return NULL;
        k->m = nm; k->cap = nc;
    }
    msg_t *m = &k->m[k->n++];
    memset(m, 0, sizeof *m);
    m->mine = mine; m->ts = ts; m->state = (uint8_t)state; m->id = id;
    m->text = strdup(text); flat_text(m->text);
    return m;
}

static void load_chat(contact_t *k) {
    if (k->loaded) return;
    k->loaded = true;
    char p[64]; path_chat(p, sizeof p, k);
    cos_stat_t st;
    if (cos_stat(p, &st) != 0 || st.size == 0 || st.size > (8u << 20)) return;
    char *b = (char *)malloc((size_t)st.size + 1);
    if (!b) return;
    ssize_t n = vread(p, b, (size_t)st.size);
    if (n < 0) { free(b); return; }
    b[n] = 0;
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        int mine = 0, state = 0; unsigned ts = 0; int off = 0; int fl = 0; unsigned mid2 = 0, rid2 = 0;
        if (l[0] == '2' && l[1] == '\t' && sscanf(l + 2, "%d\t%u\t%d\t%d\t%u\t%u\t%n", &mine, &ts, &state, &fl, &mid2, &rid2, &off) >= 6 && off > 0) {
            if (mine && state == ST_SENDING && !k->uid[0]) state = ST_FAILED;
            msg_t *m = add_msg(k, mine != 0, ts, state, mid2, l + 2 + off);
            if (m) { m->rid = rid2; m->flags = (uint8_t)(fl & 15); m->pend = (uint8_t)((fl >> 4) & 3); if (m->pend && !k->uid[0]) m->pend = 0; }
        } else if (l[0] == '3' && l[1] == '\t' && sscanf(l + 2, "%d\t%u\t%d\t%d\t%u\t%u\t%n", &mine, &ts, &state, &fl, &mid2, &rid2, &off) >= 6 && off > 0) {
            char *sid = l + 2 + off, *t1 = strchr(sid, '\t'), *who = t1 ? t1 + 1 : NULL, *t2 = who ? strchr(who, '\t') : NULL;
            if (t2) {
                *t1 = 0; *t2 = 0;
                msg_t *m = add_msg(k, mine != 0, ts, state, mid2, t2 + 1);
                if (m) { m->rid = rid2; m->flags = (uint8_t)(fl & 15); m->pend = (uint8_t)((fl >> 4) & 3); scopy(m->sid, sizeof m->sid, sid); scopy(m->who, sizeof m->who, who); }
            }
        } else if (sscanf(l, "%d\t%u\t%d\t%n", &mine, &ts, &state, &off) >= 3 && off > 0) {
            uint32_t mid = 0;
            if (mine && state == ST_SENDING) {
                if (k->uid[0]) mid = ++next_id;                 /* server chats: resend when the server connection is up */
                else state = ST_FAILED;                          /* LAN: the app closed before it was acknowledged */
            }
            add_msg(k, mine != 0, ts, state, mid, l + off);
        }
        if (!e) break;
        l = e + 1;
    }
    free(b);
}

static int find_uid(const char *uid) { for (int i = 0; i < nct; i++) if (ct[i].uid[0] && strcmp(ct[i].uid, uid) == 0) return i; return -1; }
static int find_ip(uint32_t ip) { for (int i = 0; i < nct; i++) if (ct[i].ip == ip) return i; return -1; }

static msg_t *find_remote(contact_t *k, uint32_t rid, const char *sid) {
    for (int j = k->n - 1; j >= 0; j--)
        if (!k->m[j].mine && k->m[j].rid == rid && rid && (!sid || !k->m[j].sid[0] || strcmp(k->m[j].sid, sid) == 0)) return &k->m[j];
    return NULL;
}
static void apply_event(contact_t *k, msg_t *m, int kind, const char *text) {
    if (m->flags & MF_DELETED) return;
    if (kind == EV_EDIT) { char *t = strdup(text); if (!t) return; flat_text(t); free(m->text); m->text = t; m->flags |= MF_EDITED; }
    else { free(m->text); m->text = strdup(""); m->flags |= MF_DELETED; }
    save_chat(k); dirty = true;
}

static int add_contact_ex(uint32_t ip, const char *uid, const char *name) {
    int i = uid ? find_uid(uid) : find_ip(ip);
    if (i >= 0) return i;
    if (nct == MAX_CONTACTS) return -1;
    contact_t *k = &ct[nct];
    memset(k, 0, sizeof *k);
    k->ip = ip;
    if (uid) snprintf(k->uid, sizeof k->uid, "%s", uid);
    if (name && name[0]) scopy(k->name, sizeof k->name, name);
    else if (uid) snprintf(k->name, sizeof k->name, "ID %s", uid);
    else snprintf(k->name, sizeof k->name, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    load_chat(k);
    save_contacts();
    return nct++;
}
static int add_contact(uint32_t ip, const char *name) { return add_contact_ex(ip, NULL, name); }

static void load_contacts(void) {
    cos_mkdir(DIR);
    cos_stat_t st;
    if (cos_stat(DIR "/contacts.txt", &st) != 0 || st.size == 0 || st.size > 65536) return;
    char *b = (char *)malloc((size_t)st.size + 1);
    if (!b) return;
    ssize_t n = vread(DIR "/contacts.txt", b, (size_t)st.size);
    if (n < 0) { free(b); return; }
    b[n] = 0;
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        if (strncmp(l, "me ", 3) == 0) scopy(my_name, sizeof my_name, l + 3);
        else if (l[0] == '#') {
            char *sp = strchr(l, ' ');
            if (sp) *sp = 0;
            add_contact_ex(0, l + 1, sp ? sp + 1 : NULL);
        } else if (l[0] == '@') {
            char *sp = strchr(l, ' ');
            if (sp) *sp = 0;
            char *h = sp ? strchr(sp + 1, '\t') : NULL, *av = NULL;
            if (h) { *h++ = 0; av = strchr(h, '\t'); if (av) *av++ = 0; }
            int ci = add_contact_ex(0, l + 1, sp ? sp + 1 : NULL);
            if (ci >= 0) { if (h) scopy(ct[ci].handle, sizeof ct[ci].handle, h); if (av) scopy(ct[ci].avatar, sizeof ct[ci].avatar, av); }
        } else {
            char *sp = strchr(l, ' ');
            if (sp) *sp = 0;
            uint32_t ip;
            if (cos_ip_parse(l, &ip) == 0) add_contact(ip, sp ? sp + 1 : NULL);
        }
        if (!e) break;
        l = e + 1;
    }
    free(b);
}


/* ---------------------------------------------------------------- unread counts and the arrival chime */
static bool snd_on = true, rr_on = true, pres_on = true;
static int unread_total(void) { int t = 0; for (int i = 0; i < nct; i++) if (!ct[i].muted) t += ct[i].unread; return t; }
static void unread_key(char *out, size_t n, const contact_t *k) {
    if (k->uid[0]) snprintf(out, n, "u%s", k->uid); else snprintf(out, n, "i%08x", k->ip);
}
static void save_unread(void) {
    char buf[MAX_CONTACTS * 24]; size_t o = 0;
    for (int i = 0; i < nct; i++) if (ct[i].unread) { char k[16]; unread_key(k, sizeof k, &ct[i]); o += (size_t)snprintf(buf + o, sizeof buf - o, "%s %d\n", k, ct[i].unread); }
    vwrite(DIR "/unread.txt", buf, o);
}
static void load_unread(void) {
    cos_stat_t st; char b[2048];
    if (cos_stat(DIR "/unread.txt", &st) != 0 || st.size <= 0 || st.size >= (int64_t)sizeof b) return;
    ssize_t n = vread(DIR "/unread.txt", b, sizeof b - 1); if (n <= 0) return; b[n] = 0;
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *sp = strchr(l, ' ');
        if (sp) { *sp = 0; for (int i = 0; i < nct; i++) { char k[16]; unread_key(k, sizeof k, &ct[i]); if (strcmp(k, l) == 0) ct[i].unread = atoi(sp + 1); } }
        if (!e) break;
        l = e + 1;
    }
    char v[2] = {0};
    if (cos_stat(DIR "/readrcpt.txt", &st) == 0 && cos_read_file(DIR "/readrcpt.txt", v, 1) == 1 && v[0] == '0') rr_on = false;
    if (cos_stat(DIR "/presence.txt", &st) == 0 && cos_read_file(DIR "/presence.txt", v, 1) == 1 && v[0] == '0') pres_on = false;
    if (cos_stat(DIR "/sound.txt", &st) == 0 && cos_read_file(DIR "/sound.txt", v, 1) == 1 && v[0] == '0') snd_on = false;
}
static void chime(void);

/* ---------------------------------------------------------------- pin, mute, drafts, order, @mentions */
static int order[MAX_CONTACTS];                      /* the list shows the contacts in this order: pinned first, then the newest conversation */
static uint32_t last_ts_of(const contact_t *k) { return k->n ? k->m[k->n - 1].ts : 0; }
static void reorder(void) {
    for (int i = 0; i < nct; i++) order[i] = i;
    for (int i = 1; i < nct; i++) {                  /* insertion sort (stable): few contacts, runs every frame */
        int v = order[i], j = i - 1;
        while (j >= 0) {
            const contact_t *a = &ct[order[j]], *b = &ct[v];
            bool later = a->pinned != b->pinned ? a->pinned < b->pinned : last_ts_of(a) < last_ts_of(b);
            if (!later) break;
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = v;
    }
}
static int order_pos(int ci) { for (int i = 0; i < nct; i++) if (order[i] == ci) return i; return 0; }
static void prefs_save(void) {
    char buf[MAX_CONTACTS * 40]; size_t o = 0;
    for (int i = 0; i < nct; i++) { char k[16]; unread_key(k, sizeof k, &ct[i]);
        if (ct[i].pinned) o += (size_t)snprintf(buf + o, sizeof buf - o, "P %s\n", k);
        if (ct[i].muted) o += (size_t)snprintf(buf + o, sizeof buf - o, "M %s\n", k); }
    vwrite(DIR "/prefs.txt", buf, o);
}
static bool drafts_dirty; static uint64_t drafts_t;
static void drafts_save(void) {
    size_t cap = 4096, o = 0; char *b = (char *)malloc(cap); if (!b) return;
    for (int i = 0; i < nct; i++) if (ct[i].draft && ct[i].draft[0]) {
        size_t need = strlen(ct[i].draft) + 40; if (o + need > cap) { while (o + need > cap) cap *= 2; char *nb = (char *)realloc(b, cap); if (!nb) { free(b); return; } b = nb; }
        char k[16]; unread_key(k, sizeof k, &ct[i]); o += (size_t)snprintf(b + o, cap - o, "%s\t%s\n", k, ct[i].draft);
    }
    vwrite(DIR "/drafts.txt", b, o); free(b); drafts_dirty = false;
}
static void prefs_load(void) {
    cos_stat_t st; char *b;
    if (cos_stat(DIR "/prefs.txt", &st) == 0 && st.size > 0 && st.size < 65536 && (b = (char *)malloc((size_t)st.size + 1))) {
        ssize_t n = vread(DIR "/prefs.txt", b, (size_t)st.size); if (n < 0) n = 0; b[n] = 0;
        for (char *l = b; *l;) { char *e = strchr(l, '\n'); if (e) *e = 0;
            if ((l[0] == 'P' || l[0] == 'M') && l[1] == ' ') for (int i = 0; i < nct; i++) { char k[16]; unread_key(k, sizeof k, &ct[i]); if (!strcmp(k, l + 2)) { if (l[0] == 'P') ct[i].pinned = 1; else ct[i].muted = 1; } }
            if (!e) break; l = e + 1; }
        free(b);
    }
    if (cos_stat(DIR "/drafts.txt", &st) == 0 && st.size > 0 && st.size < (1 << 20) && (b = (char *)malloc((size_t)st.size + 1))) {
        ssize_t n = vread(DIR "/drafts.txt", b, (size_t)st.size); if (n < 0) n = 0; b[n] = 0;
        for (char *l = b; *l;) { char *e = strchr(l, '\n'); if (e) *e = 0; char *tab = strchr(l, '\t');
            if (tab) { *tab = 0; for (int i = 0; i < nct; i++) { char k[16]; unread_key(k, sizeof k, &ct[i]); if (!strcmp(k, l)) { free(ct[i].draft); ct[i].draft = strdup(tab + 1); } } }
            if (!e) break; l = e + 1; }
        free(b);
    }
}
/* does the text address us:  @<our name> or @<our @handle> */
static bool mentions_me(const char *t) {
    for (const char *p = t; (p = strchr(p, '@')); p++) {
        const char *w[2] = { my_name, my_handle };
        for (int k = 0; k < 2; k++) { size_t L = strlen(w[k]); if (!L) continue; size_t i = 0; while (i < L && p[1 + i] && tolower((unsigned char)p[1 + i]) == tolower((unsigned char)w[k][i])) i++; if (i == L) return true; }
    }
    return false;
}
/* a message arrived in k: chime, unless the chat is muted (a mention still rings) */
static void notify_new(contact_t *k) {
    bool ment = k->n && !k->m[k->n - 1].mine && is_group(k) && mentions_me(msg_body(k->m[k->n - 1].text));
    if (ment) cos_puts("[MESSENGER] mention\n");
    if (ment && k != (cur >= 0 ? &ct[cur] : NULL)) k->mention = 1;
    if (!k->muted || ment) chime();
}

/* Notes only you can see: command results and /help. They appear in the conversation like a message but are never sent,
 * never saved and never reach the other side. A note is a title line followed by lines: "# heading", "key<TAB>text" or plain text. */
#define MAX_NOTES 6
static struct { char *text; } notes[MAX_NOTES]; static int nnotes; static bool note_jump;
static void note_clear(void) { for (int i = 0; i < nnotes; i++) free(notes[i].text); nnotes = 0; dirty = true; }
static void note_add(const char *text) {
    if (nnotes == MAX_NOTES) { free(notes[0].text); memmove(&notes[0], &notes[1], sizeof notes[0] * (MAX_NOTES - 1)); nnotes--; }
    char *t = strdup(text); if (!t) return;
    notes[nnotes++].text = t; scroll_px = 0; note_jump = true; dirty = true;
}
static void select_contact(int i);
static bool srv_synced;
static int ncollect; static char nbuf[3600], ngid[12];      /* 1: collecting group notes, 2: the block list; shown as a card when complete */
static const char *role_name(int r) { return r == 2 ? "管理者" : r == 1 ? "副管理者" : ""; }
static char pend_group[12];
static const char *gm_name(const contact_t *k, const char *uid) {
    for (int i = 0; i < k->ngm; i++) if (strcmp(k->gm[i].uid, uid) == 0) return k->gm[i].name;
    return NULL;
}
static void gm_set(contact_t *k, const char *uid, const char *name) {
    for (int i = 0; i < k->ngm; i++) if (strcmp(k->gm[i].uid, uid) == 0) { scopy(k->gm[i].name, sizeof k->gm[i].name, name); return; }
    struct gmem *nm = (struct gmem *)realloc(k->gm, sizeof *nm * (size_t)(k->ngm + 1)); if (!nm) return;
    k->gm = nm; memset(&nm[k->ngm], 0, sizeof nm[k->ngm]); snprintf(nm[k->ngm].uid, sizeof nm[k->ngm].uid, "%s", uid); scopy(nm[k->ngm].name, sizeof nm[k->ngm].name, name); k->ngm++;
}
static struct gmem *gm_find(contact_t *k, const char *uid) { for (int i = 0; i < k->ngm; i++) if (strcmp(k->gm[i].uid, uid) == 0) return &k->gm[i]; return NULL; }
static void gm_del(contact_t *k, const char *uid) {
    for (int i = 0; i < k->ngm; i++) if (strcmp(k->gm[i].uid, uid) == 0) { k->gm[i] = k->gm[--k->ngm]; return; }
}

/* ---------------------------------------------------------------- network */
static void send_pkt(uint32_t ip, int type, uint32_t id, uint32_t ts, const char *text) {
    uint8_t buf[sizeof(hdr_t) + LAN_TEXT];
    hdr_t *h = (hdr_t *)buf;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "CMSG", 4);
    h->type = (uint8_t)type; h->id = id; h->ts = ts;
    scopy(h->name, sizeof h->name, my_name);
    size_t n = sizeof *h;
    if (text) { size_t l = strlen(text); if (l > LAN_TEXT - 1) { l = LAN_TEXT - 1; while (l > 0 && ((unsigned char)text[l] & 0xC0) == 0x80) l--; } memcpy(buf + n, text, l); n += l; }
    ssize_t r = cos_sendto(sock, buf, n, ip, PORT);
    if (r < 0) { char lb[96]; snprintf(lb, sizeof lb, "[MESSENGER] tx type=%d to %u.%u.%u.%u -> %d\n", type, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, (int)r); cos_puts(lb); }
}

static void srv_send_msg(contact_t *k, msg_t *m);
static void send_event(contact_t *k, msg_t *m, int kind);
static void thumb_free(uint32_t *p);
static bool att_srv_frame(char **f, int n);
static bool prof_srv_frame(char **f, int n);
static bool gl_frame(char **f, int n);
static void gl_poll(void);
static void gl_on_error(const char *msg);
static void gl_cmd(void);
static bool g_linked, g_required, g_conf; static char g_masked[40];
static void prof_avatar_uploaded(const char *fid);
static void prof_set_photo(const char *path);
static void prof_cmd_photo(const char *arg);
static void prof_open_profile(void);
static void sr_start(const char *q);
static void sr_open_empty(void);
static void av_changed(void);
static bool att_upload_avatar(uint8_t *data, size_t n);
static void att_on_disconnect(void);
static void att_send_file(contact_t *k, const char *path);
static void transmit(uint32_t ip, msg_t *m);                     /* in e2ec.inc: encrypts first */
static void lan_event(contact_t *k, msg_t *m);
static bool lan_plain_del_refused(contact_t *k);
static void e2e_apply_event(contact_t *k, msg_t *m, bool decrypted, const char *plain, const char *legacy_text, bool legacy_del);
static int lan_in(contact_t *k, const char *text, uint32_t rid, int kind, char **out);
static void lan_key_request_reply(uint32_t ip);
static void lan_key_arrived(uint32_t ip, const char *b64);

static bool plug_outgoing(char *text, size_t cap);
static bool plug_has_out(void);
static void plug_incoming(contact_t *k, const char *text);
static void send_text(contact_t *k, const char *text) {
    char filtered[MAX_TEXT + 160];
    if (plug_has_out()) { scopy(filtered, sizeof filtered, text); if (plug_outgoing(filtered, sizeof filtered)) return; text = filtered; }       /* plugins may rewrite or swallow it */
    msg_t *m = add_msg(k, true, (uint32_t)cos_time_unix(), ST_SENDING, ++next_id, text);
    if (!m) return;
    if (k->uid[0]) srv_send_msg(k, m); else transmit(k->ip, m);
    save_chat(k);
    scroll_px = 0; dirty = true;
}

static void net_poll(void) {
    uint8_t buf[sizeof(hdr_t) + LAN_TEXT + 8];
    for (int guard = 0; guard < 64; guard++) {
        cos_sockaddr_t from;
        ssize_t n = cos_recvfrom(sock, buf, sizeof buf - 1, &from);
        if (n < 0) break;
        if ((size_t)n < sizeof(hdr_t) || memcmp(buf, "CMSG", 4) != 0) continue;
        hdr_t *h = (hdr_t *)buf;
        buf[n] = 0;
        char name[25]; memcpy(name, h->name, 24); name[24] = 0;
        if (from.ip == my_ip && h->type != T_MSG && h->type != T_ACK) continue;      /* our own broadcast */
        if (h->type == T_KEYREQ) { lan_key_request_reply(from.ip); }
        else if (h->type == T_KEY) { lan_key_arrived(from.ip, (char *)buf + sizeof(hdr_t)); }
        else if (h->type == T_EDIT || h->type == T_DEL) {
            int i = find_ip(from.ip);
            char *eplain = NULL;
            if (h->type == T_EDIT && i >= 0 && lan_in(&ct[i], (char *)buf + sizeof(hdr_t), h->id, 1, &eplain) == 0) continue;     /* waiting for the key: no EVACK, the sender repeats it */
            if (h->type == T_DEL && i >= 0 && lan_plain_del_refused(&ct[i])) continue;                  /* a plain delete from somebody who has a key: spoofed */
            send_pkt(from.ip, T_EVACK, h->id, 0, NULL);
            msg_t *m = i >= 0 ? find_remote(&ct[i], h->id, NULL) : NULL;
            if (m) e2e_apply_event(&ct[i], m, eplain != NULL, eplain, (char *)buf + sizeof(hdr_t), h->type == T_DEL);
            free(eplain);
        } else if (h->type == T_EVACK) {
            int i = find_ip(from.ip);
            if (i < 0) continue;
            for (int j = 0; j < ct[i].n; j++) if (ct[i].m[j].mine && ct[i].m[j].id == h->id && ct[i].m[j].pend) { ct[i].m[j].pend = 0; save_chat(&ct[i]); }
        } else if (h->type == T_ACK) {
            int i = find_ip(from.ip);
            if (i < 0) continue;
            for (int j = 0; j < ct[i].n; j++) {
                msg_t *m = &ct[i].m[j];
                if (m->mine && m->id == h->id && m->state == ST_SENDING) { m->state = ST_SENT; save_chat(&ct[i]); dirty = true; }
            }
        } else if (h->type == T_HELLO || h->type == T_HELLO_R) {
            int i = find_ip(from.ip);
            if (i < 0) { i = add_contact(from.ip, name); set_status(T("Found a new contact")); }
            if (i >= 0 && ct[i].name[0] >= '0' && ct[i].name[0] <= '9' && name[0]) { scopy(ct[i].name, sizeof ct[i].name, name); save_contacts(); }
            if (h->type == T_HELLO) send_pkt(from.ip, T_HELLO_R, 0, 0, NULL);
            dirty = true;
        } else if (h->type == T_MSG) {
            int i = find_ip(from.ip);
            if (i < 0) i = add_contact(from.ip, name);
            if (i < 0) continue;
            contact_t *k = &ct[i];
            bool dup = false;
            for (int s = 0; s < k->nseen; s++) if (k->seen[s] == h->id) dup = true;
            if (dup) { send_pkt(from.ip, T_ACK, h->id, h->ts, NULL); continue; }
            char *plain = NULL;
            if (lan_in(k, (char *)buf + sizeof(hdr_t), h->id, 0, &plain) == 0) continue;           /* waiting for the sender's key: not acknowledged, the sender repeats it */
            send_pkt(from.ip, T_ACK, h->id, h->ts, NULL);
            k->seen[k->nseen % 16] = h->id; k->nseen++;
            if (k->nseen > 1000000) k->nseen = 16;
            if (k->name[0] >= '0' && k->name[0] <= '9' && name[0]) { scopy(k->name, sizeof k->name, name); save_contacts(); }
            char *text = plain ? plain : (char *)buf + sizeof(hdr_t);
            for (char *p = text; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
            msg_t *nm = add_msg(k, false, (uint32_t)cos_time_unix(), ST_RECV, h->id, text);
            plug_incoming(k, text);
            free(plain);
            if (nm) nm->rid = h->id;
            if (cur < 0) cur = i;
            if (i != cur) k->unread++; else scroll_px = 0;
            notify_new(k);
            save_chat(k);
            dirty = true;
        }
    }
    uint64_t now = cos_time_ms();
    for (int i = 0; i < nct; i++)
        for (int j = 0; j < ct[i].n; j++) {
            msg_t *m = &ct[i].m[j];
            if (!ct[i].uid[0] && m->mine && m->pend && now - m->sent_ms >= RETRY_MS) {
                if (m->tries >= MAX_TRIES) { m->pend = 0; save_chat(&ct[i]); }
                else { lan_event(&ct[i], m); m->sent_ms = now; m->tries++; }
                continue;
            }
            if (ct[i].uid[0] || !m->mine || m->state != ST_SENDING || m->id == 0 || now - m->sent_ms < RETRY_MS) continue;
            if (m->tries >= MAX_TRIES) { m->state = ST_FAILED; save_chat(&ct[i]); dirty = true; }
            else transmit(ct[i].ip, m);
        }
}

static void scan(void) {
    cos_netinfo_t ni;
    if (cos_net_info(&ni) == 0) {
        uint32_t mask = COS_IP(ni.mask[0], ni.mask[1], ni.mask[2], ni.mask[3]);
        send_pkt(my_ip | ~mask, T_HELLO, 0, 0, NULL);
    }
    send_pkt(0xFFFFFFFFu, T_HELLO, 0, 0, NULL);
    set_status(T("Scanning the local network..."));
}


/* ---------------------------------------------------------------- relay server (internet mode) */
static void remove_contact(int i) {
    for (int j = 0; j < ct[i].n; j++) { thumb_free(ct[i].m[j].thumb); free(ct[i].m[j].text); }
    free(ct[i].m); free(ct[i].gm);
    memmove(&ct[i], &ct[i + 1], sizeof(contact_t) * (size_t)(nct - i - 1));
    nct--;
    if (cur == i) cur = nct ? 0 : -1; else if (cur > i) cur--;
    save_contacts();
}


/* ---------------------------------------------------------------- TLS */
br_prng_seeder br_prng_seeder_system(const char **name) { (void)name; return 0; }

static void pin_start_chain(const br_x509_class **ctx, const char *name) {
    pin_x509 *p = (pin_x509 *)ctx; (void)name;
    p->in_first = false; p->got_first = false; pin_new = false; pin_mismatch = false;
}
static void pin_start_cert(const br_x509_class **ctx, uint32_t len) {
    pin_x509 *p = (pin_x509 *)ctx; (void)len;
    if (p->got_first) { p->in_first = false; return; }
    p->in_first = p->got_first = true;
    br_x509_decoder_init(&p->dec, 0, 0);
    br_sha256_init(&p->sha);
}
static void pin_append(const br_x509_class **ctx, const unsigned char *b, size_t n) {
    pin_x509 *p = (pin_x509 *)ctx;
    if (!p->in_first) return;
    br_x509_decoder_push(&p->dec, b, n);
    br_sha256_update(&p->sha, b, n);
}
static void pin_end_cert(const br_x509_class **ctx) { ((pin_x509 *)ctx)->in_first = false; }
static unsigned pin_end_chain(const br_x509_class **ctx) {
    pin_x509 *p = (pin_x509 *)ctx;
    if (!p->got_first || br_x509_decoder_last_error(&p->dec) != 0) return BR_ERR_X509_NOT_TRUSTED;
    unsigned char fp[32]; br_sha256_out(&p->sha, fp);
    if (srv_pin_set) {
        if (memcmp(fp, srv_pin, 32) != 0) { pin_mismatch = true; return BR_ERR_X509_NOT_TRUSTED; }
        return 0;
    }
    memcpy(srv_pin, fp, 32); srv_pin_set = true; pin_new = true;
    return 0;
}
static const br_x509_pkey *pin_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    pin_x509 *p = (pin_x509 *)ctx;
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&p->dec);
}
static const br_x509_class pin_class = { sizeof(pin_x509), pin_start_chain, pin_start_cert, pin_append, pin_end_cert, pin_end_chain, pin_get_pkey };

static void tls_seed(void) {
    unsigned char seed[32];
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    bool has = (ecx & (1u << 30)) != 0;
    for (size_t i = 0; i < sizeof seed; i += 8) {
        uint64_t w = 0; unsigned char ok = 0;
        if (has) __asm__ volatile("rdrand %0; setc %1" : "=r"(w), "=qm"(ok));
        if (!ok) {
            uint32_t lo, hi;
            for (int j = 0; j < 64; j++) { __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); w ^= (((uint64_t)hi << 32) | lo) + ((uint64_t)j << (j & 15)); }
            w ^= cos_time_ms() << 20;
        }
        for (int b = 0; b < 8; b++) seed[i + b] = (unsigned char)(w >> (b * 8));
    }
    br_ssl_engine_inject_entropy(&tls_cc.eng, seed, sizeof seed);
}

static void tls_begin(void) {
    br_ssl_client_init_full(&tls_cc, &tls_xc, NULL, 0);
    tls_pin.vtable = &pin_class;
    br_ssl_engine_set_x509(&tls_cc.eng, &tls_pin.vtable);
    br_ssl_engine_set_buffer(&tls_cc.eng, tls_iobuf, sizeof tls_iobuf, 1);
    tls_seed();
    uint32_t ip;
    br_ssl_client_reset(&tls_cc, cos_ip_parse(srv_host, &ip) == 0 ? NULL : srv_host, 0);
    tls_t0 = cos_time_ms();
}

static void srv_drop(const char *why);

/* Moves bytes between the engine and the socket until neither side can make progress. false = the connection was dropped. */
static bool tls_pump(void) {
    br_ssl_engine_context *e = &tls_cc.eng;
    for (int guard = 0; guard < 64; guard++) {
        unsigned st = br_ssl_engine_current_state(e);
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(e);
            char b[80];
            if (pin_mismatch) snprintf(b, sizeof b, "%s", T("Server certificate changed! Refusing. (/repin to trust the new one)"));
            else if (err) snprintf(b, sizeof b, T("TLS error %d"), err);
            else snprintf(b, sizeof b, "%s", T("Disconnected from the server"));
            srv_drop(b);
            if (pin_mismatch) srv_next_try = cos_time_ms() + 60000;
            return false;
        }
        size_t len;
        if (st & BR_SSL_SENDREC) {
            unsigned char *buf = br_ssl_engine_sendrec_buf(e, &len);
            ssize_t n = cos_send(srv_sd, buf, len);
            if (n > 0) { br_ssl_engine_sendrec_ack(e, (size_t)n); continue; }
            if (n == -COS_EAGAIN) { if (!(st & BR_SSL_RECVREC)) break; }
            else { srv_drop(T("Disconnected from the server")); return false; }
        }
        if (st & BR_SSL_RECVREC) {
            unsigned char *buf = br_ssl_engine_recvrec_buf(e, &len);
            ssize_t n = cos_recv(srv_sd, buf, len);
            if (n > 0) { br_ssl_engine_recvrec_ack(e, (size_t)n); srv_last_rx = cos_time_ms(); continue; }
            if (n == -COS_EAGAIN) break;
            srv_drop(T("Disconnected from the server")); return false;
        }
        break;
    }
    return true;
}

static bool tls_write(const char *data, size_t n) {
    br_ssl_engine_context *e = &tls_cc.eng;
    for (int guard = 0; n && guard < 200; guard++) {
        size_t len; unsigned char *b = br_ssl_engine_sendapp_buf(e, &len);
        if (!b || !len) { if (!tls_pump()) return false; cos_sleep_ms(1); continue; }
        size_t k = n < len ? n : len;
        memcpy(b, data, k); br_ssl_engine_sendapp_ack(e, k);
        data += k; n -= k;
    }
    if (n) return false;
    br_ssl_engine_flush(e, 0);
    return tls_pump();
}

static void srv_drop(const char *why) {
    if (srv_sd >= 0) att_on_disconnect();
    if (srv_sd >= 0) cos_sock_close(srv_sd);
    srv_sd = -1; srv_state = SV_OFF; srv_rx_len = 0; srv_synced = false;
    srv_next_try = cos_time_ms() + 5000;
    if (why) { set_status(why); char lb[140]; snprintf(lb, sizeof lb, "[MESSENGER] server: %s\n", why); cos_puts(lb); }
    dirty = true;
}

static void esc_into(char *o, size_t cap, size_t *n, const char *s) {
    for (; *s && *n + 3 < cap; s++) {
        if (*s == '\\') { o[(*n)++] = '\\'; o[(*n)++] = '\\'; }
        else if (*s == '\t') { o[(*n)++] = '\\'; o[(*n)++] = 't'; }
        else if (*s == '\n') { o[(*n)++] = '\\'; o[(*n)++] = 'n'; }
        else o[(*n)++] = *s;
    }
}

static bool srv_tx5(const char *a, const char *b, const char *c, const char *d, const char *e) {
    if (srv_sd < 0) return false;
    static char o[14000]; size_t n = 0;
    const char *f[5] = { a, b, c, d, e };
    for (int i = 0; i < 5 && f[i]; i++) { if (i) o[n++] = '\t'; esc_into(o, sizeof o - 2, &n, f[i]); }
    o[n++] = '\n';
    if (srv_state < SV_LOGIN) return false;
    if (!tls_write(o, n)) { if (srv_sd >= 0) srv_drop(T("Disconnected from the server")); return false; }
    srv_last_tx = cos_time_ms();
    return true;
}
static bool srv_tx(const char *a, const char *b, const char *c, const char *d) { return srv_tx5(a, b, c, d, NULL); }
#include "e2ec.inc"

static void srv_send_msg(contact_t *k, msg_t *m) {
    if (m->text[0] == 1 && m->text[1] == 'U') return;               /* an upload still in progress */
    if (srv_state != SV_ONLINE) { set_status(T("Offline - it will be sent when the server is reachable")); return; }
    char id[16]; snprintf(id, sizeof id, "%u", m->id); bool sent;
    if (is_group(k)) {
        static char gwire[3400]; char fid[24]; int r = e2e_gwire(k, m->id, 0, m->text, gwire, sizeof gwire, fid, sizeof fid);
        if (r == E2E_WAIT) { m->flags |= MF_KEYWAIT; return; }
        if (r == E2E_FAIL) { m->state = ST_FAILED; set_status(e2e_why); save_chat(k); return; }
        if (r != E2E_OK && e2e_text_has_filekey(m->text)) { m->state = ST_FAILED; set_status(T("The file key must not be sent without encryption")); save_chat(k); return; }
        if (r == E2E_OK) sent = fid[0] ? srv_tx5("GSEND", id, k->uid, gwire, fid) : srv_tx("GSEND", id, k->uid, gwire);
        else sent = srv_tx("GSEND", id, k->uid, m->text);
    }
    else {
        static char wire[3400]; char fid[24]; int r = e2e_wire(k, m->id, 0, m->text, wire, sizeof wire, fid, sizeof fid);
        if (r == E2E_WAIT) { m->flags |= MF_KEYWAIT; return; }
        if (r == E2E_FAIL) { m->state = ST_FAILED; set_status(e2e_why); save_chat(k); return; }
        if (r != E2E_OK && e2e_text_has_filekey(m->text)) { m->state = ST_FAILED; set_status(T("The file key must not be sent without encryption")); save_chat(k); return; }
        if (r == E2E_OK) sent = fid[0] ? srv_tx5("SEND", id, k->uid, wire, fid) : srv_tx("SEND", id, k->uid, wire);
        else sent = srv_tx("SEND", id, k->uid, m->text);
    }
    { char lb[100]; snprintf(lb, sizeof lb, "[MESSENGER] SEND id=%s to %s -> %d\n", id, k->uid, (int)sent); cos_puts(lb); }
}

/* Tells the senders how far we have read in this conversation (friends: one pointer; groups: one per author). */
static void send_reads(contact_t *k) {
    if (!rr_on || !k->uid[0] || srv_state != SV_ONLINE) return;
    struct { char sid[12]; uint32_t rid; } au[12]; int na = 0;
    for (int j = k->n - 1; j >= 0; j--) {
        msg_t *m = &k->m[j];
        if (m->mine || !m->rid) continue;
        if (m->flags & MF_RSENT) break;
        const char *sid = is_group(k) ? m->sid : k->uid; int a = 0;
        while (a < na && strcmp(au[a].sid, sid) != 0) a++;
        if (a == na && na < 12) { snprintf(au[na].sid, sizeof au[na].sid, "%s", sid); au[na++].rid = m->rid; }
        m->flags |= MF_RSENT;
    }
    for (int a = 0; a < na; a++) {
        char r[16]; snprintf(r, sizeof r, "%u", au[a].rid);
        if (is_group(k)) srv_tx("GREAD", k->uid, au[a].sid, r); else srv_tx("READ", k->uid, r, NULL);
    }
    if (na) save_chat(k);
}

static msg_t *find_srv_msg(uint32_t id, const char *uid, contact_t **kout) {
    for (int i = 0; i < nct; i++) {
        if (!ct[i].uid[0] || (uid && strcmp(uid, ct[i].uid) != 0)) continue;
        for (int j = ct[i].n - 1; j >= 0; j--) if (ct[i].m[j].mine && ct[i].m[j].id == id) { if (kout) *kout = &ct[i]; return &ct[i].m[j]; }
    }
    return NULL;
}

static void srv_connect(void) {
    uint32_t ip;
    if (cos_ip_parse(srv_host, &ip) != 0 && cos_resolve_wait(srv_host, &ip, 2000) != 0) { srv_drop(T("Cannot resolve the server name")); return; }
    srv_sd = cos_socket(COS_SOCK_STREAM);
    if (srv_sd < 0) { srv_drop(NULL); return; }
    int r = cos_connect(srv_sd, ip, srv_port);
    if (r == 0 || r == -COS_EINPROGRESS) srv_state = SV_CONNECTING; else srv_drop(T("Cannot connect to the server"));
    srv_last_rx = cos_time_ms();
    dirty = true;
}

static void srv_hello(void) {
    srv_state = SV_LOGIN;
    if (acc_id[0]) srv_tx("LOGIN", acc_id, acc_token, NULL); else srv_tx("REG", my_name, NULL, NULL);
}

static void srv_handle(char **f, int n) {
    const char *cmd = f[0];
    if (att_srv_frame(f, n) || prof_srv_frame(f, n) || gl_frame(f, n)) { dirty = true; return; }
    if (strcmp(cmd, "OK_REG") == 0 && n >= 4) {
        snprintf(acc_id, sizeof acc_id, "%s", f[1]); snprintf(acc_token, sizeof acc_token, "%s", f[2]); last_seq = 0;
        save_account(); srv_state = SV_ONLINE;
        char b[100]; snprintf(b, sizeof b, T("Registered. Your C-OS ID is %s"), acc_id); set_status(b);
        { char lb[80]; snprintf(lb, sizeof lb, "[MESSENGER] registered id=%s\n", acc_id); cos_puts(lb); }
    } else if (strcmp(cmd, "OK_LOGIN") == 0) { srv_state = SV_ONLINE; }
    else if (strcmp(cmd, "ERR") == 0 && n >= 2) {
        set_status(f[1]);
        gl_on_error(f[1]);
    }
    else if (strcmp(cmd, "FRIEND") == 0 && n >= 3) {
        int i = find_uid(f[1]);
        if (i < 0) { i = add_contact_ex(0, f[1], f[2]); if (cur < 0) cur = i; set_status(T("New friend added")); }
        else if (strcmp(ct[i].name, f[2]) != 0) { scopy(ct[i].name, sizeof ct[i].name, f[2]); save_contacts(); }
        if (i >= 0 && !is_group(&ct[i])) e2e_request_key(&ct[i]);
        if (i >= 0 && (n >= 5)) {
            bool chg = strcmp(ct[i].handle, f[3]) != 0 || strcmp(ct[i].avatar, f[4]) != 0;
            scopy(ct[i].handle, sizeof ct[i].handle, f[3]); scopy(ct[i].avatar, sizeof ct[i].avatar, f[4]);
            if (chg) save_contacts();
        }
        for (int r = 0; r < nreq; r++) if (strcmp(reqs[r].uid, f[1]) == 0) { reqs[r] = reqs[--nreq]; break; }
    } else if (strcmp(cmd, "FRIEND_DEL") == 0 && n >= 2) { int i = find_uid(f[1]); if (i >= 0) remove_contact(i); }
    else if (strcmp(cmd, "FREQ_IN") == 0 && n >= 3) {
        int r = 0; while (r < nreq && strcmp(reqs[r].uid, f[1]) != 0) r++;
        if (r == nreq && nreq < 16) nreq++; else if (r == nreq) return;
        snprintf(reqs[r].uid, sizeof reqs[r].uid, "%s", f[1]); scopy(reqs[r].name, sizeof reqs[r].name, f[2]);
        snprintf(reqs[r].intro, sizeof reqs[r].intro, "%s", n >= 4 ? f[3] : "");
        scopy(reqs[r].handle, sizeof reqs[r].handle, n >= 5 ? f[4] : ""); scopy(reqs[r].avatar, sizeof reqs[r].avatar, n >= 6 ? f[5] : "");
        set_status(T("New friend request"));
    } else if (strcmp(cmd, "MSG") == 0 && n >= 5) {
        unsigned long seq = strtoul(f[1], NULL, 10);
        int i = find_uid(f[2]);
        { uint32_t rid0 = n >= 6 ? (uint32_t)strtoul(f[5], NULL, 10) : 0; if (rid0 ? (i >= 0 && find_remote(&ct[i], rid0, NULL)) : (seq <= last_seq)) { srv_tx("GOT", f[1], NULL, NULL); return; } }       /* a repeat, whatever sequence number the server gives it */
        if (i < 0) i = add_contact_ex(0, f[2], NULL);
        if (i < 0) return;
        char *plain = NULL;
        if (e2e_in(&ct[i], f, n, 4, n >= 6 ? (uint32_t)strtoul(f[5], NULL, 10) : 0, 0, &plain) == E2E_PARKED) return;        /* waiting for the friend's key: not acknowledged yet */
        srv_tx("GOT", f[1], NULL, NULL);
        char *t = plain ? plain : f[4];
        msg_t *nm = add_msg(&ct[i], false, (uint32_t)strtoul(f[3], NULL, 10), ST_RECV, 0, t);
        plug_incoming(&ct[i], t);
        { char lb[80]; snprintf(lb, sizeof lb, "[MESSENGER] received a message (%d bytes) from %s\n", (int)strlen(t), f[2]); cos_puts(lb); }
        if (nm && n >= 6) nm->rid = (uint32_t)strtoul(f[5], NULL, 10);
        if (seq > last_seq) last_seq = (uint32_t)seq;
        save_account();
        if (cur < 0) cur = i;
        if (i != cur) ct[i].unread++; else { scroll_px = 0; send_reads(&ct[i]); }
        notify_new(&ct[i]);
        save_chat(&ct[i]); free(plain);
    } else if ((strcmp(cmd, "EDIT_IN") == 0 && n >= 6) || (strcmp(cmd, "DEL_IN") == 0 && n >= 5)) {
        bool edit = cmd[0] == 'E';
        unsigned long seq = strtoul(f[1], NULL, 10);
        int i = find_uid(f[2]); char *plain = NULL;
        if (edit && i >= 0 && e2e_in(&ct[i], f, n, 5, (uint32_t)strtoul(f[4], NULL, 10), 1, &plain) == E2E_PARKED) return;
        srv_tx("GOT", f[1], NULL, NULL);
        if (seq > last_seq) last_seq = (uint32_t)seq;
        save_account();
        msg_t *m = i >= 0 ? find_remote(&ct[i], (uint32_t)strtoul(f[4], NULL, 10), NULL) : NULL;
        if (m) { if (!edit && (e2e_req || KP(&ct[i])->has_kb)) { /* a plain delete from a friend who has a key: the server made it up */ } else e2e_apply_event(&ct[i], m, plain != NULL, plain, edit ? f[5] : "", !edit); }
        free(plain);
    } else if ((strcmp(cmd, "OK_EDIT") == 0 || strcmp(cmd, "OK_DEL") == 0) && n >= 2) {
        contact_t *k = NULL; msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), NULL, &k);
        if (m) { m->pend = 0; save_chat(k); }
    } else if (strcmp(cmd, "SENT") == 0 && n >= 2) {
        msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), NULL, NULL);
        if (m && m->state == ST_SENDING) m->state = ST_SENT;
    } else if (strcmp(cmd, "DELIV") == 0 && n >= 3) {
        contact_t *k = NULL; msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), f[2], &k);
        if (m) { m->state = ST_DELIVERED; save_chat(k); }
    } else if (strcmp(cmd, "NOSEND") == 0 && n >= 3) {
        msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), NULL, NULL);
        if (m) m->state = ST_FAILED;
        set_status(f[2]);
    } else if (strcmp(cmd, "OK_KEYPUT") == 0) {
    } else if (strcmp(cmd, "KEY") == 0 && n >= 3) {
        pkey_t *kp = pkr_find(f[1]); if (kp && kp->kwait) e2e_on_key(f[1], f[2]);          /* only an answer to a question we asked: nobody can push a key at us */
    } else if (strcmp(cmd, "SYNCED") == 0) {
        srv_synced = true;
        srv_tx("ME", NULL, NULL, NULL);
        e2e_init(); e2e_maintain(); e2e_publish();
        for (int i = 0; i < nct; i++) if (ct[i].uid[0] && !is_group(&ct[i])) { KP(&ct[i])->kchecked = 0; e2e_request_key(&ct[i]); }
        for (int i = 0; i < nct; i++) if (is_group(&ct[i])) gk_rekey_check(&ct[i]);
        if (cur >= 0) send_reads(&ct[cur]);
        for (int i = 0; i < nct; i++) if (ct[i].uid[0])
            for (int j = 0; j < ct[i].n; j++) if (ct[i].m[j].mine && ct[i].m[j].state == ST_SENDING && ct[i].m[j].id) srv_send_msg(&ct[i], &ct[i].m[j]);
        for (int i = 0; i < nct; i++) if (ct[i].uid[0])
            for (int j = 0; j < ct[i].n; j++) if (ct[i].m[j].mine && ct[i].m[j].pend && ct[i].m[j].id) send_event(&ct[i], &ct[i].m[j], ct[i].m[j].pend);
    } else if (strcmp(cmd, "GROUP") == 0 && n >= 4) {
        int i = find_uid(f[1]); bool isnew = i < 0;
        if (isnew) { i = add_contact_ex(0, f[1], f[2]); if (i < 0) return; }
        scopy(ct[i].name, sizeof ct[i].name, f[2]); snprintf(ct[i].owner, sizeof ct[i].owner, "%s", f[3]);
        rost_pin_owner(f[1], f[3]);                                       /* the owner as first seen is the root of trust for the roster */
        save_contacts();
        if (isnew && strcmp(pend_group, f[1]) == 0) { pend_group[0] = 0; cur = i; ct[i].unread = 0; scroll_px = 0; dirty = true; } else if (cur < 0) cur = i;
        if (isnew) e2e_flush_wait(f[1]);                                  /* group keys that arrived before the group did */
    } else if (strcmp(cmd, "GMEMBER") == 0 && n >= 4) {
        int i = find_uid(f[1]); if (i < 0) return;
        bool had = gm_name(&ct[i], f[2]) != NULL;
        gm_set(&ct[i], f[2], f[3]);
        { struct gmem *g = gm_find(&ct[i], f[2]); if (g && n >= 5) g->role = (uint8_t)atoi(f[4]); }
        if (!had && srv_synced && strcmp(f[2], acc_id) != 0) { char b[100]; snprintf(b, sizeof b, T("%s joined %s"), f[3], ct[i].name); set_status(b); }
        if (!had && srv_synced && strcmp(f[2], acc_id) != 0 && e2e_ready && rost_role(&ct[i], acc_id) >= 1 && !rost_has(&ct[i], f[2])) {      /* the server says somebody joined: an admin decides whether they get the key */
            char nb[400]; snprintf(nb, sizeof nb, T("Waiting for approval in %s\n> %s (ID %s) joined. The server cannot vouch for who this is: check the safety number (/e2e verify %s), then approve with  /gapprove %s  (or the Members page). Until then they get no group key."), ct[i].name, f[3], f[2], f[2], f[2]); note_add(nb);
        }
    } else if (strcmp(cmd, "GMEMBER_DEL") == 0 && n >= 3) {
        int i = find_uid(f[1]); if (i < 0) return;
        const char *nm = gm_name(&ct[i], f[2]);
        if (nm && srv_synced) { char b[100]; snprintf(b, sizeof b, T("%s left %s"), nm, ct[i].name); set_status(b); }
        gm_del(&ct[i], f[2]);
        if (srv_synced && rost_role(&ct[i], acc_id) >= 1 && rost_has(&ct[i], f[2]) && strcmp(f[2], acc_id)) rost_set(&ct[i], f[2], -1);      /* somebody left: out of the roster too */
        gk_rekey_check(&ct[i]);                                           /* somebody left: an admin makes a new key the leaver never gets */
    } else if (strcmp(cmd, "GROUP_DEL") == 0 && n >= 2) {
        int i = find_uid(f[1]);
        if (i >= 0) { gk_forget(ct[i].uid); rost_forget(ct[i].uid); char p[64]; path_chat(p, sizeof p, &ct[i]); cos_unlink(p); char b[100]; snprintf(b, sizeof b, T("You are no longer in %s"), ct[i].name); remove_contact(i); set_status(b); }
    } else if (strcmp(cmd, "GMSG") == 0 && n >= 7) {
        unsigned long seq = strtoul(f[1], NULL, 10); uint32_t rid = (uint32_t)strtoul(f[6], NULL, 10);
        int i = find_uid(f[2]); if (i < 0) { srv_tx("GOT", f[1], NULL, NULL); return; }
        if (rid ? find_remote(&ct[i], rid, f[3]) != NULL : seq <= last_seq) { srv_tx("GOT", f[1], NULL, NULL); return; }
        char *plain = NULL;
        if (e2e_gin(&ct[i], f, n, 5, rid, 0, &plain) == E2E_PARKED) return;
        srv_tx("GOT", f[1], NULL, NULL);
        if (plain && plain[0] == 1 && plain[1] == 'R' && plain[2] == '|') {        /* a roster: not a chat message */
            if (!e2e_bad) rost_apply(&ct[i], f[3], plain + 3);
            free(plain); if (seq > last_seq) last_seq = (uint32_t)seq; save_account(); return;
        }
        msg_t *nm = add_msg(&ct[i], false, (uint32_t)strtoul(f[4], NULL, 10), ST_RECV, 0, plain ? plain : f[5]);
        plug_incoming(&ct[i], plain ? plain : f[5]);
        if (nm) {
            nm->rid = rid; scopy(nm->sid, sizeof nm->sid, f[3]);
            const char *who = gm_name(&ct[i], f[3]); scopy(nm->who, sizeof nm->who, who ? who : f[3]);
        }
        { char lb[100]; snprintf(lb, sizeof lb, "[MESSENGER] received a group message (%d bytes) from %s\n", (int)strlen(plain ? plain : f[5]), f[3]); cos_puts(lb); }
        free(plain);
        if (seq > last_seq) last_seq = (uint32_t)seq;
        save_account();
        if (cur < 0) cur = i;
        if (i != cur) ct[i].unread++; else { scroll_px = 0; send_reads(&ct[i]); }
        notify_new(&ct[i]);
        save_chat(&ct[i]);
    } else if (strcmp(cmd, "GKEY_IN") == 0 && n >= 7) {
        unsigned long seq = strtoul(f[1], NULL, 10);
        if (e2e_gkey_in(f, n)) { srv_tx("GOT", f[1], NULL, NULL); if (seq > last_seq) last_seq = (uint32_t)seq; save_account(); }
    } else if (strcmp(cmd, "OK_GKEY") == 0) { if (n >= 4) e2e_gkey_ok(f[1], f[2], (uint32_t)strtoul(f[3], NULL, 10));
    } else if ((strcmp(cmd, "GEDIT_IN") == 0 && n >= 7) || (strcmp(cmd, "GDEL_IN") == 0 && n >= 6)) {
        bool edit = cmd[1] == 'E';
        unsigned long seq = strtoul(f[1], NULL, 10);
        int i = find_uid(f[2]); char *plain = NULL;
        if (edit && i >= 0 && e2e_gin(&ct[i], f, n, 6, (uint32_t)strtoul(f[5], NULL, 10), 1, &plain) == E2E_PARKED) return;
        srv_tx("GOT", f[1], NULL, NULL);
        if (seq > last_seq) last_seq = (uint32_t)seq;
        save_account();
        msg_t *m = i >= 0 ? find_remote(&ct[i], (uint32_t)strtoul(f[5], NULL, 10), f[3]) : NULL;
        if (m) { if (!edit && (e2e_req || gk_newest(ct[i].uid))) { /* a plain delete in an encrypted group: the server made it up */ } else e2e_apply_event(&ct[i], m, plain != NULL, plain, edit ? f[6] : "", !edit); }
        free(plain);
    } else if (strcmp(cmd, "GSENT") == 0 && n >= 2) {
        msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), NULL, NULL);
        if (m && m->state == ST_SENDING) m->state = ST_SENT;
    } else if ((strcmp(cmd, "OK_GEDIT") == 0 || strcmp(cmd, "OK_GDEL") == 0) && n >= 2) {
        contact_t *k = NULL; msg_t *m = find_srv_msg((uint32_t)strtoul(f[1], NULL, 10), NULL, &k);
        if (m) { m->pend = 0; save_chat(k); }
    } else if (strcmp(cmd, "GANN") == 0 && n >= 3) {
        int i = find_uid(f[1]); if (i < 0) return;
        char *aplain = NULL;
        if (f[2][0] && n < 4) return;                                         /* an announcement always names its author: a short frame is a forgery */
        if (f[2][0] && e2e_gin(&ct[i], f, n, 2, 0, 5, &aplain) == E2E_PARKED) return;      /* waiting for the key: the old text stays until then */
        if (aplain && e2e_bad) { cos_puts("[MESSENGER] e2e: an announcement that could not be opened was ignored\n"); free(aplain); return; }
        if (aplain) { if (n < 4 || rost_role(&ct[i], f[3]) < 1) { cos_puts("[MESSENGER] e2e: an announcement from a non-admin was ignored\n"); free(aplain); return; } }
        if (aplain) {                                                         /* an encrypted announcement is "T<unix time>|text": only a newer one replaces the shown one */
            uint32_t at = 0; const char *body = aplain; char *bar = NULL;
            if (aplain[0] == 'T' && aplain[1] >= '0' && aplain[1] <= '9') { at = (uint32_t)strtoul(aplain + 1, &bar, 10); if (bar && *bar == '|') body = bar + 1; else at = 0; }
            if (at > (uint32_t)cos_time_unix() + 86400) at = 0;
            if (at < ct[i].ann_t || (at == ct[i].ann_t && at)) { cos_puts("[MESSENGER] e2e: an older announcement was ignored\n"); free(aplain); return; }
            ct[i].ann_t = at; scopy(ct[i].ann, sizeof ct[i].ann, body); cos_puts("[MESSENGER] e2e: announcement accepted\n");
        } else scopy(ct[i].ann, sizeof ct[i].ann, f[2]);                    /* (a cleared announcement keeps the time: the old one cannot come back) */
        scopy(ct[i].ann_by, sizeof ct[i].ann_by, n >= 4 ? f[3] : ""); free(aplain);
        if (f[2][0] && srv_synced) { const char *by = gm_name(&ct[i], ct[i].ann_by); char b[100]; snprintf(b, sizeof b, T("%s posted an announcement in %s"), by ? by : T("An admin"), ct[i].name); set_status(b); }
    } else if (strcmp(cmd, "GNOTE") == 0 && n >= 6) {
        int i = find_uid(f[1]); if (i < 0) return;
        char *nplain = NULL; if (e2e_gin(&ct[i], f, n, 5, 0, 6, &nplain) == E2E_PARKED) return;
        const char *by = gm_name(&ct[i], f[3]); char who[40]; snprintf(who, sizeof who, "%s", by ? by : f[3]);
        if (ncollect == 1 && strcmp(ngid, f[1]) == 0) {
            size_t o = strlen(nbuf);
            if (o + 330 < sizeof nbuf) snprintf(nbuf + o, sizeof nbuf - o, "#%s  %s\t%s\n", f[2], who, nplain ? nplain : f[5]);
        } else { char b[120]; snprintf(b, sizeof b, T("%s added a note in %s (/gnotes)"), who, ct[i].name); set_status(b); }
        free(nplain);
    } else if (strcmp(cmd, "GNOTES_END") == 0) {
        if (ncollect == 1) { if (strchr(nbuf, '\t')) note_add(nbuf); else note_add(T("Notes\n> This group has no notes yet. Add one with  /gnote <text>")); ncollect = 0; }
    } else if (strcmp(cmd, "GNOTE_DEL") == 0) set_status(T("A note was deleted"));
    else if (strcmp(cmd, "OK_GNOTE") == 0) set_status(T("Note posted  (/gnotes lists them)"));
    else if (strcmp(cmd, "OK_GNOTEDEL") == 0) set_status(T("Note deleted"));
    else if (strcmp(cmd, "OK_GANN") == 0) set_status(T("Announcement updated"));
    else if (strcmp(cmd, "OK_GROLE") == 0 || strcmp(cmd, "OK_GOWNER") == 0) set_status(T("Done"));
    else if (strcmp(cmd, "BLOCKED") == 0 && n >= 3) {
        if (ncollect == 2) { size_t o = strlen(nbuf); if (o + 100 < sizeof nbuf) snprintf(nbuf + o, sizeof nbuf - o, "%s\tID %s   (/unblock %s)\n", f[2], f[1], f[1]); }
    } else if (strcmp(cmd, "BLOCKS_END") == 0) {
        if (ncollect == 2) { if (strchr(nbuf, '\t')) note_add(nbuf); else note_add(T("Blocked people\n> Nobody is blocked.")); ncollect = 0; }
    } else if (strcmp(cmd, "OK_UNBLOCK") == 0) set_status(T("Unblocked - they can send you a friend request again"));
    else if (strcmp(cmd, "OK_REPORT") == 0) set_status(T("Reported, and the person is blocked. Thank you."));
    else if (strcmp(cmd, "PRES") == 0 && n >= 4) {
        int i = find_uid(f[1]); if (i < 0) return;
        int st = atoi(f[2]); ct[i].pres = st == 1 ? 1 : st == 2 ? 3 : 2; ct[i].pres_ts = (uint32_t)strtoul(f[3], NULL, 10);
        if (st != 1) ct[i].typing_until = 0;
        dirty = true;
    } else if (strcmp(cmd, "TYPING_IN") == 0 && n >= 2) {
        int i = find_uid(f[1]); if (i < 0) return;
        ct[i].typing_until = cos_time_ms() + 5000; ct[i].typing_name[0] = 0; dirty = true;
    } else if (strcmp(cmd, "GTYPING_IN") == 0 && n >= 3) {
        int i = find_uid(f[1]); if (i < 0) return;
        const char *nm = gm_name(&ct[i], f[2]);
        snprintf(ct[i].typing_name, sizeof ct[i].typing_name, "%s", nm ? nm : f[2]);
        ct[i].typing_until = cos_time_ms() + 5000; dirty = true;
    } else if (strcmp(cmd, "OK_SETPRES") == 0) set_status(pres_on ? T("Others can see when you are online") : T("Your online status and typing are now hidden"));
    else if (strcmp(cmd, "READ_IN") == 0 && n >= 3) {
        int i = find_uid(f[1]); if (i < 0) return;
        uint32_t upto = (uint32_t)strtoul(f[2], NULL, 10); bool chg = false;
        for (int j = 0; j < ct[i].n; j++) { msg_t *m = &ct[i].m[j]; if (m->mine && m->id && m->id <= upto && (m->state == ST_SENT || m->state == ST_DELIVERED)) { m->state = ST_READ; chg = true; } }
        if (chg) save_chat(&ct[i]);
    } else if (strcmp(cmd, "GREAD_IN") == 0 && n >= 4) {
        int i = find_uid(f[1]); if (i < 0) return;
        struct gmem *g = gm_find(&ct[i], f[2]); uint32_t upto = (uint32_t)strtoul(f[3], NULL, 10);
        if (g && upto > g->readup) g->readup = upto;
    } else if (strcmp(cmd, "OK_GNEW") == 0 && n >= 4) {
        snprintf(pend_group, sizeof pend_group, "%s", f[1]);
        char code[40]; snprintf(code, sizeof code, "%s %s", f[1], f[3]); cos_clip_set(code, strlen(code));
        char nb[400]; snprintf(nb, sizeof nb, T("Group created\nName\t%s\nGroup ID\t%s\nPassword\t%s\n> Tell people the ID and the password so they can join with  /gjoin %s %s\n> The password is shown only now (it is copied to the clipboard). /gpass makes a new one."), f[2], f[1], f[3], f[1], f[3]);
        note_add(nb);
    } else if (strcmp(cmd, "OK_GJOIN") == 0 && n >= 3) {
        snprintf(pend_group, sizeof pend_group, "%s", f[1]);
        char b[100]; snprintf(b, sizeof b, T("Joined %s"), f[2]); set_status(b);
    } else if (strcmp(cmd, "OK_GPASS") == 0 && n >= 3) {
        char code[40]; snprintf(code, sizeof code, "%s %s", f[1], f[2]); cos_clip_set(code, strlen(code));
        char nb[300]; snprintf(nb, sizeof nb, T("New group password\nGroup ID\t%s\nPassword\t%s\n> Copied to the clipboard. The old password no longer works; people already in the group stay."), f[1], f[2]);
        note_add(nb);
    } else if (strcmp(cmd, "OK_GKICK") == 0) set_status(T("Member removed - change the password (/gpass) so they cannot rejoin"));
    else if (strcmp(cmd, "OK_GLEAVE") == 0) set_status(T("Left the group"));
    else if (strcmp(cmd, "OK_FREQ") == 0) set_status(T("Friend request sent"));
    else if (strcmp(cmd, "OK_NAME") == 0) set_status(T("Name updated"));
    dirty = true;
}

static void srv_unesc(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s; } else *o++ = *s;
    }
    *o = 0;
}

static void srv_line(char *line) {
    char *f[8]; int n = 0;
    for (char *p = line; n < 8;) { f[n++] = p; char *t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
    for (int i = 0; i < n; i++) srv_unesc(f[i]);
    srv_handle(f, n);
}

static void srv_poll(void) {
    uint64_t now = cos_time_ms();
    if (srv_state == SV_OFF) {
        if (srv_host[0] && now >= srv_next_try) srv_connect();
        return;
    }
    if (srv_state == SV_CONNECTING) {
        cos_pollfd_t pf = { srv_sd, COS_POLLOUT, 0 };
        if (cos_sock_poll(&pf, 1) > 0) {
            if (pf.revents & (COS_POLLERR | COS_POLLHUP | COS_POLLNVAL)) { srv_drop(T("Cannot connect to the server")); return; }
            int r = cos_connect(srv_sd, 0, 0);
            if (r == 0 || r == -COS_EISCONN) { tls_begin(); srv_state = SV_TLS; }
            else if (r != -COS_EINPROGRESS) srv_drop(T("Cannot connect to the server"));
        } else if (since(srv_last_rx) > 8000) srv_drop(T("Server connection timed out"));
        if (srv_state != SV_TLS) return;
    }
    if (!tls_pump()) return;
    if (srv_state == SV_TLS) {
        if (br_ssl_engine_current_state(&tls_cc.eng) & BR_SSL_SENDAPP) {
            if (pin_new) {
                save_account(); pin_new = false;
                char b[100]; snprintf(b, sizeof b, T("Pinned the server certificate (%02x%02x%02x%02x...)"), srv_pin[0], srv_pin[1], srv_pin[2], srv_pin[3]);
                set_status(b);
            }
            srv_hello();
        } else if (since(tls_t0) > 15000) { char tb[80]; snprintf(tb, sizeof tb, "%s", T("TLS handshake timed out")); srv_drop(tb); return; }
        else return;
    }
    for (;;) {
        size_t len; unsigned char *b = br_ssl_engine_recvapp_buf(&tls_cc.eng, &len);
        if (!b || !len) break;
        if (srv_rx_len + (int)len >= (int)sizeof srv_rx) { srv_drop(T("Server sent an oversized line")); return; }
        memcpy(srv_rx + srv_rx_len, b, len); srv_rx_len += (int)len;
        br_ssl_engine_recvapp_ack(&tls_cc.eng, len);
        srv_rx[srv_rx_len] = 0;
        char *start = srv_rx, *nl;
        while ((nl = strchr(start, '\n')) != NULL) { *nl = 0; srv_line(start); if (srv_sd < 0) return; start = nl + 1; }
        srv_rx_len -= (int)(start - srv_rx);
        memmove(srv_rx, start, (size_t)srv_rx_len);
        if (!tls_pump()) return;
    }
    if (since(srv_last_tx) > 30000) srv_tx("PING", NULL, NULL, NULL);
    if (srv_state != SV_OFF && since(srv_last_rx) > 100000) srv_drop(T("Server connection timed out"));
}

static void srv_set(const char *arg) {
    char host[64]; unsigned port = SRV_PORT;
    snprintf(host, sizeof host, "%s", arg);
    char *c = strchr(host, ':');
    if (c) { *c = 0; port = (unsigned)atoi(c + 1); if (!port || port > 65535) port = SRV_PORT; }
    if (strcmp(host, srv_host) != 0 || port != srv_port) { acc_id[0] = 0; acc_token[0] = 0; last_seq = 0; srv_pin_set = false; }   /* a new server means a new account */
    snprintf(srv_host, sizeof srv_host, "%s", host); srv_port = (uint16_t)port;
    save_account();
    if (srv_sd >= 0) srv_drop(NULL);
    srv_next_try = 0; srv_state = SV_OFF;
    set_status(T("Connecting to the server..."));
}

static const char *srv_label(void) {
    if (!srv_host[0]) return T("No server - open the Menu");
    return srv_state == SV_ONLINE ? T("Server: online") : srv_state == SV_OFF ? T("Server: offline") : T("Server: connecting...");
}


/* Edit or delete one of our own messages and tell the peer (server: queued until delivered; LAN: resent until acknowledged). */
static void send_event(contact_t *k, msg_t *m, int kind) {
    m->pend = (uint8_t)kind;
    if (k->uid[0]) {
        if (srv_state == SV_ONLINE) {
            char id[16]; snprintf(id, sizeof id, "%u", m->id); static char wire[3400], evt[1600]; char fid[24];
            e2e_frame_event(evt, sizeof evt, kind, m->text);                                  /* an edit AND a delete travel inside an encrypted EDIT, so the server cannot forge or replay either */
            int r = is_group(k) ? e2e_gwire(k, m->id, 1, evt, wire, sizeof wire, fid, sizeof fid) : e2e_wire(k, m->id, 1, evt, wire, sizeof wire, fid, sizeof fid);
            e2e_wipe(evt, sizeof evt);
            if (r == E2E_WAIT) m->flags |= MF_KEYWAIT;
            else if (r == E2E_FAIL) { m->pend = 0; set_status(e2e_why); }
            else if (r == E2E_OK) srv_tx(is_group(k) ? "GEDIT" : "EDIT", id, k->uid, wire);
            else if (kind == EV_EDIT) srv_tx(is_group(k) ? "GEDIT" : "EDIT", id, k->uid, m->text);       /* (plain text only when encryption is optional and the peer has no key) */
            else srv_tx(is_group(k) ? "GDEL" : "DEL", id, k->uid, NULL);
        } else set_status(T("Offline - the change will be sent when the server is reachable"));
    } else {
        lan_event(k, m);
        m->sent_ms = cos_time_ms(); m->tries = 1;
    }
    save_chat(k);
}

static void edit_message(contact_t *k, msg_t *m, const char *text) {
    char *t = strdup(text); if (!t) return;
    free(m->text); m->text = t; m->flags |= MF_EDITED;
    send_event(k, m, EV_EDIT);
}
static void delete_message(contact_t *k, msg_t *m) {
    free(m->text); m->text = strdup(""); m->flags |= MF_DELETED;
    send_event(k, m, EV_DEL);
}

/* ---------------------------------------------------------------- commands */
static const char HELP_TEXT[] =
    "コマンド一覧\n"
    "# 人を探す・友達\n"
    "/find <名前 または @ハンドル>\t人を検索して友達申請する\n"
    "/join <招待コード>\t招待コードで友達申請する\n"
    "/invite [new]\t自分の招待コードをコピー (new で作り直す)\n"
    "/friend <ID> [あいさつ]\tC-OS ID を指定して友達申請\n"
    "/accept <ID>  /decline <ID>\t友達申請を承認 / 断る\n"
    "/del\tこの会話を削除 (友達解除 / グループなら退出)\n"
    "# ブロックと通報\n"
    "/block [ID]\tブロックする (ID 省略で今の会話の相手)\n"
    "/blocks\tブロック中の人の一覧\n"
    "/unblock <ID>\tブロックを解除する\n"
    "/report [ID] <理由>\t通報する (直近のメッセージが証拠として添付され、相手はブロックされる)\n"
    "# グループ (参加には6桁のパスワードが必要)\n"
    "/gnew <名前> [6桁]\tグループを作る。パスワード省略で自動生成\n"
    "/gjoin <グループID> <6桁>\tパスワードを入力して参加する\n"
    "/gleave\t開いているグループから退会する\n"
    "/gmembers\tメンバーと役職の一覧\n"
    "/gnote <文>\tノートを投稿 (全員が書ける)\n"
    "/gnotes\tノートの一覧を表示\n"
    "/gnotedel <番号>\tノートを削除 (書いた人か管理者)\n"
    "# グループの管理 (管理者・副管理者)\n"
    "/gannounce [文]\t告知を固定表示する (文を省略すると解除)\n"
    "/gkick <名前 または ID>\tメンバーを追放 (副管理者は一般メンバーのみ)\n"
    "/lang <ja|en>\tシステム全体の表示言語を切り替える\n"
    "/gapprove <名前 または ID | all>\t参加したメンバーを承認する (承認するまでグループの鍵は渡されない)\n"
    "/grename <名前>\tグループ名を変更\n"
    "# グループの管理 (管理者のみ)\n"
    "/gadmin <名前 または ID>\t副管理者に任命する\n"
    "/gunadmin <名前 または ID>\t副管理者を解任する\n"
    "/gowner <名前 または ID>\t管理者を譲る (自分は副管理者になる)\n"
    "/gpass [6桁]\tパスワードを変更 (省略で自動生成)\n"
    "# プロフィール\n"
    "/profile\t自分のプロフィールを開く\n"
    "/name <名前>\t表示名を変更\n"
    "/handle <名前>\t@ハンドルを設定 (英数字と _ .)\n"
    "/bio <文>\t自己紹介を設定\n"
    "/discover on|off\t検索に表示するかどうか\n"
    "/photo [パス]\tプロフィール画像を設定 (画像のドロップでも可)\n"
    "# 会話の整理\n"
    "/pin  /unpin\tこの会話をリストの先頭に固定 / 解除\n"
    "/mute  /unmute\t通知音を止める / 戻す (@で呼ばれたときは鳴る)\n"
    "/search <文字>\tこの会話の中を検索\n"
    "/searchall <文字>\tすべての会話を検索\n"
    "# ファイル・LAN\n"
    "/send <パス>\tファイルを送る (会話へドロップでも可)\n"
    "/add <IP> [名前]\t同じLANの相手を追加\n"
    "/scan\tLAN内の相手を探す\n"
    "# 設定・その他\n"
    "/readreceipts on|off\t自分が読んだことを相手に知らせるか (既読表示)\n"
    "/presence on|off\tオンライン状態と「入力中」を相手に見せるか\n"
    "/host on|off\tこの C-OS を Message サーバーにする (他の C-OS は /server <このマシンのIP> で使う)\n"
    "/host google <クライアントID> <シークレット>\tこのサーバーで Google ログインを使えるようにする (Google Cloud の「TV および入力が制限されたデバイス」用 OAuth クライアント)\n"
    "/host google require|optional|off\tGoogle ログインを必須にする / 任意にする / やめる\n"
    "/sound on|off\t着信音の切り替え\n"
    "/server <host[:port]>\t中継サーバーを設定 (/server だけで再接続)\n"
    "/repin\t変更されたサーバー証明書を信頼し直す\n"
    "/google\tGoogle (Gmail) アカウントを連携・確認する\n"
    "/lock on|off|change|now\tアプリのロック (パスワードで起動を保護し、履歴を暗号化)\n"
    "/lock timeout <分>\t操作がないとき自動でロックするまでの時間 (0 で自動ロックなし)\n"
    "/logout\tこの端末からログアウト (Google 連携済みの場合のみ)\n"
    "/logoutall\t他のすべての端末をログアウトさせる\n"
    "/id\t自分の C-OS ID を表示\n"
    "/clear\tこの表示を消す\n"
    "/help\tこの一覧を表示";

static contact_t *cur_group(void) { return (cur >= 0 && is_group(&ct[cur])) ? &ct[cur] : NULL; }
static bool ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; return *a == *b; }
static bool pw6(const char *t) { if (strlen(t) != 6) return false; for (int i = 0; i < 6; i++) if (t[i] < '0' || t[i] > '9') return false; return true; }
static void cmd_group(const char *s, char *arg) {
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to a server - open the Menu")); return; }
    contact_t *g = cur_group();
    if (strcmp(s, "/gnew") == 0) {
        if (!arg || !*arg) { set_status(T("Usage: /gnew <name> [6-digit password]   (the password is generated when left out)")); return; }
        char name[64]; scopy(name, sizeof name, arg);
        char *sp = strrchr(name, ' '), *pw = NULL;
        if (sp && pw6(sp + 1)) { pw = sp + 1; *sp = 0; } else if (pw6(name)) { set_status(T("Usage: /gnew <name> [6-digit password]")); return; }
        srv_tx("GNEW", name, pw ? pw : "", NULL);
    } else if (strcmp(s, "/gjoin") == 0) {
        char id[16], pw[16];
        if (!arg || sscanf(arg, "%15s %15s", id, pw) != 2) { set_status(T("Usage: /gjoin <group ID> <6-digit password>")); return; }
        srv_tx("GJOIN", id, pw, NULL);
    } else if (!g) set_status(T("Open a group first"));
    else if (strcmp(s, "/gleave") == 0) srv_tx("GLEAVE", g->uid, NULL, NULL);
    else if (strcmp(s, "/gpass") == 0) {
        if (arg && *arg && !pw6(arg)) { set_status(T("The password must be 6 digits")); return; }
        srv_tx("GPASS", g->uid, arg ? arg : "", NULL);
    } else if (strcmp(s, "/grename") == 0) { if (arg && *arg) srv_tx("GRENAME", g->uid, arg, NULL); else set_status(T("Usage: /grename <new name>")); }
    else if (strcmp(s, "/gkick") == 0 || strcmp(s, "/gadmin") == 0 || strcmp(s, "/gunadmin") == 0 || strcmp(s, "/gowner") == 0) {
        if (!arg || !*arg) { char u[40]; snprintf(u, sizeof u, T("Usage: %s <name or ID>"), s); set_status(u); return; }
        const char *hit = NULL; int cnt = 0;
        for (int i = 0; i < g->ngm; i++)
            if (strcmp(g->gm[i].uid, arg) == 0 || ieq(g->gm[i].name, arg)) { hit = g->gm[i].uid; cnt++; }
        if (cnt != 1) { set_status(cnt ? T("Several members have that name - use the ID") : T("No such member")); return; }
        if (strcmp(s, "/gkick") == 0) { srv_tx("GKICK", g->uid, hit, NULL); rost_set(g, hit, -1); }
        else if (strcmp(s, "/gowner") == 0) { srv_tx("GOWNER", g->uid, hit, NULL); if (rost_role(g, acc_id) == 2) { rost_set(g, hit, 2); rost_set(g, acc_id, 1); } }
        else { srv_tx("GROLE", g->uid, hit, s[2] == 'a' ? "1" : "0"); if (rost_has(g, hit)) rost_set(g, hit, s[2] == 'a' ? 1 : 0); else if (s[2] == 'a') set_status(T("Approve this member first (/gapprove)")); }
    } else if (strcmp(s, "/gapprove") == 0) {
        if (rost_role(g, acc_id) < 1) { set_status(T("Only an admin can approve members")); return; }
        if (!arg || !*arg) { char u[120]; snprintf(u, sizeof u, rost_pending(g) ? T("%d waiting for approval - /gapprove <name or ID>  or  /gapprove all") : T("Nobody is waiting for approval"), rost_pending(g)); set_status(u); return; }
        int c = 0; if (!strcmp(arg, "all")) c = rost_approve_all(g);
        else { const char *hit = NULL; int cnt = 0; for (int i = 0; i < g->ngm; i++) if (strcmp(g->gm[i].uid, arg) == 0 || ieq(g->gm[i].name, arg)) { hit = g->gm[i].uid; cnt++; }
               if (cnt != 1) { set_status(cnt ? T("Several members have that name - use the ID") : T("No such member")); return; }
               if (!rost_has(g, hit) && rost_set(g, hit, 0)) c = 1; }
        char u[80]; snprintf(u, sizeof u, c ? T("Approved %d member(s) - they get the group key now") : T("Nothing to approve"), c); set_status(u);
    } else if (strcmp(s, "/gannounce") == 0) {
        if (!arg || !*arg) srv_tx("GANN", g->uid, "", NULL);
        else { static char gw[3400], timed[420]; char fid[24]; snprintf(timed, sizeof timed, "T%u|%s", (unsigned)cos_time_unix(), arg); int r = e2e_gwire(g, 0, 5, timed, gw, sizeof gw, fid, sizeof fid);
               if (r == E2E_OK) srv_tx("GANN", g->uid, gw, NULL); else if (r == E2E_PLAIN) srv_tx("GANN", g->uid, arg, NULL); else set_status(r == E2E_WAIT ? T("The member list has not arrived yet") : e2e_why); }
    }
    else if (strcmp(s, "/gnote") == 0) {
        if (!arg || !*arg) { set_status(T("Usage: /gnote <text>")); return; }
        static char gw[3400]; char fid[24]; int r = e2e_gwire(g, 0, 6, arg, gw, sizeof gw, fid, sizeof fid);
        if (r == E2E_OK) srv_tx("GNOTE", g->uid, gw, NULL); else if (r == E2E_PLAIN) srv_tx("GNOTE", g->uid, arg, NULL); else set_status(r == E2E_WAIT ? T("The member list has not arrived yet") : e2e_why);
    }
    else if (strcmp(s, "/gnotes") == 0) { ncollect = 1; snprintf(ngid, sizeof ngid, "%s", g->uid); snprintf(nbuf, sizeof nbuf, T("%s - notes\n"), g->name); srv_tx("GNOTES", g->uid, NULL, NULL); }
    else if (strcmp(s, "/gnotedel") == 0) { if (arg && *arg) srv_tx("GNOTEDEL", g->uid, arg[0] == '#' ? arg + 1 : arg, NULL); else set_status(T("Usage: /gnotedel <note number>  (see /gnotes)")); }
    else if (strcmp(s, "/gmembers") == 0) {
        char nb[2800]; size_t o = (size_t)snprintf(nb, sizeof nb, T("%s - %d members\n"), g->name, g->ngm);
        for (int pass = 2; pass >= 0; pass--)                         /* owner, sub-admins, members */
            for (int i = 0; i < g->ngm && o + 100 < sizeof nb; i++) {
                if (g->gm[i].role != pass) continue;
                char tag[24] = ""; if (pass) snprintf(tag, sizeof tag, "  (%s)", role_name(pass));
                o += (size_t)snprintf(nb + o, sizeof nb - o, "%s%s\tID %s%s\n", g->gm[i].name, tag, g->gm[i].uid, strcmp(g->gm[i].uid, acc_id) == 0 ? T("  (you)") : "");
            }
        note_add(nb);
    }
}

/* This machine as a Message server: /bin/CosMsgServer.c-os (userland/programs/cosmsgd) serves everybody who uses /server <our IP>. */
#define SRV_BIN "/bin/CosMsgServer.c-os"
static int host_pid(void) {
    cos_procinfo_t pl[128]; int n = cos_proclist(pl, 128);
    for (int i = 0; i < n; i++) if (strstr(pl[i].name, "CosMsgServer") && pl[i].state != COS_PROC_STATE_ZOMBIE) return (int)pl[i].pid;
    return 0;
}
/* /host google ...: writes /cosmsgd/google.conf (the running server reads it on every sign-in, no restart needed) */
static void host_google(const char *arg) {
    char conf[700] = ""; ssize_t n = cos_read_file("/cosmsgd/google.conf", conf, sizeof conf - 1); if (n < 0) n = 0; conf[n] = 0;
    if (!arg || !*arg) {
        char id[100] = "", sec[8] = ""; (void)sec; const char *p = strstr(conf, "client_id=");
        if (p) { p += 10; int k = 0; while (p[k] && p[k] != '\n' && k < 99) { id[k] = p[k]; k++; } id[k] = 0; }
        char b[200]; snprintf(b, sizeof b, id[0] ? T("Google sign-in is set up on this server (client %.60s)%s") : T("Google sign-in is not set up  (/host google <client id> <secret>)"), id, strstr(conf, "require=1") ? T(", required") : ""); set_status(b); return;
    }
    if (!strcmp(arg, "require") || !strcmp(arg, "optional") || !strcmp(arg, "off")) {
        if (!strstr(conf, "client_id=")) { set_status(T("Set it up first:  /host google <client id> <secret>")); return; }
        char out[700] = ""; size_t o = 0;                                   /* copy every line except require= / and, for off, the client */
        for (char *l = conf; *l;) { char *e = strchr(l, '\n'); size_t ll = e ? (size_t)(e - l) + 1 : strlen(l);
            bool drop = !strncmp(l, "require=", 8) || (!strcmp(arg, "off") && (!strncmp(l, "client_id=", 10) || !strncmp(l, "client_secret=", 14)));
            if (!drop && o + ll < sizeof out) { memcpy(out + o, l, ll); o += ll; } l += ll; }
        if (!strcmp(arg, "require")) o += (size_t)snprintf(out + o, sizeof out - o, "require=1\n");
        cos_mkdir("/cosmsgd"); cos_write_file("/cosmsgd/google.conf", out, o);
        set_status(!strcmp(arg, "require") ? T("Google sign-in is now required for new accounts") : !strcmp(arg, "optional") ? T("Google sign-in is optional now") : T("Google sign-in is turned off")); return;
    }
    char id[160], sec[160]; if (sscanf(arg, "%159s %159s", id, sec) != 2 || !strstr(id, ".apps.googleusercontent.com")) { set_status(T("Usage: /host google <client id ending in .apps.googleusercontent.com> <secret>")); return; }
    char out[700]; size_t o = (size_t)snprintf(out, sizeof out, "client_id=%s\nclient_secret=%s\n", id, sec);
    for (char *l = conf; *l;) { char *e = strchr(l, '\n'); size_t ll = e ? (size_t)(e - l) + 1 : strlen(l);        /* keep the other settings (require=, domains=) */
        if (strncmp(l, "client_id=", 10) && strncmp(l, "client_secret=", 14) && o + ll < sizeof out) { memcpy(out + o, l, ll); o += ll; } l += ll; }
    cos_mkdir("/cosmsgd"); cos_write_file("/cosmsgd/google.conf", out, o);
    set_status(T("Google sign-in is set up. People use it with /google (and new accounts if you /host google require)"));
}
/* The hosted server refuses unencrypted 1:1 and group messages unless the owner says otherwise (/host e2e optional): this is the default. */
static bool host_e2e_state(void) { char conf[700] = ""; ssize_t n = cos_read_file("/cosmsgd/google.conf", conf, sizeof conf - 1); if (n < 0) n = 0; conf[n] = 0; return strstr(conf, "require_e2e=1") != NULL; }
static void host_e2e_default(void) {
    char conf[700] = ""; ssize_t n = cos_read_file("/cosmsgd/google.conf", conf, sizeof conf - 1); if (n < 0) n = 0; conf[n] = 0;
    if (strstr(conf, "require_e2e=") || (size_t)n + 16 >= sizeof conf) return;
    snprintf(conf + n, sizeof conf - (size_t)n, "%srequire_e2e=1\n", n && conf[n - 1] != '\n' ? "\n" : "");
    cos_mkdir("/cosmsgd"); cos_write_file("/cosmsgd/google.conf", conf, strlen(conf));
}
static void host_e2e(const char *arg) {
    if (!arg || (strcmp(arg, "require") && strcmp(arg, "optional"))) { set_status(host_e2e_state() ? T("This server refuses unencrypted messages  (/host e2e optional)") : T("This server also accepts unencrypted messages  (/host e2e require)")); return; }
    char conf[700] = ""; ssize_t n = cos_read_file("/cosmsgd/google.conf", conf, sizeof conf - 1); if (n < 0) n = 0; conf[n] = 0;
    char out[700] = ""; size_t o = 0;
    for (char *l = conf; *l;) { char *e = strchr(l, '\n'); size_t ll = e ? (size_t)(e - l) + 1 : strlen(l); if (strncmp(l, "require_e2e=", 12) && o + ll < sizeof out) { memcpy(out + o, l, ll); o += ll; } l += ll; }
    o += (size_t)snprintf(out + o, sizeof out - o, "require_e2e=%d\n", !strcmp(arg, "require"));
    cos_mkdir("/cosmsgd"); cos_write_file("/cosmsgd/google.conf", out, o);
    set_status(!strcmp(arg, "require") ? T("The server now refuses unencrypted messages") : T("The server now also accepts unencrypted messages (not recommended)"));
}
/* Watchdog: while /host is on, a server that stopped without being told to (it exits on purpose when it cannot get memory) is started again.
 * At most 5 restarts in 10 minutes; after that the person is told and it is left alone. */
static bool host_wanted; static uint64_t host_chk_ms, host_win_ms; static int host_restarts;
static void host_watch(void) {
    uint64_t now = cos_time_ms(); if (!host_wanted || now - host_chk_ms < 5000) return; host_chk_ms = now;
    if (host_pid()) return;
    if (!host_win_ms || now - host_win_ms > 600000) { host_win_ms = now; host_restarts = 0; }
    if (host_restarts >= 5) { if (host_restarts == 5) { host_restarts++; set_status(T("The server stopped again and again - it is left off (/host on starts it)")); host_wanted = false; } return; }
    host_restarts++; host_e2e_default(); int64_t r = cos_spawn(SRV_BIN); cos_puts(r > 0 ? "[MESSENGER] host: the server had stopped - started again\n" : "[MESSENGER] host: cannot start the server again\n");
    if (r > 0) set_status(T("The server had stopped - started again"));
}
static void cmd_host(const char *arg) {
    if (arg && !strcmp(arg, "kill")) { int p = host_pid(); if (p) { cos_kill(p, 9); set_status("Server killed (a test: the watchdog starts it again)"); } return; }
    if (arg && !strncmp(arg, "e2e", 3) && (!arg[3] || arg[3] == ' ')) { host_e2e(arg[3] ? arg + 4 : NULL); return; }
    if (arg && !strncmp(arg, "google", 6) && (!arg[6] || arg[6] == ' ')) { host_google(arg[6] ? arg + 7 : ""); return; }
    int pid = host_pid(); char ipb[24];
    snprintf(ipb, sizeof ipb, "%u.%u.%u.%u", my_ip >> 24, (my_ip >> 16) & 255, (my_ip >> 8) & 255, my_ip & 255);
    if (!arg || (strcmp(arg, "on") && strcmp(arg, "off"))) {
        char b[160]; snprintf(b, sizeof b, pid ? T("This C-OS is a Message server at %s  (/host off)") : T("This C-OS is not a Message server  (/host on)"), ipb); set_status(b); return;
    }
    if (arg[1] == 'n') {
        if (!my_ip) { set_status(T("No network: cannot host a server")); return; }
        host_e2e_default();
        if (!pid) { int64_t r = cos_spawn(SRV_BIN); if (r <= 0) { set_status(T("Cannot start the server program")); return; } }
        cos_write_file(DIR "/host.txt", "1", 1); host_wanted = true; host_restarts = 0; host_win_ms = 0;
        char nb[400]; snprintf(nb, sizeof nb, T("Message server\nAddress\t%s  (port 5151)\nData\t/cosmsgd on this machine\n> Everybody uses it with  /server %s .\n> This machine's own app is switched to it now. Keep this C-OS running; /host off stops the server."), ipb, ipb);
        note_add(nb); srv_set(ipb);
    } else {
        if (pid) cos_kill(pid, 15);
        cos_write_file(DIR "/host.txt", "0", 1); host_wanted = false; set_status(pid ? T("Server stopped") : T("No server was running"));
    }
}

static bool ci_has(const char *hay, const char *needle) {
    size_t nl = strlen(needle); if (!nl) return false;
    for (const char *p = hay; *p; p++) { size_t i = 0; while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++; if (i == nl) return true; }
    return false;
}
/* /search <text> in the open chat, /searchall <text> in every chat: a card with the newest matches (only you see it) */
static void cmd_search(const char *q, bool all) {
    if (!q || !*q) { set_status(all ? T("Usage: /searchall <text>") : T("Usage: /search <text>   (in this chat; /searchall looks everywhere)")); return; }
    if (!all && cur < 0) { set_status(T("Open a chat first, or use /searchall")); return; }
    char body[3200]; size_t o = 0; int cnt = 0, shown = 0;
    for (int r = 0; r < nct; r++) {
        int ci = all ? r : cur; if (!all && r > 0) break;
        contact_t *k = &ct[ci];
        for (int j = k->n - 1; j >= 0; j--) {
            msg_t *m = &k->m[j]; if (m->flags & MF_DELETED) continue;
            const char *bd = msg_body(m->text); if (!ci_has(bd, q)) continue;
            cnt++;
            if (shown < 30 && o + 220 < sizeof body) {
                int y, mo, d; civil_from_days((int64_t)(m->ts / 86400), &y, &mo, &d); uint32_t sec = m->ts % 86400;
                const char *who = m->mine ? T("You") : is_group(k) && m->who[0] ? m->who : k->name;
                char snip[100]; clean_snip(snip, sizeof snip, bd, 70);
                o += (size_t)snprintf(body + o, sizeof body - o, "%d/%d %02u:%02u  %s%s%s\t%s\n", mo, d, sec / 3600, (sec / 60) % 60, all ? k->name : "", all ? " - " : "", who, snip);
                shown++;
            }
        }
    }
    char nb[3500]; snprintf(nb, sizeof nb, "検索: %s  (%d)\n%s", q, cnt, cnt ? body : "> 見つかりませんでした。");
    { char lb[60]; snprintf(lb, sizeof lb, "[MESSENGER] search %d matches\n", cnt); cos_puts(lb); }
    note_add(nb);
}
static void cmd_pin(const char *s) {
    if (cur < 0) { set_status(T("Open a chat first")); return; }
    contact_t *k = &ct[cur];
    if (!strcmp(s, "/pin")) k->pinned = 1; else if (!strcmp(s, "/unpin")) k->pinned = 0; else if (!strcmp(s, "/mute")) k->muted = 1; else k->muted = 0;
    { char lb[60]; snprintf(lb, sizeof lb, "[MESSENGER] %s\n", s + 1); cos_puts(lb); }
    prefs_save(); set_status(!strcmp(s, "/pin") ? T("Pinned to the top") : !strcmp(s, "/unpin") ? T("Unpinned") : !strcmp(s, "/mute") ? T("Muted: no sound, not counted in the title (a @mention still rings)") : T("Sound is on again")); dirty = true;
}

/* Blocking, reporting and the read-receipt setting. */
static void cmd_social(const char *s, char *arg) {
    if (strcmp(s, "/readreceipts") == 0) {
        if (!arg || (strcmp(arg, "on") && strcmp(arg, "off"))) { set_status(rr_on ? T("Read receipts are on  (/readreceipts off)") : T("Read receipts are off  (/readreceipts on)")); return; }
        rr_on = arg[1] == 'n'; cos_write_file(DIR "/readrcpt.txt", rr_on ? "1" : "0", 1);
        set_status(rr_on ? T("Others can see when you have read their messages") : T("Others no longer see when you read their messages"));
        if (rr_on && cur >= 0) send_reads(&ct[cur]);
        return;
    }
    if (strcmp(s, "/presence") == 0) {
        if (!arg || (strcmp(arg, "on") && strcmp(arg, "off"))) { set_status(pres_on ? T("Your online status is shown  (/presence off)") : T("Your online status is hidden  (/presence on)")); return; }
        pres_on = arg[1] == 'n'; cos_write_file(DIR "/presence.txt", pres_on ? "1" : "0", 1);
        if (srv_state == SV_ONLINE) srv_tx("SETPRES", pres_on ? "1" : "0", NULL, NULL); else set_status(T("Saved - it applies when you are connected to the server"));
        return;
    }
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to a server - open the Menu")); return; }
    if (strcmp(s, "/blocks") == 0) { ncollect = 2; snprintf(nbuf, sizeof nbuf, "%s", T("Blocked people\n")); srv_tx("BLOCKS", NULL, NULL, NULL); }
    else if (strcmp(s, "/unblock") == 0) { if (arg && *arg) srv_tx("UNBLOCK", arg, NULL, NULL); else set_status(T("Usage: /unblock <ID>   (see /blocks)")); }
    else if (strcmp(s, "/block") == 0) {
        if (arg && *arg) srv_tx("BLOCK", arg, NULL, NULL);
        else if (cur >= 0 && ct[cur].uid[0] && !is_group(&ct[cur])) srv_tx("BLOCK", ct[cur].uid, NULL, NULL);
        else set_status(T("Usage: /block <ID>   (or open a friend's conversation and type /block)"));
    } else if (strcmp(s, "/report") == 0) {
        char id[12] = ""; const char *reason = arg ? arg : "";
        if (arg && strlen(arg) >= 8 && isdigit((unsigned char)arg[0])) { sscanf(arg, "%11s", id); reason = arg + strlen(id); while (*reason == ' ') reason++; }
        else if (cur >= 0 && ct[cur].uid[0] && !is_group(&ct[cur])) snprintf(id, sizeof id, "%s", ct[cur].uid);
        if (!id[0] || !*reason) { set_status(T("Usage: /report [ID] <reason>   (in a friend's conversation the ID can be left out; in a group use the member's ID from /gmembers)")); return; }
        char ev[900]; ev[0] = 0; int got = 0;                        /* the last few messages from that person are sent along as evidence */
        if (cur >= 0) for (int j = ct[cur].n - 1; j >= 0 && got < 5; j--) {
            msg_t *m = &ct[cur].m[j]; if (m->mine || (m->flags & MF_DELETED)) continue;
            if (is_group(&ct[cur]) ? strcmp(m->sid, id) != 0 : strcmp(ct[cur].uid, id) != 0) continue;
            size_t o = strlen(ev); if (o + 180 > sizeof ev) break;
            snprintf(ev + o, sizeof ev - o, "%s%.170s", o ? "\n" : "", msg_body(m->text)); got++;
        }
        srv_tx("REPORT", id, reason, ev);
    }
}

/* /e2e [on|off|safety|verify|trust]: end-to-end encryption settings and the safety number of the open chat */
static void form_confirm(const char *title, const char *text, const char *cmd);
static void cmd_e2e(const char *arg0) {
    char a[200]; scopy(a, sizeof a, arg0 ? arg0 : ""); char *sp = strchr(a, ' '); const char *who = NULL, *fp8 = NULL; if (sp) { *sp = 0; who = sp + 1; while (*who == ' ') who++; }
    { char *sp2 = who ? strchr((char *)who, ' ') : NULL; if (sp2) { *sp2 = 0; fp8 = sp2 + 1; while (*fp8 == ' ') fp8++; if (!*fp8) fp8 = NULL; } }
    const char *arg = a; char b[400];
    if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
        e2e_req = arg[1] == 'n'; e2e_save_mode(); set_status(e2e_req ? T("Encryption is required: plain text is neither sent nor shown") : T("Encryption is optional: friends without a key get plain text (NOT recommended)")); return;
    }
    if (!*arg) { snprintf(b, sizeof b, T("End-to-end encryption: %s, %s.  /e2e on|off  /e2e safety [id]  /e2e verify [id]  /e2e trust [id]"), e2e_ready ? T("ready") : T("NOT available"), e2e_req ? T("required") : T("optional")); set_status(b); return; }
    const char *uid = who && *who ? who : (cur >= 0 && !is_group(&ct[cur]) ? ckey(&ct[cur]) : NULL);
    if (!uid) { set_status(T("Open a chat with a friend first (or give a C-OS ID)")); return; }
    if (!strcmp(arg, "verify")) { if (!pkr(uid)->ks || pkr(uid)->kchanged) set_status(T("No key to verify (yet)")); else e2e_verify_uid(uid); return; }
    if (!strcmp(arg, "trust")) { if (!pkr(uid)->kchanged) set_status(T("Nothing to trust: the key has not changed")); else e2e_trust_uid(uid, fp8); return; }
    if (!strcmp(arg, "safety")) {
        char sn[80];
        if (!e2e_safety_uid(uid, sn)) { set_status(e2e_ready ? T("No encryption key for this person yet") : e2e_why); return; }
        { char lb[160]; snprintf(lb, sizeof lb, "[MESSENGER] e2e: safety number %s\n", sn); cos_puts(lb); }
        bool ch = pkr(uid)->kchanged; const char *nm = e2e_who(uid);
        snprintf(b, sizeof b, T("%s  Compare these digits with %s in person or on a call. If they match, nobody is in between.%s"), sn, nm, ch ? T("  THIS IS THE NEW KEY.") : "");
        char title[90]; snprintf(title, sizeof title, T("Safety number with %s"), nm);
        char cmd[96]; if (ch) { uint8_t nf[32]; e2e_fingerprint(pkr(uid)->knb.ik_dh, pkr(uid)->knb.ik_sg, nf); snprintf(cmd, sizeof cmd, "/e2e trust %s %02x%02x%02x%02x", uid, nf[0], nf[1], nf[2], nf[3]); }      /* the key compared is the key trusted */
        else snprintf(cmd, sizeof cmd, "/e2e verify %s", uid);
        form_confirm(title, b, cmd); return;
    }
}
#include "plugins.inc"
static void run_command(char *s) {
    char *arg = strchr(s, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    if (strcmp(s, "/handle") == 0) { if (srv_state == SV_ONLINE) srv_tx("HANDLE", arg ? arg : "", NULL, NULL); else set_status(T("Not connected to the server")); }
    else if (strcmp(s, "/bio") == 0) { if (srv_state == SV_ONLINE) { srv_tx("BIO", arg ? arg : "", NULL, NULL); scopy(my_bio, sizeof my_bio, arg ? arg : ""); } else set_status(T("Not connected to the server")); }
    else if (strcmp(s, "/discover") == 0) {
        if (srv_state != SV_ONLINE) set_status(T("Not connected to the server"));
        else if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) srv_tx("DISC", arg[1] == 'n' ? "1" : "0", NULL, NULL);
        else set_status(T("Usage: /discover on|off   (whether people can find you in search)"));
    }
    else if (strcmp(s, "/help") == 0 || strcmp(s, "/?") == 0) note_add(HELP_TEXT);
    else if (strcmp(s, "/clear") == 0) note_clear();
    else if (strcmp(s, "/gnew") == 0 || strcmp(s, "/gjoin") == 0 || strcmp(s, "/gleave") == 0 || strcmp(s, "/gpass") == 0 ||
             strcmp(s, "/gkick") == 0 || strcmp(s, "/grename") == 0 || strcmp(s, "/gmembers") == 0 || strcmp(s, "/gadmin") == 0 ||
             strcmp(s, "/gunadmin") == 0 || strcmp(s, "/gowner") == 0 || strcmp(s, "/gannounce") == 0 || strcmp(s, "/gnote") == 0 ||
             strcmp(s, "/gnotes") == 0 || strcmp(s, "/gnotedel") == 0 || strcmp(s, "/gapprove") == 0) cmd_group(s, arg);
    else if (strcmp(s, "/blocks") == 0 || strcmp(s, "/unblock") == 0 || strcmp(s, "/report") == 0 || strcmp(s, "/readreceipts") == 0 || strcmp(s, "/presence") == 0 || (strcmp(s, "/block") == 0 && !(arg && *arg))) cmd_social(s, arg);
    else if (strcmp(s, "/photo") == 0) prof_cmd_photo(arg);
    else if (strcmp(s, "/search") == 0 || strcmp(s, "/searchall") == 0) cmd_search(arg, s[7] == 'a');
    else if (strcmp(s, "/pin") == 0 || strcmp(s, "/unpin") == 0 || strcmp(s, "/mute") == 0 || strcmp(s, "/unmute") == 0) cmd_pin(s);
    else if (strcmp(s, "/find") == 0 && arg && *arg) sr_start(arg);
    else if (strcmp(s, "/invite") == 0) {
        if (arg && !strcmp(arg, "new")) srv_tx("INVITE_NEW", NULL, NULL, NULL);
        else if (my_invite[0]) { cos_clip_set(my_invite, strlen(my_invite)); char b[80]; snprintf(b, sizeof b, T("Your invite code: %.4s-%s (copied)"), my_invite, my_invite + 4); set_status(b); }
        else set_status(T("Connect to a server first (/server <host>)"));
    }
    else if (strcmp(s, "/join") == 0 && arg && *arg) { if (srv_state == SV_ONLINE) srv_tx("JOIN", arg, NULL, NULL); else set_status(T("Not connected to the server")); }
    else if (strcmp(s, "/profile") == 0) prof_open_profile();
    else if (strcmp(s, "/google") == 0) gl_cmd();
    else if (strcmp(s, "/lock") == 0) vault_cmd(arg);
    else if (strcmp(s, "/logoutall") == 0) { if (srv_state == SV_ONLINE) srv_tx("LOGOUT_ALL", NULL, NULL, NULL); else set_status(T("Not connected to the server")); }
    else if (strcmp(s, "/logout") == 0) {
        if (!g_linked && !(arg && !strcmp(arg, "force"))) { set_status(T("This account is NOT linked to Google - logging out would lose it for good. Link it with /google first (or /logout force)")); return; }
        acc_id[0] = 0; acc_token[0] = 0; last_seq = 0; g_linked = false; save_account();
        if (srv_sd >= 0) srv_drop(NULL);
        srv_next_try = 0; set_status(T("Logged out of this device. Sign in again with /server"));
    }
    else if (strcmp(s, "/send") == 0 && arg && *arg) { if (cur >= 0) att_send_file(&ct[cur], arg); else set_status(T("Select a friend first")); }
    else if (strcmp(s, "/server") == 0 && arg && *arg) srv_set(arg);
    else if (strcmp(s, "/server") == 0 && !arg) { if (srv_host[0]) { srv_state = SV_OFF; srv_next_try = 0; set_status(T("Reconnecting...")); } else set_status(T("Usage: /server <host[:port]>")); }
    else if (strcmp(s, "/lang") == 0) { bool ja = arg && !strncmp(arg, "ja", 2); cos_settings_set(COS_SET_LANGUAGE, ja ? 1 : 0); i18n_poll(true); set_status(ja ? "日本語に切り替えました" : "Switched to English"); }
    else if (strncmp(s, "/sound", 6) == 0) { snd_on = !(strstr(s, "off") != NULL); cos_write_file(DIR "/sound.txt", snd_on ? "1" : "0", 1); set_status(snd_on ? T("Message sound on") : T("Message sound off")); if (snd_on) chime(); }
    else if (strcmp(s, "/repin") == 0) { srv_pin_set = false; save_account(); if (srv_sd >= 0) srv_drop(NULL); srv_next_try = 0; set_status(T("Will trust the certificate the server presents next")); }
    else if (strcmp(s, "/host") == 0) cmd_host(arg);
    else if (strcmp(s, "/e2e") == 0 || strcmp(s, "/safety") == 0) cmd_e2e(strcmp(s, "/safety") == 0 ? "safety" : arg);
    else if (strcmp(s, "/id") == 0) { char b[80]; snprintf(b, sizeof b, acc_id[0] ? T("Your C-OS ID: %s") : T("No account yet - set a server with /server"), acc_id); set_status(b); }
    else if (strcmp(s, "/friend") == 0 && arg && *arg) {
        char *hello = strchr(arg, ' ');
        if (hello) { *hello++ = 0; while (*hello == ' ') hello++; }
        if (srv_state != SV_ONLINE) set_status(T("Not connected to a server (/server <host>)"));
        else if (!acc_id[0] || strcmp(arg, acc_id) == 0) set_status(T("Enter the other person's C-OS ID"));
        else srv_tx("FREQ", arg, hello ? hello : "", NULL);
    } else if ((strcmp(s, "/accept") == 0 || strcmp(s, "/decline") == 0) && arg && *arg) {
        int r = 0; while (r < nreq && strcmp(reqs[r].uid, arg) != 0) r++;
        if (r == nreq) set_status(T("No such request"));
        else { srv_tx(s[1] == 'a' ? "FACC" : "FDEC", arg, NULL, NULL); reqs[r] = reqs[--nreq]; }
    } else if (strcmp(s, "/block") == 0 && arg && *arg) { srv_tx("BLOCK", arg, NULL, NULL); }
    else if (strcmp(s, "/add") == 0 && arg && *arg) {
        char *nm = strchr(arg, ' ');
        if (nm) { *nm++ = 0; while (*nm == ' ') nm++; }
        uint32_t ip;
        if (cos_ip_parse(arg, &ip) != 0) { set_status(T("Usage: /add <ip like 192.168.70.12> [name]")); return; }
        int i = add_contact(ip, nm);
        if (i < 0) { set_status(T("Contact list is full")); return; }
        cur = i; ct[i].unread = 0; scroll_px = 0;
        send_pkt(ip, T_HELLO, 0, 0, NULL);
        set_status(T("Contact added"));
    } else if (strcmp(s, "/name") == 0 && arg && *arg) {
        scopy(my_name, sizeof my_name, arg);
        save_contacts();
        if (srv_state == SV_ONLINE) srv_tx("NAME", my_name, NULL, NULL);
        set_status(T("Name changed"));
    } else if (strcmp(s, "/del") == 0 && cur >= 0) {
        if (is_group(&ct[cur])) { if (srv_state == SV_ONLINE) srv_tx("GLEAVE", ct[cur].uid, NULL, NULL); else set_status(T("Not connected to the server")); return; }
        char p[64]; path_chat(p, sizeof p, &ct[cur]);
        cos_unlink(p);
        if (ct[cur].uid[0]) srv_tx("FDEL", ct[cur].uid, NULL, NULL);
        remove_contact(cur);
        set_status(T("Contact deleted"));
    } else if (strcmp(s, "/scan") == 0) scan();
    else if (strcmp(s, "/plugins") == 0) cmd_plugins(arg);
    else if (plug_command(s, arg)) { }
    else { cos_puts("[MESSENGER] unknown command\n"); char nb[200]; snprintf(nb, sizeof nb, T("Unknown command\n> %.60s is not a command. Type /help for the list. (Commands are never sent to the other person.)"), s); note_add(nb); }
}


/* ---------------------------------------------------------------- input line editing (UTF-8, with a caret) */
static void in_set(const char *t) { scopy(in, sizeof in, t); in_len = (int)strlen(in); in_pos = in_len; }
static void in_clear(void) { in[0] = 0; in_len = 0; in_pos = 0; }
/* tell the other side we are typing (at most every 3 s; never for commands, edits or when presence is hidden) */
static void typing_ping(void) {
    static uint64_t last; uint64_t now = cos_time_ms();
    if (!pres_on || cur < 0 || !ct[cur].uid[0] || srv_state != SV_ONLINE || edit_msg >= 0 || in[0] == '/' || now - last < 3000) return;
    last = now; srv_tx(is_group(&ct[cur]) ? "GTYPING" : "TYPING", ct[cur].uid, NULL, NULL);
}
static void in_insert(const char *t) {
    int l = (int)strlen(t);
    if (in_len + l >= MAX_TEXT - 4) { set_status(T("Message is too long")); return; }
    memmove(in + in_pos + l, in + in_pos, (size_t)(in_len - in_pos) + 1);
    memcpy(in + in_pos, t, (size_t)l); in_pos += l; in_len += l; dirty = true;
    typing_ping();
}
static int in_prev(int p) { if (p > 0) { do p--; while (p > 0 && ((unsigned char)in[p] & 0xC0) == 0x80); } return p; }
static int in_next(int p) { if (p < in_len) { do p++; while (p < in_len && ((unsigned char)in[p] & 0xC0) == 0x80); } return p; }
static void in_backspace(void) { if (!in_pos) return; int q = in_prev(in_pos); memmove(in + q, in + in_pos, (size_t)(in_len - in_pos) + 1); in_len -= in_pos - q; in_pos = q; dirty = true; }
static void in_delete(void) { if (in_pos >= in_len) return; int q = in_next(in_pos); memmove(in + in_pos, in + q, (size_t)(in_len - q) + 1); in_len -= q - in_pos; dirty = true; }

static void draw(void);
static cpop_menu ctxm;                      /* the right-click menu (see "right-click menus" below) */
static const cpop_theme cx_theme = { 0x1C2333, 0x3A4668, 0xE4E9F2, 0x7C8AA5, 0x2F5C9E, 0xFFFFFF };
static void ime_ensure_loaded(void);
static void toggle_ime(void) {
    if (!ime) ime = cosime_new();
    bool on = !cosime_enabled(ime);
    cosime_set_enabled(ime, on);
    cos_write_file(IME_PREF, on ? "1" : "0", 1);
    set_status(on ? T("Japanese input on  (Ctrl+Space to switch)") : T("Japanese input off"));
    dirty = true;
    if (on) ime_ensure_loaded();
}

/* The dictionary is large: it is loaded the first time a letter is typed with the IME on. */
static void ime_ensure_loaded(void) {
    if (ime_tried || cosime_loaded(ime)) return;
    ime_tried = true;
    set_status(T("Loading the Japanese dictionary..."));
    draw(); cos_win2_present(g_win);
    uint64_t t0 = cos_time_ms();
    int lr = cosime_load(ime, IME_DIC);
    char lb[80]; snprintf(lb, sizeof lb, "[MESSENGER] ime dictionary load=%d in %d ms\n", lr, (int)(cos_time_ms() - t0)); cos_puts(lb);
    if (lr != 0) set_status(T("No dictionary found - typing kana only"));
    else set_status(T("Japanese dictionary loaded"));
}

static void submit(void) {
    while (in_len > 0 && in[in_len - 1] == ' ') in[--in_len] = 0;
    if (in_pos > in_len) in_pos = in_len;
    if (!in_len && !(nstg > 0 && edit_msg < 0)) return;
    if (edit_msg >= 0) {
        if (cur >= 0 && edit_msg < ct[cur].n) {
            msg_t *m = &ct[cur].m[edit_msg];
            if (m->mine && !(m->flags & MF_DELETED)) {
                char full[MAX_TEXT + 160]; const char *bd = msg_body(m->text); size_t hl = (size_t)(bd - m->text);      /* an edited reply keeps its quote */
                snprintf(full, sizeof full, "%.*s%s", (int)hl, m->text, in);
                if (strcmp(m->text, full) != 0) edit_message(&ct[cur], m, full);
            }
        }
        edit_msg = -1; sel_msg = -1; in_clear(); dirty = true;
        return;
    }
    if (in[0] == '/') run_command(in);
    else if (cur < 0) set_status(T("Add a contact first:  /add <ip> [name]"));
    else {
        note_clear(); div_idx = -1;
        char full[MAX_TEXT]; const char *out = in;
        if (reply_hdr[0] && in_len) {
            size_t hl = strlen(reply_hdr);
            snprintf(full, sizeof full, "%s%s", reply_hdr, in);
            size_t fl = strlen(full); while (fl > hl && fl >= MAX_TEXT - 1) fl--;
            full[fl] = 0; while (fl > hl && ((unsigned char)full[fl] & 0xC0) == 0x80) full[--fl] = 0;
            out = full;
        }
        if (nstg > 0) {                                   /* the chosen files go first (one upload at a time), then the text */
            if (sq.n || sq.has_text) { set_status(T("The previous files are still being sent")); return; }
            sq.ci = cur; sq.n = nstg; sq.head = 0;
            for (int i = 0; i < nstg; i++) snprintf(sq.path[i], sizeof sq.path[i], "%s", stg[i].path);
            nstg = 0;
            if (in_len) { snprintf(sq.text, sizeof sq.text, "%s", out); sq.has_text = true; }
        } else send_text(&ct[cur], out);
        reply_hdr[0] = 0;
    }
    if (cur >= 0) { free(ct[cur].draft); ct[cur].draft = NULL; drafts_dirty = true; drafts_t = cos_time_ms(); }
    in_clear(); dirty = true;
}

/* ---------------------------------------------------------------- drawing */
#define MAXL 80
static const char *ls_[MAXL]; static int ll_[MAXL];

/* line-break rules for Japanese text */
static const char NO_START[] = "、。，．・：；？！）」』】〕｝〉》〟ゝゞヽヾーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮ々…‥),.;:!?]}%";
static const char NO_END[] = "（「『【〔｛〈《〝([{";
static uint32_t cpat(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return c;
    int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    uint32_t cp = c & (0xFF >> (l + 1));
    for (int i = 1; i < l && s[i]; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}
static bool cp_in(uint32_t cp, const char *set) {
    while (*set) { uint32_t o = cpat(set); if (o == cp) return true; unsigned char c = (unsigned char)*set; set += c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }
    return false;
}

/* Splits text into lines no wider than maxw; returns the number of lines. */
static int wrap(cui_font *f, const char *t, int maxw) {
    int n = 0; const char *p = t;
    while (*p && n < MAXL) {
        const char *start = p, *last_sp = NULL; int w = 0;
        while (*p) {
            int cl = 1; unsigned char u = (unsigned char)*p;
            if (u >= 0xF0) cl = 4; else if (u >= 0xE0) cl = 3; else if (u >= 0xC0) cl = 2;
            char tmp[5]; memcpy(tmp, p, (size_t)cl); tmp[cl] = 0;
            int cw = cui_text_width(f, tmp);
            if (w + cw > maxw && p > start) break;
            if (*p == ' ') last_sp = p;
            w += cw; p += cl;
        }
        for (int kk = 0; *p && p > start && kk < 3; kk++) {             /* kinsoku: no closing mark at the start of a line, no opening mark at its end */
            const char *q = p; do q--; while (q > start && ((unsigned char)*q & 0xC0) == 0x80);
            if (q <= start) break;
            if (cp_in(cpat(p), NO_START) || cp_in(cpat(q), NO_END)) p = q; else break;
        }
        if (*p && last_sp && last_sp > start) p = last_sp + 1;
        ls_[n] = start; ll_[n] = (int)(p - start); n++;
    }
    if (n == 0) { ls_[0] = t; ll_[0] = 0; n = 1; }
    return n;
}

static void draw_line(int x, int y, const char *s, int len, uint32_t col) {
    char b[MAX_TEXT + 1];
    if (len > MAX_TEXT) len = MAX_TEXT;
    memcpy(b, s, (size_t)len); b[len] = 0;
    cui_text(&c, f_t, x, y, b, col);
}

static void fmt_time(uint32_t ts, char *out, size_t n) { uint32_t s = ts % 86400; snprintf(out, n, "%02u:%02u", s / 3600, (s / 60) % 60); }

static const char *const TXT_DELETED = "このメッセージは削除されました";
static const char *const TXT_EDITED = "編集しました";
#define COL_EDITED 0x5A6578

static void add_hit(int msg, int x, int y, int w, int h, int kind) {
    if (nhits < (int)(sizeof hits / sizeof hits[0])) hits[nhits++] = (hit_t){ msg, x, y, w, h, kind };
}

#include "attach.inc"

/* A short two-note chime. Skipped while the music player has the device, and the device is released again straight away. */
static void chime(void) {
    if (!snd_on || aud.playing) return;
    if (cos_audio_open(22050, 1) != 0) return;
    static int16_t pcm[22050 / 8];
    int half = (int)(sizeof pcm / sizeof pcm[0]) / 2;
    for (int i = 0; i < (int)(sizeof pcm / sizeof pcm[0]); i++) {
        int period = i < half ? 22050 / 880 : 22050 / 1320, ph = i % period;
        int amp = 5000 * ((i < half ? half - i : (int)(sizeof pcm / sizeof pcm[0]) - i)) / half;
        pcm[i] = (int16_t)(ph < period / 2 ? amp : -amp);
    }
    cos_audio_write(pcm, sizeof pcm / sizeof pcm[0]);
    cos_audio_drain(); cos_audio_close();
}
#include "profile.inc"
#include "google.inc"
#include "vault.inc"

/* Draws (or only measures) one note card; returns its height. */
static int note_draw(const char *t, int x, int y, int w, bool dodraw) {
    int sl = cui_font_height(f_s) + 4, ys = y + 12, colw = 200, descw = w - 28 - colw - 8, idx = 0;
    const char *p = t;
    while (*p) {
        const char *e = strchr(p, '\n'), *next = e ? e + 1 : p + strlen(p);
        char ln[240]; size_t len = (size_t)((e ? e : p + strlen(p)) - p); if (len >= sizeof ln) len = sizeof ln - 1;
        memcpy(ln, p, len); ln[len] = 0; p = next;
        if (idx++ == 0) { if (dodraw) cui_text_fit(&c, f_b, x + 14, ys, w - 28, ln, TEXT); ys += lh + 8; }
        else if (ln[0] == '#' && ln[1] == ' ') { ys += 6; if (dodraw) cui_text(&c, f_b, x + 14, ys, ln + 2, ACCENT); ys += lh + 2; }
        else {
            char *tab = strchr(ln, '\t'); const char *body = ln; int bx = x + 14, bw = w - 28;
            if (ln[0] == '>' && ln[1] == ' ') body = ln + 2;
            else if (tab) { *tab = 0; body = tab + 1; bx = x + 14 + colw + 8; bw = descw; }
            int nl = wrap(f_s, body, bw);
            if (dodraw) {
                if (tab) cui_text_fit(&c, f_s, x + 14, ys, colw, ln, ACCENT);
                for (int l = 0; l < nl; l++) { char b[240]; int n2 = ll_[l] > 239 ? 239 : ll_[l]; memcpy(b, ls_[l], (size_t)n2); b[n2] = 0; cui_text(&c, f_s, bx, ys + l * sl, b, tab ? TEXT : MUTED); }
            }
            ys += nl * sl + 2;
        }
    }
    ys += 6;
    if (dodraw) cui_text(&c, f_s, x + 14, ys, "あなただけに表示されています(相手には見えません)  -  /clear で消す", MUTED);
    return ys + sl + 8 - y;
}

static void draw_chat(int x0, int y0, int w, int h) {
    cui_clip(&c, x0, y0, w, h);
    cui_fill(&c, x0, y0, w, h, BG);
    contact_t *k = cur >= 0 ? &ct[cur] : NULL;
    int kn = k ? k->n : 0; bool grp = k && is_group(k);
    int maxbw = w * 68 / 100 - 24;
    if (note_jump) {                       /* a tall new note starts at its top instead of its end */
        note_jump = false;
        if (nnotes) { int nh = note_draw(notes[nnotes - 1].text, 0, 0, w - 32, false); if (nh > h - 40) scroll_px = nh - h + 38; }
    }
    int ty = y0 + h - 30 + scroll_px;      /* bottom of the newest bubble */
    int total = 30;
    char tb[16];
    nhits = 0; thumb_budget = 2;
    for (int ni = nnotes - 1; ni >= 0; ni--) {
        int nh = note_draw(notes[ni].text, 0, 0, w - 32, false), top = ty - nh;
        total += nh + 14;
        if (top + nh + 6 > y0 && top < y0 + h) {
            cui_round_rect(&c, x0 + 16, top, w - 32, nh, 12, 0x232845);
            cui_round_rect_outline(&c, x0 + 16, top, w - 32, nh, 12, 1.0f, ACCENT);
            note_draw(notes[ni].text, x0 + 16, top, w - 32, true);
        }
        ty = top - 14;
    }
    struct { int i, x, y, w; bool mine, att; } tbar = { -1, 0, 0, 0, false, false };      /* the Reply/Copy bar of the selected message, drawn last */
    for (int i = kn - 1; i >= 0; i--) {
        msg_t *m = &k->m[i];
        if (m->flags & MF_DELETED) {                      /* no bubble: a small red note */
            int bh = 18, top = ty - bh, tw = cui_text_width(f_s, TXT_DELETED);
            total += bh + 22;
            if (top + bh + 6 > y0 && top < y0 + h) cui_text(&c, f_s, m->mine ? x0 + w - 16 - tw : x0 + 16, top + 2, TXT_DELETED, BAD);
            ty = top - 22;
            goto separators;
        }
        int abw = 0, abh = 0; bool isatt = att_measure(m, &abw, &abh);
        int nl = 0, tw = 0, qh = 0, qw = 0; char qn[24], qs[64]; bool hasq = !isatt && msg_quote(m->text, qn, sizeof qn, qs, sizeof qs);
        const char *body = msg_body(m->text);
        if (!isatt) {
            if (hasq) { qh = 38; int a = cui_text_width(f_s, qn), b2 = cui_text_width(f_s, qs); qw = (a > b2 ? a : b2) + 22; if (qw > maxbw - 8) qw = maxbw - 8; }
            nl = wrap(f_t, body, maxbw);
            for (int l = 0; l < nl; l++) { char b[MAX_TEXT + 1]; int len = ll_[l] > MAX_TEXT ? MAX_TEXT : ll_[l]; memcpy(b, ls_[l], (size_t)len); b[len] = 0; int lw = cui_text_width(f_t, b); if (lw > tw) tw = lw; }
        }
        int bh = isatt ? abh : nl * lh + 14 + qh, bw = isatt ? abw : tw + 24;
        if (hasq && bw < qw + 16) bw = qw + 16;
        int top = ty - bh, hdr = (grp && !m->mine) ? 16 : 0;
        total += bh + 22 + hdr;
        if (top - hdr + bh + 20 > y0 && top < y0 + h) {
            int bx = m->mine ? x0 + w - 16 - bw : x0 + 16;
            if (hdr) {
                const char *nm = gm_name(k, m->sid); if (!nm) nm = m->who[0] ? m->who : m->sid;
                uint32_t hv = 0; for (const char *q = m->sid; *q; q++) hv = hv * 31 + (unsigned char)*q;
                cui_text_fit(&c, f_s, bx + 4, top - hdr + 1, maxbw, nm, cui_mix(ACCENT, 0xE0A050, hv & 255));
            }
            add_hit(i, bx, top, bw, bh, 0);
            if (isatt) att_draw(m, i, bx, top, bw, bh);
            else {
                uint32_t bc = m->mine ? MINE : THEIRS;
                cui_round_rect(&c, bx, top, bw, bh, 12, bc);
                if (hasq) {
                    cui_round_rect(&c, bx + 8, top + 6, bw - 16, qh - 4, 6, cui_mix(bc, 0x000000, 70));
                    cui_fill(&c, bx + 8, top + 8, 3, qh - 8, ACCENT);
                    cui_text_fit(&c, f_s, bx + 17, top + 7, bw - 34, qn, ACCENT);
                    cui_text_fit(&c, f_s, bx + 17, top + 7 + 15, bw - 34, qs, MUTED);
                }
                for (int l = 0; l < nl; l++) draw_line(bx + 12, top + 7 + qh + l * lh, ls_[l], ll_[l], TEXT);
            }
            if (grp && !m->mine && !isatt && mentions_me(body)) cui_round_rect_outline(&c, bx - 1, top - 1, bw + 2, bh + 2, 12, 2.0f, WARN);
            if (i == sel_msg) {
                cui_round_rect_outline(&c, bx - 1, top - 1, bw + 2, bh + 2, 12, 1.5f, ACCENT);
                tbar.i = i; tbar.x = bx; tbar.y = top; tbar.w = bw; tbar.mine = m->mine; tbar.att = isatt;
            }
            if (m->mine && i == sel_msg) {
                int by = top + (bh - 22) / 2;
                int ex = bx - 8 - 58 - (isatt ? 0 : 4 + 58);
                if (!isatt) { cui_round_rect(&c, ex, by, 58, 22, 8, 0x3A4A70);  cui_text_center(&c, f_s, ex, by, 58, 22, T("Edit"), 0xFFFFFF); add_hit(i, ex, by, 58, 22, 1); ex += 62; }
                cui_round_rect(&c, ex, by, 58, 22, 8, 0x703A44); cui_text_center(&c, f_s, ex, by, 58, 22, T("Delete"), 0xFFFFFF);
                add_hit(i, ex, by, 58, 22, 2);
            }
            fmt_time(m->ts, tb, sizeof tb);
            const char *st = m->mine ? (m->state == ST_READ ? "  既読" : m->state == ST_DELIVERED ? T("  delivered") : m->state == ST_SENT ? T("  sent") : m->state == ST_FAILED ? T("  failed") : "  ...") : "";
            char rb[24];
            if (m->mine && grp && m->id && m->state != ST_FAILED && m->state != ST_SENDING) {        /* groups: how many members have read it */
                int rc = 0; for (int q = 0; q < k->ngm; q++) if (strcmp(k->gm[q].uid, acc_id) != 0 && k->gm[q].readup >= m->id) rc++;
                if (rc) { snprintf(rb, sizeof rb, "  既読 %d", rc); st = rb; }
            }
            char meta[40]; snprintf(meta, sizeof meta, "%s%s", tb, st);
            int mw = cui_text_width(f_s, meta);
            bool ed = (m->flags & MF_EDITED) != 0;
            int ew = ed ? cui_text_width(f_s, TXT_EDITED) + 8 : 0;
            int mx = m->mine ? bx + bw - mw - ew : bx;
            cui_text(&c, f_s, mx, top + bh + 3, meta, m->state == ST_FAILED ? BAD : MUTED);
            if (ed) cui_text(&c, f_s, mx + mw + 8, top + bh + 3, TXT_EDITED, COL_EDITED);
        }
        ty = top - 22 - hdr;
separators:
        if (i == div_idx) {                               /* "ここから未読" */
            int top2 = ty - 18; total += 28;
            if (top2 + 24 > y0 && top2 < y0 + h) {
                const char *tx = "ここから未読"; int tw2 = cui_text_width(f_s, tx);
                cui_fill(&c, x0 + 16, top2 + 9, w - 32, 1, 0x7A4A55);
                cui_fill(&c, x0 + (w - tw2) / 2 - 10, top2, tw2 + 20, 18, BG);
                cui_text(&c, f_s, x0 + (w - tw2) / 2, top2 + 1, tx, 0xE07A8A);
            }
            ty = top2 - 10;
        }
        if (m->ts >= 31536000u && (i == 0 || k->m[i - 1].ts / 86400 != m->ts / 86400)) {      /* a new day starts here (old histories only kept the time of day: no chip) */
            char db[40]; fmt_day(m->ts, db, sizeof db);
            int dw = cui_text_width(f_s, db) + 22, top2 = ty - 20; total += 30;
            if (top2 + 24 > y0 && top2 < y0 + h) { cui_round_rect(&c, x0 + (w - dw) / 2, top2, dw, 20, 10, 0x2A3350); cui_text_center(&c, f_s, x0 + (w - dw) / 2, top2, dw, 20, db, 0xC0C8DC); }
            ty = top2 - 10;
        }
    }
    if (tbar.i >= 0) {                                   /* Reply / Copy / Link above the selected bubble */
        msg_t *m = &k->m[tbar.i]; const char *bd = msg_body(m->text); 
        int nb = 1 + (tbar.att ? 0 : 1) + ((!tbar.att && (strstr(bd, "http://") || strstr(bd, "https://"))) ? 1 : 0), bwid = 54, by = tbar.y - 28;
        if (by < y0 + 2) by = y0 + 2;
        int bxx = tbar.mine ? tbar.x + tbar.w - (bwid + 4) * nb + 4 : tbar.x;
        if (bxx < x0 + 4) bxx = x0 + 4;
        const char *nm[3] = { T("Reply"), T("Copy"), T("Link") };
        for (int q = 0; q < nb; q++) {
            int kind = q == 0 ? 5 : q == 1 ? 6 : 7;
            cui_round_rect(&c, bxx, by, bwid, 22, 8, 0x2F3C5E); cui_text_center(&c, f_s, bxx, by, bwid, 22, nm[q], 0xFFFFFF);
            add_hit(tbar.i, bxx, by, bwid, 22, kind); bxx += bwid + 4;
        }
    }
    int maxscroll = total - h; if (maxscroll < 0) maxscroll = 0;
    if (scroll_px > maxscroll) scroll_px = maxscroll;
    if (kn == 0 && nnotes == 0) cui_text_center(&c, f_s, x0, y0, w, h, k ? T("No messages yet. Say hello!") : T("Press  Menu  (top left) to connect to a server and add friends  -  everything is a button there"), MUTED);
    cui_unclip(&c);
}

#include <stdarg.h>
#include "gui.inc"

#define ROW_H 54
#define ANN_H 30
static int list_max(void) { int m = nreq * REQ_H + nct * ROW_H - (win_h - HEAD_H - 1); return m > 0 ? m : 0; }
static void list_clamp(void) { int m = list_max(); if (list_scroll > m) list_scroll = m; if (list_scroll < 0) list_scroll = 0; }

static void draw(void) {
    if (vault_locked()) { pw_draw(); return; }
    cui_fill(&c, 0, 0, win_w, win_h, BG);
    /* contact list */
    cui_fill(&c, 0, 0, LIST_W, win_h, PANEL);
    av_draw(my_avatar, my_name, 12, 10, 44, ACCENT);
    cui_text_fit(&c, f_b, 66, 10, LIST_W - 78, my_name, TEXT);
    char ipb[64];
    if (acc_id[0]) snprintf(ipb, sizeof ipb, "%s%s", my_handle[0] ? "@" : "ID ", my_handle[0] ? my_handle : acc_id); else snprintf(ipb, sizeof ipb, "%s", T("No C-OS ID yet"));
    cui_text_fit(&c, f_s, 66, 32, 80, ipb, ACCENT);
    gbtn(150, 34, 88, 20, T("Menu"), 0x2F5C9E);
    cui_text(&c, f_s, 16, 62, srv_label(), srv_state == SV_ONLINE ? 0x58D678 : srv_host[0] ? WARN : MUTED);
    snprintf(ipb, sizeof ipb, "LAN %u.%u.%u.%u : %d", my_ip >> 24, (my_ip >> 16) & 255, (my_ip >> 8) & 255, my_ip & 255, PORT);
    cui_text(&c, f_s, 16, 78, my_ip ? ipb : T("LAN: no network"), MUTED);
    if (g_linked) { char gb[60]; snprintf(gb, sizeof gb, "Google: %s", g_masked); cui_text_fit(&c, f_s, 16, 92, 130, gb, 0x58D678); }
    else if (g_conf && srv_state == SV_ONLINE) cui_text_fit(&c, f_s, 16, 92, 130, "Google: /google", WARN);
    cui_round_rect(&c, 150, 58, 88, 20, 8, 0x2B3550); cui_text_center(&c, f_s, 150, 58, 88, 20, T("Find people"), 0xFFFFFF);
    cui_round_rect(&c, 150, 80, 88, 20, 8, 0x2B3550); cui_text_center(&c, f_s, 150, 80, 88, 20, T("My profile"), 0xFFFFFF);
    cui_fill(&c, 0, HEAD_H, LIST_W, 1, LINE);
    list_clamp();
    cui_clip(&c, 0, HEAD_H + 1, LIST_W, win_h - HEAD_H - 1);
    for (int r = 0; r < nreq; r++) {
        int y = HEAD_H + 1 + r * REQ_H - list_scroll;
        if (y + REQ_H < HEAD_H || y > win_h) continue;
        cui_fill(&c, 0, y, LIST_W, REQ_H, 0x24304A);
        av_draw(reqs[r].avatar, reqs[r].name, 12, y + 6, 32, 0x5A6A98);
        char t[80]; if (reqs[r].handle[0]) snprintf(t, sizeof t, "%s  @%s", reqs[r].name, reqs[r].handle); else snprintf(t, sizeof t, "%s  (ID %s)", reqs[r].name, reqs[r].uid);
        cui_text_fit(&c, f_b, 52, y + 5, LIST_W - 64, t, TEXT);
        cui_text_fit(&c, f_s, 52, y + 24, LIST_W - 64, reqs[r].intro[0] ? reqs[r].intro : T("wants to be your friend"), MUTED);
        cui_round_rect(&c, 12, y + 40, 100, 20, 8, MINE);
        cui_text_center(&c, f_s, 12, y + 40, 100, 20, T("Accept"), 0xFFFFFF);
        cui_round_rect(&c, 122, y + 40, 100, 20, 8, 0x4A5068);
        cui_text_center(&c, f_s, 122, y + 40, 100, 20, T("Decline"), 0xFFFFFF);
    }
    reorder();
    for (int r = 0; r < nct; r++) {
        int i = order[r];
        int y = HEAD_H + 1 + nreq * REQ_H + r * ROW_H - list_scroll;
        if (y + ROW_H < HEAD_H || y > win_h) continue;
        if (i == cur) cui_fill(&c, 0, y, LIST_W, ROW_H, SELBG);
        av_draw(ct[i].avatar, ct[i].name, 12, y + 9, 36, is_group(&ct[i]) ? 0x2F8F6A : cui_mix(ACCENT, 0x7040C0, (i * 53) & 255));
        cui_text_fit(&c, f_t, 58, y + 8, LIST_W - 58 - 40, ct[i].name, TEXT);
        char pvb[160]; const char *last = ct[i].n ? ((ct[i].m[ct[i].n - 1].flags & MF_DELETED) ? TXT_DELETED : att_preview(&ct[i].m[ct[i].n - 1], pvb, sizeof pvb)) : "";
        char gb[40]; if (!ct[i].n && is_group(&ct[i])) { snprintf(gb, sizeof gb, T("Group - %d members"), ct[i].ngm); last = gb; }
        if (ct[i].typing_until && cos_time_ms() >= ct[i].typing_until) ct[i].typing_until = 0;
        if (ct[i].typing_until) cui_text_fit(&c, f_s, 58, y + 29, LIST_W - 58 - 12, "入力中…", 0x58D678);
        else if (ct[i].draft && ct[i].draft[0] && i != cur) { char db[200]; snprintf(db, sizeof db, "[下書き] %s", ct[i].draft); cui_text_fit(&c, f_s, 58, y + 29, LIST_W - 58 - 12, db, WARN); }
        else cui_text_fit(&c, f_s, 58, y + 29, LIST_W - 58 - 12, last, MUTED);
        if (ct[i].pinned) cui_text(&c, f_s, LIST_W - 62, y + 33, "固定", ACCENT);
        if (ct[i].muted) cui_text(&c, f_s, LIST_W - 36, y + 33, "消音", MUTED);
        if (ct[i].pres == 1 && !is_group(&ct[i])) { cui_round_rect(&c, 12 + 26, y + 9 + 26, 12, 12, 6, PANEL); cui_round_rect(&c, 12 + 28, y + 9 + 28, 8, 8, 4, 0x58D678); }
        if (ct[i].unread) {
            char ub[8]; snprintf(ub, sizeof ub, "%d", ct[i].unread > 99 ? 99 : ct[i].unread);
            cui_round_rect(&c, LIST_W - 36, y + 10, 24, 20, 10, ct[i].muted ? 0x4A5068 : BAD);
            cui_text_center(&c, f_s, LIST_W - 36, y + 10, 24, 20, ub, 0xFFFFFF);
            if (ct[i].mention) { cui_round_rect(&c, LIST_W - 62, y + 10, 22, 20, 10, WARN); cui_text_center(&c, f_s, LIST_W - 62, y + 10, 22, 20, "@", 0x000000); }
        }
    }
    cui_unclip(&c);
    if (list_max() > 0) {            /* scrollbar */
        int vh = win_h - HEAD_H - 1, th = vh * vh / (vh + list_max()); if (th < 24) th = 24;
        cui_round_rect(&c, LIST_W - 6, HEAD_H + 1 + (vh - th) * list_scroll / list_max(), 4, th, 2, 0x4A5068);
    }
    if (nct == 0 && nreq == 0) cui_text_center(&c, f_s, 0, HEAD_H, LIST_W, 120, T("No contacts yet - open the Menu"), MUTED);
    cui_fill(&c, LIST_W, 0, 1, win_h, LINE);
    /* header */
    int cx = LIST_W + 1, cw = win_w - cx;
    cui_fill(&c, cx, 0, cw, HEAD_H, PANEL);
    if (cur >= 0) {
        av_draw(ct[cur].avatar, ct[cur].name, cx + 18, 22, 60, is_group(&ct[cur]) ? 0x2F8F6A : cui_mix(ACCENT, 0x7040C0, (cur * 53) & 255));
        cui_text(&c, f_h, cx + 92, 30, ct[cur].name, TEXT);
        char b[80];
        char gl[200];
        if (is_group(&ct[cur])) {
            size_t o = (size_t)snprintf(gl, sizeof gl, T("Group ID %s  -  %d members: "), ct[cur].uid, ct[cur].ngm);
            for (int q = 0; q < ct[cur].ngm && o + 30 < sizeof gl; q++) o += (size_t)snprintf(gl + o, sizeof gl - o, "%s%s", q ? ", " : "", ct[cur].gm[q].name);
            snprintf(b, sizeof b, "%s", ""); cui_text_fit(&c, f_s, cx + 92, 58, cw - 110, gl, MUTED);
        }
        else if (ct[cur].uid[0]) snprintf(b, sizeof b, "%s%s%sC-OS ID %s", ct[cur].handle[0] ? "@" : "", ct[cur].handle, ct[cur].handle[0] ? "   -   " : "", ct[cur].uid);
        else snprintf(b, sizeof b, "LAN %u.%u.%u.%u", ct[cur].ip >> 24, (ct[cur].ip >> 16) & 255, (ct[cur].ip >> 8) & 255, ct[cur].ip & 255);
        cui_text(&c, f_s, cx + 92, 58, b, MUTED);
        if (is_group(&ct[cur])) {
            bool ready = e2e_ready && gk_newest(ct[cur].uid) != NULL; bool chg = false; for (int q = 0; q < ct[cur].ngm; q++) if (pkr(ct[cur].gm[q].uid)->kchanged) chg = true;
            cui_text(&c, f_s, cx + 92, 76, chg ? "! メンバーの鍵が変わりました" : ready ? "E2E 暗号化" : e2e_ready ? "暗号化 (最初の発言で鍵を作ります)" : "暗号化なし", chg ? BAD : ready ? 0x58D678 : WARN);
        }
        if (!is_group(&ct[cur])) {
            contact_t *kk = &ct[cur]; const char *lt = KP(kk)->kchanged ? "! 鍵が変わりました" : KP(kk)->ks == 2 ? "E2E 確認済み" : KP(kk)->ks ? "E2E 暗号化" : "暗号化なし";
            uint32_t lc = KP(kk)->kchanged ? BAD : KP(kk)->ks ? 0x58D678 : WARN; int lx = cx + 92 + cui_text_width(f_s, b) + 16;
            cui_text(&c, f_s, lx, 58, lt, lc);
        }
        {   contact_t *kc = &ct[cur]; char sb[80] = ""; uint32_t scol = MUTED;
            if (kc->typing_until && cos_time_ms() >= kc->typing_until) kc->typing_until = 0;
            if (kc->typing_until) { if (is_group(kc) && kc->typing_name[0]) snprintf(sb, sizeof sb, "%s が入力中…", kc->typing_name); else snprintf(sb, sizeof sb, "入力中…"); scol = 0x58D678; }
            else if (!is_group(kc) && kc->uid[0] && kc->pres == 1) { snprintf(sb, sizeof sb, "オンライン"); scol = 0x58D678; }
            else if (!is_group(kc) && kc->uid[0] && kc->pres == 2 && kc->pres_ts) {
                int64_t d = (int64_t)cos_time_unix() - (int64_t)kc->pres_ts; if (d < 0) d = 0;
                if (d < 90) snprintf(sb, sizeof sb, "たった今オンラインでした"); else if (d < 3600) snprintf(sb, sizeof sb, "最終ログイン %d分前", (int)(d / 60));
                else if (d < 86400) snprintf(sb, sizeof sb, "最終ログイン %d時間前", (int)(d / 3600)); else snprintf(sb, sizeof sb, "最終ログイン %d日前", (int)(d / 86400));
            }
            if (sb[0]) cui_text(&c, f_s, cx + 92, 76, sb, scol);
        }
        gbtn(win_w - 110, 28, 94, 30, T("Actions"), 0x2F5C9E);
        int bar = (is_group(&ct[cur]) && ct[cur].ann[0]) ? ANN_H : 0;
        draw_chat(cx, HEAD_H + bar, cw, win_h - HEAD_H - INPUT_H - bar);
        if (bar) {
            cui_fill(&c, cx, HEAD_H, cw, bar, 0x2A2540); cui_fill(&c, cx, HEAD_H + bar - 1, cw, 1, LINE);
            cui_text(&c, f_b, cx + 16, HEAD_H + 7, "告知", WARN);
            cui_text_fit(&c, f_s, cx + 64, HEAD_H + 9, cw - 80, ct[cur].ann, TEXT);
        }
    } else {
        draw_chat(cx, HEAD_H, cw, win_h - HEAD_H - INPUT_H);
    }
    cui_fill(&c, cx, HEAD_H, cw, 1, LINE);
    /* input */
    int iy = win_h - INPUT_H;
    if (edit_msg >= 0) {
        cui_fill(&c, cx, iy - 24, cw, 24, 0x3A3320);
        cui_text(&c, f_s, cx + 16, iy - 19, T("Editing a message  -  Enter: save   Esc: cancel"), WARN);
    }
    else if (reply_hdr[0]) {
        char rl[140]; snprintf(rl, sizeof rl, "返信: %s  %s", reply_name, reply_snip);
        cui_fill(&c, cx, iy - 24, cw, 24, 0x23304A); cui_fill(&c, cx, iy - 24, 3, 24, ACCENT);
        cui_text_fit(&c, f_s, cx + 16, iy - 19, cw - 130, rl, 0xC0D0F0);
        cui_text(&c, f_s, cx + cw - 110, iy - 19, T("Esc: cancel"), MUTED);
    }
    stg_draw();
    cui_fill(&c, cx, iy, cw, INPUT_H, PANEL);
    cui_fill(&c, cx, iy, cw, 1, LINE);
    int boxw = cw - 28 - 84 - 52 - 48, bxl = cx + 14 + 48;
    cui_round_rect(&c, cx + 14, iy + 10, 40, INPUT_H - 20, 10, 0x2B3550);
    cui_text_center(&c, f_h, cx + 14, iy + 10, 40, INPUT_H - 20, "+", 0xFFFFFF);
    cui_round_rect(&c, bxl, iy + 10, boxw, INPUT_H - 20, 10, BG);
    {
        int avail = boxw - 24, ty = iy + (INPUT_H - lh) / 2 + 1;
        int st = ime ? cosime_state(ime) : COSIME_IDLE;
        const char *pre = st ? cosime_text(ime) : "";
        char left[MAX_TEXT]; memcpy(left, in, (size_t)in_pos); left[in_pos] = 0;
        int wl = cui_text_width(f_t, left), wp = pre[0] ? cui_text_width(f_t, pre) : 0;
        int caret = wl + wp, ox = caret > avail - 6 ? caret - (avail - 6) : 0;
        int x0 = bxl + 12 - ox;
        cui_clip(&c, bxl + 12, iy + 10, avail, INPUT_H - 20);
        if (in_len || pre[0]) {
            if (in_pos) cui_text(&c, f_t, x0, ty, left, TEXT);
            int px = x0 + wl, focus_x = px, focus_w = 0;
            if (st == COSIME_CONVERTING) {
                int sx = px;
                for (int i = 0; i < cosime_nseg(ime); i++) {
                    const char *t = cosime_seg_text(ime, i); int tw = cui_text_width(f_t, t);
                    if (i == cosime_focus(ime)) { cui_fill(&c, sx, ty - 1, tw, lh + 2, 0x2F5C9E); focus_x = sx; focus_w = tw; }
                    else cui_fill(&c, sx, ty + lh, tw, 1, MUTED);
                    cui_text(&c, f_t, sx, ty, t, TEXT);
                    sx += tw;
                }
            } else if (pre[0]) {
                cui_text(&c, f_t, px, ty, pre, TEXT);
                cui_fill(&c, px, ty + lh, wp, 1, ACCENT);
            }
            if (in_pos < in_len) cui_text(&c, f_t, px + wp, ty, in + in_pos, TEXT);
            ime_popup_x = focus_x; (void)focus_w;
        } else cui_text(&c, f_t, bxl + 12, ty, T("Message  (+ or drop files to attach)"), MUTED);
        if (((cos_time_ms() / 500) & 1) && st != COSIME_CONVERTING) cui_fill(&c, x0 + caret, iy + 17, 2, INPUT_H - 34, ACCENT);
        cui_unclip(&c);
        if (st == COSIME_CONVERTING) {                       /* candidate list above the input line */
            int n = cosime_ncand(ime), sel = cosime_cand_sel(ime), page = sel / 9, first = page * 9, rows = n - first < 9 ? n - first : 9;
            int mw = 120;
            for (int i = 0; i < rows; i++) { int w2 = cui_text_width(f_t, cosime_cand(ime, first + i)) + 56; if (w2 > mw) mw = w2; }
            int px = ime_popup_x; if (px + mw > win_w - 8) px = win_w - 8 - mw; if (px < bxl) px = bxl;
            int ph = rows * 26 + 24, py = iy - ph - 6;
            cui_round_rect(&c, px, py, mw, ph, 8, 0x0E1220);
            cui_round_rect_outline(&c, px, py, mw, ph, 8, 1.0f, LINE);
            for (int i = 0; i < rows; i++) {
                int ry = py + 6 + i * 26;
                if (first + i == sel) cui_round_rect(&c, px + 4, ry, mw - 8, 24, 6, 0x2F5C9E);
                char nb[8]; snprintf(nb, sizeof nb, "%d", i + 1);
                cui_text(&c, f_s, px + 12, ry + 6, nb, MUTED);
                cui_text(&c, f_t, px + 34, ry + 3, cosime_cand(ime, first + i), TEXT);
            }
            char pg[24]; snprintf(pg, sizeof pg, "%d / %d", sel + 1, n);
            cui_text(&c, f_s, px + mw - cui_text_width(f_s, pg) - 10, py + ph - 17, pg, MUTED);
        }
    }
    {   /* Japanese input mode button */
        bool on = ime && cosime_enabled(ime);
        int bx = win_w - 14 - 76 - 8 - 44;
        cui_round_rect(&c, bx, iy + 10, 44, INPUT_H - 20, 10, on ? 0x2F5C9E : 0x2B3550);
        cui_text_center(&c, f_b, bx, iy + 10, 44, INPUT_H - 20, on ? "あ" : "A", 0xFFFFFF);
    }
    cui_round_rect(&c, win_w - 14 - 76, iy + 10, 76, INPUT_H - 20, 10, ACCENT);
    cui_text_center(&c, f_b, win_w - 14 - 76, iy + 10, 76, INPUT_H - 20, T("Send"), 0xFFFFFF);
    if (status[0] && cos_time_ms() < status_until) {
        int sw = cui_text_width(f_s, status) + 24;
        cui_round_rect(&c, cx + (cw - sw) / 2, HEAD_H + 10, sw, 26, 13, 0x000000);
        cui_text(&c, f_s, cx + (cw - sw) / 2 + 12, HEAD_H + 15, status, WARN);
    }
    sr_draw();
    pf_draw();
    gl_draw();
    pw_draw();
    pk_draw();
    menu_draw();
    form_draw();
    view_draw();
    cpop_draw(&ctxm, &c, &cx_theme);
}

/* ---------------------------------------------------------------- input */
static void start_reply(contact_t *k, msg_t *m) {
    if (m->flags & MF_DELETED) return;
    const char *nm = m->mine ? my_name : is_group(k) ? (gm_name(k, m->sid) ? gm_name(k, m->sid) : (m->who[0] ? m->who : m->sid)) : k->name;
    char pv[160]; const char *body = att_preview(m, pv, sizeof pv);
    clean_snip(reply_name, sizeof reply_name, nm, 20); clean_snip(reply_snip, sizeof reply_snip, body, 48);
    for (char *q = reply_name; *q; q++) if (*q == 0x1f) *q = ' ';
    snprintf(reply_hdr, sizeof reply_hdr, "\x01Q%s\x1f%s\x02", reply_name, reply_snip);
    edit_msg = -1; sel_msg = -1; dirty = true;
}
static void cancel_edit(void) { if (edit_msg >= 0) in_clear(); edit_msg = -1; sel_msg = -1; reply_hdr[0] = 0; dirty = true; }
static void draft_keep(int ci) {                    /* what is typed now stays with the chat that is being left */
    if (ci < 0 || ci >= nct || edit_msg >= 0) return;
    char *nd = in_len ? strdup(in) : NULL; free(ct[ci].draft); ct[ci].draft = nd; drafts_dirty = true; drafts_t = cos_time_ms();
}
static void select_contact(int i) {
    if (i != cur) { draft_keep(cur); in_clear(); reply_hdr[0] = 0; if (ct[i].draft) { in_set(ct[i].draft); char lb[60]; snprintf(lb, sizeof lb, "[MESSENGER] draft restored (%d bytes)\n", in_len); cos_puts(lb); } }
    cancel_edit(); if (i != cur) note_clear();
    load_chat(&ct[i]); div_idx = -1;                       /* the line above the first message that was not read yet */
    if (ct[i].unread > 0) { int u = ct[i].unread; for (int j = ct[i].n - 1; j >= 0; j--) if (!ct[i].m[j].mine && --u == 0) { if (j > 0) div_idx = j; break; } }
    cur = i; ct[i].unread = 0; ct[i].mention = 0; scroll_px = 0; dirty = true; send_reads(&ct[i]);
    reorder(); int top = nreq * REQ_H + order_pos(i) * ROW_H, vh = win_h - HEAD_H - 1;           /* scroll the list so the row is visible */
    if (top < list_scroll) list_scroll = top; else if (top + ROW_H > list_scroll + vh) list_scroll = top + ROW_H - vh;
    list_clamp();
}

static void key(const cos_win_event_t *ev) {
    if (fm.open && form_special(ev)) return;
    if (mn.open) { if (ev->special == COS_KEY_ESC) menu_close(); return; }
    if (view.kind) { view_key(ev); return; }
    if (pk.open) { pk_key(ev); return; }
    if (gl.open) { if (ev->special == COS_KEY_ESC) { gl.open = false; dirty = true; } return; }
    if ((sr.open || pf.open) && ev->special == COS_KEY_ESC) { sr.open = pf.open = false; dirty = true; return; }
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == ' ' || ((ev->mods & COS_MOD_SHIFT) && (ev->ascii == 'j' || ev->ascii == 'J' || ev->ascii == 10)))) { toggle_ime(); return; }
    if (ime && cosime_enabled(ime)) {
        bool bypass = in_len == 0 && ev->ascii == '/' && cosime_state(ime) == COSIME_IDLE;      /* commands start with a plain "/" */
        if (!bypass) {
            if (ev->ascii >= 'a' && ev->ascii <= 'z') ime_ensure_loaded();
            char commit[1024];
            int r = cosime_key(ime, ev, commit, sizeof commit);
            if (r & COSIME_COMMIT) in_insert(commit);
            dirty = true;
            if (r & COSIME_CONSUMED) return;
        }
    }
    if (ev->special == COS_KEY_ESC) { cancel_edit(); return; }
    if (ev->special == COS_KEY_ENTER) { submit(); return; }
    if (ev->special == COS_KEY_BACKSPACE) { in_backspace(); return; }
    if (ev->special == COS_KEY_DELETE) { in_delete(); return; }
    if (ev->special == COS_KEY_LEFT) { in_pos = in_prev(in_pos); dirty = true; return; }
    if (ev->special == COS_KEY_RIGHT) { in_pos = in_next(in_pos); dirty = true; return; }
    if (ev->special == COS_KEY_HOME) { in_pos = 0; dirty = true; return; }
    if (ev->special == COS_KEY_END) { in_pos = in_len; dirty = true; return; }
    if (ev->special == COS_KEY_UP && nct) { reorder(); int p = cur >= 0 ? order_pos(cur) : 0; select_contact(order[p > 0 ? p - 1 : nct - 1]); return; }
    if (ev->special == COS_KEY_DOWN && nct) { reorder(); int p = cur >= 0 ? order_pos(cur) : -1; select_contact(order[(p + 1) % nct]); return; }
    if (ev->special == COS_KEY_PGUP) { scroll_px += 200; dirty = true; return; }
    if (ev->special == COS_KEY_PGDN) { scroll_px -= 200; if (scroll_px < 0) scroll_px = 0; dirty = true; return; }
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == 'v' || ev->ascii == 'V')) {
        char tmp[MAX_TEXT]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1);
        if (n < 0) n = 0;
        tmp[n] = 0;
        for (char *q = tmp; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
        in_insert(tmp); return;
    }
    if ((unsigned char)ev->ascii >= 32 && ev->ascii != 127 && !(ev->mods & COS_MOD_CTRL)) { char one[2] = { ev->ascii, 0 }; in_insert(one); }
}

/* A file dropped on the window. Where it lands decides what happens: on your own picture (top left) or while the profile
 * is open it becomes your profile photo; on a friend in the list it is sent to that friend; anywhere else it is sent to
 * the open conversation. x,y are -1 for a file the system opened with the app (no position). */
static void handle_drop(int x, int y, const char *path) {
    if (view.kind) return;
    if (pk.open && pk.avatar) { pk.open = pk.avatar = false; prof_set_photo(path); dirty = true; return; }
    if (pk.open) return;
    if (pf.open) { prof_set_photo(path); return; }
    if (x >= 0 && x < LIST_W && y < 56) { prof_set_photo(path); return; }
    if (x >= 0 && x < LIST_W && y > HEAD_H && y + list_scroll > HEAD_H + nreq * REQ_H) {
        int i = (y + list_scroll - HEAD_H - 1 - nreq * REQ_H) / ROW_H;
        if (i >= 0 && i < nct) { select_contact(i); stage_add(path); return; }
    }
    if (cur >= 0) stage_add(path); else set_status(T("Select a friend first"));
}

/* ---------------------------------------------------------------- right-click menus */
enum { CM_REPLY = 1, CM_COPY, CM_LINK, CM_EDIT, CM_DELETE, CM_ATT_OPEN, CM_ATT_SAVE, CM_SELECT,
       CM_OPEN_CHAT, CM_CHAT_ACTIONS, CM_COPY_ID, CM_ACCEPT, CM_DECLINE,
       CM_MYID, CM_PROFILE, CM_FIND, CM_MENU,
       CM_PASTE, CM_COPY_INPUT, CM_CUT_INPUT, CM_CLEAR_INPUT, CM_IME, CM_SEND, CM_ATTACH, CM_LATEST };
static int cx_arg;
static void cx_add(int cmd, const char *label, bool en) { cpop_add(&ctxm, T(label), "", cmd, en, false); }
static void cx_show(int x, int y) { cpop_show(&ctxm, x, y, win_w, win_h, f_s, f_s); dirty = true; }
static void in_paste(void) {
    char tmp[MAX_TEXT]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1);
    if (n < 0) n = 0;
    tmp[n] = 0;
    for (char *q = tmp; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    in_insert(tmp);
}
static void cx_run(int cmd) {
    dirty = true;
    contact_t *k = cur >= 0 ? &ct[cur] : NULL;
    switch (cmd) {
        case CM_REPLY: if (k && cx_arg < k->n) start_reply(k, &k->m[cx_arg]); return;
        case CM_COPY: if (k && cx_arg < k->n) { const char *bd = msg_body(k->m[cx_arg].text); cos_clip_set(bd, strlen(bd)); set_status(T("Copied")); } return;
        case CM_LINK: if (k && cx_arg < k->n) { char u[256]; if (first_url(msg_body(k->m[cx_arg].text), u, sizeof u)) { cos_clip_set(u, strlen(u)); set_status(T("Link copied")); } } return;
        case CM_EDIT: if (k && cx_arg < k->n && k->m[cx_arg].mine) { edit_msg = cx_arg; reply_hdr[0] = 0; in_set(msg_body(k->m[cx_arg].text)); } return;
        case CM_DELETE: if (k && cx_arg < k->n && k->m[cx_arg].mine) { delete_message(k, &k->m[cx_arg]); sel_msg = -1; } return;
        case CM_ATT_OPEN: if (k && cx_arg < k->n) att_action(k, &k->m[cx_arg], 3); return;
        case CM_ATT_SAVE: if (k && cx_arg < k->n) att_action(k, &k->m[cx_arg], 4); return;
        case CM_SELECT: sel_msg = sel_msg == cx_arg ? -1 : cx_arg; return;
        case CM_OPEN_CHAT: if (cx_arg >= 0 && cx_arg < nct) select_contact(cx_arg); return;
        case CM_CHAT_ACTIONS: if (cx_arg >= 0 && cx_arg < nct) { select_contact(cx_arg); menu_open(PG_CHAT); } return;
        case CM_COPY_ID: if (cx_arg >= 0 && cx_arg < nct && ct[cx_arg].uid[0]) { cos_clip_set(ct[cx_arg].uid, strlen(ct[cx_arg].uid)); set_status(T("ID copied to the clipboard")); } return;
        case CM_ACCEPT: case CM_DECLINE: if (cx_arg >= 0 && cx_arg < nreq) { char cc[24]; snprintf(cc, sizeof cc, "%s %s", cmd == CM_ACCEPT ? "/accept" : "/decline", reqs[cx_arg].uid); run_command(cc); } return;
        case CM_MYID: if (acc_id[0]) { cos_clip_set(acc_id, strlen(acc_id)); set_status(T("ID copied to the clipboard")); } return;
        case CM_PROFILE: prof_open_profile(); return;
        case CM_FIND: menu_do(A_FIND, 0); return;
        case CM_MENU: menu_open(PG_MAIN); return;
        case CM_PASTE: in_paste(); return;
        case CM_COPY_INPUT: if (in_len) { cos_clip_set(in, (size_t)in_len); set_status(T("Copied")); } return;
        case CM_CUT_INPUT: if (in_len) { cos_clip_set(in, (size_t)in_len); in_clear(); set_status(T("Copied")); } return;
        case CM_CLEAR_INPUT: in_clear(); return;
        case CM_IME: toggle_ime(); return;
        case CM_SEND: submit(); return;
        case CM_ATTACH: pick_launch(); return;
        case CM_LATEST: scroll_px = 0; return;
    }
}
/* a right button press at (x, y): opens the menu that belongs to what is under the pointer */
static void right_click(int x, int y) {
    cpop_clear(&ctxm);
    if (fm.open || mn.open || gl.open || pk.open || sr.open || pf.open || view.kind) return;
    contact_t *k = cur >= 0 ? &ct[cur] : NULL;
    if (x < LIST_W && y < HEAD_H) {                                      /* your own card */
        cx_add(CM_MYID, "Copy my ID", acc_id[0] != 0);
        cx_add(CM_PROFILE, "Profile and photo", true);
        cx_add(CM_FIND, "Find people", true);
        cpop_sep(&ctxm);
        cx_add(CM_MENU, "Menu", true);
        cx_show(x, y); return;
    }
    if (x < LIST_W) {
        int yy = y + list_scroll;
        if (yy < HEAD_H + 1 + nreq * REQ_H) {                            /* a friend request */
            int r = (yy - HEAD_H - 1) / REQ_H;
            if (r >= 0 && r < nreq) { cx_arg = r; cx_add(CM_ACCEPT, "Accept", true); cx_add(CM_DECLINE, "Decline", true); cx_show(x, y); }
            return;
        }
        reorder(); int i = (yy - HEAD_H - 1 - nreq * REQ_H) / ROW_H;
        if (y > HEAD_H && i >= 0 && i < nct) {                           /* a conversation */
            cx_arg = order[i];
            cx_add(CM_OPEN_CHAT, "Open", true);
            cx_add(CM_COPY_ID, "Copy ID", ct[cx_arg].uid[0] != 0);
            cpop_sep(&ctxm);
            cx_add(CM_CHAT_ACTIONS, "Actions...", true);
            cx_show(x, y);
        }
        return;
    }
    if (k && y >= win_h - INPUT_H) {                                     /* the input line */
        cx_add(CM_PASTE, "Paste", true);
        cx_add(CM_CUT_INPUT, "Cut all", in_len > 0);
        cx_add(CM_COPY_INPUT, "Copy all", in_len > 0);
        cx_add(CM_CLEAR_INPUT, "Clear", in_len > 0);
        cpop_sep(&ctxm);
        cx_add(CM_ATTACH, "Attach a file...", true);
        cpop_add(&ctxm, T("Japanese input"), "Ctrl+Space", CM_IME, true, ime && cosime_enabled(ime));
        cx_add(CM_SEND, "Send", in_len > 0);
        cx_show(x, y); return;
    }
    if (k && y < HEAD_H) {                                               /* the conversation's header */
        cx_arg = cur; cx_add(CM_CHAT_ACTIONS, "Actions...", true); cx_add(CM_LATEST, "Jump to the latest message", true);
        cx_show(x, y); return;
    }
    if (k && y >= HEAD_H) {                                              /* the message area */
        int pick = -1, kind = -1;
        for (int i = 0; i < nhits; i++) {
            hit_t *t = &hits[i];
            if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h && t->kind == 0 && t->kind >= kind) { pick = t->msg; kind = t->kind; }
        }
        if (pick >= 0 && pick < k->n) {
            msg_t *m = &k->m[pick]; cx_arg = pick; sel_msg = pick;
            att_t a; bool is = false; att_inspect(m, &a, &is);
            const char *bd = msg_body(m->text); char u[256];
            cx_add(CM_REPLY, "Reply", true);
            if (is) {
                cx_add(CM_ATT_OPEN, m->att == AS_REMOTE ? "Download" : "Open", m->att == AS_REMOTE || m->att == AS_READY);
                cx_add(CM_ATT_SAVE, "Save as...", true);
            } else {
                cx_add(CM_COPY, "Copy", true);
                cx_add(CM_LINK, "Copy link", first_url(bd, u, sizeof u));
            }
            cpop_sep(&ctxm);
            if (m->mine) { if (!is) cx_add(CM_EDIT, "Edit", true); cx_add(CM_DELETE, "Delete", true); cpop_sep(&ctxm); }
            cx_add(CM_SELECT, "Select", true);
            cx_show(x, y); return;
        }
        cx_arg = cur; cx_add(CM_LATEST, "Jump to the latest message", true); cx_add(CM_CHAT_ACTIONS, "Actions...", true);
        cx_show(x, y);
    }
}

static void click(int x, int y) {
    if (fm.open) { form_click(x, y); return; }
    if (mn.open) { menu_click(x, y); return; }
    if (gl.open) { gl_click(x, y); return; }
    if (view.kind) { view_click(x, y); return; }
    if (pk.open) { pk_click(x, y); return; }
    if (sr.open) { sr_click(x, y); return; }
    if (pf.open) { pf_click(x, y); return; }
    if (stg_click(x, y)) return;
    if (x < LIST_W && y < HEAD_H) {
        if (x >= 150 && x < 238 && y >= 34 && y < 54) menu_open(PG_MAIN);
        else if (x >= 150 && x < 238 && y >= 58 && y < 78) menu_do(A_FIND, 0);
        else if (x >= 150 && x < 238 && y >= 80 && y < 100) prof_open_profile();
        else if (acc_id[0]) { cos_clip_set(acc_id, strlen(acc_id)); set_status(T("ID copied to the clipboard")); }
    } else if (x < LIST_W && y > HEAD_H && y + list_scroll < HEAD_H + 1 + nreq * REQ_H) {
        int r = (y + list_scroll - HEAD_H - 1) / REQ_H;
        if (y + list_scroll - HEAD_H - 1 - r * REQ_H >= 40) {
            char cmd[24]; snprintf(cmd, sizeof cmd, "%s %s", x < 117 ? "/accept" : "/decline", reqs[r].uid);
            run_command(cmd); dirty = true;
        }
    } else if (x < LIST_W) {
        int yy = y + list_scroll, i = (yy - HEAD_H - 1 - nreq * REQ_H) / ROW_H;
        reorder();
        if (y > HEAD_H && yy > HEAD_H + nreq * REQ_H && i >= 0 && i < nct) select_contact(order[i]);
    } else if (cur >= 0 && x >= win_w - 110 && x < win_w - 16 && y >= 28 && y < 58) {
        menu_open(PG_CHAT);
    } else if (cur >= 0 && x > LIST_W && y >= HEAD_H && y < HEAD_H + ANN_H && is_group(&ct[cur]) && ct[cur].ann[0]) {
        const char *by = gm_name(&ct[cur], ct[cur].ann_by); char nb[400];
        snprintf(nb, sizeof nb, "告知%s%s\n> %s", by ? " - " : "", by ? by : "", ct[cur].ann); note_add(nb);
    } else if (y >= win_h - INPUT_H && x >= win_w - 14 - 76) submit();
    else if (y >= win_h - INPUT_H && x >= win_w - 14 - 76 - 8 - 44 && x < win_w - 14 - 76 - 8) toggle_ime();
    else if (y >= win_h - INPUT_H && x >= LIST_W + 1 + 14 && x < LIST_W + 1 + 14 + 40) pick_launch();
    else if (cur >= 0 && y >= HEAD_H && y < win_h - INPUT_H) {
        int pick = -1, kind = -1;
        for (int i = 0; i < nhits; i++) {
            hit_t *t = &hits[i];
            if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h && t->kind >= kind) { pick = t->msg; kind = t->kind; }
        }
        if (kind == 5 && pick < ct[cur].n) start_reply(&ct[cur], &ct[cur].m[pick]);
        else if ((kind == 6 || kind == 7) && pick < ct[cur].n) {
            const char *bd = msg_body(ct[cur].m[pick].text);
            if (kind == 6) { cos_clip_set(bd, strlen(bd)); set_status(T("Copied")); }
            else { char u[256]; if (first_url(bd, u, sizeof u)) { cos_clip_set(u, strlen(u)); set_status(T("Link copied")); } }
            sel_msg = -1;
        }
        else if (kind == 1 && pick < ct[cur].n && ct[cur].m[pick].mine) {
            msg_t *m = &ct[cur].m[pick];
            edit_msg = pick; reply_hdr[0] = 0; in_set(msg_body(m->text));
        } else if (kind == 2 && pick < ct[cur].n && ct[cur].m[pick].mine) { delete_message(&ct[cur], &ct[cur].m[pick]); sel_msg = -1; }
        else if ((kind == 3 || kind == 4) && pick < ct[cur].n) att_action(&ct[cur], &ct[cur].m[pick], kind);
        else if (kind == 0) sel_msg = (sel_msg == pick) ? -1 : pick;
        else sel_msg = -1;
        dirty = true;
    }
}

static void i18n_poll(bool force) {                              /* the interface follows the system language, also when it is changed while the app runs */
    static uint64_t last; uint64_t now = cos_time_ms(); if (!force && last && now - last < 1000) return; last = now;
    cos_settings_t st; if (cos_settings_get(&st) != 0) return;
    if ((st.japanese != 0) != ui_ja) { ui_ja = st.japanese != 0; dirty = true; }
}
/* what the app reads from disk at start (and again after an unlock) */
static void load_all(void) {
    load_account();
    load_contacts();
    e2e_init(); e2e_load_peers(); gk_load(); rost_load();
    load_unread();
    prefs_load();
    cur = nct ? 0 : -1;
    if (cur >= 0 && ct[cur].draft) in_set(ct[cur].draft);
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("C-OS Message", 860, 560, &wi);
    if (h <= 0) return 1;
    g_win = h;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    load_jp_font();
    f_t = mkfont(15, false); f_s = mkfont(12, false); f_b = mkfont(14, true); f_h = mkfont(17, true);
    lh = cui_font_height(f_t) + 2;

    cos_netinfo_t ni;
    for (int i = 0; i < 100; i++) { if (cos_net_info(&ni) == 0 && ni.has_lease) break; cos_sleep_ms(100); }
    my_ip = COS_IP(ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3]);
    snprintf(my_name, sizeof my_name, "C-OS-%u", my_ip & 255);
    {   cos_stat_t pst; char pv[4] = {0};
        if (cos_stat(IME_PREF, &pst) == 0 && cos_read_file(IME_PREF, pv, 1) == 1 && pv[0] == '1') { ime = cosime_new(); cosime_set_enabled(ime, true); }
    }
    next_id = (uint32_t)(cos_time_unix() % 86400) << 8;
    vault_startup();
    if (!vault_locked()) load_all();
    {   char hv[2] = {0}; if (cos_stat(DIR "/host.txt", &(cos_stat_t){0}) == 0 && cos_read_file(DIR "/host.txt", hv, 1) == 1 && hv[0] == '1') { host_wanted = true; if (!host_pid()) { host_e2e_default(); cos_spawn(SRV_BIN); } } }     /* /host on stays on after a restart */

    sock = cos_socket(COS_SOCK_DGRAM);
    if (sock < 0 || cos_bind(sock, 0, PORT) != 0) set_status(T("Cannot open UDP port 5150 (is Messenger already running?)"));
    else cos_puts("[MESSENGER] listening on udp 5150\n");

    plug_init();
    for (;;) {
        if (ime && cosime_enabled(ime) && !ime_tried) { draw(); cos_win2_present(h); ime_ensure_loaded(); }   /* the saved Japanese-input mode: load the dictionary once the window is up */
        i18n_poll(false);
        host_watch();
        vault_tick();
        if (!vault_locked()) drafts_tick();
        if (!vault_locked()) {
        plug_poll();
        net_poll();
        srv_poll(); e2e_tick();
        gl_poll();
        att_upload_pump();
        aud_pump();
        }
        {   static int last_total = -1; int t = unread_total();
            if (t != last_total) { last_total = t; char ttl[48]; if (t) snprintf(ttl, sizeof ttl, "C-OS Message (%d)", t); else snprintf(ttl, sizeof ttl, "C-OS Message"); cos_win2_set_title(h, ttl); save_unread(); } }
        if (status[0] && cos_time_ms() >= status_until) { status[0] = 0; dirty = true; }
        pick_poll(); sq_pump();
        if (dirty || (cos_time_ms() / 500) != ((cos_time_ms() - 100) / 500)) { draw(); cos_win2_present(h); dirty = false; }
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, (up.active || aud.playing) ? 20 : 100);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            vault_note_input(&ev);
            if (pwp.open || vault_locked()) {                  /* the password window gets every key and click; nothing else is touched */
                if (ev.type == COS_EV_KEY) pw_key(&ev);
                else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) pw_click(ev.x, ev.y);
                dirty = true; r = cos_win2_poll(h, &ev); continue;
            }
            if (ev.type == COS_EV_KEY && ctxm.open) { int rc = cpop_key(&ctxm, ev.special); if (rc >= 0) cx_run(rc); dirty = true; }
            else if (ev.type == COS_EV_KEY) { key(&ev); if (fm.open) form_store_focus(); }
            else if (ev.type == COS_EV_WHEEL) {
                if (mn.open) menu_wheel(ev.wheel);
                else if (view.kind == 2) { view.scroll -= ev.wheel * 3; int rows = (win_h - VIEW_BAR - 20) / lh, mx = view.nlines > rows ? view.nlines - rows : 0; if (view.scroll < 0) view.scroll = 0; if (view.scroll > mx) view.scroll = mx; dirty = true; }
                else if (pk.open) { pk.scroll -= ev.wheel * 2; int rows = (PK_H - 110) / PK_ROW, mx = pk.n + 1 > rows ? pk.n + 1 - rows : 0; if (pk.scroll < 0) pk.scroll = 0; if (pk.scroll > mx) pk.scroll = mx; dirty = true; }
                else if (!sr.open && !pf.open && !gl.open && ev.x < LIST_W && ev.y > HEAD_H) { list_scroll -= ev.wheel * ROW_H; list_clamp(); dirty = true; }
                else { scroll_px += ev.wheel * 60; if (scroll_px < 0) scroll_px = 0; dirty = true; }
            }
            else if (ev.type == COS_EV_DROP || ev.type == COS_EV_OPEN) {
                char dp[256];
                if (cos_win2_get_path(h, dp, sizeof dp) >= 0) handle_drop(ev.type == COS_EV_DROP ? ev.x : -1, ev.type == COS_EV_DROP ? ev.y : -1, dp);
            }
            else if (ev.type == COS_EV_MOUSE_MOVE && ctxm.open) { cpop_mouse_move(&ctxm, ev.x, ev.y); dirty = true; }
            else if (ev.type == COS_EV_MOUSE_DOWN && ctxm.open) { int rc = cpop_mouse_down(&ctxm, ev.x, ev.y); if (rc >= 0) cx_run(rc); dirty = true; }
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) click(ev.x, ev.y);
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_RIGHT) right_click(ev.x, ev.y);
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_plugins_unload_all(&plugs);
    cos_win2_close(h);
    return 0;
}
