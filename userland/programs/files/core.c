/* core.c - file system side of Files: listing, sorting, history, tree, places,
 * config, file operations, the copy/move/delete job and thumbnails. */
#include "files.h"
#include "../../third_party/stb_image.h"

App G;
Pal P;

static const Pal PAL_LIGHT = { 0xF4F6FA, 0xFFFFFF, 0xEEF2F8, 0xF0F4FA, 0xD0D8E8, 0x202428, 0x808898, 0x1A73E8, 0xE8F0FE, 0xC8DFFE, 0xD93025, 0xB06000 };
static const Pal PAL_DARK  = { 0x1A1E28, 0x202430, 0x181C26, 0x1E2230, 0x384050, 0xE8ECF4, 0x8A92A4, 0x4D9FFF, 0x303848, 0x284060, 0xFF6B60, 0xE5B04B };

void apply_theme(void) { P = G.dark ? PAL_DARK : PAL_LIGHT; G.dirty = true; }

/* ---------------------------------------------------------------- */
/* fonts: Inter, with Noto Sans JP behind it for kana/kanji            */
/* ---------------------------------------------------------------- */
static uint8_t *s_jp;
static bool s_jp_tried;
cui_font *FN(int px, bool bold) {
    static cui_font *cache[2][40];
    if (px < 8) px = 8;
    if (px > 39) px = 39;
    if (!s_jp_tried) {
        s_jp_tried = true;
        cos_stat_t st;
        const char *fp = cos_getenv("FILES_JPFONT");                 /* host test override */
        if (!fp || !fp[0]) fp = "/system/fonts/NotoSansJP-Regular.ttf";
        if (cos_stat(fp, &st) == 0 && st.size > 1000) {
            s_jp = (uint8_t *)malloc((size_t)st.size);
            if (s_jp && cos_read_file(fp, s_jp, (size_t)st.size) != (ssize_t)st.size) { free(s_jp); s_jp = NULL; }
        }
    }
    if (!cache[bold][px]) {
        cui_font *f = bold ? cui_font_bold(px) : cui_font_default(px);
        if (f && s_jp) { cui_font *jp = cui_font_load(s_jp, px); if (jp) cui_font_set_fallback(f, jp); }
        cache[bold][px] = f;
    }
    return cache[bold][px];
}

void status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(G.status, sizeof G.status, fmt, ap);
    va_end(ap);
    G.status_t = cos_time_ms();
    G.dirty = true;
}

/* ---------------------------------------------------------------- */
/* paths                                                             */
/* ---------------------------------------------------------------- */
void path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = strlen(a);
    if (n && a[n - 1] == '/') snprintf(out, cap, "%s%s", a, b); else snprintf(out, cap, "%s/%s", a, b);
}
const char *path_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
void path_parent(const char *p, char *out, size_t cap) {
    const char *s = strrchr(p, '/');
    if (!s || s == p) { snprintf(out, cap, "/"); return; }
    size_t n = (size_t)(s - p); if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
}
void path_normalize(const char *in, char *out, size_t cap) {
    char tmp[PATHN]; snprintf(tmp, sizeof tmp, "%s", in);
    char *parts[64]; int np = 0;
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".") || !t[0]) continue;
        if (!strcmp(t, "..")) { if (np) --np; continue; }
        if (np < 64) parts[np++] = t;
    }
    if (!np) { snprintf(out, cap, "/"); return; }
    size_t k = 0; out[0] = 0;
    for (int i = 0; i < np && k + 2 < cap; ++i) k += (size_t)snprintf(out + k, cap - k, "/%s", parts[i]);
}
bool exists_path(const char *p) { cos_stat_ex_t st; return cos_stat_ex(p, &st) == 0; }
bool is_dir_path(const char *p) { cos_stat_ex_t st; return cos_stat_ex(p, &st) == 0 && st.is_dir; }

