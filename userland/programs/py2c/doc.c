/* doc.c - text documents: a flat buffer with undo, line index, UTF-8 helpers, file load/save. */
#include "p2.h"

#define DOC_MAX (8u * 1024u * 1024u)
#define UNDO_BYTES_MAX (6u * 1024u * 1024u)

size_t u8_prev(const char *s, size_t i) { if (i == 0) return 0; --i; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i; return i; }
size_t u8_next(const char *s, size_t n, size_t i) { if (i >= n) return n; ++i; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) ++i; return i; }

int lang_of_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return L_TEXT;
    if (!strcasecmp(dot, ".py") || !strcasecmp(dot, ".pyw")) return L_PY;
    if (!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h")) return L_C;
    return L_TEXT;
}
void doc_name_from_path(Doc *d) {
    const char *b = strrchr(d->path, '/');
    snprintf(d->name, sizeof d->name, "%s", b ? b + 1 : d->path);
}

Doc *doc_new(const char *path, int lang) {
    Doc *d = (Doc *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cap = 4096; d->t = (char *)malloc(d->cap);
    if (!d->t) { free(d); return NULL; }
    d->t[0] = 0; d->ver = 1; d->lang = lang; d->next_group = 1;
    if (path && path[0]) { snprintf(d->path, sizeof d->path, "%s", path); doc_name_from_path(d); }
    else { snprintf(d->name, sizeof d->name, "untitled.py"); d->untitled = true; }
    return d;
}
static void undo_clear(Doc *d) {
    for (int i = 0; i < d->nops; ++i) { free(d->ops[i].del); free(d->ops[i].ins); }
    d->nops = 0; d->head = 0; d->undo_bytes = 0;
}
void doc_free(Doc *d) {
    if (!d) return;
    undo_clear(d);
    free(d->ops); free(d->t); free(d->ls); free(d->lst); free(d);
}
static bool reserve(Doc *d, size_t need) {
    if (need + 1 <= d->cap) return true;
    size_t nc = d->cap;
    while (nc < need + 1) nc = nc * 2;
    char *nb = (char *)realloc(d->t, nc);
    if (!nb) return false;
    d->t = nb; d->cap = nc; return true;
}
void doc_set_text(Doc *d, const char *s, size_t n) {
    if (n > DOC_MAX) n = DOC_MAX;
    if (!reserve(d, n)) return;
    memcpy(d->t, s, n); d->t[n] = 0; d->len = n;
    d->caret = d->anchor = 0; d->want_col = 0; d->top = d->left = 0;
    d->ver++; d->dirty = false; undo_clear(d);
}

bool doc_load(Doc *d, const char *path) {
    cos_stat_ex_t st;
    if (cos_stat_ex(path, &st) != 0 || st.is_dir) return false;
    size_t sz = (size_t)st.size;
    if (sz > DOC_MAX) return false;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return false;
    ssize_t n = sz ? cos_read_file(path, buf, sz) : 0;
    if (n < 0) { free(buf); return false; }
    size_t len = (size_t)n;
    /* a UTF-8 byte order mark is dropped; CRLF becomes LF in memory and is restored on save */
    size_t o = 0, i = 0; int crlf = 0, lf = 0;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) i = 3;
    for (; i < len; ++i) {
        if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n') { crlf++; continue; }
        if (buf[i] == '\n') lf++;
        buf[o++] = buf[i];
    }
    d->crlf = crlf > 0 && crlf >= lf;
    snprintf(d->path, sizeof d->path, "%s", path); doc_name_from_path(d);
    d->lang = lang_of_name(d->name); d->untitled = false;
    doc_set_text(d, buf, o); free(buf);
    d->mtime = st.mtime;
    return true;
}
bool doc_save(Doc *d, const char *path) {
    const char *p = path ? path : d->path;
    if (!p[0]) return false;
    size_t extra = 0;
    if (d->crlf) for (size_t i = 0; i < d->len; ++i) if (d->t[i] == '\n') extra++;
    char *buf = (char *)malloc(d->len + extra + 1);
    if (!buf) return false;
    size_t o = 0;
    for (size_t i = 0; i < d->len; ++i) { if (d->crlf && d->t[i] == '\n') buf[o++] = '\r'; buf[o++] = d->t[i]; }
    ssize_t w = p2_write_file(p, buf, o);
    free(buf);
    if (w != (ssize_t)o) return false;
    if (path) { snprintf(d->path, sizeof d->path, "%s", path); doc_name_from_path(d); d->untitled = false; d->lang = lang_of_name(d->name); }
    d->dirty = false;
    cos_stat_ex_t st; if (cos_stat_ex(d->path, &st) == 0) d->mtime = st.mtime;
    return true;
}

