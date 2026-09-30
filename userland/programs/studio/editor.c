/* editor.c - the document model: editing, undo/redo, caret movement.
 *
 * Every mutation of the text goes through doc_replace(), which records an
 * UndoOp holding both the text it removed and the text it inserted, so undo
 * and redo are exact inverses and never need to re-derive anything.
 *
 * Caret positions are byte offsets that always sit on UTF-8 code point
 * boundaries (doc_prev_cp / doc_next_cp skip continuation bytes).
 */
#include "studio.h"

/* ---------------------------------------------------------------- */
/* small helpers                                                     */
/* ---------------------------------------------------------------- */
static inline bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
static inline bool is_word(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c & 0x80); }
static inline bool is_space(int c) { return c == ' ' || c == '\t'; }
#define MERGE_MS 800

size_t doc_prev_cp(Doc *d, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && is_cont((unsigned char)buf_at(&d->b, pos))) --pos;
    return pos;
}
size_t doc_next_cp(Doc *d, size_t pos) {
    size_t n = buf_len(&d->b);
    if (pos >= n) return n;
    ++pos;
    while (pos < n && is_cont((unsigned char)buf_at(&d->b, pos))) ++pos;
    return pos;
}

/* ---------------------------------------------------------------- */
/* lifecycle                                                         */
/* ---------------------------------------------------------------- */
Doc *doc_new(const char *path) {
    Doc *d = (Doc *)calloc(1, sizeof *d);
    if (!d) return NULL;
    buf_init(&d->b);
    if (path && path[0]) {
        snprintf(d->path, sizeof d->path, "%s", path);
        snprintf(d->name, sizeof d->name, "%s", path_base(path));
    } else {
        snprintf(d->name, sizeof d->name, "untitled.c");
        d->untitled = true;
    }
    return d;
}

static void undo_free_op(UndoOp *o) { free(o->del); free(o->ins); o->del = o->ins = NULL; }

void doc_free(Doc *d) {
    if (!d) return;
    for (size_t i = 0; i < d->u.n; ++i) undo_free_op(&d->u.ops[i]);
    free(d->u.ops);
    free(d->hs);
    buf_free(&d->b);
    free(d);
}

bool doc_load(Doc *d, const char *path) {
    size_t n = 0;
    char *data = fs_read_all(path, &n);
    if (!data) return false;
    /* normalise CRLF -> LF; remember it so save() can restore the original */
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < n; ++i) if (data[i] == '\n') { if (i > 0 && data[i - 1] == '\r') ++crlf; else ++lf; }
    d->crlf = crlf > lf;
    if (crlf) {
        size_t o = 0;
        for (size_t i = 0; i < n; ++i) { if (data[i] == '\r' && i + 1 < n && data[i + 1] == '\n') continue; data[o++] = data[i]; }
        n = o;
    }
    if (n >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) { memmove(data, data + 3, n - 3); n -= 3; }
    buf_free(&d->b);
    buf_init(&d->b);
    buf_insert(&d->b, 0, data, n);
    free(data);
    d->caret = d->anchor = 0;
    d->dirty = false;
    d->hs_valid = 0;
    snprintf(d->path, sizeof d->path, "%s", path);
    snprintf(d->name, sizeof d->name, "%s", path_base(path));
    d->untitled = false;
    return true;
}

bool doc_save(Doc *d, const char *path) {
    if (!path || !path[0]) path = d->path;
    if (!path[0]) return false;
    size_t n = buf_len(&d->b);
    const char *t = buf_text(&d->b);
    bool ok;
    if (d->crlf) {
        size_t extra = 0;
        for (size_t i = 0; i < n; ++i) if (t[i] == '\n') ++extra;
        char *o = (char *)malloc(n + extra + 1);
        if (!o) return false;
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) { if (t[i] == '\n') o[k++] = '\r'; o[k++] = t[i]; }
        ok = fs_write_all(path, o, k);
        free(o);
    } else {
        ok = fs_write_all(path, t, n);
    }
    if (ok) {
        if (path != d->path) snprintf(d->path, sizeof d->path, "%s", path);
        snprintf(d->name, sizeof d->name, "%s", path_base(d->path));
        d->dirty = false;
        d->untitled = false;
    }
    return ok;
}

