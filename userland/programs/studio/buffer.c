/* buffer.c - gap buffer with a lazily rebuilt line table.
 *
 * The gap sits at the last edit, so typing is O(1) amortised and only a big
 * jump moves memory. The line-start table is rebuilt from the text on demand
 * (when `version` moved past `ls_ver`); one linear scan of a 10 000-line file
 * is well under a millisecond, and it only happens once per frame that
 * follows an edit. */
#include "studio.h"

#define GAP_MIN 4096

void buf_init(Buffer *b) {
    memset(b, 0, sizeof *b);
    b->cap = GAP_MIN;
    b->buf = (char *)malloc(b->cap);
    b->gs = 0; b->ge = b->cap;
    b->version = 1;
}
void buf_free(Buffer *b) {
    free(b->buf); free(b->ls);
    memset(b, 0, sizeof *b);
}
size_t buf_len(const Buffer *b) { return b->cap - (b->ge - b->gs); }

char buf_at(const Buffer *b, size_t i) {
    return i < b->gs ? b->buf[i] : b->buf[i + (b->ge - b->gs)];
}

static void gap_move(Buffer *b, size_t pos) {
    if (pos == b->gs) return;
    if (pos < b->gs) {
        size_t n = b->gs - pos;
        memmove(b->buf + b->ge - n, b->buf + pos, n);
        b->gs -= n; b->ge -= n;
    } else {
        size_t n = pos - b->gs;
        memmove(b->buf + b->gs, b->buf + b->ge, n);
        b->gs += n; b->ge += n;
    }
}
static void gap_reserve(Buffer *b, size_t need) {
    size_t gap = b->ge - b->gs;
    if (gap >= need) return;
    size_t len = buf_len(b);
    size_t ncap = b->cap * 2;
    while (ncap - len < need + GAP_MIN) ncap *= 2;
    char *nb = (char *)malloc(ncap);
    if (!nb) return;
    memcpy(nb, b->buf, b->gs);
    size_t tail = b->cap - b->ge;
    memcpy(nb + ncap - tail, b->buf + b->ge, tail);
    free(b->buf);
    b->buf = nb; b->ge = ncap - tail; b->cap = ncap;
}

void buf_insert(Buffer *b, size_t pos, const char *s, size_t n) {
    if (!n) return;
    size_t len = buf_len(b);
    if (pos > len) pos = len;
    gap_reserve(b, n);
    if ((b->ge - b->gs) < n) return;            /* out of memory: refuse rather than corrupt */
    gap_move(b, pos);
    memcpy(b->buf + b->gs, s, n);
    b->gs += n;
    b->version++;
}
void buf_delete(Buffer *b, size_t pos, size_t n) {
    size_t len = buf_len(b);
    if (pos >= len || !n) return;
    if (pos + n > len) n = len - pos;
    gap_move(b, pos);
    b->ge += n;
    b->version++;
}
void buf_copy(const Buffer *b, size_t pos, size_t n, char *out) {
    size_t len = buf_len(b);
    if (pos >= len) return;
    if (pos + n > len) n = len - pos;
    for (size_t i = 0; i < n; ++i) out[i] = buf_at(b, pos + i);
}
const char *buf_text(Buffer *b) {
    gap_move(b, buf_len(b));                    /* gap to the end: text is contiguous */
    return b->buf;
}

static void ls_rebuild(Buffer *b) {
    if (b->ls_ver == b->version && b->ls) return;
    const char *t = buf_text(b);
    size_t len = buf_len(b), n = 1;
    for (size_t i = 0; i < len; ++i) if (t[i] == '\n') ++n;
    if (n + 1 > b->ls_cap) {
        size_t nc = b->ls_cap ? b->ls_cap : 256;
        while (nc < n + 1) nc *= 2;
        uint32_t *nl = (uint32_t *)realloc(b->ls, nc * sizeof(uint32_t));
        if (!nl) return;
        b->ls = nl; b->ls_cap = nc;
    }
    size_t k = 0;
    b->ls[k++] = 0;
    for (size_t i = 0; i < len; ++i) if (t[i] == '\n') b->ls[k++] = (uint32_t)(i + 1);
    b->nl = k;
    b->ls_ver = b->version;
}
size_t buf_lines(Buffer *b) { ls_rebuild(b); return b->nl; }
size_t buf_line_start(Buffer *b, size_t line) {
    ls_rebuild(b);
    if (line >= b->nl) line = b->nl - 1;
    return b->ls[line];
}
/* End of the line's text, excluding the '\n' (documents are normalised to LF on load). */
size_t buf_line_end(Buffer *b, size_t line) {
    ls_rebuild(b);
    if (line >= b->nl) line = b->nl - 1;
    return (line + 1 < b->nl) ? b->ls[line + 1] - 1 : buf_len(b);
}
size_t buf_line_of(Buffer *b, size_t pos) {
    ls_rebuild(b);
    size_t lo = 0, hi = b->nl;                  /* last line whose start <= pos */
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (b->ls[mid] <= pos) lo = mid; else hi = mid;
    }
    return lo;
}
