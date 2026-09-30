/* input.c - events and actions for Files. */
#include "files.h"

static uint64_t now(void) { return cos_time_ms(); }
static bool inr(R r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* ---------------------------------------------------------------- */
/* text fields                                                       */
/* ---------------------------------------------------------------- */
static bool edit_text(char *buf, size_t cap, const cos_win_event_t *ev) {
    size_t n = strlen(buf);
    if (ev->special == COS_KEY_BACKSPACE) {
        if (!n) return true;
        while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) --n;
        if (n) buf[n - 1] = 0;
        return true;
    }
    if (ev->special == COS_KEY_NONE && ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & (COS_MOD_CTRL | COS_MOD_ALT))) {
        if (n + 1 < cap) { buf[n] = ev->ascii; buf[n + 1] = 0; }
        return true;
    }
    return false;
}

/* like edit_text, but a non-empty [G.dlg_sel0, G.dlg_sel1) is replaced by what's typed,
 * the way every desktop's rename field pre-selects the name (or its stem) so the first
 * keystroke overwrites it rather than appending to it. */
static void dlg_sel_clear(void) { G.dlg_sel0 = G.dlg_sel1 = (int)strlen(G.dlg_text); }
static void dlg_sel_delete(void) {
    int a = G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel0 : G.dlg_sel1, b = G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel1 : G.dlg_sel0;
    size_t n = strlen(G.dlg_text);
    if (a < 0) a = 0; if (b > (int)n) b = (int)n;
    if (a < b) memmove(G.dlg_text + a, G.dlg_text + b, n - (size_t)b + 1);
    G.dlg_sel0 = G.dlg_sel1 = a;
}
/* the caret is G.dlg_sel0 == G.dlg_sel1 when nothing is selected; every insertion or
 * deletion happens there, not at the end of the string - a stem-only selection (rename)
 * always leaves a suffix (the extension) after it, so "insert at end" would land past it. */
static int u8_prev(const char *s, int p) { if (p <= 0) return 0; --p; while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) --p; return p; }
static int u8_next(const char *s, int p) { int n = (int)strlen(s); if (p >= n) return n; ++p; while (p < n && ((unsigned char)s[p] & 0xC0) == 0x80) ++p; return p; }