/* ---------------------------------------------------------------- */
/* undo stack                                                        */
/* ---------------------------------------------------------------- */
static UndoOp *undo_push(Doc *d) {
    UndoStack *u = &d->u;
    for (size_t i = u->head; i < u->n; ++i) undo_free_op(&u->ops[i]);   /* a new edit kills redo */
    u->n = u->head;
    if (u->n == u->cap) {
        size_t nc = u->cap ? u->cap * 2 : 64;
        UndoOp *p = (UndoOp *)realloc(u->ops, nc * sizeof(UndoOp));
        if (!p) return NULL;
        u->ops = p; u->cap = nc;
    }
    UndoOp *o = &u->ops[u->n++];
    memset(o, 0, sizeof *o);
    u->head = u->n;
    return o;
}

static void doc_touch(Doc *d) {
    d->dirty = true;
    d->hs_valid = 0;               /* block-comment state is recomputed lazily */
    d->hs_ver = d->b.version;
    G.caret_t = cos_time_ms();
    G.dirty_frame = true;
}

/* The one place text changes. `merge` lets adjacent typing/deleting coalesce
 * into the previous UndoOp when it happened within MERGE_MS. */
void doc_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t ilen, bool merge) {
    size_t len = buf_len(&d->b);
    if (pos > len) pos = len;
    if (pos + del > len) del = len - pos;
    if (!del && !ilen) return;
    uint64_t now = cos_time_ms();

    char *dtext = NULL;
    if (del) { dtext = (char *)malloc(del); if (!dtext) return; buf_copy(&d->b, pos, del, dtext); }

    UndoStack *u = &d->u;
    UndoOp *prev = (u->head > 0 && u->head == u->n) ? &u->ops[u->head - 1] : NULL;
    bool merged = false;
    if (merge && u->cur_group == 0 && prev && prev->solo && now - prev->t <= MERGE_MS) {
        if (!del && ilen && prev->dlen == 0 && pos == prev->pos + prev->ilen && !(ilen == 1 && ins[0] == '\n')) {
            char *ni = (char *)realloc(prev->ins, prev->ilen + ilen);
            if (ni) { memcpy(ni + prev->ilen, ins, ilen); prev->ins = ni; prev->ilen += ilen; prev->t = now; prev->caret_after = pos + ilen; merged = true; }
        } else if (del && !ilen && prev->ilen == 0 && pos + del == prev->pos) {          /* backspace run */
            char *nd = (char *)malloc(prev->dlen + del);
            if (nd) { memcpy(nd, dtext, del); memcpy(nd + del, prev->del, prev->dlen); free(prev->del); prev->del = nd; prev->dlen += del; prev->pos = pos; prev->t = now; prev->caret_after = pos; merged = true; }
        } else if (del && !ilen && prev->ilen == 0 && pos == prev->pos) {                 /* forward-delete run */
            char *nd = (char *)malloc(prev->dlen + del);
            if (nd) { memcpy(nd, prev->del, prev->dlen); memcpy(nd + prev->dlen, dtext, del); free(prev->del); prev->del = nd; prev->dlen += del; prev->t = now; prev->caret_after = pos; merged = true; }
        }
    }
    if (merged) {
        free(dtext);
    } else {
        UndoOp *o = undo_push(d);
        if (!o) { free(dtext); return; }
        o->pos = pos; o->del = dtext; o->dlen = del;
        if (ilen) { o->ins = (char *)malloc(ilen); if (o->ins) memcpy(o->ins, ins, ilen); else ilen = 0; }
        o->ilen = ilen; o->t = now;
        o->group = u->cur_group ? u->cur_group : ++u->next_group;
        o->solo = (u->cur_group == 0);
        o->caret_before = d->caret; o->caret_after = pos + ilen;
    }
    if (del) buf_delete(&d->b, pos, del);
    if (ilen) buf_insert(&d->b, pos, ins, ilen);
    doc_touch(d);
}

/* A command that makes several doc_replace() calls wraps them in
 * group_begin/group_end so they undo as one step. Outside a group every op
 * gets its own id ("solo"), and only solo ops may merge with each other. */
