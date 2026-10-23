/* cosime.c - the C-OS Japanese IME (see userland/include/cosime.h).
 *
 * Conversion: the kana is turned into a word lattice (every dictionary reading that is a prefix of the remaining kana,
 * one node per distinct part-of-speech pair, plus a one-character fallback), then the cheapest path under
 * word cost + connection cost is found with Viterbi. The path is cut into bunsetsu (an independent word plus the
 * particles/auxiliaries attached to it); each bunsetsu gets a candidate list: the path's own reading first, then other
 * dictionary entries for the same head, then hiragana and katakana. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../third_party/stb_image.h"
#include "cosime.h"

#define MAX_KANA   200            /* bytes of kana in one composition */
#define MAX_SEGS   40
#define MAX_CANDS  16
#define CAND_LEN   96
#define SURF_SAME  0xFFFFFFFFu
#define SURF_KATA  0xFFFFFFFEu

typedef struct {
    uint8_t *raw;
    uint32_t N, n_read, n_ent, pool_bytes, maxlen, unknown;
    const uint8_t *attached;
    const int16_t *conn;
    const uint32_t *read_off, *ent_start, *surf;
    const uint16_t *lid, *rid, *cost;
    const char *pool;
} dict_t;

typedef struct { char reading[MAX_KANA]; char cand[MAX_CANDS][CAND_LEN]; int ncand, sel; } seg_t;

struct cosime {
    bool enabled, loaded;
    dict_t d;
    int state;
    char kana[MAX_KANA];                /* converted hiragana/katakana typed so far */
    char pend[8];                       /* romaji letters waiting for the rest of their syllable */
    seg_t seg[MAX_SEGS]; int nseg, focus;
    char text[MAX_SEGS * CAND_LEN];     /* scratch for cosime_text() */
};

/* ---------------------------------------------------------------- UTF-8 helpers */
static int u8len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }
static uint32_t u8get(const char *s, int *n) {
    unsigned char c = (unsigned char)s[0]; int l = u8len(c);
    for (int i = 1; i < l; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) { l = 1; c = 0x3F; break; }
    *n = l;
    if (l == 1) return c;
    uint32_t cp = c & (0xFF >> (l + 1));
    for (int i = 1; i < l; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}
static int u8put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}
static bool append(char *dst, size_t cap, const char *s) {
    size_t a = strlen(dst), b = strlen(s);
    if (a + b + 1 > cap) return false;
    memcpy(dst + a, s, b + 1);
    return true;
}
static void to_katakana(const char *in, char *out, size_t cap) {
    size_t o = 0;
    while (*in && o + 5 < cap) { int n; uint32_t cp = u8get(in, &n); in += n; if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60; o += (size_t)u8put(cp, out + o); }
    out[o] = 0;
}
static void to_hiragana(const char *in, char *out, size_t cap) {
    size_t o = 0;
    while (*in && o + 5 < cap) { int n; uint32_t cp = u8get(in, &n); in += n; if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60; o += (size_t)u8put(cp, out + o); }
    out[o] = 0;
}

/* ---------------------------------------------------------------- dictionary */
int cosime_load(cosime *m, const char *path) {
    if (m->loaded) return 0;
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.size < 64) return -1;
    uint8_t *file = (uint8_t *)malloc((size_t)st.size);
    if (!file) return -1;
    if (cos_read_file(path, file, (size_t)st.size) != (ssize_t)st.size) { free(file); return -1; }
    uint32_t rawlen; memcpy(&rawlen, file, 4);
    int outlen = 0;
    char *raw = stbi_zlib_decode_malloc_guesssize((const char *)file + 4, (int)st.size - 4, (int)rawlen + 64, &outlen);
    free(file);
    if (!raw || (uint32_t)outlen != rawlen || memcmp(raw, "COSIME1", 8) != 0) { free(raw); return -2; }
    dict_t *d = &m->d;
    d->raw = (uint8_t *)raw;
    const uint32_t *h = (const uint32_t *)(raw + 8);
    d->N = h[0]; d->n_read = h[1]; d->n_ent = h[2]; d->pool_bytes = h[3]; d->maxlen = h[4]; d->unknown = h[5];
    const uint8_t *p = (const uint8_t *)raw + 8 + 24;