/* ---- line index ---- */
static void lines_rebuild(Doc *d) {
    if (d->ls_ver == d->ver && d->ls) return;
    size_t n = 1;
    for (size_t i = 0; i < d->len; ++i) if (d->t[i] == '\n') n++;
    if (n + 1 > d->ls_cap) { size_t nc = n + 256; size_t *nl = (size_t *)realloc(d->ls, nc * sizeof *nl); if (!nl) return; d->ls = nl; d->ls_cap = nc; }
    size_t k = 0; d->ls[k++] = 0;
    for (size_t i = 0; i < d->len; ++i) if (d->t[i] == '\n') d->ls[k++] = i + 1;
    d->nl = k; d->ls_ver = d->ver;
}
size_t doc_lines(Doc *d) { lines_rebuild(d); return d->nl; }
size_t doc_line_start(Doc *d, size_t l) { lines_rebuild(d); if (l >= d->nl) l = d->nl - 1; return d->ls[l]; }
size_t doc_line_end(Doc *d, size_t l) {
    lines_rebuild(d); if (l >= d->nl) l = d->nl - 1;
    size_t e = (l + 1 < d->nl) ? d->ls[l + 1] - 1 : d->len;
    return e;
}
size_t doc_line_of(Doc *d, size_t pos) {
    lines_rebuild(d);
    size_t lo = 0, hi = d->nl;
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (d->ls[mid] <= pos) lo = mid; else hi = mid; }
    return lo;
}
int doc_col_of(Doc *d, size_t pos) {
    size_t l = doc_line_of(d, pos), s = doc_line_start(d, l); int col = 0;
    for (size_t i = s; i < pos && i < d->len; i = u8_next(d->t, d->len, i)) col += d->t[i] == '\t' ? 4 - col % 4 : 1;
    return col;
}
size_t doc_pos_at_col(Doc *d, size_t line, int col) {
    size_t s = doc_line_start(d, line), e = doc_line_end(d, line), i = s; int c = 0;
    while (i < e) {
        int w = d->t[i] == '\t' ? 4 - c % 4 : 1;
        if (c + w > col) { if (col - c > w / 2 && w > 1) i = u8_next(d->t, d->len, i); break; }
        c += w; i = u8_next(d->t, d->len, i);
    }
    return i;
}
bool doc_has_sel(const Doc *d) { return d->caret != d->anchor; }
void doc_sel(const Doc *d, size_t *a, size_t *b) { if (d->caret < d->anchor) { *a = d->caret; *b = d->anchor; } else { *a = d->anchor; *b = d->caret; } }
char *doc_sel_text(Doc *d, size_t *len) {
    if (!doc_has_sel(d)) return NULL;
    size_t a, b; doc_sel(d, &a, &b);
    char *s = (char *)malloc(b - a + 1);
    if (!s) return NULL;
    memcpy(s, d->t + a, b - a); s[b - a] = 0; if (len) *len = b - a; return s;
}

/* ---- editing with undo ---- */
void doc_group_begin(Doc *d) { d->group = d->next_group++; }
void doc_group_end(Doc *d) { d->group = 0; }