static bool edit_dlg_text(const cos_win_event_t *ev) {
    bool has_sel = G.dlg_sel0 != G.dlg_sel1;
    int n = (int)strlen(G.dlg_text);
    int c = G.dlg_sel0 < 0 ? 0 : G.dlg_sel0 > n ? n : G.dlg_sel0;
    if (ev->special == COS_KEY_LEFT)  { G.dlg_sel0 = G.dlg_sel1 = has_sel ? (G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel0 : G.dlg_sel1) : u8_prev(G.dlg_text, c); return true; }
    if (ev->special == COS_KEY_RIGHT) { G.dlg_sel0 = G.dlg_sel1 = has_sel ? (G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel1 : G.dlg_sel0) : u8_next(G.dlg_text, c); return true; }
    if (ev->special == COS_KEY_HOME)  { G.dlg_sel0 = G.dlg_sel1 = 0; return true; }
    if (ev->special == COS_KEY_END)   { G.dlg_sel0 = G.dlg_sel1 = n; return true; }
    if (ev->special == COS_KEY_DELETE) {
        if (has_sel) { dlg_sel_delete(); return true; }
        int e = u8_next(G.dlg_text, c);
        if (e > c) memmove(G.dlg_text + c, G.dlg_text + e, (size_t)(n - e) + 1);
        return true;
    }
    if (ev->special == COS_KEY_BACKSPACE) {
        if (has_sel) { dlg_sel_delete(); return true; }
        if (c == 0) return true;
        int p = u8_prev(G.dlg_text, c);
        memmove(G.dlg_text + p, G.dlg_text + c, (size_t)(n - c) + 1);
        G.dlg_sel0 = G.dlg_sel1 = p;
        return true;
    }
    if (ev->special == COS_KEY_NONE && ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & (COS_MOD_CTRL | COS_MOD_ALT))) {
        if (has_sel) { dlg_sel_delete(); n = (int)strlen(G.dlg_text); c = G.dlg_sel0; }
        if (n + 1 < (int)sizeof G.dlg_text) {
            memmove(G.dlg_text + c + 1, G.dlg_text + c, (size_t)(n - c) + 1);   /* shift the tail right, NUL included */
            G.dlg_text[c] = ev->ascii;
            G.dlg_sel0 = G.dlg_sel1 = c + 1;
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* actions                                                           */
/* ---------------------------------------------------------------- */
void open_dialog(int kind) { G.dlg = kind; G.dlg_btn_hover = 0; G.dlg_sel0 = G.dlg_sel1 = 0; G.caret_t = now(); G.menu_open = -1; G.ctx_open = false; G.dirty = true; }

static int first_selected(void) { for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel) return i; return -1; }

void start_rename(int vi) {
    Ent *e = ent_at(vi);
    if (!e) return;
    G.dlg_in = IN_RENAME;
    snprintf(G.dlg_text, sizeof G.dlg_text, "%s", e->name);
    path_join(G.dlg_target, sizeof G.dlg_target, G.path, e->name);
    open_dialog(D_INPUT);
    const char *dot = e->is_dir ? NULL : strrchr(e->name, '.');                 /* select the stem, keep the extension typeable-over */
    G.dlg_sel0 = 0; G.dlg_sel1 = (dot && dot != e->name) ? (int)(dot - e->name) : (int)strlen(e->name);
}

static void show_props(void) {
    int vi = first_selected();
    char p[PATHN];
    memset(&G.props_ent, 0, sizeof G.props_ent);
    if (vi >= 0 && sel_count() == 1) { G.props_ent = *ent_at(vi); path_join(p, sizeof p, G.path, G.props_ent.name); }
    else {
        snprintf(p, sizeof p, "%s", G.path);
        cos_stat_ex_t st; if (cos_stat_ex(p, &st) == 0) { G.props_ent.mtime = st.mtime; G.props_ent.attr = st.attr; }
        snprintf(G.props_ent.name, sizeof G.props_ent.name, "%s", strcmp(p, "/") ? path_base(p) : L("Disk", "ディスク"));
        G.props_ent.is_dir = 1; G.props_ent.kind = K_FOLDER;
    }
    snprintf(G.props_path, sizeof G.props_path, "%s", p);
    G.props_files = G.props_dirs = G.props_bytes = 0;
    if (G.props_ent.is_dir) dir_stats(p, &G.props_files, &G.props_dirs, &G.props_bytes);
    open_dialog(D_PROPS);
}

static void select_named(const char *name) {
    for (int i = 0; i < G.nvis; ++i) if (!strcmp(ent_at(i)->name, name)) { sel_set_only(i); ensure_visible(i); return; }
}
static void set_view(int v) { G.view = v; G.scroll = 0; config_save(); G.dirty = true; ensure_visible(G.focus); }
static void set_sort(int col) {
    if (G.sort == col) G.sort_asc = !G.sort_asc; else { G.sort = col; G.sort_asc = 1; }
    rebuild_view(); config_save();
}
static void go(const char *p) { nav_to(p, true); }

void do_action(int a) {
    G.menu_open = -1; G.ctx_open = false; G.dirty = true;
    switch (a) {
        case A_OPEN: {
            int n = sel_count();
            if (n == 1) { open_entry(first_selected()); break; }
            for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel && !ent_at(i)->is_dir) open_entry(i);
            break; }
        case A_NEW_FOLDER: G.dlg_in = IN_NEWFOLDER; snprintf(G.dlg_text, sizeof G.dlg_text, "%s", L("New folder", "新しいフォルダー")); open_dialog(D_INPUT); G.dlg_sel0 = 0; G.dlg_sel1 = (int)strlen(G.dlg_text); break;
        case A_NEW_FILE: G.dlg_in = IN_NEWFILE; snprintf(G.dlg_text, sizeof G.dlg_text, "%s", L("New file.txt", "新しいファイル.txt")); open_dialog(D_INPUT); { const char *dot = strrchr(G.dlg_text, '.'); G.dlg_sel0 = 0; G.dlg_sel1 = dot ? (int)(dot - G.dlg_text) : (int)strlen(G.dlg_text); } break;
        case A_RENAME: { int vi = G.focus >= 0 && ent_at(G.focus) && ent_at(G.focus)->sel ? G.focus : first_selected(); if (vi >= 0 && sel_count() == 1) start_rename(vi); break; }
        case A_DELETE: if (sel_count() > 0) open_dialog(D_DELETE); break;
        case A_PROPS: show_props(); break;
        case A_CLOSE: G.quit = true; break;
        case A_CUT: clip_put(true); break;
        case A_COPY: clip_put(false); break;
        case A_PASTE: clip_paste(); break;
        case A_SELECT_ALL: if (G.nvis) sel_range(0, G.nvis - 1, false); break;
        case A_INVERT: for (int i = 0; i < G.nvis; ++i) ent_at(i)->sel ^= 1; break;
        case A_COPY_PATH: copy_path_text(); break;
        case A_VIEW_DETAILS: set_view(V_DETAILS); break;
        case A_VIEW_LIST: set_view(V_LIST); break;
        case A_VIEW_ICONS: set_view(V_ICONS); break;
        case A_VIEW_THUMBS: set_view(V_THUMBS); break;
        case A_TOGGLE_PREVIEW: G.preview = !G.preview; config_save(); ensure_visible(G.focus); break;
        case A_TOGGLE_HIDDEN: G.show_hidden = !G.show_hidden; rebuild_view(); tree_rebuild(); config_save(); break;
        case A_TOGGLE_TREE: G.tree_vis = !G.tree_vis; config_save(); ensure_visible(G.focus); break;
        case A_REFRESH: dir_load(true); tree_rebuild(); status(L("Refreshed", "更新しました")); break;
        case A_SORT_NAME: set_sort(S_NAME); break; case A_SORT_SIZE: set_sort(S_SIZE); break;
        case A_SORT_DATE: set_sort(S_DATE); break; case A_SORT_TYPE: set_sort(S_TYPE); break;
        case A_SORT_ORDER: G.sort_asc = !G.sort_asc; rebuild_view(); config_save(); break;
        case A_BACK: nav_back(); break; case A_FORWARD: nav_forward(); break; case A_UP: nav_up(); break;
        case A_ADD_MARK: marks_add(strcmp(G.path, "/") ? path_base(G.path) : "Disk", G.path); config_save(); status(L("Bookmark added", "ブックマークに追加しました")); break;
        case A_REMOVE_MARK: for (int i = 0; i < G.nmarks; ++i) if (!G.marks[i].builtin && !strcmp(G.marks[i].path, G.path)) { marks_remove(i); break; } break;
        case A_FOCUS_ADDR: G.addr_edit = true; G.search_focus = false; snprintf(G.addr, sizeof G.addr, "%s", G.path); G.caret_t = now(); break;
        case A_FOCUS_SEARCH: G.search_focus = true; G.addr_edit = false; G.caret_t = now(); break;
        case A_GO_ROOT: go("/"); break; case A_GO_STUDIO: go("/C-OS Studio"); break; case A_GO_DESKTOP: go("/desktop"); break;
        case A_GO_DOCS: go("/documents"); break; case A_GO_MUSIC: go("/music"); break; case A_GO_PICTURES: go("/pictures"); break;
        case A_GO_DOWNLOADS: go("/downloads"); break; case A_GO_BIN: go("/bin"); break;
        case A_ABOUT: open_dialog(D_ABOUT); break;
        default: break;
    }
}