#define ALIGN4(x) (((x) + 3u) & ~3u)
    d->attached = p; p += ALIGN4(d->N);
    d->conn = (const int16_t *)p; p += ALIGN4(d->N * d->N * 2u);
    d->read_off = (const uint32_t *)p; p += (d->n_read + 1) * 4u;
    d->ent_start = (const uint32_t *)p; p += (d->n_read + 1) * 4u;
    d->lid = (const uint16_t *)p; p += ALIGN4(d->n_ent * 2u);
    d->rid = (const uint16_t *)p; p += ALIGN4(d->n_ent * 2u);
    d->cost = (const uint16_t *)p; p += ALIGN4(d->n_ent * 2u);
    d->surf = (const uint32_t *)p; p += d->n_ent * 4u;
    d->pool = (const char *)p;
    m->loaded = true;
    return 0;
}

/* index of the reading exactly equal to s[0..len), or -1 */
static int find_reading(const dict_t *d, const char *s, size_t len) {
    int lo = 0, hi = (int)d->n_read - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *a = d->pool + d->read_off[mid];
        size_t al = strlen(a), ml = al < len ? al : len;
        int c = memcmp(a, s, ml);
        if (c == 0) c = al < len ? -1 : al > len ? 1 : 0;
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void surface_of(const dict_t *d, int ent, const char *reading, char *out, size_t cap) {
    uint32_t sf = d->surf[ent];
    if (sf == SURF_SAME) snprintf(out, cap, "%s", reading);
    else if (sf == SURF_KATA) to_katakana(reading, out, cap);
    else snprintf(out, cap, "%s", d->pool + sf);
}

/* ---------------------------------------------------------------- lattice + Viterbi */
typedef struct { int16_t s, e; uint16_t lid, rid; int cost; int ent; int best; int prev; } node_t;
typedef struct { node_t *n; int cnt, cap; } nodes_t;

static node_t *node_add(nodes_t *v) {
    if (v->cnt == v->cap) {
        int nc = v->cap ? v->cap * 2 : 1024;
        node_t *nn = (node_t *)realloc(v->n, sizeof(node_t) * (size_t)nc);
        if (!nn) return NULL;
        v->n = nn; v->cap = nc;
    }
    return &v->n[v->cnt++];
}

/* Best path for kana k (hiragana). Fills path[] with node copies (in order); returns its length or -1. */
static int viterbi(const dict_t *d, const char *k, node_t *path, int maxpath) {
    int nb = (int)strlen(k);
    int pos[MAX_KANA + 1], nc = 0;
    for (int i = 0; i < nb; ) { int n; u8get(k + i, &n); pos[nc++] = i; i += n; }
    pos[nc] = nb;
    if (nc == 0) return 0;
    nodes_t v = {0};
    int *start_idx = (int *)calloc((size_t)nc + 2, sizeof(int));
    if (!start_idx) return -1;
    static uint32_t seen_key[2048], seen_gen[2048]; static uint32_t gen;
    for (int i = 0; i < nc; i++) {
        start_idx[i] = v.cnt;
        int maxl = (int)d->maxlen < nc - i ? (int)d->maxlen : nc - i;
        for (int l = 1; l <= maxl; l++) {
            int r = find_reading(d, k + pos[i], (size_t)(pos[i + l] - pos[i]));
            if (r < 0) continue;
            gen++;
            for (uint32_t e = d->ent_start[r]; e < d->ent_start[r + 1]; e++) {
                uint32_t key = ((uint32_t)d->lid[e] << 16) | d->rid[e];
                uint32_t hsh = (key * 2654435761u) >> 21;
                bool dup = false;
                while (seen_gen[hsh & 2047] == gen) { if (seen_key[hsh & 2047] == key) { dup = true; break; } hsh++; }
                if (dup) continue;
                seen_gen[hsh & 2047] = gen; seen_key[hsh & 2047] = key;
                node_t *nn = node_add(&v); if (!nn) goto fail;
                *nn = (node_t){ (int16_t)i, (int16_t)(i + l), d->lid[e], d->rid[e], d->cost[e], (int)e, 0, -1 };
            }
        }
        node_t *fb = node_add(&v); if (!fb) goto fail;          /* one-character fallback so a path always exists */
        *fb = (node_t){ (int16_t)i, (int16_t)(i + 1), (uint16_t)d->unknown, (uint16_t)d->unknown, 12000, -1, 0, -1 };
    }
    start_idx[nc] = v.cnt;
    /* nodes ending at each position */
    int *end_cnt = (int *)calloc((size_t)nc + 2, sizeof(int)), *end_list = (int *)malloc(sizeof(int) * (size_t)(v.cnt + 1));
    int *end_first = (int *)calloc((size_t)nc + 2, sizeof(int));
    if (!end_cnt || !end_list || !end_first) { free(end_cnt); free(end_list); free(end_first); goto fail; }
    for (int i = 0; i < v.cnt; i++) end_cnt[v.n[i].e]++;
    for (int p = 1; p <= nc + 1; p++) end_first[p] = end_first[p - 1] + end_cnt[p - 1];
    int *fill = (int *)calloc((size_t)nc + 2, sizeof(int));
    for (int i = 0; i < v.cnt; i++) { int e = v.n[i].e; end_list[end_first[e] + fill[e]++] = i; }
    free(fill);
    const int16_t *conn = d->conn; const int N = (int)d->N;
    for (int p = 0; p < nc; p++) {                              /* nodes starting at p */
        for (int i = start_idx[p]; i < start_idx[p + 1]; i++) {
            node_t *n = &v.n[i];
            int best = 0x7FFFFFFF, prev = -1;
            if (p == 0) { best = conn[0 * N + n->lid] + (d->attached[n->lid] ? 6000 : 0); }   /* an auxiliary/suffix cannot start the text (行きました, not いきました) */
            else for (int t = 0; t < end_cnt[p]; t++) {
                int pi = end_list[end_first[p] + t]; node_t *q = &v.n[pi];
                int c = q->best + conn[q->rid * N + n->lid];
                if (c < best) { best = c; prev = pi; }
            }
            n->best = best + n->cost; n->prev = prev;
        }
    }
    int bestend = 0x7FFFFFFF, last = -1;
    for (int t = 0; t < end_cnt[nc]; t++) {
        int pi = end_list[end_first[nc] + t]; node_t *q = &v.n[pi];
        int c = q->best + conn[q->rid * N + 0];
        if (c < bestend) { bestend = c; last = pi; }
    }
    int len = 0; int tmp[MAX_KANA + 1];
    for (int x = last; x >= 0 && len < MAX_KANA; x = v.n[x].prev) tmp[len++] = x;
    if (len > maxpath) len = maxpath;
    for (int i = 0; i < len; i++) path[i] = v.n[tmp[len - 1 - i]];
    free(end_cnt); free(end_list); free(end_first); free(start_idx); free(v.n);
    return len;
fail:
    free(start_idx); free(v.n);
    return -1;
}

/* ---------------------------------------------------------------- bunsetsu + candidates */
static bool cand_has(const seg_t *s, const char *c) { for (int i = 0; i < s->ncand; i++) if (strcmp(s->cand[i], c) == 0) return true; return false; }
static void cand_add(seg_t *s, const char *c) {
    if (s->ncand >= MAX_CANDS || !c[0] || cand_has(s, c)) return;
    snprintf(s->cand[s->ncand++], CAND_LEN, "%s", c);
}

/* Builds the candidate list of one bunsetsu from the best-path nodes [a,b) of kana k (hiragana). */
static void make_candidates(const cosime *m, seg_t *sg, const char *k, const int *pos, const node_t *path, int a, int b) {
    const dict_t *d = &m->d;
    char surf[CAND_LEN], first[CAND_LEN] = "", tail[CAND_LEN] = "";
    sg->ncand = 0; sg->sel = 0;
    for (int i = a; i < b; i++) {
        const node_t *n = &path[i];
        if (n->ent >= 0) { char rd[CAND_LEN]; int rl = pos[n->e] - pos[n->s]; if (rl > CAND_LEN - 1) rl = CAND_LEN - 1; memcpy(rd, k + pos[n->s], (size_t)rl); rd[rl] = 0; surface_of(d, n->ent, rd, surf, sizeof surf); }
        else { int rl = pos[n->e] - pos[n->s]; memcpy(surf, k + pos[n->s], (size_t)rl); surf[rl] = 0; }
        append(first, sizeof first, surf);
    }
    cand_add(sg, first);
    /* head/tail split: trailing attached words (particles, auxiliaries) stay fixed, the head gets alternatives */
    int t0 = b;
    while (t0 > a + 1 && d->attached[path[t0 - 1].lid]) t0--;
    int head_end_byte = pos[path[t0 - 1].e];
    for (int i = t0; i < b; i++) {
        const node_t *n = &path[i]; int rl = pos[n->e] - pos[n->s];
        char rd[CAND_LEN]; if (rl > CAND_LEN - 1) rl = CAND_LEN - 1; memcpy(rd, k + pos[n->s], (size_t)rl); rd[rl] = 0;
        if (n->ent >= 0) surface_of(d, n->ent, rd, surf, sizeof surf); else snprintf(surf, sizeof surf, "%s", rd);
        append(tail, sizeof tail, surf);
    }
    int hs = pos[path[a].s], he = head_end_byte;
    int hr = find_reading(d, k + hs, (size_t)(he - hs));
    if (hr >= 0) {
        char rd[CAND_LEN]; int rl = he - hs; if (rl > CAND_LEN - 1) rl = CAND_LEN - 1; memcpy(rd, k + hs, (size_t)rl); rd[rl] = 0;
        for (uint32_t e = d->ent_start[hr]; e < d->ent_start[hr + 1] && sg->ncand < MAX_CANDS - 2; e++) {
            char c[CAND_LEN]; surface_of(d, (int)e, rd, c, sizeof c);
            append(c, sizeof c, tail);
            cand_add(sg, c);
        }
    }
    char rd[MAX_KANA]; int rl = pos[path[b - 1].e] - hs; if (rl > MAX_KANA - 1) rl = MAX_KANA - 1; memcpy(rd, k + hs, (size_t)rl); rd[rl] = 0;
    cand_add(sg, rd);                                           /* hiragana */
    char kt[MAX_KANA * 2]; to_katakana(rd, kt, sizeof kt); cand_add(sg, kt);
    snprintf(sg->reading, sizeof sg->reading, "%s", rd);
}

/* Converts kana (hiragana) into bunsetsu; returns the number of segments. */
static int convert_all(cosime *m, const char *kana_hira) {
    node_t path[MAX_KANA + 1];
    int np = m->loaded ? viterbi(&m->d, kana_hira, path, MAX_KANA) : -1;
    int pos[MAX_KANA + 1], nc = 0;
    for (int i = 0; kana_hira[i]; ) { int n; u8get(kana_hira + i, &n); pos[nc++] = i; i += n; }
    pos[nc] = (int)strlen(kana_hira);
    m->nseg = 0;
    if (np <= 0) {                                              /* no dictionary or no path: a single kana segment */
        seg_t *sg = &m->seg[m->nseg++]; memset(sg, 0, sizeof *sg);
        snprintf(sg->reading, sizeof sg->reading, "%s", kana_hira);
        cand_add(sg, kana_hira); char kt[MAX_KANA * 2]; to_katakana(kana_hira, kt, sizeof kt); cand_add(sg, kt);
        return m->nseg;
    }
    int a = 0;
    while (a < np && m->nseg < MAX_SEGS) {
        int b = a + 1;
        while (b < np && m->d.attached[path[b].lid] && !(path[b].ent < 0)) b++;
        if (m->nseg == MAX_SEGS - 1) b = np;
        seg_t *sg = &m->seg[m->nseg++]; memset(sg, 0, sizeof *sg);
        make_candidates(m, sg, kana_hira, pos, path, a, b);
        a = b;
    }
    return m->nseg;
}

/* Re-converts one bunsetsu reading on its own (after the user resized it). */
static void reconvert_seg(cosime *m, seg_t *sg, const char *reading) {
    node_t path[MAX_KANA + 1];
    int pos[MAX_KANA + 1], nc = 0;
    for (int i = 0; reading[i]; ) { int n; u8get(reading + i, &n); pos[nc++] = i; i += n; }
    pos[nc] = (int)strlen(reading);
    memset(sg, 0, sizeof *sg);
    int np = m->loaded ? viterbi(&m->d, reading, path, MAX_KANA) : -1;
    if (np <= 0) { snprintf(sg->reading, sizeof sg->reading, "%s", reading); cand_add(sg, reading); return; }
    make_candidates(m, sg, reading, pos, path, 0, np);
}

/* ---------------------------------------------------------------- romaji */
static const struct { const char *r, *k; } ROMAJI[] = {
#include "cosime_romaji.inc"
};
#define NROMAJI ((int)(sizeof ROMAJI / sizeof ROMAJI[0]))

static const char *romaji_exact(const char *s) { for (int i = 0; i < NROMAJI; i++) if (strcmp(ROMAJI[i].r, s) == 0) return ROMAJI[i].k; return NULL; }
static bool romaji_prefix(const char *s) {
    size_t n = strlen(s);
    for (int i = 0; i < NROMAJI; i++) if (strlen(ROMAJI[i].r) > n && strncmp(ROMAJI[i].r, s, n) == 0) return true;
    return false;
}
static bool is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

static void kana_put(cosime *m, const char *s) { append(m->kana, sizeof m->kana, s); }

static void romaji_push(cosime *m, char ch) {
    size_t pl = strlen(m->pend);
    if (pl + 1 >= sizeof m->pend) { m->pend[0] = 0; pl = 0; }
    m->pend[pl] = ch; m->pend[pl + 1] = 0;
    if (strcmp(m->pend, "nn") == 0) return;               /* "nn" waits: "nna" is ん+な, "nn" followed by anything else is just ん */
    if (m->pend[0] == 'n' && m->pend[1] == 'n') {
        kana_put(m, "ん");
        if (is_vowel(ch) || ch == 'y') { m->pend[0] = 'n'; m->pend[1] = ch; m->pend[2] = 0; }
        else { m->pend[0] = ch; m->pend[1] = 0; }
    }
    for (;;) {
        const char *ex = romaji_exact(m->pend); bool pre = romaji_prefix(m->pend);
        if (ex && !pre) { kana_put(m, ex); m->pend[0] = 0; return; }
        if (pre) return;
        size_t n = strlen(m->pend);
        if (n >= 2 && m->pend[0] == m->pend[1] && !is_vowel(m->pend[0]) && m->pend[0] != 'n') { kana_put(m, "っ"); memmove(m->pend, m->pend + 1, n); continue; }
        if (n >= 2 && m->pend[0] == 'n' && !is_vowel(m->pend[1]) && m->pend[1] != 'y') { kana_put(m, "ん"); memmove(m->pend, m->pend + 1, n); continue; }
        char one[2] = { m->pend[0], 0 };                        /* not romaji: keep the letter as it is */
        kana_put(m, one); memmove(m->pend, m->pend + 1, n);
        if (!m->pend[0]) return;
    }
}
static void romaji_flush(cosime *m) {
    if ((m->pend[0] == 'n' && !m->pend[1]) || strcmp(m->pend, "nn") == 0) kana_put(m, "ん");
    else if (m->pend[0]) { const char *ex = romaji_exact(m->pend); if (ex) kana_put(m, ex); else kana_put(m, m->pend); }
    m->pend[0] = 0;
}

/* ---------------------------------------------------------------- state machine */
cosime *cosime_new(void) { return (cosime *)calloc(1, sizeof(cosime)); }
bool cosime_loaded(const cosime *m) { return m->loaded; }
bool cosime_enabled(const cosime *m) { return m->enabled; }
int cosime_state(const cosime *m) { return m->state; }
static void reset(cosime *m) { m->state = COSIME_IDLE; m->kana[0] = 0; m->pend[0] = 0; m->nseg = 0; m->focus = 0; }
void cosime_set_enabled(cosime *m, bool on) { if (!on) reset(m); m->enabled = on; }

static void join_chosen(const cosime *m, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < m->nseg; i++) append(out, cap, m->seg[i].cand[m->seg[i].sel]);
}