void doc_group_begin(Doc *d) { d->u.cur_group = ++d->u.next_group; }
void doc_group_end(Doc *d) { d->u.cur_group = 0; }

bool doc_undo(Doc *d) {
    UndoStack *u = &d->u;
    if (u->head == 0) return false;
    uint32_t g = u->ops[u->head - 1].group;
    for (;;) {
        UndoOp *o = &u->ops[--u->head];
        if (o->ilen) buf_delete(&d->b, o->pos, o->ilen);
        if (o->dlen) buf_insert(&d->b, o->pos, o->del, o->dlen);
        d->caret = d->anchor = o->caret_before;
        if (u->head == 0 || u->ops[u->head - 1].group != g) break;
    }
    doc_touch(d);
    return true;
}

bool doc_redo(Doc *d) {
    UndoStack *u = &d->u;
    if (u->head >= u->n) return false;
    uint32_t g = u->ops[u->head].group;
    for (;;) {
        UndoOp *o = &u->ops[u->head++];
        if (o->dlen) buf_delete(&d->b, o->pos, o->dlen);
        if (o->ilen) buf_insert(&d->b, o->pos, o->ins, o->ilen);
        d->caret = d->anchor = o->caret_after;
        if (u->head >= u->n || u->ops[u->head].group != g) break;
    }
    doc_touch(d);
    return true;
}

/* ---------------------------------------------------------------- */
/* selection                                                         */
/* ---------------------------------------------------------------- */
bool doc_has_sel(const Doc *d) { return d->caret != d->anchor; }
void doc_sel_range(const Doc *d, size_t *a, size_t *b) {
    if (d->caret < d->anchor) { *a = d->caret; *b = d->anchor; } else { *a = d->anchor; *b = d->caret; }
}
char *doc_sel_text(Doc *d, size_t *len) {
    size_t a, b; doc_sel_range(d, &a, &b);
    char *s = (char *)malloc(b - a + 1);
    if (!s) return NULL;
    buf_copy(&d->b, a, b - a, s);
    s[b - a] = 0;
    if (len) *len = b - a;
    return s;
}
static bool delete_selection(Doc *d) {
    if (!doc_has_sel(d)) return false;
    size_t a, b; doc_sel_range(d, &a, &b);
    d->caret = d->anchor = a;
    doc_replace(d, a, b - a, NULL, 0, false);
    return true;
}
void doc_select_all(Doc *d) { d->anchor = 0; d->caret = buf_len(&d->b); G.dirty_frame = true; }

void doc_select_word_at(Doc *d, size_t pos) {
    size_t n = buf_len(&d->b), a = pos, b = pos;
    if (pos < n && is_word((unsigned char)buf_at(&d->b, pos))) {
        while (a > 0 && is_word((unsigned char)buf_at(&d->b, a - 1))) --a;
        while (b < n && is_word((unsigned char)buf_at(&d->b, b))) ++b;
    } else if (pos > 0 && is_word((unsigned char)buf_at(&d->b, pos - 1))) {
        a = pos; while (a > 0 && is_word((unsigned char)buf_at(&d->b, a - 1))) --a;
    } else if (pos < n) { b = pos + 1; }
    d->anchor = a; d->caret = b;
}
void doc_select_line_at(Doc *d, size_t pos) {
    size_t l = buf_line_of(&d->b, pos);
    d->anchor = buf_line_start(&d->b, l);
    size_t e = buf_line_end(&d->b, l);
    d->caret = e < buf_len(&d->b) ? e + 1 : e;
}