/* context menu actions that refer to a place chip or a tree node, not to the current folder */
static void ctx_exec(int a) {
    int t = G.ctx_target, kind = G.ctx_kind;
    G.ctx_open = false;
    if (kind == 2 && t >= 0 && t < G.nmarks) {
        if (a == A_OPEN) go(G.marks[t].path); else if (a == A_REMOVE_MARK) marks_remove(t);
        return;
    }
    if (kind == 3 && t >= 0 && t < G.ntree) {
        char p[PATHN]; snprintf(p, sizeof p, "%s", G.tree[t].path);
        if (a == A_OPEN) go(p);
        else if (a == A_ADD_MARK) { marks_add(G.tree[t].name, p); config_save(); status(L("Bookmark added", "ブックマークに追加しました")); }
        else if (a == A_NEW_FOLDER) { go(p); do_action(A_NEW_FOLDER); }
        else if (a == A_REFRESH) { tree_rebuild(); if (!strcmp(p, G.path)) dir_load(true); }
        return;
    }
    do_action(a);
}

/* ---------------------------------------------------------------- */
/* dialogs                                                           */
/* ---------------------------------------------------------------- */
static void dlg_close(void) { G.dlg = D_NONE; G.dirty = true; }
static void dlg_accept(int id) {
    switch (G.dlg) {
        case D_INPUT:
            if (id != 1) { dlg_close(); break; }
            {
                char made[300] = "";
                bool ok = false;
                if (G.dlg_in == IN_RENAME) { ok = op_rename(G.dlg_target, G.dlg_text); snprintf(made, sizeof made, "%s", G.dlg_text); }
                else if (G.dlg_in == IN_NEWFOLDER) ok = op_mkdir(G.dlg_text, made, sizeof made);
                else ok = op_newfile(G.dlg_text, made, sizeof made);
                if (ok) { dlg_close(); dir_load(false); tree_rebuild(); select_named(made); }
            }
            break;
        case D_DELETE: dlg_close(); if (id == 1) job_start_delete(); break;
        case D_CONFLICT:
            if (id == 6) { G.dlg_all = !G.dlg_all; break; }
            if (id == 3) job_answer(1, G.dlg_all); else if (id == 4) job_answer(2, G.dlg_all); else if (id == 5) job_answer(3, G.dlg_all);
            else if (id == 2) job_cancel();
            break;
        case D_PROGRESS: if (id == 2) job_cancel(); break;
        default: dlg_close(); break;
    }
    G.dirty = true;
}
static void dlg_key(const cos_win_event_t *ev) {
    G.caret_t = now(); G.dirty = true;
    if (G.dlg == D_INPUT) {
        if (ev->special == COS_KEY_ESC) { dlg_close(); return; }
        if (ev->special == COS_KEY_ENTER) { dlg_accept(1); return; }
        if (ev->special == COS_KEY_NONE && (ev->mods & COS_MOD_CTRL) && (ev->ascii | 32) == 'a') { G.dlg_sel0 = 0; G.dlg_sel1 = (int)strlen(G.dlg_text); return; }
        edit_dlg_text(ev);
        return;
    }
    if (ev->special == COS_KEY_ESC) { if (G.dlg == D_PROGRESS || G.dlg == D_CONFLICT) job_cancel(); else dlg_close(); return; }
    if (ev->special == COS_KEY_ENTER) { if (G.dlg == D_CONFLICT) dlg_accept(3); else if (G.dlg != D_PROGRESS) dlg_accept(1); return; }
    if (G.dlg == D_CONFLICT && ev->special == COS_KEY_NONE) {
        char c = (char)(ev->ascii | 32);
        if (c == 'r') dlg_accept(3); else if (c == 's') dlg_accept(4); else if (c == 'k') dlg_accept(5); else if (c == 'a') G.dlg_all = !G.dlg_all;
    }
}