static bool ext_is(const char *name, const char *list) {          /* list: " .png .jpg " */
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    char key[24]; size_t n = 0;
    key[n++] = ' ';
    for (const char *p = dot; *p && n < 20; ++p) key[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    key[n++] = ' '; key[n] = 0;
    return strstr(list, key) != NULL;
}
int kind_of(const char *name, bool is_dir) {
    if (is_dir) return K_FOLDER;
    if (ext_is(name, " .png .jpg .jpeg .jpe .bmp .gif .webp .tga .psd ")) return K_IMAGE;
    if (ext_is(name, " .mp3 .wav .ogg .flac .m4a .aac ")) return K_AUDIO;
    if (ext_is(name, " .mp4 .mkv .avi .mov .webm ")) return K_VIDEO;
    if (ext_is(name, " .c .h .cpp .hpp .cc .py .js .java .rs .go .s .asm .sh .lua .json .xml .html .css ")) return K_CODE;
    if (ext_is(name, " .zip .tar .gz .bz2 .xz .7z .rar .iso ")) return K_ARCHIVE;
    if (ext_is(name, " .c-os .c-osll .o .a ")) return K_EXEC;
    if (ext_is(name, " .txt .md .log .cfg .conf .ini .csv ") || !strcasecmp(name, "Makefile") || !strcasecmp(name, "README")) return K_TEXT;
    if (ext_is(name, " .pdf .doc .docx .xls .xlsx .ppt .pptx ")) return K_DOC;
    return K_OTHER;
}
const char *kind_name(int k) {
    switch (k) {
        case K_FOLDER:  return L("Folder", "フォルダー");
        case K_IMAGE:   return L("Image", "画像");
        case K_AUDIO:   return L("Audio", "音声");
        case K_VIDEO:   return L("Video", "動画");
        case K_CODE:    return L("Source", "ソースコード");
        case K_ARCHIVE: return L("Archive", "アーカイブ");
        case K_EXEC:    return L("Program", "プログラム");
        case K_TEXT:    return L("Text", "テキスト");
        case K_DOC:     return L("Document", "ドキュメント");
        default:        return L("File", "ファイル");
    }
}
void fmt_size(uint64_t n, char *out, size_t cap) {
    if (n < 1024) snprintf(out, cap, "%llu B", (unsigned long long)n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%llu KB", (unsigned long long)((n + 512) / 1024));
    else if (n < 1024ull * 1024 * 1024) snprintf(out, cap, "%d.%d MB", (int)(n / (1024 * 1024)), (int)((n % (1024 * 1024)) * 10 / (1024 * 1024)));
    else snprintf(out, cap, "%d.%02d GB", (int)(n / (1024ull * 1024 * 1024)), (int)((n % (1024ull * 1024 * 1024)) * 100 / (1024ull * 1024 * 1024)));
}
void fmt_date(uint64_t t, char *out, size_t cap) {
    if (!t) { snprintf(out, cap, "-"); return; }
    int64_t days = (int64_t)(t / 86400); int sec = (int)(t % 86400);
    int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", (int)y, m, d, sec / 3600, (sec % 3600) / 60);
}

/* ---------------------------------------------------------------- */
/* directory listing, sorting, selection                             */
/* ---------------------------------------------------------------- */
static int s_sort, s_asc;
static int cmp_ent(const void *pa, const void *pb) {
    const Ent *a = &G.all[*(const int *)pa], *b = &G.all[*(const int *)pb];
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;                    /* folders first, always */
    int r = 0;
    switch (s_sort) {
        case S_SIZE: r = a->size < b->size ? -1 : a->size > b->size ? 1 : 0; break;
        case S_DATE: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime ? 1 : 0; break;
        case S_TYPE: r = a->kind - b->kind; break;
        default: break;
    }
    if (!r) r = strcasecmp(a->name, b->name);
    return s_asc ? r : -r;
}

/* case-insensitive substring (ASCII folding; UTF-8 bytes >= 0x80 compare exactly), since the C-OS libc has no strcasestr */
static bool ci_contains(const char *h, const char *n) {
    size_t ln = strlen(n);
    if (!ln) return true;
    for (; *h; ++h) {
        size_t i = 0;
        for (; i < ln && h[i]; ++i) {
            unsigned char a = (unsigned char)h[i], b = (unsigned char)n[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == ln) return true;
    }
    return false;
}

Ent *ent_at(int vi) { return (vi >= 0 && vi < G.nvis) ? &G.all[G.vis[vi]] : NULL; }

void rebuild_view(void) {
    G.nvis = 0;
    for (int i = 0; i < G.nall; ++i) {
        Ent *e = &G.all[i];
        bool hidden = (e->attr & COS_ATTR_HIDDEN) || e->name[0] == '.';
        if (hidden && !G.show_hidden) { e->sel = 0; continue; }
        if (G.search[0] && !ci_contains(e->name, G.search)) { e->sel = 0; continue; }
        G.vis[G.nvis++] = i;
    }
    s_sort = G.sort; s_asc = G.sort_asc;
    qsort(G.vis, (size_t)G.nvis, sizeof(int), cmp_ent);
    if (G.focus >= G.nvis) G.focus = G.nvis - 1;
    G.hover = -1;
    G.dirty = true;
}

void refresh_space(void) {
    uint64_t t = 0, f = 0;
    if (cos_fs_space(&t, &f) == 0) { G.sp_total = t; G.sp_free = f; }
    G.sp_t = cos_time_ms();
}

void dir_load(bool keep_sel) {
    static cos_dirent_ex_t *raw;
    if (!raw) raw = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * MAX_ENT);
    char keep[48][256]; int nk = 0;
    if (keep_sel) for (int i = 0; i < G.nall && nk < 48; ++i) if (G.all[i].sel) snprintf(keep[nk++], 256, "%s", G.all[i].name);
    int n = raw ? cos_listdir_ex(G.path, raw, MAX_ENT) : -1;
    if (n < 0) {
        status(L("Cannot open this folder", "このフォルダーを開けません"));
        snprintf(G.path, sizeof G.path, "/");
        n = raw ? cos_listdir_ex("/", raw, MAX_ENT) : 0;
        if (n < 0) n = 0;
    }
    G.truncated = n >= MAX_ENT;
    G.nall = 0;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(raw[i].name, ".") || !strcmp(raw[i].name, "..")) continue;
        Ent *e = &G.all[G.nall++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", raw[i].name);
        e->is_dir = raw[i].is_dir; e->size = raw[i].size; e->mtime = raw[i].mtime; e->attr = raw[i].attr;
        e->kind = (uint8_t)kind_of(e->name, e->is_dir);
        for (int k = 0; k < nk; ++k) if (!strcmp(keep[k], e->name)) e->sel = 1;
    }
    rebuild_view();
    if (G.truncated) status(L("Showing the first %d items", "先頭の%d件を表示しています"), MAX_ENT);
    refresh_space();
    G.pv_ent_hash = 0;
}

int sel_count(void) { int n = 0; for (int i = 0; i < G.nvis; ++i) if (G.all[G.vis[i]].sel) ++n; return n; }
void sel_clear(void) { for (int i = 0; i < G.nall; ++i) G.all[i].sel = 0; G.dirty = true; }
void sel_set_only(int vi) { sel_clear(); Ent *e = ent_at(vi); if (e) e->sel = 1; G.focus = G.anchor = vi; G.dirty = true; }
void sel_range(int a, int b, bool add) {
    if (!add) sel_clear();
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; ++i) { Ent *e = ent_at(i); if (e) e->sel = 1; }
    G.dirty = true;
}