/* ---------------------------------------------------------------- */
/* columns                                                           */
/* ---------------------------------------------------------------- */
int doc_visual_col(Doc *d, size_t line, size_t pos) {
    size_t a = buf_line_start(&d->b, line);
    int col = 0, tw = G.tab_width > 0 ? G.tab_width : 4;
    for (size_t i = a; i < pos;) {
        if (buf_at(&d->b, i) == '\t') col += tw - (col % tw); else ++col;
        i = doc_next_cp(d, i);
    }
    return col;
}
/* 1-based line, 1-based column (counted in code points, like the status bar shows) */
void doc_line_col(Doc *d, size_t pos, int *line, int *col) {
    size_t l = buf_line_of(&d->b, pos);
    size_t a = buf_line_start(&d->b, l);
    int c = 0;
    for (size_t i = a; i < pos; i = doc_next_cp(d, i)) ++c;
    if (line) *line = (int)l + 1;
    if (col) *col = c + 1;
}
/* 0-based line, 0-based visual column -> byte offset (clamped to the line) */
size_t doc_pos_from_line_col(Doc *d, int line, int col) {
    size_t nl = buf_lines(&d->b);
    if (line < 0) line = 0;
    if ((size_t)line >= nl) line = (int)nl - 1;
    size_t a = buf_line_start(&d->b, (size_t)line), e = buf_line_end(&d->b, (size_t)line);
    int c = 0, tw = G.tab_width > 0 ? G.tab_width : 4;
    size_t i = a;
    while (i < e) {
        int w = buf_at(&d->b, i) == '\t' ? tw - (c % tw) : 1;
        if (c + w > col) { if (col - c > w / 2 && w > 1) i = doc_next_cp(d, i); break; }
        c += w; i = doc_next_cp(d, i);
    }
    return i;
}

/* ---------------------------------------------------------------- */
/* movement                                                          */
/* ---------------------------------------------------------------- */
static void finish_move(Doc *d, bool sel, bool keep_col) {
    if (!sel) d->anchor = d->caret;
    if (!keep_col) d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
    G.caret_t = cos_time_ms();
    G.comp_open = false;
    G.dirty_frame = true;
}
void doc_move_left(Doc *d, bool sel, bool word) {
    if (!sel && doc_has_sel(d)) { size_t a, b; doc_sel_range(d, &a, &b); d->caret = a; finish_move(d, false, false); return; }
    if (word) {
        size_t p = d->caret;
        while (p > 0 && !is_word((unsigned char)buf_at(&d->b, p - 1)) && buf_at(&d->b, p - 1) != '\n') --p;
        while (p > 0 && is_word((unsigned char)buf_at(&d->b, p - 1))) --p;
        if (p == d->caret && p > 0) --p;
        d->caret = p;
    } else d->caret = doc_prev_cp(d, d->caret);
    finish_move(d, sel, false);
}
void doc_move_right(Doc *d, bool sel, bool word) {
    if (!sel && doc_has_sel(d)) { size_t a, b; doc_sel_range(d, &a, &b); d->caret = b; finish_move(d, false, false); return; }
    size_t n = buf_len(&d->b);
    if (word) {
        size_t p = d->caret;
        while (p < n && is_word((unsigned char)buf_at(&d->b, p))) ++p;
        while (p < n && !is_word((unsigned char)buf_at(&d->b, p)) && buf_at(&d->b, p) != '\n') ++p;
        if (p == d->caret && p < n) ++p;
        d->caret = p;
    } else d->caret = doc_next_cp(d, d->caret);
    finish_move(d, sel, false);
}
void doc_move_vert(Doc *d, int lines, bool sel) {
    size_t l = buf_line_of(&d->b, d->caret), nl = buf_lines(&d->b);
    long t = (long)l + lines;
    if (t < 0) { d->caret = 0; finish_move(d, sel, false); return; }
    if ((size_t)t >= nl) { d->caret = buf_len(&d->b); finish_move(d, sel, false); return; }
    d->caret = doc_pos_from_line_col(d, (int)t, d->want_col);
    finish_move(d, sel, true);
}
void doc_move_home(Doc *d, bool sel) {
    size_t l = buf_line_of(&d->b, d->caret);
    size_t a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l), f = a;
    while (f < e && is_space((unsigned char)buf_at(&d->b, f))) ++f;
    d->caret = (d->caret == f || d->caret > f) && d->caret != f ? f : (d->caret == f ? a : f);
    finish_move(d, sel, false);
}
void doc_move_end(Doc *d, bool sel) {
    d->caret = buf_line_end(&d->b, buf_line_of(&d->b, d->caret));
    finish_move(d, sel, false);
}
void doc_move_doc_edge(Doc *d, bool end, bool sel) {
    d->caret = end ? buf_len(&d->b) : 0;
    finish_move(d, sel, false);
}
void doc_goto_line(Doc *d, int line, int col) {
    if (line < 1) line = 1;
    size_t nl = buf_lines(&d->b);
    if ((size_t)line > nl) line = (int)nl;
    if (col < 1) col = 1;
    d->caret = d->anchor = doc_pos_from_line_col(d, line - 1, col - 1);
    d->center_req = true;
    finish_move(d, false, false);
}

