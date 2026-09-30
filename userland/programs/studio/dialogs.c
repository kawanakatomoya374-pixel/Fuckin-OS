/* dialogs.c - modal dialogs (Go to Line/File/Symbol, Open, Save As, New
 * Project, Settings, unsaved-changes prompt, About, Shortcuts, text input) and
 * the Explorer context menu.
 *
 * Drawing records a clickable region for every control (H()); dlg_mouse() then
 * just finds the region under the pointer. That keeps what is drawn and what
 * is clickable from ever drifting apart. */
#include "studio.h"

enum { ACT_CLOSE_TAB = 1, ACT_EXIT, ACT_DELETE, ACT_NEW_FILE, ACT_NEW_FOLDER, ACT_RENAME, ACT_OPEN_FOLDER_FIRST };
enum { ID_OK = 1, ID_CANCEL, ID_DISCARD, ID_FIELD0 = 10, ID_FIELD1, ID_LIST = 100, ID_RADIO = 300, ID_OPT = 400, ID_UP = 500, ID_CLOSE = 501 };

typedef struct { int x, y, w, h, id; } Hit;
static Hit s_hit[160]; static int s_nhit;
static void H(int x, int y, int w, int h, int id) { if (s_nhit < 160) s_hit[s_nhit++] = (Hit){ x, y, w, h, id }; }
static int hit_at(int x, int y) { for (int i = s_nhit - 1; i >= 0; --i) if (pt_in(x, y, s_hit[i].x, s_hit[i].y, s_hit[i].w, s_hit[i].h)) return s_hit[i].id; return -1; }
static bool hov(int x, int y, int w, int h) { return pt_in(G.mx, G.my, x, y, w, h); }

/* ---------------------------------------------------------------- */
/* file lists                                                        */
/* ---------------------------------------------------------------- */
#define MAX_FILES 400
static char (*s_files)[256]; static int s_nfiles;
static int s_match[MAX_FILES]; static int s_nmatch;
static Sym s_dsyms[128]; static int s_ndsyms;

static void collect(const char *dir, int depth) {
    if (depth > 5 || s_nfiles >= MAX_FILES) return;
    int fd = cos_opendir(dir);
    if (fd < 0) return;
    cos_dirent_t de;
    char (*nm)[64] = (char (*)[64])malloc(64 * 64);
    bool *dd = (bool *)malloc(64);
    int n = 0;
    while (nm && dd && n < 64 && cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.') continue;
        snprintf(nm[n], 64, "%s", de.name); dd[n] = de.is_dir != 0; ++n;
    }
    cos_close(fd);
    for (int i = 0; i < n && s_nfiles < MAX_FILES; ++i) {
        char p[300]; path_join(p, sizeof p, dir, nm[i]);
        if (dd[i]) collect(p, depth + 1); else snprintf(s_files[s_nfiles++], 256, "%s", p);
    }
    free(nm); free(dd);
}

/* subsequence match, scored: earlier and tighter is better; -1 = no match */
static int fuzzy(const char *pat, const char *s) {
    if (!pat[0]) return 0;
    int score = 0, last = -2, si = 0;
    for (const char *p = pat; *p; ++p) {
        char pc = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
        bool found = false;
        for (; s[si]; ++si) {
            char sc = (s[si] >= 'A' && s[si] <= 'Z') ? s[si] + 32 : s[si];
            if (sc == pc) { score += (si == last + 1) ? 0 : 3 + si / 8; last = si; ++si; found = true; break; }
        }
        if (!found) return -1;
    }
    return score;
}
static void refilter(void) {
    s_nmatch = 0;
    int scores[MAX_FILES];
    int total = G.dlg_arg == 1 ? s_ndsyms : s_nfiles;
    for (int i = 0; i < total; ++i) {
        const char *name = G.dlg_arg == 1 ? s_dsyms[i].name : path_base(s_files[i]);
        int sc = fuzzy(G.dlg_text, name);
        if (sc < 0) continue;
        int k = s_nmatch++;
        while (k > 0 && scores[k - 1] > sc) { scores[k] = scores[k - 1]; s_match[k] = s_match[k - 1]; --k; }
        scores[k] = sc; s_match[k] = i;
    }
    G.dlg_sel = 0; G.dlg_scroll = 0;
}