/* ---------------------------------------------------------------- */
/* navigation                                                        */
/* ---------------------------------------------------------------- */
void fm_set_title(void) {
    char t[64];
    const char *b = strcmp(G.path, "/") ? path_base(G.path) : L("Disk", "ディスク");
    snprintf(t, sizeof t, "%s - %s", L("File Manager", "ファイルマネージャー"), b);
    cos_win2_set_title(G.win, t);
}

void nav_to(const char *path, bool push) {
    char np[PATHN];
    path_normalize(path, np, sizeof np);
    if (!is_dir_path(np)) { status(L("Not a folder: %s", "フォルダーではありません: %s"), np); return; }
    snprintf(G.path, sizeof G.path, "%s", np);
    if (push) {
        if (G.hi + 1 < G.hn) G.hn = G.hi + 1;                           /* going somewhere new drops the forward list */
        if (G.hn == 0 || strcmp(G.hist[G.hn - 1], np)) {
            if (G.hn == MAX_HIST) { memmove(G.hist[0], G.hist[1], (size_t)(MAX_HIST - 1) * PATHN); --G.hn; }
            snprintf(G.hist[G.hn++], PATHN, "%s", np);
        }
        G.hi = G.hn - 1;
    }
    sel_clear(); G.scroll = 0; G.focus = G.anchor = -1; G.search[0] = 0; G.addr_edit = false;
    dir_load(false);
    tree_reveal(np);
    tree_rebuild();
    fm_set_title();
    config_save();
}
void nav_back(void)    { if (G.hi > 0) { --G.hi; nav_to(G.hist[G.hi], false); } }
void nav_forward(void) { if (G.hi + 1 < G.hn) { ++G.hi; nav_to(G.hist[G.hi], false); } }
void nav_up(void) {
    if (!strcmp(G.path, "/")) return;
    char up[PATHN], from[256];
    snprintf(from, sizeof from, "%s", path_base(G.path));
    path_parent(G.path, up, sizeof up);
    nav_to(up, true);
    for (int i = 0; i < G.nvis; ++i) if (!strcmp(ent_at(i)->name, from)) { sel_set_only(i); ensure_visible(i); break; }   /* select where we came from */
}

/* ---------------------------------------------------------------- */
/* folder tree                                                       */
/* ---------------------------------------------------------------- */
static bool topen_has(const char *p) { for (int i = 0; i < G.ntopen; ++i) if (!strcmp(G.topen[i], p)) return true; return false; }
static void topen_set(const char *p, bool on) {
    for (int i = 0; i < G.ntopen; ++i) if (!strcmp(G.topen[i], p)) {
        if (!on) { memmove(G.topen[i], G.topen[i + 1], (size_t)(G.ntopen - i - 1) * PATHN); --G.ntopen; }
        return;
    }
    if (on && G.ntopen < 64) snprintf(G.topen[G.ntopen++], PATHN, "%s", p);
}
static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void tree_children(const char *path, int depth) {
    static cos_dirent_ex_t *raw[8];                                     /* one buffer per depth: recursion must not reuse it */
    if (depth >= 8 || G.ntree >= MAX_TREE) return;
    if (!raw[depth]) raw[depth] = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * 256);
    if (!raw[depth]) return;
    int n = cos_listdir_ex(path, raw[depth], 256);
    if (n <= 0) return;
    char (*names)[256] = (char (*)[256])malloc((size_t)n * 256);
    if (!names) return;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (!raw[depth][i].is_dir || !strcmp(raw[depth][i].name, ".") || !strcmp(raw[depth][i].name, "..")) continue;
        if ((raw[depth][i].name[0] == '.' || (raw[depth][i].attr & COS_ATTR_HIDDEN)) && !G.show_hidden) continue;
        snprintf(names[k++], 256, "%s", raw[depth][i].name);
    }
    qsort(names, (size_t)k, 256, cmp_names);
    for (int i = 0; i < k && G.ntree < MAX_TREE; ++i) {
        TNode *t = &G.tree[G.ntree++];
        path_join(t->path, sizeof t->path, path, names[i]);
        snprintf(t->name, sizeof t->name, "%s", names[i]);
        t->depth = depth; t->open = topen_has(t->path);
        if (t->open) { char child[PATHN]; snprintf(child, sizeof child, "%s", t->path); tree_children(child, depth + 1); }
    }
    free(names);
}
void tree_rebuild(void) {
    G.ntree = 0;
    TNode *r = &G.tree[G.ntree++];
    snprintf(r->path, sizeof r->path, "/"); snprintf(r->name, sizeof r->name, "%s", L("Root", "ルート"));
    r->depth = 0; r->open = true;
    tree_children("/", 1);
    G.dirty = true;
}
void tree_toggle(int i) {
    if (i <= 0 || i >= G.ntree) return;
    topen_set(G.tree[i].path, !G.tree[i].open);
    tree_rebuild();
}
void tree_reveal(const char *path) {
    char acc[PATHN] = "";
    char tmp[PATHN]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        size_t l = strlen(acc);
        snprintf(acc + l, sizeof acc - l, "/%s", t);
        char up[PATHN]; snprintf(up, sizeof up, "%s", acc);
        if (strcmp(up, path)) topen_set(up, true);               /* every ancestor open; the folder itself stays as it was */
    }
}