/* ---------------------------------------------------------------- */
/* keyboard                                                          */
/* ---------------------------------------------------------------- */
static void move_focus(int to, bool shift, bool ctrl) {
    if (!G.nvis) return;
    if (to < 0) to = 0;
    if (to >= G.nvis) to = G.nvis - 1;
    if (G.focus < 0) G.focus = 0;
    if (shift) { if (G.anchor < 0) G.anchor = G.focus; sel_range(G.anchor, to, false); }
    else if (ctrl) { /* focus moves, selection stays */ }
    else sel_set_only(to);
    G.focus = to;
    ensure_visible(to);
    G.dirty = true;
}

static void typeahead(char c) {
    uint64_t t = now();
    if (t - G.ta_t > 900) G.ta_n = 0;
    G.ta_t = t;
    if (G.ta_n < 60) { G.ta[G.ta_n++] = c; G.ta[G.ta_n] = 0; }
    for (int i = 0; i < G.nvis; ++i) if (!strncasecmp(ent_at(i)->name, G.ta, (size_t)G.ta_n)) { move_focus(i, false, false); return; }
}

static void key_event(const cos_win_event_t *ev) {
    bool sh = (ev->mods & COS_MOD_SHIFT) != 0, ctrl = (ev->mods & COS_MOD_CTRL) != 0, alt = (ev->mods & COS_MOD_ALT) != 0;
    G.caret_t = now(); G.dirty = true;
    if (G.dlg) { dlg_key(ev); return; }
    if (G.menu_open >= 0 || G.ctx_open) { if (ev->special == COS_KEY_ESC) { G.menu_open = -1; G.ctx_open = false; } return; }

    if (G.addr_edit) {
        if (ev->special == COS_KEY_ESC) { G.addr_edit = false; return; }
        if (ev->special == COS_KEY_ENTER) {
            char np[PATHN]; path_normalize(G.addr, np, sizeof np);
            if (is_dir_path(np)) { G.addr_edit = false; nav_to(np, true); }
            else if (exists_path(np)) { char up[PATHN]; path_parent(np, up, sizeof up); G.addr_edit = false; nav_to(up, true); select_named(path_base(np)); }
            else status(L("Path not found: %s", "パスが見つかりません: %s"), np);
            return;
        }
        if (edit_text(G.addr, sizeof G.addr, ev)) return;
    }
    if (G.search_focus) {
        if (ev->special == COS_KEY_ESC) { G.search[0] = 0; G.search_focus = false; rebuild_view(); return; }
        if (ev->special == COS_KEY_ENTER || ev->special == COS_KEY_DOWN) { G.search_focus = false; if (G.nvis) move_focus(0, false, false); return; }
        if (edit_text(G.search, sizeof G.search, ev)) { rebuild_view(); G.scroll = 0; return; }
    }

    if (alt && !ctrl) {
        if (ev->special == COS_KEY_LEFT) { do_action(A_BACK); return; }
        if (ev->special == COS_KEY_RIGHT) { do_action(A_FORWARD); return; }
        if (ev->special == COS_KEY_UP) { do_action(A_UP); return; }
        if (ev->special == COS_KEY_ENTER) { do_action(A_PROPS); return; }
    }
    if (ctrl && ev->special == COS_KEY_NONE) {
        char c = (char)(ev->ascii | 32);
        switch (c) {
            case 'a': do_action(A_SELECT_ALL); return;
            case 'c': do_action(sh ? A_COPY_PATH : A_COPY); return;
            case 'x': do_action(A_CUT); return;
            case 'v': do_action(A_PASTE); return;
            case 'i': do_action(A_INVERT); return;
            case 'l': do_action(A_FOCUS_ADDR); return;
            case 'f': do_action(A_FOCUS_SEARCH); return;
            case 'd': do_action(A_ADD_MARK); return;
            case 'p': do_action(A_TOGGLE_PREVIEW); return;
            case 'h': do_action(A_TOGGLE_HIDDEN); return;
            case 'b': do_action(A_TOGGLE_TREE); return;
            case 'n': do_action(sh ? A_NEW_FOLDER : A_NEW_FILE); return;
            case 'r': do_action(A_REFRESH); return;
            case '1': do_action(A_VIEW_DETAILS); return; case '2': do_action(A_VIEW_LIST); return;
            case '3': do_action(A_VIEW_ICONS); return; case '4': do_action(A_VIEW_THUMBS); return;
            default: break;
        }
    }
    int cols = nav_cols();
    switch (ev->special) {
        case COS_KEY_UP:   move_focus((G.focus < 0 ? 0 : G.focus) - (G.view == V_DETAILS ? 1 : cols), sh, ctrl); return;
        case COS_KEY_DOWN: move_focus(G.focus < 0 ? 0 : G.focus + (G.view == V_DETAILS ? 1 : cols), sh, ctrl); return;
        case COS_KEY_LEFT:  if (G.view == V_DETAILS) { if (G.tree_vis) { do_action(A_UP); } } else move_focus((G.focus < 0 ? 0 : G.focus) - 1, sh, ctrl); return;
        case COS_KEY_RIGHT: if (G.view == V_DETAILS) { if (G.focus >= 0 && ent_at(G.focus) && ent_at(G.focus)->is_dir) open_entry(G.focus); } else move_focus(G.focus < 0 ? 0 : G.focus + 1, sh, ctrl); return;
        case COS_KEY_HOME: move_focus(0, sh, ctrl); return;
        case COS_KEY_END:  move_focus(G.nvis - 1, sh, ctrl); return;
        case COS_KEY_PGUP: { R c = content_rect(); int page = (c.h / (G.view == V_DETAILS ? ROW_H : 100)) * (G.view == V_DETAILS ? 1 : cols); move_focus((G.focus < 0 ? 0 : G.focus) - (page ? page : 1), sh, ctrl); return; }
        case COS_KEY_PGDN: { R c = content_rect(); int page = (c.h / (G.view == V_DETAILS ? ROW_H : 100)) * (G.view == V_DETAILS ? 1 : cols); move_focus((G.focus < 0 ? 0 : G.focus) + (page ? page : 1), sh, ctrl); return; }
        case COS_KEY_ENTER: do_action(A_OPEN); return;
        case COS_KEY_BACKSPACE: do_action(A_UP); return;
        case COS_KEY_DELETE: do_action(A_DELETE); return;
        case COS_KEY_ESC: if (G.job.mode) job_cancel(); else { sel_clear(); G.search[0] = 0; rebuild_view(); } return;
        case COS_KEY_F1 + 1: do_action(A_RENAME); return;
        case COS_KEY_F1 + 4: do_action(A_REFRESH); return;
        case COS_KEY_NONE:
            if (ctrl && ev->ascii == ' ') { Ent *e = ent_at(G.focus); if (e) e->sel ^= 1; return; }
            if (ev->ascii > 32 && ev->ascii < 127 && !ctrl && !alt) typeahead(ev->ascii);
            return;
        default: return;
    }
}