const char *cosime_text(cosime *m) {
    if (m->state == COSIME_CONVERTING) { join_chosen(m, m->text, sizeof m->text); return m->text; }
    snprintf(m->text, sizeof m->text, "%s%s", m->kana, m->pend);
    return m->text;
}
int cosime_nseg(const cosime *m) { return m->state == COSIME_CONVERTING ? m->nseg : 0; }
int cosime_focus(const cosime *m) { return m->focus; }
const char *cosime_seg_text(const cosime *m, int i) { return m->seg[i].cand[m->seg[i].sel]; }
int cosime_ncand(const cosime *m) { return m->state == COSIME_CONVERTING ? m->seg[m->focus].ncand : 0; }
const char *cosime_cand(const cosime *m, int i) { return m->seg[m->focus].cand[i]; }
int cosime_cand_sel(const cosime *m) { return m->seg[m->focus].sel; }

static int commit_text(cosime *m, const char *s, char *commit, size_t cap) {
    snprintf(commit, cap, "%s", s);
    reset(m);
    return COSIME_CONSUMED | COSIME_COMMIT;
}

static const char *punct(char c) {
    switch (c) {
    case ',': return "、"; case '.': return "。"; case '-': return "ー"; case '[': return "「"; case ']': return "」";
    case '/': return "・"; case '?': return "？"; case '!': return "！"; case '~': return "〜"; case '(': return "（"; case ')': return "）";
    default: return NULL;
    }
}