/* ---- directory browser (Open / Save As) ---- */
#define MAX_ENT 200
static struct { char name[64]; bool dir; } *s_ent; static int s_nent;
static int ent_cmp(const void *a, const void *b) {
    const typeof(s_ent[0]) *x = (const typeof(s_ent[0]) *)a, *y = (const typeof(s_ent[0]) *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void browse(const char *dir) {
    if (!s_ent) s_ent = calloc(MAX_ENT, sizeof *s_ent);
    snprintf(G.dlg_dir, sizeof G.dlg_dir, "%s", dir);
    size_t n = strlen(G.dlg_dir);
    while (n > 1 && G.dlg_dir[n - 1] == '/') G.dlg_dir[--n] = 0;
    s_nent = 0;
    int fd = cos_opendir(G.dlg_dir);
    if (fd >= 0) {
        cos_dirent_t de;
        while (s_nent < MAX_ENT && cos_readdir(fd, &de) == 1) {
            if (de.name[0] == '.') continue;
            snprintf(s_ent[s_nent].name, 64, "%s", de.name); s_ent[s_nent].dir = de.is_dir != 0; ++s_nent;
        }
        cos_close(fd);
    }
    qsort(s_ent, (size_t)s_nent, sizeof *s_ent, ent_cmp);
    G.dlg_sel = -1; G.dlg_scroll = 0;
}

/* ---------------------------------------------------------------- */
/* open / close                                                      */
/* ---------------------------------------------------------------- */
void dlg_close(void) { G.dlg = DLG_NONE; G.dirty_frame = true; G.caret_t = cos_time_ms(); }

void dlg_open(int kind) {
    G.dlg = kind; G.dlg_sel = 0; G.dlg_scroll = 0; G.dlg_field = 0; G.dlg_text[0] = 0; G.dlg_text2[0] = 0;
    G.caret_t = cos_time_ms(); G.open_menu = -1; G.ctx_open = false; G.comp_open = false; G.tip_visible = false;
    Doc *d = app_doc();
    switch (kind) {
        case DLG_GOTO_LINE: snprintf(G.dlg_title, sizeof G.dlg_title, "Go to Line"); break;
        case DLG_GOTO_FILE:
            if (!s_files) s_files = malloc((size_t)MAX_FILES * 256);
            s_nfiles = 0;
            if (G.dlg_arg == 1) {
                s_ndsyms = d ? outline_scan(d, s_dsyms, 128) : 0;
                snprintf(G.dlg_title, sizeof G.dlg_title, "Go to Symbol");
            } else {
                if (G.proj.loaded && s_files) collect(G.proj.root, 0);
                for (int i = 0; i < G.ndocs && s_files; ++i) {
                    bool have = false;
                    for (int k = 0; k < s_nfiles; ++k) if (!strcmp(s_files[k], G.docs[i]->path)) have = true;
                    if (!have && G.docs[i]->path[0] && s_nfiles < MAX_FILES) snprintf(s_files[s_nfiles++], 256, "%s", G.docs[i]->path);
                }
                snprintf(G.dlg_title, sizeof G.dlg_title, "Go to File");
            }
            refilter();
            break;
        case DLG_OPEN: {
            snprintf(G.dlg_title, sizeof G.dlg_title, G.dlg_arg == 1 ? "Open Folder" : "Open File");
            const char *start = G.proj.loaded ? G.proj.root : (G.dlg_dir[0] ? G.dlg_dir : "/");
            browse(start);
            break; }
        case DLG_SAVE_AS: {
            snprintf(G.dlg_title, sizeof G.dlg_title, "Save As");
            char dir[256];
            /* a file that already has a home is saved back there; a NEW file goes to deliverables/ */
            if (d && d->path[0] && !d->untitled) path_dir(d->path, dir, sizeof dir);
            else { studio_ensure_dirs(); snprintf(dir, sizeof dir, "%s", studio_deliverables()); }
            browse(dir);
            snprintf(G.dlg_text, sizeof G.dlg_text, "%s", d ? d->name : "untitled.c");
            break; }
        case DLG_NEW_PROJECT:
            snprintf(G.dlg_title, sizeof G.dlg_title, "New Project");
            snprintf(G.dlg_text, sizeof G.dlg_text, "my_app");
            studio_ensure_dirs();
            snprintf(G.dlg_text2, sizeof G.dlg_text2, "%s", studio_deliverables());
            G.dlg_templ = 1;
            break;
        case DLG_SETTINGS: snprintf(G.dlg_title, sizeof G.dlg_title, "Settings"); break;
        case DLG_ABOUT: snprintf(G.dlg_title, sizeof G.dlg_title, "About C-OS Studio"); break;
        case DLG_SHORTCUTS: snprintf(G.dlg_title, sizeof G.dlg_title, "Keyboard Shortcuts"); break;
        default: break;
    }
    G.dirty_frame = true;
}

void dlg_confirm(const char *title, const char *msg, int action, int arg) {
    dlg_open(DLG_CONFIRM);
    snprintf(G.dlg_title, sizeof G.dlg_title, "%s", title);
    snprintf(G.dlg_msg, sizeof G.dlg_msg, "%s", msg);
    G.dlg_action = action; G.dlg_arg = arg;
}
static void dlg_input(const char *title, const char *initial, int action, const char *target) {
    dlg_open(DLG_INPUT);
    snprintf(G.dlg_title, sizeof G.dlg_title, "%s", title);
    snprintf(G.dlg_text, sizeof G.dlg_text, "%s", initial);
    snprintf(G.dlg_dir, sizeof G.dlg_dir, "%s", target);
    G.dlg_action = action;
}

/* ---------------------------------------------------------------- */
/* actions                                                           */
/* ---------------------------------------------------------------- */
static void act_open_selected_file(void) {
    if (G.dlg_arg == 1) {
        if (G.dlg_sel >= 0 && G.dlg_sel < s_nmatch) { Doc *d = app_doc(); if (d) { doc_goto_line(d, s_dsyms[s_match[G.dlg_sel]].line, 1); ed_center_caret(d); } dlg_close(); }
        return;
    }
    if (G.dlg_sel >= 0 && G.dlg_sel < s_nmatch) { char p[256]; snprintf(p, sizeof p, "%s", s_files[s_match[G.dlg_sel]]); dlg_close(); app_open_file(p, 0); }
}

static void act_ok(void) {
    switch (G.dlg) {
        case DLG_GOTO_LINE: {
            Doc *d = app_doc();
            int line = atoi(G.dlg_text), col = 1;
            const char *c = strchr(G.dlg_text, ':');
            if (c) col = atoi(c + 1);
            dlg_close();
            if (d && line > 0) { doc_goto_line(d, line, col); ed_center_caret(d); }
            break; }
        case DLG_GOTO_FILE: act_open_selected_file(); break;
        case DLG_OPEN:
            if (G.dlg_arg == 1) { char p[256]; snprintf(p, sizeof p, "%s", G.dlg_dir); dlg_close(); app_open_path(p); }
            else if (G.dlg_text[0]) { char p[300]; path_join(p, sizeof p, G.dlg_dir, G.dlg_text); dlg_close(); app_open_path(p); }
            break;
        case DLG_SAVE_AS:
            if (G.dlg_text[0]) {
                char p[300]; path_join(p, sizeof p, G.dlg_dir, G.dlg_text);
                Doc *d = app_doc();
                dlg_close();
                if (d) { if (doc_save(d, p)) { app_status("Saved %s", d->name); if (G.proj.loaded) project_rescan(&G.proj); } else app_status("Could not save %s", p); }
            }
            break;
        case DLG_NEW_PROJECT: {
            char mainp[300];
            if (project_create(G.dlg_text2, G.dlg_text, G.dlg_templ, mainp, sizeof mainp)) {
                char root[300]; path_join(root, sizeof root, G.dlg_text2, G.dlg_text);
                dlg_close();
                app_open_path(root);
                app_open_file(mainp, 0);
            } else app_status("Could not create the project (does the folder already exist?)");
            break; }
        case DLG_INPUT: {
            char p[300];
            if (!G.dlg_text[0] || strchr(G.dlg_text, '/')) { app_status("Invalid name"); break; }
            if (G.dlg_action == ACT_NEW_FILE) {
                path_join(p, sizeof p, G.dlg_dir, G.dlg_text);
                dlg_close();
                if (fs_exists(p)) app_status("%s already exists", G.dlg_text);
                else if (fs_write_all(p, "", 0)) { if (G.proj.loaded) project_rescan(&G.proj); app_open_file(p, 0); }
                else app_status("Could not create %s", p);
            } else if (G.dlg_action == ACT_NEW_FOLDER) {
                path_join(p, sizeof p, G.dlg_dir, G.dlg_text);
                dlg_close();
                if (cos_mkdir(p) == 0) { if (G.proj.loaded) project_rescan(&G.proj); } else app_status("Could not create %s", p);
            } else if (G.dlg_action == ACT_RENAME) {
                char dir[256]; path_dir(G.dlg_dir, dir, sizeof dir);
                path_join(p, sizeof p, dir, G.dlg_text);
                char old[256]; snprintf(old, sizeof old, "%s", G.dlg_dir);
                dlg_close();
                if (cos_rename(old, p) == 0) {
                    for (int i = 0; i < G.ndocs; ++i) if (!strcmp(G.docs[i]->path, old)) { snprintf(G.docs[i]->path, 256, "%s", p); snprintf(G.docs[i]->name, 64, "%s", G.dlg_text); }
                    if (G.proj.loaded) project_rescan(&G.proj);
                } else app_status("Could not rename");
            }
            break; }
        case DLG_SETTINGS: app_settings_save(); dlg_close(); break;
        case DLG_ABOUT: case DLG_SHORTCUTS: dlg_close(); break;
        default: dlg_close(); break;
    }
}

static void act_confirm(int which) {          /* 1 = primary (Save / Delete), 3 = Don't Save, 2 = Cancel */
    int action = G.dlg_action, arg = G.dlg_arg;
    if (which == ID_CANCEL) { dlg_close(); return; }
    if (action == ACT_CLOSE_TAB) {
        Doc *d = arg >= 0 && arg < G.ndocs ? G.docs[arg] : NULL;
        if (d && which == ID_OK) {
            if (d->untitled) { dlg_close(); G.cur = arg; dlg_open(DLG_SAVE_AS); return; }
            doc_save(d, NULL);
        }
        dlg_close();
        app_close_tab(arg, true);
    } else if (action == ACT_EXIT) {
        if (which == ID_OK) for (int i = 0; i < G.ndocs; ++i) if (G.docs[i]->dirty && !G.docs[i]->untitled) doc_save(G.docs[i], NULL);
        dlg_close();
        G.quit = true;
    } else if (action == ACT_DELETE) {
        char p[256]; snprintf(p, sizeof p, "%s", G.dlg_dir);
        dlg_close();
        if (cos_unlink(p) == 0) {
            for (int i = 0; i < G.ndocs; ++i) if (!strcmp(G.docs[i]->path, p)) { app_close_tab(i, true); break; }
            if (G.proj.loaded) project_rescan(&G.proj);
        } else app_status("Could not delete %s", path_base(p));
    } else dlg_close();
}

/* ---------------------------------------------------------------- */
/* drawing                                                           */
/* ---------------------------------------------------------------- */
typedef struct { int x, y, w, h; } Box;

static Box dlg_frame(int w, int h, const char *title) {
    cui_canvas *c = &G.cv;
    cui_unclip(c);
    cui_fill_alpha(c, 0, 0, c->w, c->h, 0x000000, 120);
    Box b = { (c->w - w) / 2, (c->h - h) / 2 - 16, w, h };
    cui_round_rect_alpha(c, b.x - 2, b.y + 4, b.w + 4, b.h + 4, 12, 0x000000, 80);
    cui_round_rect(c, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 10, T->border);
    cui_round_rect(c, b.x, b.y, b.w, b.h, 9, T->bar);
    cui_fill(c, b.x + 1, b.y + 38, b.w - 2, 1, T->border);
    cui_text(c, font_ui_bold(13), b.x + 16, b.y + 11, title, T->text);
    int cx = b.x + b.w - 26, cy = b.y + 19;
    uint32_t xc = hov(cx - 10, cy - 12, 24, 24) ? T->text : T->dim;
    cui_line(c, (float)cx - 4, (float)cy - 4, (float)cx + 4, (float)cy + 4, 1.4f, xc);
    cui_line(c, (float)cx + 4, (float)cy - 4, (float)cx - 4, (float)cy + 4, 1.4f, xc);
    H(cx - 10, cy - 12, 24, 24, ID_CLOSE);
    return b;
}
static void btn(int x, int y, int w, const char *label, bool primary, int id) {
    ui_button(x, y, w, 28, label, primary, hov(x, y, w, 28));
    H(x, y, w, 28, id);
}
static void check(int x, int y, const char *label, bool on, int id) {
    cui_canvas *c = &G.cv;
    cui_round_rect(c, x, y, 16, 16, 4, on ? T->accent : T->field);
    if (!on) cui_round_rect_outline(c, x, y, 16, 16, 4, 1.0f, T->border);
    else { cui_line(c, (float)x + 4, (float)y + 8.5f, (float)x + 7, (float)y + 11.5f, 1.8f, 0xFFFFFF); cui_line(c, (float)x + 7, (float)y + 11.5f, (float)x + 12, (float)y + 4.5f, 1.8f, 0xFFFFFF); }
    cui_text(c, font_ui(12), x + 26, y + 1, label, T->text);
    H(x, y - 2, 200, 20, id);
}
static void radio(int x, int y, const char *label, const char *sub, bool on, int id) {
    cui_canvas *c = &G.cv;
    cui_ring(c, (float)x + 8, (float)y + 8, 7.5f, 1.2f, on ? T->accent : T->border);
    if (on) cui_circle(c, (float)x + 8, (float)y + 8, 4.0f, T->accent);
    cui_text(c, font_ui(12), x + 26, y, label, T->text);
    cui_text(c, font_ui(11), x + 26, y + 15, sub, T->dim);
    H(x, y - 3, 380, 34, id);
}

static void draw_list_rows(Box b, int y0, int rows, int total, int (*row)(int idx, char *l1, char *l2, size_t cap)) {
    cui_canvas *c = &G.cv;
    cui_font *f = font_ui(12), *fs = font_ui(11);
    if (G.dlg_sel >= 0 && G.dlg_sel < G.dlg_scroll) G.dlg_scroll = G.dlg_sel;
    if (G.dlg_sel >= G.dlg_scroll + rows) G.dlg_scroll = G.dlg_sel - rows + 1;
    if (G.dlg_scroll > total - rows) G.dlg_scroll = total - rows;
    if (G.dlg_scroll < 0) G.dlg_scroll = 0;
    for (int i = 0; i < rows && G.dlg_scroll + i < total; ++i) {
        int idx = G.dlg_scroll + i, y = y0 + i * 26;
        char l1[128], l2[200];
        int kind = row(idx, l1, l2, sizeof l1);
        bool sel = idx == G.dlg_sel, hv = hov(b.x + 10, y, b.w - 20, 26);
        if (sel) cui_round_rect(c, b.x + 10, y, b.w - 20, 26, 4, T->row_sel);
        else if (hv) cui_round_rect(c, b.x + 10, y, b.w - 20, 26, 4, T->bar2);
        uint32_t col = kind == 1 ? T->folder : T->text;
        cui_text(c, f, b.x + 22, y + 6, l1, col);
        if (l2[0]) { int w1 = cui_text_width(f, l1); cui_text_fit(c, fs, b.x + 32 + w1, y + 8, b.w - 60 - w1, l2, T->dim); }
        H(b.x + 10, y, b.w - 20, 26, ID_LIST + i);
    }
}
static int row_match(int i, char *l1, char *l2, size_t cap) {
    (void)cap;
    if (G.dlg_arg == 1) { snprintf(l1, 128, "%s", s_dsyms[s_match[i]].name); snprintf(l2, 200, "%s   line %d", s_dsyms[s_match[i]].kind, s_dsyms[s_match[i]].line); return 0; }
    const char *p = s_files[s_match[i]];
    snprintf(l1, 128, "%s", path_base(p));
    const char *rel = p;
    if (G.proj.loaded && !strncmp(p, G.proj.root, strlen(G.proj.root))) { rel = p + strlen(G.proj.root); if (*rel == '/') ++rel; }
    snprintf(l2, 200, "%s", rel);
    return 0;
}
static int row_ent(int i, char *l1, char *l2, size_t cap) {
    (void)cap; l2[0] = 0;
    if (i == 0) { snprintf(l1, 128, ".."); return 1; }
    snprintf(l1, 128, "%s%s", s_ent[i - 1].name, s_ent[i - 1].dir ? "/" : "");
    return s_ent[i - 1].dir ? 1 : 0;
}

static void draw_goto_line(void) {
    Box b = dlg_frame(360, 138, G.dlg_title);
    Doc *d = app_doc();
    char hint[64]; snprintf(hint, sizeof hint, "Line number (1 - %zu), or line:column", d ? buf_lines(&d->b) : 0);
    ui_field(b.x + 16, b.y + 52, b.w - 32, 28, G.dlg_text, true, hint);
    btn(b.x + b.w - 16 - 70, b.y + 94, 70, "Go", true, ID_OK);
    btn(b.x + b.w - 16 - 70 - 8 - 76, b.y + 94, 76, "Cancel", false, ID_CANCEL);
}
static void draw_goto_file(void) {
    int rows = 8;
    Box b = dlg_frame(500, 96 + rows * 26 + 14, G.dlg_title);
    ui_field(b.x + 16, b.y + 50, b.w - 32, 28, G.dlg_text, true, G.dlg_arg == 1 ? "Type a symbol name" : "Type a file name");
    if (!s_nmatch) cui_text_center(&G.cv, font_ui(12), b.x, b.y + 90, b.w, 60, "No matches", T->dim);
    draw_list_rows(b, b.y + 90, rows, s_nmatch, row_match);
}
static void draw_browser(void) {
    int rows = 9;
    bool save = G.dlg == DLG_SAVE_AS, dirmode = G.dlg_arg == 1 && G.dlg == DLG_OPEN;
    Box b = dlg_frame(560, 138 + rows * 26 + 58, G.dlg_title);
    cui_canvas *c = &G.cv;
    cui_round_rect(c, b.x + 16, b.y + 50, b.w - 32, 26, 4, T->field);
    cui_text_fit(c, font_ui(12), b.x + 26, b.y + 56, b.w - 52, G.dlg_dir, T->dim);
    int total = s_nent + 1;
    draw_list_rows(b, b.y + 88, rows, total, row_ent);
    int fy = b.y + 88 + rows * 26 + 12;
    if (!dirmode) ui_field(b.x + 16, fy, b.w - 32, 28, G.dlg_text, true, save ? "File name" : "File name (or pick one above)");
    btn(b.x + b.w - 16 - 84, fy + 38, 84, save ? "Save" : (dirmode ? "Open Folder" : "Open"), true, ID_OK);
    btn(b.x + b.w - 16 - 84 - 8 - 76, fy + 38, 76, "Cancel", false, ID_CANCEL);
    if (!dirmode) return;
}
static void draw_new_project(void) {
    static const char *const T_NAME[] = { "Console app", "GUI window", "Audio tone", "Empty" };
    static const char *const T_SUB[] = { "printf hello world", "cos_win2 window with a text label", "plays a 660 Hz tone", "just int main(void)" };
    Box b = dlg_frame(460, 380, G.dlg_title);
    cui_canvas *c = &G.cv;
    cui_text(c, font_ui(11), b.x + 16, b.y + 52, "Name", T->dim);
    ui_field(b.x + 16, b.y + 68, b.w - 32, 28, G.dlg_text, G.dlg_field == 0, "project name");
    H(b.x + 16, b.y + 68, b.w - 32, 28, ID_FIELD0);
    cui_text(c, font_ui(11), b.x + 16, b.y + 106, "Location", T->dim);
    ui_field(b.x + 16, b.y + 122, b.w - 32, 28, G.dlg_text2, G.dlg_field == 1, "parent folder");
    H(b.x + 16, b.y + 122, b.w - 32, 28, ID_FIELD1);
    cui_text(c, font_ui(11), b.x + 16, b.y + 162, "Template", T->dim);
    for (int i = 0; i < 4; ++i) radio(b.x + 20, b.y + 184 + i * 34, T_NAME[i], T_SUB[i], G.dlg_templ == i, ID_RADIO + i);
    btn(b.x + b.w - 16 - 84, b.y + 336, 84, "Create", true, ID_OK);
    btn(b.x + b.w - 16 - 84 - 8 - 76, b.y + 336, 76, "Cancel", false, ID_CANCEL);
}
static void draw_settings(void) {
    Box b = dlg_frame(440, 330, G.dlg_title);
    cui_canvas *c = &G.cv;
    cui_font *f = font_ui(12);
    int y = b.y + 56;
    cui_text(c, f, b.x + 20, y + 4, "Theme", T->text);
    ui_button(b.x + 250, y, 76, 26, "Dark", G.dark, hov(b.x + 250, y, 76, 26)); H(b.x + 250, y, 76, 26, ID_OPT + 0);
    ui_button(b.x + 332, y, 76, 26, "Light", !G.dark, hov(b.x + 332, y, 76, 26)); H(b.x + 332, y, 76, 26, ID_OPT + 1);
    y += 44;
    char v[16];
    cui_text(c, f, b.x + 20, y + 4, "Editor font size", T->text);
    snprintf(v, sizeof v, "%d px", G.code_px);
    ui_button(b.x + 250, y, 30, 26, "-", false, hov(b.x + 250, y, 30, 26)); H(b.x + 250, y, 30, 26, ID_OPT + 2);
    cui_text_center(c, f, b.x + 282, y, 76, 26, v, T->text);
    ui_button(b.x + 378, y, 30, 26, "+", false, hov(b.x + 378, y, 30, 26)); H(b.x + 378, y, 30, 26, ID_OPT + 3);
    y += 44;
    cui_text(c, f, b.x + 20, y + 4, "Tab width", T->text);
    snprintf(v, sizeof v, "%d", G.tab_width);
    ui_button(b.x + 250, y, 30, 26, "-", false, hov(b.x + 250, y, 30, 26)); H(b.x + 250, y, 30, 26, ID_OPT + 4);
    cui_text_center(c, f, b.x + 282, y, 76, 26, v, T->text);
    ui_button(b.x + 378, y, 30, 26, "+", false, hov(b.x + 378, y, 30, 26)); H(b.x + 378, y, 30, 26, ID_OPT + 5);
    y += 48;
    check(b.x + 20, y, "Save all files before Run / Build", G.save_before_run, ID_OPT + 6);
    y += 30;
    check(b.x + 20, y, "Auto-save when Studio loses focus", G.autosave, ID_OPT + 7);
    btn(b.x + b.w - 16 - 80, b.y + 284, 80, "Done", true, ID_OK);
}
static void draw_confirm(void) {
    Box b = dlg_frame(420, 152, G.dlg_title);
    cui_text_fit(&G.cv, font_ui(12), b.x + 20, b.y + 56, b.w - 40, G.dlg_msg, T->text);
    cui_text(&G.cv, font_ui(11), b.x + 20, b.y + 76, G.dlg_action == ACT_DELETE ? "This cannot be undone." : "Your changes will be lost if you do not save them.", T->dim);
    int y = b.y + 108;
    if (G.dlg_action == ACT_DELETE) {
        btn(b.x + b.w - 16 - 80, y, 80, "Delete", true, ID_OK);
    } else {
        btn(b.x + b.w - 16 - 80, y, 80, "Save", true, ID_OK);
        btn(b.x + b.w - 16 - 80 - 8 - 110, y, 110, "Don't Save", false, ID_DISCARD);
    }
    btn(b.x + b.w - 16 - (G.dlg_action == ACT_DELETE ? 80 : 80 + 8 + 110) - 8 - 76, y, 76, "Cancel", false, ID_CANCEL);
}
static void draw_input(void) {
    Box b = dlg_frame(380, 140, G.dlg_title);
    ui_field(b.x + 16, b.y + 54, b.w - 32, 28, G.dlg_text, true, "name");
    btn(b.x + b.w - 16 - 70, b.y + 98, 70, "OK", true, ID_OK);
    btn(b.x + b.w - 16 - 70 - 8 - 76, b.y + 98, 76, "Cancel", false, ID_CANCEL);
}
static void draw_about(void) {
    Box b = dlg_frame(420, 262, G.dlg_title);
    cui_canvas *c = &G.cv;
    cui_round_rect(c, b.x + 20, b.y + 56, 48, 48, 12, T->accent);
    cui_text_center(c, font_ui_bold(22), b.x + 20, b.y + 56, 48, 48, "C", 0xFFFFFF);
    cui_text(c, font_ui_bold(16), b.x + 82, b.y + 58, "C-OS Studio", T->text);
    cui_text(c, font_ui(12), b.x + 82, b.y + 82, "A C IDE that runs on C-OS", T->dim);
    cui_font *f = font_ui(12);
    cui_text(c, f, b.x + 20, b.y + 122, "Version", T->dim);       cui_text(c, f, b.x + 130, b.y + 122, "1.0", T->text);
    cui_text(c, f, b.x + 20, b.y + 144, "Compiler", T->dim);      cui_text(c, f, b.x + 130, b.y + 144, "TinyCC 0.9.28rc (mob 9db1105c)", T->text);
    cui_text(c, f, b.x + 20, b.y + 166, "Editor font", T->dim);   cui_text(c, f, b.x + 130, b.y + 166, "DejaVu Sans Mono", T->text);
    cui_text(c, f, b.x + 20, b.y + 188, "Target", T->dim);        cui_text(c, f, b.x + 130, b.y + 188, "x86_64 ELF (.c-os), static, program region", T->text);
    btn(b.x + b.w - 16 - 80, b.y + 218, 80, "Close", true, ID_OK);
}
static void draw_shortcuts(void) {
    static const char *const K[][2] = {
        {"F5 / Shift+F5", "Run / Stop"}, {"F7", "Build"}, {"Ctrl+S / Ctrl+Alt+S", "Save / Save all"}, {"Ctrl+N / Ctrl+O", "New file / Open"},
        {"Ctrl+F / Ctrl+H", "Find / Replace"}, {"Ctrl+Shift+F", "Find in project"}, {"Ctrl+G", "Go to line"}, {"Ctrl+P", "Go to file"},
        {"Ctrl+Shift+O", "Go to symbol"}, {"F8 / Shift+F8", "Next / previous problem"}, {"Alt+Enter", "Apply quick fix"}, {"Ctrl+/", "Toggle comment"},
        {"Ctrl+D", "Duplicate line"}, {"Alt+Up / Alt+Down", "Move line"}, {"Ctrl+B / Ctrl+J", "Sidebar / Panel"}, {"Ctrl++ / Ctrl+-", "Zoom"},
    };
    Box b = dlg_frame(560, 360, G.dlg_title);
    cui_canvas *c = &G.cv;
    for (int i = 0; i < 16; ++i) {
        int col = i / 8, row = i % 8, x = b.x + 20 + col * 270, y = b.y + 54 + row * 34;
        cui_round_rect(c, x, y, 118, 24, 4, T->btn);
        cui_text_center(c, font_ui(11), x, y, 118, 24, K[i][0], T->text);
        cui_text(c, font_ui(12), x + 126, y + 5, K[i][1], T->dim);
    }
    btn(b.x + b.w - 16 - 80, b.y + 316, 80, "Close", true, ID_OK);
}

void dlg_draw(void) {
    s_nhit = 0;
    if (!G.dlg) return;
    switch (G.dlg) {
        case DLG_GOTO_LINE: draw_goto_line(); break;
        case DLG_GOTO_FILE: draw_goto_file(); break;
        case DLG_OPEN: case DLG_SAVE_AS: draw_browser(); break;
        case DLG_NEW_PROJECT: draw_new_project(); break;
        case DLG_SETTINGS: draw_settings(); break;
        case DLG_CONFIRM: draw_confirm(); break;
        case DLG_INPUT: draw_input(); break;
        case DLG_ABOUT: draw_about(); break;
        case DLG_SHORTCUTS: draw_shortcuts(); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- */
/* input                                                             */
/* ---------------------------------------------------------------- */
static void edit(char *buf, size_t cap, const cos_win_event_t *ev) {
    size_t n = strlen(buf);
    if (ev->special == COS_KEY_BACKSPACE) { while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) --n; if (n) buf[n - 1] = 0; }
    else if (ev->special == COS_KEY_NONE && ev->ascii >= 32 && !(ev->mods & COS_MOD_CTRL) && n + 1 < cap) { buf[n] = ev->ascii; buf[n + 1] = 0; }
}

static void enter_dir(const char *name) {
    char p[300];
    if (!strcmp(name, "..")) path_dir(G.dlg_dir, p, sizeof p); else path_join(p, sizeof p, G.dlg_dir, name);
    browse(p);
}

static void list_click(int idx, bool dbl) {
    if (G.dlg == DLG_GOTO_FILE) { G.dlg_sel = idx; act_open_selected_file(); return; }
    /* browser */
    if (idx == 0) { enter_dir(".."); return; }
    if (idx - 1 >= s_nent) return;
    if (s_ent[idx - 1].dir) { enter_dir(s_ent[idx - 1].name); return; }
    G.dlg_sel = idx;
    snprintf(G.dlg_text, sizeof G.dlg_text, "%s", s_ent[idx - 1].name);
    if (dbl && G.dlg == DLG_OPEN) act_ok();
}

bool dlg_key(const cos_win_event_t *ev) {
    if (!G.dlg) return false;
    G.dirty_frame = true;
    G.caret_t = cos_time_ms();
    if (ev->special == COS_KEY_ESC) { dlg_close(); return true; }
    bool list = G.dlg == DLG_GOTO_FILE || G.dlg == DLG_OPEN || G.dlg == DLG_SAVE_AS;
    if (list) {
        int total = G.dlg == DLG_GOTO_FILE ? s_nmatch : s_nent + 1;
        if (ev->special == COS_KEY_DOWN) { if (G.dlg_sel + 1 < total) ++G.dlg_sel; return true; }
        if (ev->special == COS_KEY_UP) { if (G.dlg_sel > 0) --G.dlg_sel; return true; }
        if (ev->special == COS_KEY_PGDN) { G.dlg_sel = G.dlg_sel + 8 < total ? G.dlg_sel + 8 : total - 1; return true; }
        if (ev->special == COS_KEY_PGUP) { G.dlg_sel = G.dlg_sel > 8 ? G.dlg_sel - 8 : 0; return true; }
    }
    if (ev->special == COS_KEY_ENTER) {
        if (G.dlg == DLG_CONFIRM) { act_confirm(ID_OK); return true; }
        if ((G.dlg == DLG_OPEN || G.dlg == DLG_SAVE_AS) && G.dlg_sel >= 0 && G.dlg_sel < s_nent + 1 && G.dlg_text[0] == 0) { list_click(G.dlg_sel, true); return true; }
        act_ok();
        return true;
    }
    switch (G.dlg) {
        case DLG_GOTO_LINE: edit(G.dlg_text, sizeof G.dlg_text, ev); break;
        case DLG_GOTO_FILE: edit(G.dlg_text, sizeof G.dlg_text, ev); refilter(); break;
        case DLG_OPEN: case DLG_SAVE_AS: edit(G.dlg_text, sizeof G.dlg_text, ev); break;
        case DLG_INPUT: edit(G.dlg_text, sizeof G.dlg_text, ev); break;
        case DLG_NEW_PROJECT:
            if (ev->special == COS_KEY_TAB) G.dlg_field ^= 1;
            else if (G.dlg_field == 0) edit(G.dlg_text, sizeof G.dlg_text, ev); else edit(G.dlg_text2, sizeof G.dlg_text2, ev);
            break;
        case DLG_CONFIRM:
            if (ev->special == COS_KEY_NONE) {
                char c = ev->ascii | 32;
                if (c == 's' || c == 'y') act_confirm(ID_OK);
                else if (c == 'd' || c == 'n') act_confirm(ID_DISCARD);
            }
            break;
        default: break;
    }
    return true;
}

bool dlg_mouse(int x, int y, uint8_t button, bool down) {
    (void)button;
    G.dirty_frame = true;
    if (!down) return true;
    int id = hit_at(x, y);
    if (id < 0) return true;
    if (id == ID_CLOSE || id == ID_CANCEL) { dlg_close(); return true; }
    if (id == ID_OK) { if (G.dlg == DLG_CONFIRM) act_confirm(ID_OK); else act_ok(); return true; }
    if (id == ID_DISCARD) { act_confirm(ID_DISCARD); return true; }
    if (id == ID_FIELD0 || id == ID_FIELD1) { G.dlg_field = id - ID_FIELD0; return true; }
    if (id >= ID_LIST && id < ID_LIST + 40) {
        uint64_t now = cos_time_ms();
        bool dbl = G.click_count > 0 && now - G.last_click_t < 400 && G.last_click_x == id;
        G.click_count = 1; G.last_click_t = now; G.last_click_x = id;
        list_click(G.dlg_scroll + (id - ID_LIST), dbl);
        return true;
    }
    if (id >= ID_RADIO && id < ID_RADIO + 4) { G.dlg_templ = id - ID_RADIO; return true; }
    if (id >= ID_OPT) {
        switch (id - ID_OPT) {
            case 0: app_apply_theme(true); break;
            case 1: app_apply_theme(false); break;
            case 2: if (G.code_px > 10) --G.code_px; break;
            case 3: if (G.code_px < 20) ++G.code_px; break;
            case 4: if (G.tab_width > 1) --G.tab_width; break;
            case 5: if (G.tab_width < 8) ++G.tab_width; break;
            case 6: G.save_before_run = !G.save_before_run; break;
            case 7: G.autosave = !G.autosave; break;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Explorer context menu                                             */
/* ---------------------------------------------------------------- */
static const struct { const char *label; int cmd; } CTX_ITEMS[] = {
    {"New File", CMD_NEW_FILE_HERE}, {"New Folder", CMD_NEW_FOLDER_HERE}, {"Rename", CMD_RENAME}, {"Delete", CMD_DELETE}
};
#define CTX_W 168
#define CTX_ITEM_H 26

void ctx_open_at(int x, int y, int node) {
    G.ctx_open = true; G.ctx_x = x; G.ctx_y = y; G.ctx_node = node; G.ctx_hover = -1; G.dirty_frame = true;
}
static bool ctx_enabled(int i) { return !(i >= 2 && G.ctx_node == 0); }

void ctx_draw(void) {
    if (!G.ctx_open) return;
    cui_canvas *c = &G.cv;
    cui_unclip(c);
    int h = 4 * CTX_ITEM_H + 8, x = G.ctx_x, y = G.ctx_y;
    if (x + CTX_W > c->w - 4) x = c->w - CTX_W - 4;
    if (y + h > c->h - 4) y = c->h - h - 4;
    G.ctx_x = x; G.ctx_y = y;
    cui_round_rect_alpha(c, x, y + 3, CTX_W, h, 8, 0x000000, 90);
    cui_round_rect(c, x - 1, y - 1, CTX_W + 2, h + 2, 7, T->border);
    cui_round_rect(c, x, y, CTX_W, h, 6, T->tip_bg);
    for (int i = 0; i < 4; ++i) {
        int iy = y + 4 + i * CTX_ITEM_H;
        bool en = ctx_enabled(i);
        if (i == G.ctx_hover && en) cui_round_rect(c, x + 4, iy, CTX_W - 8, CTX_ITEM_H, 4, T->accent);
        cui_text(c, font_ui(12), x + 16, iy + 6, CTX_ITEMS[i].label, !en ? T->faint : (i == G.ctx_hover ? T->on_accent : T->text));
    }
}

bool ctx_mouse(int x, int y, bool down) {
    int i = -1;
    if (pt_in(x, y, G.ctx_x, G.ctx_y + 4, CTX_W, 4 * CTX_ITEM_H)) i = (y - G.ctx_y - 4) / CTX_ITEM_H;
    if (!down) { if (i != G.ctx_hover) { G.ctx_hover = i; G.dirty_frame = true; } return true; }
    G.ctx_open = false; G.dirty_frame = true;
    if (i < 0 || !ctx_enabled(i)) return true;
    Node *n = &G.proj.nodes[G.ctx_node];
    char dir[256];
    if (n->is_dir) snprintf(dir, sizeof dir, "%s", n->path); else path_dir(n->path, dir, sizeof dir);
    switch (CTX_ITEMS[i].cmd) {
        case CMD_NEW_FILE_HERE: dlg_input("New File", "untitled.c", ACT_NEW_FILE, dir); break;
        case CMD_NEW_FOLDER_HERE: dlg_input("New Folder", "new_folder", ACT_NEW_FOLDER, dir); break;
        case CMD_RENAME: dlg_input("Rename", n->name, ACT_RENAME, n->path); break;
        case CMD_DELETE: { char m[160]; snprintf(m, sizeof m, "Delete \"%s\"?", n->name); dlg_confirm("Delete", m, ACT_DELETE, 0); snprintf(G.dlg_dir, sizeof G.dlg_dir, "%s", n->path); break; }
    }
    return true;
}

/* used by app_do for the toolbar/menu "new file here" commands */
void dlg_new_entry(bool folder) {
    char dir[256];
    if (G.proj.loaded) {
        int i = G.ctx_node;
        (void)i;
        snprintf(dir, sizeof dir, "%s", G.proj.root);
        Doc *d = app_doc();
        if (d && d->path[0] && !strncmp(d->path, G.proj.root, strlen(G.proj.root))) path_dir(d->path, dir, sizeof dir);
    } else snprintf(dir, sizeof dir, "/");
    dlg_input(folder ? "New Folder" : "New File", folder ? "new_folder" : "untitled.c", folder ? ACT_NEW_FOLDER : ACT_NEW_FILE, dir);
}
void dlg_confirm_exit(void) { dlg_confirm("Unsaved changes", "Some files have unsaved changes.", ACT_EXIT, 0); }
void dlg_confirm_close(int tab) {
    char m[160]; snprintf(m, sizeof m, "Save changes to %s?", G.docs[tab]->name);
    dlg_confirm("Unsaved changes", m, ACT_CLOSE_TAB, tab);
}