/* ---------------------------------------------------------------- */
/* mouse                                                             */
/* ---------------------------------------------------------------- */
static void rubber_update(void) {
    R c = content_rect();
    int x0 = G.drag_x0 < G.mx ? G.drag_x0 : G.mx, x1 = G.drag_x0 < G.mx ? G.mx : G.drag_x0;
    int y0 = G.drag_y0 < G.my ? G.drag_y0 : G.my, y1 = G.drag_y0 < G.my ? G.my : G.drag_y0;
    for (int i = 0; i < G.nvis; ++i) {
        R r;
        item_rect(i, &r);
        R hit = r;                                              /* the band must touch the icon/name, not the empty cell margin */
        if (G.view == V_DETAILS) { hit.x = r.x + 4; hit.w = r.w - 8; }
        bool in = hit.x < x1 && hit.x + hit.w > x0 && hit.y < y1 && hit.y + hit.h > y0 && hit.y + hit.h > c.y && hit.y < c.y + c.h;
        ent_at(i)->sel = in ? 1 : 0;
    }
}

static int drop_target(char *out, size_t cap) {          /* -1 none; else fills the folder path */
    int p = place_hit(G.mx, G.my);
    if (p >= 0) { snprintf(out, cap, "%s", G.marks[p].path); return 0; }
    R t = tree_rect();
    if (inr(t, G.mx, G.my)) { int i = tree_hit(G.mx, G.my, NULL); if (i >= 0) { snprintf(out, cap, "%s", G.tree[i].path); return 0; } }
    int vi = hit_item(G.mx, G.my);
    Ent *e = ent_at(vi);
    if (e && e->is_dir && !e->sel) { path_join(out, cap, G.path, e->name); return 0; }
    return -1;
}