/* ---------------------------------------------------------------- */
/* places (bookmarks)                                                */
/* ---------------------------------------------------------------- */
void marks_add(const char *name, const char *path) {
    for (int i = 0; i < G.nmarks; ++i) if (!strcmp(G.marks[i].path, path)) return;
    if (G.nmarks >= MAX_MARK) { status(L("Too many bookmarks", "ブックマークが多すぎます")); return; }
    Mark *m = &G.marks[G.nmarks++];
    snprintf(m->name, sizeof m->name, "%s", name); snprintf(m->path, sizeof m->path, "%s", path); m->builtin = false;
}
void marks_remove(int i) {
    if (i < 0 || i >= G.nmarks || G.marks[i].builtin) return;
    memmove(&G.marks[i], &G.marks[i + 1], (size_t)(G.nmarks - i - 1) * sizeof(Mark)); --G.nmarks;
    config_save(); G.dirty = true;
}
void marks_default(void) {
    G.nmarks = 0;
    static const struct { const char *en, *ja, *path; } D[] = {
        { "Disk", "ディスク", "/" }, { "C-OS Studio", "C-OS Studio", "/C-OS Studio" }, { "Desktop", "デスクトップ", "/desktop" },
        { "Documents", "ドキュメント", "/documents" }, { "Music", "ミュージック", "/music" },
        { "Pictures", "ピクチャ", "/pictures" }, { "Downloads", "ダウンロード", "/downloads" } };
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; ++i) {
        Mark *m = &G.marks[G.nmarks++];
        snprintf(m->name, sizeof m->name, "%s", G.jp ? D[i].ja : D[i].en); snprintf(m->path, sizeof m->path, "%s", D[i].path); m->builtin = true;
    }
}