/* ---------------------------------------------------------------- */
/* editing commands                                                  */
/* ---------------------------------------------------------------- */
void doc_insert_at_caret(Doc *d, const char *s, size_t n) {
    doc_group_begin(d);
    delete_selection(d);
    doc_replace(d, d->caret, 0, s, n, false);
    doc_group_end(d);
    d->caret = d->anchor = d->caret + n;
    d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
}

static size_t line_indent_len(Doc *d, size_t line) {
    size_t a = buf_line_start(&d->b, line), e = buf_line_end(&d->b, line), i = a;
    while (i < e && is_space((unsigned char)buf_at(&d->b, i))) ++i;
    return i - a;
}
static int prev_nonspace(Doc *d, size_t pos) {
    while (pos > 0) { char c = buf_at(&d->b, pos - 1); if (!is_space((unsigned char)c)) return (unsigned char)c; --pos; if (c == '\n') break; }
    return 0;
}

static char closer_for(char c) { return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == '"' ? '"' : c == '\'' ? '\'' : 0; }

void doc_type_char(Doc *d, char c) {
    char cl = closer_for(c);
    size_t n = buf_len(&d->b);

    if (doc_has_sel(d) && cl && c != '\'') {
        /* wrap the selection in the pair, like every editor does */
        size_t a, b, len; doc_sel_range(d, &a, &b);
        char *t = doc_sel_text(d, &len);
        char *w = t ? (char *)malloc(len + 2) : NULL;
        if (w) {
            w[0] = c; memcpy(w + 1, t, len); w[len + 1] = cl;
            doc_replace(d, a, len, w, len + 2, false);
            d->anchor = a + 1; d->caret = a + 1 + len;
            d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
            free(w); free(t);
            return;
        }
        free(t);
    }
    bool had_sel = doc_has_sel(d);
    if (had_sel) { doc_group_begin(d); delete_selection(d); n = buf_len(&d->b); }

    char next = d->caret < n ? buf_at(&d->b, d->caret) : 0;
    char prev = d->caret > 0 ? buf_at(&d->b, d->caret - 1) : 0;

    /* type over a closer that is already there */
    if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && next == c) {
        d->caret = d->anchor = d->caret + 1;
        G.dirty_frame = true;
        if (had_sel) doc_group_end(d);
        return;
    }
    /* '}' on an otherwise blank line: outdent one level first */
    if (c == '}') {
        size_t l = buf_line_of(&d->b, d->caret), a = buf_line_start(&d->b, l);
        bool blank = true;
        for (size_t i = a; i < d->caret; ++i) if (!is_space((unsigned char)buf_at(&d->b, i))) { blank = false; break; }
        int tw = G.tab_width > 0 ? G.tab_width : 4;
        if (blank && d->caret > a) {
            size_t k = 0;
            while (k < (size_t)tw && k < d->caret - a && buf_at(&d->b, d->caret - 1 - k) == ' ') ++k;
            if (k) {
                if (!had_sel) doc_group_begin(d), had_sel = true;
                doc_replace(d, d->caret - k, k, NULL, 0, false);
                d->caret -= k;
            }
        }
    }
    char pair[2] = { c, 0 };
    size_t ilen = 1;
    if (cl) {
        /* auto-close unless it would glue onto a word: it's, don't, foo(bar */
        bool word_before = is_word((unsigned char)prev) && (c == '"' || c == '\'');
        bool word_after = is_word((unsigned char)next);
        if (!word_before && !word_after) { pair[1] = cl; ilen = 2; }
    }
    doc_replace(d, d->caret, 0, pair, ilen, ilen == 1 && !had_sel);
    d->caret = d->anchor = d->caret + 1;
    d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
    if (had_sel) doc_group_end(d);
}