static void mouse_down(const cos_win_event_t *ev) {
    int x = ev->x, y = ev->y;
    bool right = ev->button == COS_MOUSE_BTN_RIGHT, sh = (ev->mods & COS_MOD_SHIFT) != 0, ctrl = (ev->mods & COS_MOD_CTRL) != 0;
    G.mx = x; G.my = y; G.mdown = true; G.mbtn = ev->button; G.dirty = true; G.caret_t = now();

    if (G.dlg) { int id = dlg_hit(x, y); if (id) dlg_accept(id); return; }
    if (G.ctx_open) {
        int idx, a = ctx_hit(x, y, &idx);
        if (a > 0) { ctx_exec(a); return; }
        G.ctx_open = false;
        if (a == 0) return;                                     /* clicked a disabled item or a separator */
    }
    if (G.menu_open >= 0) {
        int m = menubar_hit(x, y);
        if (m >= 0) { G.menu_open = G.menu_open == m ? -1 : m; return; }
        int idx, a = menu_item_hit(G.menu_open, x, y, &idx);
        if (a > 0) { do_action(a); return; }
        if (a == 0) return;
        G.menu_open = -1;
        return;
    }
    bool was_addr = G.addr_edit, was_search = G.search_focus;
    G.addr_edit = false; G.search_focus = false;
    (void)was_addr; (void)was_search;

    if (y < MENU_H) { int m = menubar_hit(x, y); if (m >= 0) { G.menu_open = m; G.menu_hover = -1; } return; }
    if (y >= TOOL_Y && y < TOOL_Y + TOOL_H) { int a = tb_hit(x, y); if (a && action_enabled(a)) do_action(a); return; }
    if (y >= PLACES_Y && y < PLACES_Y + PLACES_H) {
        int p = place_hit(x, y);
        if (p >= 0) { if (right) { G.ctx_open = true; G.ctx_kind = 2; G.ctx_target = p; G.ctx_x = x; G.ctx_y = y; G.ctx_hover = -1; } else go(G.marks[p].path); }
        return;
    }
    if (y >= ADDR_Y && y < ADDR_Y + ADDR_H) {
        if (search_hit(x, y)) {
            R s = { WIN_W - 12 - 210, ADDR_Y + 4, 210, ADDR_H - 8 };
            if (G.search[0] && x > s.x + s.w - 28) { G.search[0] = 0; rebuild_view(); }
            G.search_focus = true; return;
        }
        int c = crumb_hit(x, y);
        if (c >= 0) { char p[PATHN]; snprintf(p, sizeof p, "%s", crumb_path(c)); go(p); }
        else if (c == -2 && !right) { G.addr_edit = true; snprintf(G.addr, sizeof G.addr, "%s", G.path); }
        return;
    }
    if (y >= BODY_Y && y < WIN_H - STATUS_H) {
        R t = tree_rect();
        if (inr(t, x, y)) {
            bool arrow = false; int i = tree_hit(x, y, &arrow);
            if (i < 0) return;
            if (right) { G.ctx_open = true; G.ctx_kind = 3; G.ctx_target = i; G.ctx_x = x; G.ctx_y = y; G.ctx_hover = -1; }
            else if (arrow) tree_toggle(i);
            else { char p[PATHN]; snprintf(p, sizeof p, "%s", G.tree[i].path); go(p); }
            return;
        }
        int hc = header_hit(x, y);
        if (hc >= 0) { set_sort(hc); return; }
        R c = content_rect();
        if (!inr(c, x, y)) return;
        int ty0, th;
        if (scrollbar_hit(x, y, &ty0, &th)) {
            if (y >= ty0 && y < ty0 + th) { G.drag_kind = 3; G.scroll_drag_off = y - ty0; }
            else { G.scroll += (y < ty0 ? -c.h : c.h) + 0; int m = content_height() - c.h; if (G.scroll < 0) G.scroll = 0; if (G.scroll > m) G.scroll = m; }
            return;
        }
        int vi = hit_item(x, y);
        if (right) {
            if (vi >= 0) { if (!ent_at(vi)->sel) sel_set_only(vi); G.focus = vi; G.ctx_kind = 0; }
            else { sel_clear(); G.ctx_kind = 1; }
            G.ctx_open = true; G.ctx_x = x; G.ctx_y = y; G.ctx_hover = -1; G.ctx_target = vi;
            return;
        }
        if (vi >= 0) {
            uint64_t t0 = now();
            bool dbl = vi == G.click_vi && t0 - G.click_t < 420 && abs(x - G.click_x) < 6 && abs(y - G.click_y) < 6;
            G.click_t = t0; G.click_x = x; G.click_y = y; G.click_vi = vi;
            if (dbl) { G.click_vi = -1; open_entry(vi); return; }
            if (sh) { if (G.anchor < 0) G.anchor = G.focus < 0 ? vi : G.focus; sel_range(G.anchor, vi, false); G.focus = vi; }
            else if (ctrl) { ent_at(vi)->sel ^= 1; G.focus = G.anchor = vi; }
            else { if (!ent_at(vi)->sel) sel_set_only(vi); G.focus = G.anchor = vi; }
            if (ent_at(vi)->sel && !sh) { G.drag_pending = 1; G.drag_x0 = x; G.drag_y0 = y; }        /* becomes a file drag after a few pixels */
        } else {
            if (!ctrl && !sh) sel_clear();
            G.drag_kind = 1; G.drag_x0 = x; G.drag_y0 = y + 0;
        }
    }
}

