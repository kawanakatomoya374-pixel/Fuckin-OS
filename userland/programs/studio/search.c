/* search.c - Find / Replace inside a document and Find in Project. */
#include "studio.h"

static bool is_wordc(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static inline int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool match_at(const char *t, size_t n, size_t i, const char *w, size_t wl, bool icase, bool word) {
    if (i + wl > n) return false;
    for (size_t k = 0; k < wl; ++k) {
        int a = (unsigned char)t[i + k], b = (unsigned char)w[k];
        if (icase ? lc(a) != lc(b) : a != b) return false;
    }
    if (word) {
        if (i > 0 && is_wordc((unsigned char)t[i - 1])) return false;
        if (i + wl < n && is_wordc((unsigned char)t[i + wl])) return false;
    }
    return true;
}

long find_next(Doc *d, const FindState *f, size_t from, bool backwards) {
    size_t wl = strlen(f->what);
    if (!wl) return -1;
    const char *t = buf_text(&d->b);
    size_t n = buf_len(&d->b);
    if (n < wl) return -1;
    if (!backwards) {
        for (size_t i = from; i + wl <= n; ++i) if (match_at(t, n, i, f->what, wl, f->icase, f->word)) return (long)i;
        for (size_t i = 0; i < from && i + wl <= n; ++i) if (match_at(t, n, i, f->what, wl, f->icase, f->word)) return (long)i;   /* wrap */
    } else {
        size_t start = from > 0 ? from - 1 : 0;
        if (start + wl > n) start = n - wl;
        for (long i = (long)start; i >= 0; --i) if (match_at(t, n, (size_t)i, f->what, wl, f->icase, f->word)) return i;
        for (long i = (long)(n - wl); i > (long)start; --i) if (match_at(t, n, (size_t)i, f->what, wl, f->icase, f->word)) return i;   /* wrap */
    }
    return -1;
}

int find_count(Doc *d, const FindState *f) {
    size_t wl = strlen(f->what);
    if (!wl) return 0;
    const char *t = buf_text(&d->b);
    size_t n = buf_len(&d->b);
    int c = 0;
    for (size_t i = 0; i + wl <= n; ) {
        if (match_at(t, n, i, f->what, wl, f->icase, f->word)) { ++c; i += wl; } else ++i;
    }
    return c;
}

int find_replace_all(Doc *d, const FindState *f) {
    size_t wl = strlen(f->what), rl = strlen(f->with);
    if (!wl) return 0;
    /* collect positions first (the buffer moves as we edit), replace back to front */
    const char *t = buf_text(&d->b);
    size_t n = buf_len(&d->b), cap = 64, cnt = 0;
    size_t *pos = (size_t *)malloc(cap * sizeof(size_t));
    if (!pos) return 0;
    for (size_t i = 0; i + wl <= n; ) {
        if (match_at(t, n, i, f->what, wl, f->icase, f->word)) {
            if (cnt == cap) { cap *= 2; size_t *np = (size_t *)realloc(pos, cap * sizeof(size_t)); if (!np) break; pos = np; }
            pos[cnt++] = i; i += wl;
        } else ++i;
    }
    if (cnt) {
        doc_group_begin(d);
        for (size_t k = cnt; k-- > 0;) doc_replace(d, pos[k], wl, f->with, rl, false);
        doc_group_end(d);
        if (d->caret > buf_len(&d->b)) d->caret = buf_len(&d->b);
        d->anchor = d->caret;
    }
    free(pos);
    return (int)cnt;
}

/* ---------------- Find in Project ---------------- */
#define MAX_HITS 300
static int s_files_seen;

static void scan_text(const char *path, const char *t, size_t n, const char *what, bool icase) {
    size_t wl = strlen(what);
    if (!wl) return;
    size_t line = 1, ls = 0;
    for (size_t i = 0; i < n && G.nhits < MAX_HITS; ++i) {
        if (t[i] == '\n') { ++line; ls = i + 1; continue; }
        if (match_at(t, n, i, what, wl, icase, false)) {
            size_t le = ls; while (le < n && t[le] != '\n' && t[le] != '\r') ++le;
            size_t a = ls; while (a < le && (t[a] == ' ' || t[a] == '\t')) ++a;
            size_t len = le - a; if (len > 90) len = 90;
            snprintf(G.hits[G.nhits].path, sizeof G.hits[G.nhits].path, "%s", path);
            memcpy(G.hits[G.nhits].text, t + a, len); G.hits[G.nhits].text[len] = 0;
            G.hits[G.nhits].line = (int)line;
            ++G.nhits;
            while (i < n && t[i] != '\n') ++i;          /* one hit per line */
            if (i < n) { ++line; ls = i + 1; }
        }
    }
}

static bool searchable(const char *name) {
    return has_ext(name, ".c") || has_ext(name, ".h") || has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".cpp") || has_ext(name, ".s");
}

static void walk(const char *dir, int depth, const char *what, bool icase) {
    if (depth > 6 || G.nhits >= MAX_HITS || s_files_seen > 600) return;
    int fd = cos_opendir(dir);
    if (fd < 0) return;
    cos_dirent_t de;
    static char names[64][64]; static bool isd[64];      /* per-level copy: readdir state is per fd, but keep stack small */
    int n = 0;
    char (*nm)[64] = (char (*)[64])malloc(64 * 64);
    bool *dd = (bool *)malloc(64);
    if (!nm || !dd) { free(nm); free(dd); cos_close(fd); return; }
    (void)names; (void)isd;
    while (n < 64 && cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.') continue;
        snprintf(nm[n], 64, "%s", de.name); dd[n] = de.is_dir != 0; ++n;
    }
    cos_close(fd);
    for (int i = 0; i < n && G.nhits < MAX_HITS; ++i) {
        char p[300]; path_join(p, sizeof p, dir, nm[i]);
        if (dd[i]) { walk(p, depth + 1, what, icase); continue; }
        if (!searchable(nm[i])) continue;
        ++s_files_seen;
        Doc *open = NULL;
        for (int k = 0; k < G.ndocs; ++k) if (!strcmp(G.docs[k]->path, p)) open = G.docs[k];
        if (open) { scan_text(p, buf_text(&open->b), buf_len(&open->b), what, icase); continue; }
        size_t len; char *txt = fs_read_all(p, &len);
        if (txt) { if (len < (512u << 10)) scan_text(p, txt, len, what, icase); free(txt); }
    }
    free(nm); free(dd);
}

void search_project(const char *what, bool icase) {
    if (!G.hits) G.hits = calloc(MAX_HITS, sizeof(*G.hits));
    G.nhits = 0; s_files_seen = 0;
    if (!what[0] || !G.proj.loaded || !G.hits) return;
    walk(G.proj.root, 0, what, icase);
}