void doc_newline(Doc *d) {
    doc_group_begin(d);
    delete_selection(d);
    size_t l = buf_line_of(&d->b, d->caret);
    size_t ind = line_indent_len(d, l), a = buf_line_start(&d->b, l);
    if (ind > d->caret - a) ind = d->caret - a;           /* caret inside the indent: keep only what's left of it */
    int tw = G.tab_width > 0 ? G.tab_width : 4;
    char prevc = (char)prev_nonspace(d, d->caret);
    size_t n = buf_len(&d->b);
    char next = d->caret < n ? buf_at(&d->b, d->caret) : 0;

    char ibuf[256]; size_t k = 0;
    ibuf[k++] = '\n';
    for (size_t i = 0; i < ind && k < 200; ++i) ibuf[k++] = buf_at(&d->b, a + i);
    bool open = prevc == '{' || prevc == '(' || prevc == '[';
    if (open) for (int i = 0; i < tw && k < 220; ++i) ibuf[k++] = ' ';
    size_t caret_off = k;
    if (open && (next == '}' || next == ')' || next == ']')) {      /* split "{|}" onto three lines */
        ibuf[k++] = '\n';
        for (size_t i = 0; i < ind && k < 250; ++i) ibuf[k++] = buf_at(&d->b, a + i);
    }
    /* trim trailing blanks left on the line we are leaving */
    size_t tstart = d->caret;
    while (tstart > a && is_space((unsigned char)buf_at(&d->b, tstart - 1))) --tstart;
    doc_replace(d, tstart, d->caret - tstart, ibuf, k, false);
    doc_group_end(d);
    d->caret = d->anchor = tstart + caret_off;
    d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
    d->center_req = false;
}

void doc_backspace(Doc *d, bool word) {
    if (doc_has_sel(d)) { doc_group_begin(d); delete_selection(d); doc_group_end(d); return; }
    if (d->caret == 0) return;
    size_t n = buf_len(&d->b), p;
    if (word) {
        p = d->caret;
        while (p > 0 && is_space((unsigned char)buf_at(&d->b, p - 1))) --p;
        if (p > 0 && is_word((unsigned char)buf_at(&d->b, p - 1))) while (p > 0 && is_word((unsigned char)buf_at(&d->b, p - 1))) --p;
        else if (p > 0) --p;
    } else {
        p = doc_prev_cp(d, d->caret);
        char pc = buf_at(&d->b, p), nc = d->caret < n ? buf_at(&d->b, d->caret) : 0;
        if (closer_for(pc) && nc == closer_for(pc) && d->caret - p == 1) {        /* delete an empty pair together */
            doc_replace(d, p, 2, NULL, 0, false);
            d->caret = d->anchor = p;
            d->want_col = doc_visual_col(d, buf_line_of(&d->b, p), p);
            return;
        }
    }
    doc_replace(d, p, d->caret - p, NULL, 0, !word);
    d->caret = d->anchor = p;
    d->want_col = doc_visual_col(d, buf_line_of(&d->b, p), p);
}
void doc_delete(Doc *d, bool word) {
    if (doc_has_sel(d)) { doc_group_begin(d); delete_selection(d); doc_group_end(d); return; }
    size_t n = buf_len(&d->b);
    if (d->caret >= n) return;
    size_t p;
    if (word) {
        p = d->caret;
        while (p < n && is_word((unsigned char)buf_at(&d->b, p))) ++p;
        while (p < n && is_space((unsigned char)buf_at(&d->b, p))) ++p;
        if (p == d->caret) p = doc_next_cp(d, p);
    } else p = doc_next_cp(d, d->caret);
    doc_replace(d, d->caret, p - d->caret, NULL, 0, !word);
}

/* line range covered by the selection (or the caret line) */
static void sel_lines(Doc *d, size_t *l0, size_t *l1) {
    size_t a, b; doc_sel_range(d, &a, &b);
    *l0 = buf_line_of(&d->b, a);
    *l1 = buf_line_of(&d->b, b);
    if (b > a && b == buf_line_start(&d->b, *l1) && *l1 > *l0) --*l1;       /* selection ended at the start of a line */
}