static void mouse_move(const cos_win_event_t *ev) {
    G.mx = ev->x; G.my = ev->y;
    if (G.dlg) { G.dirty = true; return; }
    if (G.ctx_open) { int idx; ctx_hit(G.mx, G.my, &idx); if (idx != G.ctx_hover) { G.ctx_hover = idx; G.dirty = true; } return; }
    if (G.menu_open >= 0) {
        int m = menubar_hit(G.mx, G.my);
        if (m >= 0 && m != G.menu_open) { G.menu_open = m; G.dirty = true; }
        int idx; menu_item_hit(G.menu_open, G.mx, G.my, &idx);
        if (idx != G.menu_hover) { G.menu_hover = idx; G.dirty = true; }
        return;
    }
    int tip = tb_hit(G.mx, G.my);
    if (tip != G.tip_btn) { G.tip_btn = tip; G.tip_t = now(); G.dirty = true; }
    int hv = G.drag_kind == 3 ? G.hover : hit_item(G.mx, G.my);
    if (hv != G.hover) { G.hover = hv; G.dirty = true; }
    if (G.drag_pending && (abs(G.mx - G.drag_x0) > 5 || abs(G.my - G.drag_y0) > 5)) { G.drag_kind = 2; G.drag_pending = 0; }
    if (G.drag_kind == 2) { G.drag_copy = (ev->mods & COS_MOD_CTRL) != 0; G.dirty = true; }
    else if (G.drag_kind == 1) {
        R c = content_rect();
        if (G.my < c.y + 6) G.scroll -= 14; else if (G.my > c.y + c.h - 6) G.scroll += 14;
        int m = content_height() - c.h; if (m < 0) m = 0;
        if (G.scroll < 0) G.scroll = 0;
        if (G.scroll > m) G.scroll = m;
        rubber_update(); G.dirty = true;
    } else if (G.drag_kind == 3) {
        R c = content_rect(); int ty0, th; scrollbar_hit(c.x + c.w - 4, c.y + 1, &ty0, &th);
        int total = content_height(), m = total - c.h, span = c.h - th;
        if (m > 0 && span > 0) { int pos = G.my - G.scroll_drag_off - c.y; if (pos < 0) pos = 0; if (pos > span) pos = span; G.scroll = (int)((long)m * pos / span); }
        G.dirty = true;
    }
}