static void start_convert(cosime *m) {
    romaji_flush(m);
    char hira[MAX_KANA];
    to_hiragana(m->kana, hira, sizeof hira);
    convert_all(m, hira);
    m->focus = 0; m->state = COSIME_CONVERTING;
}

/* Moves the boundary between the focused bunsetsu and its right neighbour by one kana (delta +1 grows the focused one). */
static void resize_focus(cosime *m, int delta) {
    int f = m->focus;
    if (f + 1 >= m->nseg && delta > 0) return;
    char left[MAX_KANA], right[MAX_KANA];
    snprintf(left, sizeof left, "%s", m->seg[f].reading);
    if (f + 1 < m->nseg) snprintf(right, sizeof right, "%s", m->seg[f + 1].reading); else right[0] = 0;
    if (delta > 0) {
        int n; u8get(right, &n);
        if (!right[0]) return;
        char mv[8]; memcpy(mv, right, (size_t)n); mv[n] = 0;
        append(left, sizeof left, mv); memmove(right, right + n, strlen(right + n) + 1);
    } else {
        size_t l = strlen(left); if (l == 0) return;
        size_t i = l; do i--; while (i > 0 && ((unsigned char)left[i] & 0xC0) == 0x80);
        if (i == 0) return;                                    /* keep at least one kana */
        char mv[8]; snprintf(mv, sizeof mv, "%s", left + i); left[i] = 0;
        char nr[MAX_KANA]; snprintf(nr, sizeof nr, "%s%s", mv, right); snprintf(right, sizeof right, "%s", nr);
        if (f + 1 >= m->nseg) {                                /* shrinking the last segment creates a new one */
            if (m->nseg >= MAX_SEGS) return;
            m->nseg++;
        }
    }
    reconvert_seg(m, &m->seg[f], left);
    if (right[0]) reconvert_seg(m, &m->seg[f + 1], right);
    else if (f + 1 < m->nseg) { memmove(&m->seg[f + 1], &m->seg[f + 2], sizeof(seg_t) * (size_t)(m->nseg - f - 2)); m->nseg--; }
}