/* ---------------------------------------------------------------- */
/* config: /etc/files.conf                                           */
/* ---------------------------------------------------------------- */
#define CONF "/etc/files.conf"
void config_save(void) {
    char b[2048]; size_t k = 0;
    k += (size_t)snprintf(b + k, sizeof b - k, "view=%d\nsort=%d\nasc=%d\nhidden=%d\npreview=%d\ntree=%d\npath=%s\n",
                          G.view, G.sort, G.sort_asc, G.show_hidden, G.preview, G.tree_vis, G.path);
    for (int i = 0; i < G.nmarks && k + 320 < sizeof b; ++i) if (!G.marks[i].builtin)
        k += (size_t)snprintf(b + k, sizeof b - k, "mark=%s|%s\n", G.marks[i].name, G.marks[i].path);
    cos_write_file(CONF, b, k);
}
void config_load(void) {
    cos_stat_t st;
    if (cos_stat(CONF, &st) != 0 || st.size == 0 || st.size > 8192) return;
    char *t = (char *)malloc((size_t)st.size + 1);
    if (!t) return;
    ssize_t n = cos_read_file(CONF, t, (size_t)st.size);
    if (n <= 0) { free(t); return; }
    t[n] = 0;
    for (char *save = NULL, *ln = strtok_r(t, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(ln, '='); if (!eq) continue;
        *eq = 0; const char *v = eq + 1; int iv = atoi(v);
        if (!strcmp(ln, "view") && iv >= 0 && iv <= 3) G.view = iv;
        else if (!strcmp(ln, "sort") && iv >= 0 && iv <= 3) G.sort = iv;
        else if (!strcmp(ln, "asc")) G.sort_asc = iv != 0;
        else if (!strcmp(ln, "hidden")) G.show_hidden = iv != 0;
        else if (!strcmp(ln, "preview")) G.preview = iv != 0;
        else if (!strcmp(ln, "tree")) G.tree_vis = iv != 0;
        else if (!strcmp(ln, "path")) snprintf(G.path, sizeof G.path, "%s", v);
        else if (!strcmp(ln, "mark")) { char *bar = strchr((char *)v, '|'); if (bar) { *bar = 0; marks_add(v, bar + 1); } }
    }
    free(t);
}

/* ---------------------------------------------------------------- */
/* file operations                                                   */
/* ---------------------------------------------------------------- */
/* "name", "name (2)", "name (3)" ... keeping the extension */
void unique_name(const char *dir, const char *name, char *out, size_t cap) {
    char p[PATHN];
    path_join(p, sizeof p, dir, name);
    if (!exists_path(p)) { snprintf(out, cap, "%s", name); return; }
    char stem[256], ext[64] = "";
    snprintf(stem, sizeof stem, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    for (int i = 2; i < 1000; ++i) {
        char cand[300]; snprintf(cand, sizeof cand, "%s (%d)%s", stem, i, ext);
        path_join(p, sizeof p, dir, cand);
        if (!exists_path(p)) { snprintf(out, cap, "%s", cand); return; }
    }
    snprintf(out, cap, "%s", name);
}
static bool name_ok(const char *n) {
    if (!n[0] || strchr(n, '/') || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (const char *p = n; *p; ++p) if (*p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') return false;
    return true;
}
bool op_mkdir(const char *name, char *created, size_t cap) {
    if (!name_ok(name)) { status(L("Invalid name", "名前が正しくありません")); return false; }
    char nm[300], p[PATHN];
    unique_name(G.path, name, nm, sizeof nm);
    path_join(p, sizeof p, G.path, nm);
    if (cos_mkdir(p) != 0) { status(L("Could not create the folder", "フォルダーを作成できません")); return false; }
    if (created) snprintf(created, cap, "%s", nm);
    return true;
}
bool op_newfile(const char *name, char *created, size_t cap) {
    if (!name_ok(name)) { status(L("Invalid name", "名前が正しくありません")); return false; }
    char nm[300], p[PATHN];
    unique_name(G.path, name, nm, sizeof nm);
    path_join(p, sizeof p, G.path, nm);
    int fd = cos_open(p, COS_O_WRONLY_CREATE);                          /* len 0: cos_write_file() rejects empty writes */
    if (fd < 0) { status(L("Could not create the file", "ファイルを作成できません")); return false; }
    cos_close(fd);
    if (created) snprintf(created, cap, "%s", nm);
    return true;
}
bool op_rename(const char *from, const char *new_name) {
    if (!name_ok(new_name)) { status(L("Invalid name", "名前が正しくありません")); return false; }
    if (!strcmp(path_base(from), new_name)) return true;
    char dir[PATHN], to[PATHN];
    path_parent(from, dir, sizeof dir);
    path_join(to, sizeof to, dir, new_name);
    if (exists_path(to) && strcasecmp(path_base(from), new_name)) { status(L("\"%s\" already exists", "「%s」は既に存在します"), new_name); return false; }
    if (cos_rename(from, to) != 0) { status(L("Could not rename", "名前を変更できません")); return false; }
    return true;
}

/* ---------------------------------------------------------------- */
/* clipboard: the system clipboard, text "COSFILES1\n<cut|copy>\n<path>\n..." */
/* ---------------------------------------------------------------- */
void clip_put(bool cut) {
    char *b = (char *)malloc(COS_CLIP_MAX);
    if (!b) return;
    size_t k = (size_t)snprintf(b, COS_CLIP_MAX, "COSFILES1\n%s\n", cut ? "cut" : "copy");
    int n = 0;
    for (int i = 0; i < G.nvis; ++i) {
        Ent *e = ent_at(i);
        if (!e->sel) continue;
        char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
        if (k + strlen(p) + 2 >= COS_CLIP_MAX) break;
        k += (size_t)snprintf(b + k, COS_CLIP_MAX - k, "%s\n", p);
        ++n;
    }
    if (n == 0) { free(b); status(L("Nothing selected", "何も選択されていません")); return; }
    cos_clip_set(b, k);
    free(b);
    status(cut ? L("Cut %d item(s)", "%d個を切り取りました") : L("Copied %d item(s)", "%d個をコピーしました"), n);
}
bool clip_has_files(void) {
    char h[16] = "";
    int64_t n = cos_clip_get(NULL, 0);
    if (n < 12) return false;
    cos_clip_get(h, 10);
    return !memcmp(h, "COSFILES1\n", 10);
}
void clip_paste(void) {
    int64_t total = cos_clip_get(NULL, 0);
    if (total < 12) { status(L("Nothing to paste", "貼り付けるものがありません")); return; }
    char *b = (char *)malloc((size_t)total + 1);
    if (!b) return;
    int64_t n = cos_clip_get(b, (size_t)total);
    if (n < 12 || memcmp(b, "COSFILES1\n", 10)) { free(b); status(L("Nothing to paste", "貼り付けるものがありません")); return; }
    b[n] = 0;
    char *save = NULL;
    strtok_r(b, "\n", &save);                                            /* header */
    char *mode = strtok_r(NULL, "\n", &save);
    bool cut = mode && !strcmp(mode, "cut");
    static char srcs[64][PATHN]; int ns = 0;
    for (char *p = strtok_r(NULL, "\n", &save); p && ns < 64; p = strtok_r(NULL, "\n", &save))
        if (exists_path(p)) snprintf(srcs[ns++], PATHN, "%s", p);
    free(b);
    if (!ns) { status(L("The copied items no longer exist", "コピー元が見つかりません")); return; }
    job_start_transfer(cut ? 2 : 1, srcs, ns, G.path);
    if (cut) cos_clip_set("", 0);                                        /* a cut can be pasted once */
}
void copy_path_text(void) {
    Ent *e = ent_at(G.focus >= 0 ? G.focus : 0);
    char p[PATHN];
    if (e && e->sel) path_join(p, sizeof p, G.path, e->name); else snprintf(p, sizeof p, "%s", G.path);
    cos_clip_set(p, strlen(p));
    status(L("Path copied", "パスをコピーしました"));
}

/* ---------------------------------------------------------------- */
/* jobs: copy / move / delete, a few hundred KB per UI tick             */
/* ---------------------------------------------------------------- */
static void task_add(int op, const char *src, const char *dst, uint64_t size, bool is_dir) {
    Job *j = &G.job;
    if (j->nt >= MAX_TASKS) return;
    if (j->nt == j->cap) {
        int nc = j->cap ? j->cap * 2 : 64; if (nc > MAX_TASKS) nc = MAX_TASKS;
        Task *nt = (Task *)realloc(j->t, (size_t)nc * sizeof(Task));
        if (!nt) return;
        j->t = nt; j->cap = nc;
    }
    Task *t = &j->t[j->nt++];
    memset(t, 0, sizeof *t);
    t->op = (uint8_t)op; t->is_dir = is_dir; t->size = size;
    snprintf(t->src, sizeof t->src, "%s", src); if (dst) snprintf(t->dst, sizeof t->dst, "%s", dst);
    if (op == OP_COPY) { j->total += size; ++j->files_total; }
}

/* the whole tree as tasks: copy = parents first, delete = children first */
static void walk(const char *src, const char *dst, int mode, int depth) {
    cos_stat_ex_t st;
    if (cos_stat_ex(src, &st) != 0) return;
    if (!st.is_dir) { task_add(mode == OP_DEL ? OP_DEL : OP_COPY, src, dst, st.size, false); return; }
    if (depth > 24) return;
    if (mode != OP_DEL) task_add(OP_MKDIR, src, dst, 0, true);
    cos_dirent_ex_t *raw = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * MAX_ENT);
    if (raw) {
        int n = cos_listdir_ex(src, raw, MAX_ENT);
        for (int i = 0; i < n; ++i) {
            if (!strcmp(raw[i].name, ".") || !strcmp(raw[i].name, "..")) continue;
            char s2[PATHN], d2[PATHN];
            path_join(s2, sizeof s2, src, raw[i].name);
            if (dst) path_join(d2, sizeof d2, dst, raw[i].name);
            walk(s2, dst ? d2 : NULL, mode, depth + 1);
        }
        free(raw);
    }
    if (mode == OP_DEL) task_add(OP_DEL, src, NULL, 0, true);
}

static void job_reset(int mode) {
    Job *j = &G.job;
    free(j->t);
    memset(j, 0, sizeof *j);
    j->mode = mode; j->in_fd = j->out_fd = -1; j->t0 = cos_time_ms();
}

void job_start_delete(void) {
    if (G.job.mode) { status(L("Another operation is running", "別の操作を実行中です")); return; }
    job_reset(3);
    for (int i = 0; i < G.nvis; ++i) {
        Ent *e = ent_at(i);
        if (!e->sel) continue;
        char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
        walk(p, NULL, OP_DEL, 0);
    }
    if (!G.job.nt) { G.job.mode = 0; return; }
    G.dlg = D_PROGRESS; G.dirty = true;
}

void job_start_transfer(int mode, char (*srcs)[PATHN], int n, const char *dstdir) {
    if (G.job.mode) { status(L("Another operation is running", "別の操作を実行中です")); return; }
    job_reset(mode);
    snprintf(G.job.dstdir, sizeof G.job.dstdir, "%s", dstdir);
    for (int i = 0; i < n; ++i) {
        const char *base = path_base(srcs[i]);
        char dst[PATHN];
        path_join(dst, sizeof dst, dstdir, base);
        char sp[PATHN]; path_parent(srcs[i], sp, sizeof sp);
        bool same_dir = !strcmp(sp, dstdir);
        if (is_dir_path(srcs[i])) {                                     /* a folder into itself would never end */
            size_t l = strlen(srcs[i]);
            if (!strncmp(dstdir, srcs[i], l) && (dstdir[l] == '/' || dstdir[l] == 0)) { status(L("Cannot copy a folder into itself", "フォルダーを自分自身の中には置けません")); continue; }
        }
        if (mode == 2 && same_dir) continue;                            /* moving onto itself: nothing to do */
        if (same_dir) { char nm[300]; unique_name(dstdir, base, nm, sizeof nm); path_join(dst, sizeof dst, dstdir, nm); }   /* paste next to the original: "name (2)" */
        if (mode == 2 && !exists_path(dst)) {
            if (cos_rename(srcs[i], dst) == 0) { ++G.job.files_done; continue; }   /* same volume: a rename is instant */
        }
        walk(srcs[i], dst, OP_COPY, 0);
        if (mode == 2) walk(srcs[i], NULL, OP_DEL, 0);                  /* after the copy: remove the source */
    }
    if (!G.job.nt) {
        int moved = G.job.files_done;
        G.job.mode = 0;
        if (moved) { dir_load(false); tree_rebuild(); status(L("Moved %d item(s)", "%d個を移動しました"), moved); }
        return;
    }
    G.dlg = D_PROGRESS; G.dirty = true;
}

void job_answer(int policy, bool all) {
    Job *j = &G.job;
    if (!j->waiting) return;
    Task *t = &j->t[j->cur];
    if (all) j->policy = policy;
    if (policy == 2) { t->op = 255; j->total -= t->size; }              /* skip */
    else if (policy == 3) {                                             /* keep both: a new name for this one */
        char dir[PATHN], nm[300];
        path_parent(t->dst, dir, sizeof dir);
        unique_name(dir, path_base(t->dst), nm, sizeof nm);
        path_join(t->dst, sizeof t->dst, dir, nm);
    } else if (policy == 1) cos_unlink(t->dst);                         /* replace */
    j->waiting = false;
    if (G.dlg == D_CONFLICT) G.dlg = D_PROGRESS;
    G.dirty = true;
}
void job_cancel(void) { G.job.cancel = true; if (G.job.waiting) { G.job.waiting = false; } }

static void job_finish(void) {
    Job *j = &G.job;
    if (j->in_fd >= 0) cos_close(j->in_fd);
    if (j->out_fd >= 0) cos_close(j->out_fd);
    j->in_fd = j->out_fd = -1;
    int mode = j->mode, errs = j->errors, done = j->files_done;
    bool cancelled = j->cancel;
    j->mode = 0;
    if (G.dlg == D_PROGRESS || G.dlg == D_CONFLICT) G.dlg = D_NONE;
    dir_load(true);
    tree_rebuild();
    if (cancelled) status(L("Cancelled", "キャンセルしました"));
    else if (errs) status(L("Finished with %d error(s)", "%d件のエラーがありました"), errs);
    else if (mode == 3) status(L("Deleted", "削除しました"));
    else status(mode == 2 ? L("Moved %d file(s)", "%d個のファイルを移動しました") : L("Copied %d file(s)", "%d個のファイルをコピーしました"), done);
}

void job_tick(void) {
    Job *j = &G.job;
    if (!j->mode) return;
    if (j->cancel) { job_finish(); return; }
    if (j->waiting) return;
    uint64_t t0 = cos_time_ms();
    uint64_t budget = 512 * 1024;
    while (cos_time_ms() - t0 < 20 && budget) {
        if (j->cur >= j->nt) { job_finish(); return; }
        Task *t = &j->t[j->cur];
        snprintf(j->cur_name, sizeof j->cur_name, "%s", path_base(t->src));
        if (t->op == 255) { ++j->cur; continue; }
        if (t->op == OP_MKDIR) {
            if (!is_dir_path(t->dst) && cos_mkdir(t->dst) != 0) { ++j->errors; t->op = 255; }
            ++j->cur; continue;
        }
        if (t->op == OP_DEL) {
            if (cos_unlink(t->src) != 0) ++j->errors;
            ++j->cur; ++j->files_done; continue;
        }
        /* OP_COPY */
        if (j->in_fd < 0) {
            if (exists_path(t->dst)) {
                if (j->policy == 0) { j->waiting = true; snprintf(j->conflict_dst, sizeof j->conflict_dst, "%s", t->dst); G.dlg = D_CONFLICT; G.dlg_all = false; G.dirty = true; return; }
                job_answer(j->policy, false);
                if (t->op == 255) { ++j->cur; continue; }
            }
            j->in_fd = cos_open(t->src, COS_O_RDONLY);
            j->out_fd = j->in_fd >= 0 ? cos_open(t->dst, COS_O_WRONLY_CREATE) : -1;
            if (j->in_fd < 0 || j->out_fd < 0) {
                if (j->in_fd >= 0) cos_close(j->in_fd);
                j->in_fd = j->out_fd = -1; ++j->errors; ++j->cur; continue;
            }
            j->off = 0;
        }
        static uint8_t buf[64 * 1024];
        ssize_t n = cos_fd_read(j->in_fd, buf, sizeof buf);
        if (n > 0) {
            ssize_t w = cos_fd_write(j->out_fd, buf, (size_t)n);
            if (w != n) { ++j->errors; n = 0; }
            else { j->off += (uint64_t)n; j->done += (uint64_t)n; budget = budget > (uint64_t)n ? budget - (uint64_t)n : 0; }
        }
        if (n <= 0 || j->off >= t->size) {
            cos_close(j->in_fd); cos_close(j->out_fd);
            j->in_fd = j->out_fd = -1;
            ++j->cur; ++j->files_done;
        }
    }
    G.dirty = true;
}

void dir_stats(const char *path, uint64_t *files, uint64_t *dirs, uint64_t *bytes) {
    cos_dirent_ex_t *raw = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * MAX_ENT);
    if (!raw) return;
    int n = cos_listdir_ex(path, raw, MAX_ENT);
    for (int i = 0; i < n; ++i) {
        if (!strcmp(raw[i].name, ".") || !strcmp(raw[i].name, "..")) continue;
        if (raw[i].is_dir) {
            ++*dirs;
            if (*dirs + *files < 6000) { char c[PATHN]; path_join(c, sizeof c, path, raw[i].name); dir_stats(c, files, dirs, bytes); }
        } else { ++*files; *bytes += raw[i].size; }
    }
    free(raw);
}

/* ---------------------------------------------------------------- */
/* opening                                                           */
/* ---------------------------------------------------------------- */
void open_path_external(const char *path) {
    if (cos_open_path(path) != 0) status(L("Could not open the file", "ファイルを開けません"));
}
void open_entry(int vi) {
    Ent *e = ent_at(vi);
    if (!e) return;
    char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
    if (e->is_dir) nav_to(p, true); else open_path_external(p);
}

/* ---------------------------------------------------------------- */
/* thumbnails and preview                                            */
/* ---------------------------------------------------------------- */
void blit_argb(cui_canvas *c, int x, int y, const uint32_t *px, int w, int h) {
    for (int j = 0; j < h; ++j) {
        int dy = y + j;
        if (dy < c->cy0 || dy >= c->cy1) continue;
        for (int i = 0; i < w; ++i) {
            int dx = x + i;
            if (dx < c->cx0 || dx >= c->cx1) continue;
            uint32_t s = px[j * w + i]; int a = (int)(s >> 24);
            uint32_t *d = &c->px[dy * c->stride + dx];
            if (a == 255) *d = s & 0xFFFFFF;
            else if (a) {
                uint32_t dd = *d;
                int r = (int)((s >> 16) & 255) * a + (int)((dd >> 16) & 255) * (255 - a);
                int g = (int)((s >> 8) & 255) * a + (int)((dd >> 8) & 255) * (255 - a);
                int b = (int)(s & 255) * a + (int)(dd & 255) * (255 - a);
                *d = ((uint32_t)(r / 255) << 16) | ((uint32_t)(g / 255) << 8) | (uint32_t)(b / 255);
            }
        }
    }
}

/* decode and box-filter down to fit maxw x maxh (never enlarged) */
static uint32_t *decode_scaled(const char *path, int maxw, int maxh, int *ow, int *oh) {
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.size == 0 || st.size > 6u * 1024 * 1024) return NULL;
    uint8_t *buf = (uint8_t *)malloc((size_t)st.size);
    if (!buf) return NULL;
    if (cos_read_file(path, buf, (size_t)st.size) != (ssize_t)st.size) { free(buf); return NULL; }
    int w = 0, h = 0, comp = 0;
    uint8_t *img = stbi_load_from_memory(buf, (int)st.size, &w, &h, &comp, 4);
    free(buf);
    if (!img || w <= 0 || h <= 0 || (long)w * h > 24L * 1000 * 1000) { if (img) stbi_image_free(img); return NULL; }
    int dw = w, dh = h;
    if (w > maxw || h > maxh) {
        long sx = (long)maxw * 1000 / w, sy = (long)maxh * 1000 / h, s = sx < sy ? sx : sy;
        dw = (int)((long)w * s / 1000); dh = (int)((long)h * s / 1000);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    uint32_t *out = (uint32_t *)malloc((size_t)dw * (size_t)dh * 4);
    if (!out) { stbi_image_free(img); return NULL; }
    for (int y = 0; y < dh; ++y) {
        int y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh); if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw); if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) {
                const uint8_t *p = &img[((size_t)yy * (size_t)w + (size_t)xx) * 4];
                r += p[0]; g += p[1]; b += p[2]; a += p[3]; ++cnt;
            }
            out[y * dw + x] = ((a / cnt) << 24) | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
        }
    }
    stbi_image_free(img);
    *ow = dw; *oh = dh;
    return out;
}