static void mouse_up(const cos_win_event_t *ev) {
    G.mx = ev->x; G.my = ev->y; G.mdown = false; G.dirty = true;
    if (G.drag_kind == 2) {
        char dst[PATHN];
        if (drop_target(dst, sizeof dst) == 0) {
            static char srcs[64][PATHN]; int n = 0;
            for (int i = 0; i < G.nvis && n < 64; ++i) if (ent_at(i)->sel) path_join(srcs[n++], PATHN, G.path, ent_at(i)->name);
            if (n) job_start_transfer((ev->mods & COS_MOD_CTRL) ? 1 : 2, srcs, n, dst);
        }
    }
    G.drag_kind = 0; G.drag_pending = 0;
}

static void wheel(const cos_win_event_t *ev) {
    int step = ev->wheel > 0 ? -1 : 1;
    R t = tree_rect(), c = center_rect();
    if (inr(t, G.mx, G.my)) { G.tree_scroll += step * 78; if (G.tree_scroll < 0) G.tree_scroll = 0; }
    else if (inr(c, G.mx, G.my)) {
        R cc = content_rect(); int m = content_height() - cc.h; if (m < 0) m = 0;
        G.scroll += step * (G.view == V_DETAILS ? 78 : 60);
        if (G.scroll < 0) G.scroll = 0;
        if (G.scroll > m) G.scroll = m;
        G.hover = hit_item(G.mx, G.my);
    }
    G.dirty = true;
}

void input_event(const cos_win_event_t *ev) {
    switch (ev->type) {
        case COS_EV_CLOSE: G.quit = true; break;
        case COS_EV_KEY: key_event(ev); break;
        case COS_EV_MOUSE_MOVE: mouse_move(ev); break;
        case COS_EV_MOUSE_DOWN: mouse_down(ev); break;
        case COS_EV_MOUSE_UP: mouse_up(ev); break;
        case COS_EV_WHEEL: wheel(ev); break;
        case COS_EV_OPEN: case COS_EV_DROP: {
            char p[256];
            if (cos_win2_get_path(G.win, p, sizeof p) >= 0 && p[0]) {
                if (is_dir_path(p)) nav_to(p, true);
                else if (exists_path(p)) { char up[PATHN]; path_parent(p, up, sizeof up); nav_to(up, true); select_named(path_base(p)); }
            }
            break; }
        default: break;
    }
}