static void undo_trim(Doc *d) {
    while (d->undo_bytes > UNDO_BYTES_MAX && d->nops > 1 && d->head > 0) {          /* drop the oldest */
        UndoOp *o = &d->ops[0];
        d->undo_bytes -= o->dlen + o->ilen; free(o->del); free(o->ins);
        memmove(&d->ops[0], &d->ops[1], sizeof d->ops[0] * (size_t)(d->nops - 1));
        d->nops--; d->head--;
    }
}
static void raw_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t ilen) {
    if (pos > d->len) pos = d->len;
    if (del > d->len - pos) del = d->len - pos;
    if (d->len - del + ilen > DOC_MAX) return;
    if (!reserve(d, d->len - del + ilen)) return;
    memmove(d->t + pos + ilen, d->t + pos + del, d->len - pos - del);
    if (ilen) memcpy(d->t + pos, ins, ilen);
    d->len = d->len - del + ilen; d->t[d->len] = 0;
    d->ver++; d->dirty = true;
}
void doc_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t ilen, bool merge) {
    if (d->readonly) return;
    if (pos > d->len) pos = d->len;
    if (del > d->len - pos) del = d->len - pos;
    if (!del && !ilen) return;
    /* the redo tail is gone */
    for (int i = d->head; i < d->nops; ++i) { d->undo_bytes -= d->ops[i].dlen + d->ops[i].ilen; free(d->ops[i].del); free(d->ops[i].ins); }
    d->nops = d->head;
    uint64_t now = cos_time_ms();
    if (merge && d->head > 0 && d->group == 0 && del == 0) {
        UndoOp *o = &d->ops[d->head - 1];
        if (o->dlen == 0 && o->group == 0 && o->pos + o->ilen == pos && now - o->t < 1000 && o->ilen + ilen < 256) {      /* typing continues the last insertion */
            char *ni = (char *)realloc(o->ins, o->ilen + ilen);
            if (ni) {
                memcpy(ni + o->ilen, ins, ilen); o->ins = ni; o->ilen += ilen; o->t = now; d->undo_bytes += ilen;
                size_t cb = d->caret; raw_replace(d, pos, 0, ins, ilen); d->caret = d->anchor = pos + ilen; o->ca = d->caret; (void)cb;
                return;
            }
        }
    }
    if (d->nops == d->ops_cap) { int nc = d->ops_cap ? d->ops_cap * 2 : 64; UndoOp *no = (UndoOp *)realloc(d->ops, (size_t)nc * sizeof *no); if (!no) return; d->ops = no; d->ops_cap = nc; }
    UndoOp *o = &d->ops[d->nops];
    memset(o, 0, sizeof *o);
    o->pos = pos; o->dlen = del; o->ilen = ilen; o->group = d->group; o->cb = d->caret; o->t = now;
    if (del) { o->del = (char *)malloc(del); if (o->del) memcpy(o->del, d->t + pos, del); else return; }
    if (ilen) { o->ins = (char *)malloc(ilen); if (o->ins) memcpy(o->ins, ins, ilen); else { free(o->del); return; } }
    d->nops++; d->head = d->nops; d->undo_bytes += del + ilen;
    raw_replace(d, pos, del, ins, ilen);
    d->caret = d->anchor = pos + ilen; o->ca = d->caret;
    undo_trim(d);
}
static void apply_inverse(Doc *d, UndoOp *o) { raw_replace(d, o->pos, o->ilen, o->del, o->dlen); }
static void apply_forward(Doc *d, UndoOp *o) { raw_replace(d, o->pos, o->dlen, o->ins, o->ilen); }
void doc_undo(Doc *d) {
    if (d->head == 0) return;
    uint32_t g = d->ops[d->head - 1].group;
    do {
        UndoOp *o = &d->ops[--d->head];
        apply_inverse(d, o); d->caret = d->anchor = o->cb <= d->len ? o->cb : d->len;
        if (!g) break;
    } while (d->head > 0 && d->ops[d->head - 1].group == g);
}
void doc_redo(Doc *d) {
    if (d->head >= d->nops) return;
    uint32_t g = d->ops[d->head].group;
    do {
        UndoOp *o = &d->ops[d->head++];
        apply_forward(d, o); d->caret = d->anchor = o->ca <= d->len ? o->ca : d->len;
        if (!g) break;
    } while (d->head < d->nops && d->ops[d->head].group == g);
}

/* ---- search ---- */
static inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
long find_next(Doc *d, const char *what, size_t wl, size_t from, bool icase, bool backwards) {
    if (!wl || wl > d->len) return -1;
    size_t n = d->len - wl + 1;
    if (!backwards) {
        for (size_t k = 0; k < n; ++k) {
            size_t i = (from + k) % n;
            size_t j = 0;
            while (j < wl && (icase ? lc(d->t[i + j]) == lc(what[j]) : d->t[i + j] == what[j])) j++;
            if (j == wl) return (long)i;
        }
    } else {
        for (size_t k = 0; k < n; ++k) {
            size_t i = (from + n - (k % n)) % n;
            size_t j = 0;
            while (j < wl && (icase ? lc(d->t[i + j]) == lc(what[j]) : d->t[i + j] == what[j])) j++;
            if (j == wl) return (long)i;
        }
    }
    return -1;
}