Thumb *thumb_lookup(const Ent *e) {
    char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
    for (int i = 0; i < MAX_THUMB; ++i) {
        Thumb *t = &G.thumbs[i];
        if (t->state && t->mtime == e->mtime && !strcmp(t->path, p)) { t->used = ++G.thumb_clock; return t; }
    }
    return NULL;
}
void thumb_load_pending(void) {
    if (G.dlg == D_PROGRESS || G.mdown) return;                          /* never decode while the user is dragging or copying */
    if (G.view != V_THUMBS && !G.preview) return;
    R c = content_rect();
    for (int vi = 0; vi < G.nvis; ++vi) {
        R r;
        if (!item_rect(vi, &r) || r.y + r.h < c.y || r.y > c.y + c.h) continue;
        Ent *e = ent_at(vi);
        if (e->kind != K_IMAGE || G.view != V_THUMBS || thumb_lookup(e)) continue;
        char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
        Thumb *slot = NULL;                                             /* a free slot, else the least recently used */
        for (int i = 0; i < MAX_THUMB; ++i) if (!G.thumbs[i].state) { slot = &G.thumbs[i]; break; }
        if (!slot) { slot = &G.thumbs[0]; for (int i = 1; i < MAX_THUMB; ++i) if (G.thumbs[i].used < slot->used) slot = &G.thumbs[i]; }
        free(slot->px); slot->px = NULL;
        snprintf(slot->path, sizeof slot->path, "%s", p); slot->mtime = e->mtime; slot->used = ++G.thumb_clock;
        int w = 0, h = 0;
        slot->px = decode_scaled(p, 132, 96, &w, &h);
        slot->w = w; slot->h = h; slot->state = slot->px ? 1 : 2;
        G.dirty = true;
        return;                                                          /* one per tick keeps the window responsive */
    }
}
Thumb *preview_image(const Ent *e) {
    char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
    if (G.pv.state && G.pv.mtime == e->mtime && !strcmp(G.pv.path, p)) return G.pv.state == 1 ? &G.pv : NULL;
    free(G.pv.px); G.pv.px = NULL;
    snprintf(G.pv.path, sizeof G.pv.path, "%s", p); G.pv.mtime = e->mtime;
    int w = 0, h = 0;
    G.pv.px = decode_scaled(p, PREV_W - 32, 200, &w, &h);
    G.pv.w = w; G.pv.h = h; G.pv.state = G.pv.px ? 1 : 2;
    return G.pv.state == 1 ? &G.pv : NULL;
}
const char *preview_text(const Ent *e) {
    char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
    if (!strcmp(G.pv_text_path, p)) return G.pv_text;
    snprintf(G.pv_text_path, sizeof G.pv_text_path, "%s", p);
    G.pv_text[0] = 0;
    int fd = cos_open(p, COS_O_RDONLY);
    if (fd < 0) return G.pv_text;
    ssize_t n = cos_fd_read(fd, G.pv_text, sizeof G.pv_text - 1);
    cos_close(fd);
    if (n < 0) n = 0;
    G.pv_text[n] = 0;
    for (ssize_t i = 0; i < n; ++i) if ((unsigned char)G.pv_text[i] < 9 && G.pv_text[i] != '\n') { G.pv_text[0] = 0; break; }   /* binary */
    return G.pv_text;
}