void doc_indent(Doc *d, bool outdent) {
    int tw = G.tab_width > 0 ? G.tab_width : 4;
    if (!doc_has_sel(d) && !outdent) {                                   /* plain Tab: spaces up to the next stop */
        int col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
        int n = tw - (col % tw);
        char sp[16]; memset(sp, ' ', sizeof sp);
        doc_insert_at_caret(d, sp, (size_t)n);
        return;
    }
    size_t l0, l1; sel_lines(d, &l0, &l1);
    size_t a0, b0; doc_sel_range(d, &a0, &b0);
    doc_group_begin(d);
    long shift_start = 0, shift_end = 0;
    for (size_t l = l1 + 1; l-- > l0;) {
        size_t a = buf_line_start(&d->b, l);
        if (!outdent) {
            if (buf_line_end(&d->b, l) == a) continue;                       /* leave blank lines alone */
            char sp[16]; memset(sp, ' ', (size_t)tw);
            doc_replace(d, a, 0, sp, (size_t)tw, false);
            if (a <= a0) shift_start += tw;
            shift_end += tw;
        } else {
            size_t k = 0;
            if (buf_at(&d->b, a) == '\t' && buf_line_end(&d->b, l) > a) k = 1;
            else while (k < (size_t)tw && a + k < buf_line_end(&d->b, l) && buf_at(&d->b, a + k) == ' ') ++k;
            if (k) { doc_replace(d, a, k, NULL, 0, false); if (a < a0) shift_start -= (long)(a + k <= a0 ? k : a0 - a); shift_end -= (long)k; }
        }
    }
    doc_group_end(d);
    long na = (long)a0 + shift_start, nb = (long)b0 + shift_end;
    if (na < 0) na = 0;
    if (nb < na) nb = na;
    if (d->caret < d->anchor) { d->caret = (size_t)na; d->anchor = (size_t)nb; } else { d->anchor = (size_t)na; d->caret = (size_t)nb; }
}

void doc_toggle_comment(Doc *d) {
    size_t l0, l1; sel_lines(d, &l0, &l1);
    bool all = true;
    for (size_t l = l0; l <= l1; ++l) {
        size_t a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l), i = a;
        while (i < e && is_space((unsigned char)buf_at(&d->b, i))) ++i;
        if (i == e) continue;
        if (!(i + 1 < e && buf_at(&d->b, i) == '/' && buf_at(&d->b, i + 1) == '/')) { all = false; break; }
    }
    size_t a0, b0; doc_sel_range(d, &a0, &b0);
    long sa = 0, sb = 0;
    doc_group_begin(d);
    for (size_t l = l1 + 1; l-- > l0;) {
        size_t a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l), i = a;
        while (i < e && is_space((unsigned char)buf_at(&d->b, i))) ++i;
        if (i == e) continue;
        if (all) {
            size_t k = 2; if (i + 2 < e && buf_at(&d->b, i + 2) == ' ') k = 3;
            doc_replace(d, i, k, NULL, 0, false);
            if (i < a0) sa -= (long)k;
            sb -= (long)k;
        } else {
            doc_replace(d, i, 0, "// ", 3, false);
            if (i <= a0) sa += 3;
            sb += 3;
        }
    }
    doc_group_end(d);
    long na = (long)a0 + sa, nb = (long)b0 + sb;
    if (na < 0) na = 0;
    if (nb < na) nb = na;
    if (d->caret < d->anchor) { d->caret = (size_t)na; d->anchor = (size_t)nb; } else { d->anchor = (size_t)na; d->caret = (size_t)nb; }
}

void doc_duplicate_line(Doc *d) {
    size_t l0, l1; sel_lines(d, &l0, &l1);
    size_t a = buf_line_start(&d->b, l0), e = buf_line_end(&d->b, l1);
    size_t n = e - a;
    char *t = (char *)malloc(n + 2);
    if (!t) return;
    t[0] = '\n'; buf_copy(&d->b, a, n, t + 1);
    doc_group_begin(d);
    doc_replace(d, e, 0, t, n + 1, false);
    doc_group_end(d);
    d->caret = d->anchor = d->caret + n + 1;
    free(t);
}