int cosime_key(cosime *m, const cos_win_event_t *ev, char *commit, size_t cap) {
    commit[0] = 0;
    if (!m->enabled) return COSIME_PASS;
    uint8_t sp = ev->special; char ch = ev->ascii;
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0, shift = (ev->mods & COS_MOD_SHIFT) != 0;
    if (ctrl) {                                                 /* shortcuts: commit what is being typed, then let the caller handle the key */
        if (m->state == COSIME_IDLE) return COSIME_PASS;
        if (m->state == COSIME_COMPOSING) romaji_flush(m);
        char t[MAX_SEGS * CAND_LEN]; snprintf(t, sizeof t, "%s", cosime_text(m)); reset(m); snprintf(commit, cap, "%s", t);
        return COSIME_COMMIT;
    }

    if (m->state == COSIME_IDLE) {
        if (ch >= 'a' && ch <= 'z') { m->state = COSIME_COMPOSING; romaji_push(m, ch); return COSIME_CONSUMED; }
        const char *p = (unsigned char)ch >= 32 ? punct(ch) : NULL;
        if (p) return commit_text(m, p, commit, cap);
        return COSIME_PASS;
    }

    if (m->state == COSIME_COMPOSING) {
        if (sp == COS_KEY_ENTER) { romaji_flush(m); return commit_text(m, m->kana, commit, cap); }
        if (sp == COS_KEY_ESC) { reset(m); return COSIME_CONSUMED; }
        if (sp == COS_KEY_BACKSPACE) {
            if (m->pend[0]) m->pend[strlen(m->pend) - 1] = 0;
            else { size_t l = strlen(m->kana); if (l) { size_t i = l; do i--; while (i > 0 && ((unsigned char)m->kana[i] & 0xC0) == 0x80); m->kana[i] = 0; } }
            if (!m->kana[0] && !m->pend[0]) reset(m);
            return COSIME_CONSUMED;
        }
        if (ch == ' ' || sp == COS_KEY_DOWN) { start_convert(m); return COSIME_CONSUMED; }
        if (sp == COS_KEY_F1 + 5) { romaji_flush(m); char h[MAX_KANA]; to_hiragana(m->kana, h, sizeof h); snprintf(m->kana, sizeof m->kana, "%s", h); return COSIME_CONSUMED; }
        if (sp == COS_KEY_F1 + 6) { romaji_flush(m); char k[MAX_KANA * 2]; to_katakana(m->kana, k, sizeof k); snprintf(m->kana, sizeof m->kana, "%.*s", MAX_KANA - 1, k); return COSIME_CONSUMED; }
        if (sp == COS_KEY_LEFT || sp == COS_KEY_RIGHT || sp == COS_KEY_UP || sp == COS_KEY_HOME || sp == COS_KEY_END || sp == COS_KEY_TAB) return COSIME_CONSUMED;
        if (ch >= 'a' && ch <= 'z') { romaji_push(m, ch); return COSIME_CONSUMED; }
        if ((unsigned char)ch >= 32 && ch != 127) {
            const char *p = punct(ch);
            romaji_flush(m);
            if (p) kana_put(m, p); else { char one[2] = { ch, 0 }; kana_put(m, one); }
            return COSIME_CONSUMED;
        }
        return COSIME_PASS;
    }

    /* converting */
    seg_t *sg = &m->seg[m->focus];
    if (sp == COS_KEY_ENTER) { char t[MAX_SEGS * CAND_LEN]; join_chosen(m, t, sizeof t); return commit_text(m, t, commit, cap); }
    if (sp == COS_KEY_ESC || sp == COS_KEY_BACKSPACE) { m->state = COSIME_COMPOSING; m->nseg = 0; return COSIME_CONSUMED; }
    if (ch == ' ' || sp == COS_KEY_DOWN) { sg->sel = (sg->sel + 1) % sg->ncand; return COSIME_CONSUMED; }
    if (sp == COS_KEY_UP) { sg->sel = (sg->sel + sg->ncand - 1) % sg->ncand; return COSIME_CONSUMED; }
    if (sp == COS_KEY_LEFT) { if (shift) resize_focus(m, -1); else if (m->focus > 0) m->focus--; return COSIME_CONSUMED; }
    if (sp == COS_KEY_RIGHT) { if (shift) resize_focus(m, +1); else if (m->focus + 1 < m->nseg) m->focus++; return COSIME_CONSUMED; }
    if (sp == COS_KEY_F1 + 5 || sp == COS_KEY_F1 + 6) {
        const char *want_kind = sp == COS_KEY_F1 + 5 ? "h" : "k"; (void)want_kind;
        char t[MAX_KANA * 2]; if (sp == COS_KEY_F1 + 5) to_hiragana(sg->reading, t, sizeof t); else to_katakana(sg->reading, t, sizeof t);
        for (int i = 0; i < sg->ncand; i++) if (strcmp(sg->cand[i], t) == 0) { sg->sel = i; break; }
        return COSIME_CONSUMED;
    }
    if (ch >= '1' && ch <= '9') { int i = ch - '1'; if (i < sg->ncand) { sg->sel = i; if (m->focus + 1 < m->nseg) m->focus++; } return COSIME_CONSUMED; }
    /* any other character: commit what is chosen and let the key start (or be) the next input */
    {
        char t[MAX_SEGS * CAND_LEN]; join_chosen(m, t, sizeof t);
        reset(m);
        snprintf(commit, cap, "%s", t);
        if (ch >= 'a' && ch <= 'z') { m->state = COSIME_COMPOSING; romaji_push(m, ch); }
        else if (ch && (unsigned char)ch >= 32) { const char *p = punct(ch); if (p) append(commit, cap, p); else { char one[2] = { ch, 0 }; append(commit, cap, one); } }
        return COSIME_CONSUMED | COSIME_COMMIT;
    }
}