/* ---------------------------------------------------------------- */
void app_init_state(void) {
    memset(&G, 0, sizeof G);
    G.all = (Ent *)calloc(MAX_ENT, sizeof(Ent));
    G.vis = (int *)calloc(MAX_ENT, sizeof(int));
    G.view = V_DETAILS; G.sort = S_NAME; G.sort_asc = 1; G.preview = false; G.tree_vis = true;
    G.hover = -1; G.focus = G.anchor = -1; G.menu_open = -1; G.menu_hover = -1; G.tip_btn = -1;
    G.click_vi = -1;
    snprintf(G.path, sizeof G.path, "/");
    uint32_t pf = cos_ui_prefs();
    G.jp = (pf & COS_UI_JAPANESE) != 0; G.dark = (pf & COS_UI_DARK) != 0;
    apply_theme();
    marks_default();
    config_load();
}

/* Notice changes made by other programs (Studio writing a build, a download finishing): every
 * 2.5 s compare a cheap signature of the folder with the one we loaded, reload only if it moved. */
static uint64_t sig_of(int n, const cos_dirent_ex_t *r) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) {
        for (const char *p = r[i].name; *p; ++p) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
        h ^= r[i].size; h *= 1099511628211ull; h ^= r[i].mtime; h *= 1099511628211ull;
    }
    return h ^ (uint64_t)n;
}
void dir_poll(void) {
    static uint64_t last, sig; static char sig_path[PATHN];
    uint64_t t = cos_time_ms();
    if (t - last < 2500 || G.dlg || G.mdown || G.job.mode) return;
    last = t;
    static cos_dirent_ex_t *raw;
    if (!raw) raw = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * MAX_ENT);
    if (!raw) return;
    int n = cos_listdir_ex(G.path, raw, MAX_ENT);
    if (n < 0) return;
    uint64_t s = sig_of(n, raw);
    if (strcmp(sig_path, G.path)) { snprintf(sig_path, sizeof sig_path, "%s", G.path); sig = s; return; }   /* first look at this folder */
    if (s != sig) { sig = s; dir_load(true); tree_rebuild(); }
}