void doc_move_line(Doc *d, int dir) {
    size_t l0, l1; sel_lines(d, &l0, &l1);
    size_t nl = buf_lines(&d->b);
    if ((dir < 0 && l0 == 0) || (dir > 0 && l1 + 1 >= nl)) return;
    size_t a = buf_line_start(&d->b, l0), e = buf_line_end(&d->b, l1);
    size_t oa, oe;
    if (dir < 0) { oa = buf_line_start(&d->b, l0 - 1); oe = buf_line_end(&d->b, l0 - 1); }
    else { oa = buf_line_start(&d->b, l1 + 1); oe = buf_line_end(&d->b, l1 + 1); }
    size_t blk = e - a, oth = oe - oa;
    char *A = (char *)malloc(blk + oth + 2);
    if (!A) return;
    size_t k = 0;
    if (dir < 0) { buf_copy(&d->b, a, blk, A); A[blk] = '\n'; buf_copy(&d->b, oa, oth, A + blk + 1); k = blk + 1 + oth; }
    else { buf_copy(&d->b, oa, oth, A); A[oth] = '\n'; buf_copy(&d->b, a, blk, A + oth + 1); k = oth + 1 + blk; }
    size_t start = dir < 0 ? oa : a, span = dir < 0 ? e - oa : oe - a;
    size_t old_a, old_b; doc_sel_range(d, &old_a, &old_b);
    doc_group_begin(d);
    doc_replace(d, start, span, A, k, false);
    doc_group_end(d);
    long delta = dir < 0 ? -(long)(oth + 1) : (long)(oth + 1);
    d->anchor = (size_t)((long)d->anchor + delta); d->caret = (size_t)((long)d->caret + delta);
    free(A);
}

/* ---------------------------------------------------------------- */
/* clipboard                                                         */
/* ---------------------------------------------------------------- */
static char *s_clip; static size_t s_clip_n;
void clip_set(const char *s, size_t n) {
    char *c = (char *)malloc(n + 1);
    if (!c) return;
    memcpy(c, s, n); c[n] = 0;
    free(s_clip); s_clip = c; s_clip_n = n;
}
const char *clip_get(size_t *n) { if (n) *n = s_clip_n; return s_clip ? s_clip : ""; }

void doc_cut_copy(Doc *d, bool cut) {
    size_t len = 0; char *t = NULL;
    if (doc_has_sel(d)) t = doc_sel_text(d, &len);
    else {                                                  /* no selection: the whole line, like every editor */
        size_t l = buf_line_of(&d->b, d->caret), a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l);
        len = e - a + 1;
        t = (char *)malloc(len + 1);
        if (t) { buf_copy(&d->b, a, e - a, t); t[e - a] = '\n'; t[len] = 0; }
        if (cut && t) { d->anchor = a; d->caret = e < buf_len(&d->b) ? e + 1 : e; if (e >= buf_len(&d->b)) len -= 1; }
    }
    if (!t) return;
    clip_set(t, len);
    free(t);
    if (cut) { doc_group_begin(d); delete_selection(d); doc_group_end(d); }
}
void doc_paste(Doc *d) {
    size_t n; const char *c = clip_get(&n);
    if (!n) return;
    doc_insert_at_caret(d, c, n);
    d->center_req = false;
}

/* ---------------------------------------------------------------- */
/* bracket matching                                                  */
/* ---------------------------------------------------------------- */
bool doc_match_bracket(Doc *d, size_t pos, size_t *a, size_t *b) {
    size_t n = buf_len(&d->b);
    size_t cand[2]; int nc = 0;
    if (pos < n) cand[nc++] = pos;
    if (pos > 0) cand[nc++] = pos - 1;
    for (int ci = 0; ci < nc; ++ci) {
        size_t p = cand[ci];
        char c = buf_at(&d->b, p), m = 0; int dir = 0;
        switch (c) {
            case '(': m = ')'; dir = 1; break;  case ')': m = '('; dir = -1; break;
            case '[': m = ']'; dir = 1; break;  case ']': m = '['; dir = -1; break;
            case '{': m = '}'; dir = 1; break;  case '}': m = '{'; dir = -1; break;
            default: break;
        }
        if (!dir) continue;
        int depth = 0; size_t steps = 0;
        for (long i = (long)p; i >= 0 && (size_t)i < n && steps < 40000; i += dir, ++steps) {
            char x = buf_at(&d->b, (size_t)i);
            if (x == c) ++depth;
            else if (x == m && --depth == 0) { *a = p; *b = (size_t)i; return true; }
        }
    }
    return false;
}
