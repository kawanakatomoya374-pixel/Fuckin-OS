/* main.c - C-OS Studio entry point: application state, command dispatch,
 * event routing and the main loop.
 *
 * The loop blocks in cos_win2_wait() with a short timeout so it can still
 * poll the build/run child and blink the caret while nobody touches the
 * keyboard; it only redraws (and presents) when something changed. */
#include "studio.h"
#include <stdarg.h>

/* host-only fixture (demo.c); the device build has no such code */
__attribute__((weak)) void app_demo_setup(void) {}

void dlg_new_entry(bool folder);
void dlg_confirm_exit(void);
void dlg_confirm_close(int tab);

/* ---------------------------------------------------------------- */
/* small state helpers                                               */
/* ---------------------------------------------------------------- */
Doc *app_doc(void) { return (G.cur >= 0 && G.cur < G.ndocs) ? G.docs[G.cur] : NULL; }

void app_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(G.status_msg, sizeof G.status_msg, fmt, ap);
    va_end(ap);
    G.status_t = cos_time_ms();
    G.dirty_frame = true;
}

void app_output_clear(void) { G.out_len = 0; if (G.out) G.out[0] = 0; G.out_scroll = 0; G.out_follow = true; G.dirty_frame = true; }
void app_output_add(const char *s, size_t n) {
    if (!G.out) return;
    if (G.out_len + n + 1 > OUT_MAX) {                       /* keep the newest half */
        size_t keep = OUT_MAX / 2, from = G.out_len - keep;
        while (from < G.out_len && G.out[from] != '\n') ++from;
        memmove(G.out, G.out + from, G.out_len - from);
        G.out_len -= from;
    }
    for (size_t i = 0; i < n; ++i) G.out[G.out_len++] = s[i] ? s[i] : '?';      /* a stray NUL would hide the rest of its line */
    G.out[G.out_len] = 0;
    G.out_follow = true; G.dirty_frame = true;
}
void app_output_addf(const char *fmt, ...) {
    char b[600];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    if (n > 0) app_output_add(b, (size_t)n);
}

/* ---------------------------------------------------------------- */
/* documents                                                         */
/* ---------------------------------------------------------------- */
static void nav_push(void) {
    Doc *d = app_doc();
    if (!d || !d->path[0]) return;
    if (G.nav_i < G.nav_n) G.nav_n = G.nav_i;               /* going somewhere new drops the forward list */
    if (G.nav_n > 0 && !strcmp(G.nav[G.nav_n - 1].path, d->path)) { G.nav[G.nav_n - 1].caret = d->caret; G.nav_i = G.nav_n; return; }
    if (G.nav_n == 32) { memmove(&G.nav[0], &G.nav[1], sizeof G.nav[0] * 31); --G.nav_n; }
    snprintf(G.nav[G.nav_n].path, 256, "%s", d->path);
    G.nav[G.nav_n].caret = d->caret;
    G.nav_i = ++G.nav_n;
}

static void recent_add(const char *path) {
    int at = -1;
    for (int i = 0; i < G.nrecent; ++i) if (!strcmp(G.recent[i], path)) at = i;
    if (at < 0) { if (G.nrecent < 6) ++G.nrecent; at = G.nrecent - 1; }
    for (int i = at; i > 0; --i) memcpy(G.recent[i], G.recent[i - 1], 256);
    snprintf(G.recent[0], 256, "%s", path);
}

static int add_doc(Doc *d) {
    if (G.ndocs >= MAX_DOCS) { doc_free(d); app_status("Too many open files (%d)", MAX_DOCS); return -1; }
    G.docs[G.ndocs] = d;
    G.cur = G.ndocs++;
    G.dirty_frame = true;
    return G.cur;
}

void app_open_file(const char *path, int line) {
    if (!path || !path[0]) return;
    nav_push();
    int at = -1;
    for (int i = 0; i < G.ndocs; ++i) if (!strcmp(G.docs[i]->path, path)) at = i;
    if (at < 0) {
        Doc *d = doc_new(path);
        if (!d) return;
        if (fs_exists(path) && !doc_load(d, path)) { doc_free(d); app_status("Could not open %s", path_base(path)); return; }
        d->untitled = false;
        at = add_doc(d);
        if (at < 0) return;
        recent_add(path);
    }
    G.cur = at;
    Doc *d = G.docs[at];
    if (line > 0) { doc_goto_line(d, line, 1); ed_center_caret(d); }
    else ed_ensure_caret_visible(d);
    G.find.count = G.find_open ? find_count(d, &G.find) : 0;
    G.dirty_frame = true;
}

void app_open_path(const char *path) {
    if (!path || !path[0]) return;
    if (fs_is_dir(path)) {
        if (project_open(&G.proj, path)) {
            G.sidebar_open = true; G.view = VIEW_EXPLORER; G.side_scroll = 0;
            app_status("Opened %s", G.proj.name);
        } else app_status("Could not open %s", path);
        G.dirty_frame = true;
        return;
    }
    if (!G.proj.loaded) {
        char root[256];
        if (project_find_root(path, root, sizeof root)) project_open(&G.proj, root);
    }
    app_open_file(path, 0);
}

void app_close_tab(int i, bool force) {
    if (i < 0 || i >= G.ndocs) return;
    Doc *d = G.docs[i];
    if (!force && d->dirty) { dlg_confirm_close(i); return; }
    doc_free(d);
    for (int k = i; k + 1 < G.ndocs; ++k) G.docs[k] = G.docs[k + 1];
    --G.ndocs;
    if (G.cur > i || G.cur >= G.ndocs) --G.cur;
    if (G.cur < 0 && G.ndocs) G.cur = 0;
    G.syms_doc = -1;
    G.dirty_frame = true;
}

static void new_untitled(void) {
    Doc *d = doc_new(NULL);
    if (!d) return;
    int n = 1;
    for (int i = 0; i < G.ndocs; ++i) if (G.docs[i]->untitled) ++n;
    if (n > 1) snprintf(d->name, sizeof d->name, "untitled-%d.c", n);
    add_doc(d);
}

static void save_doc(Doc *d) {
    if (!d) return;
    if (d->untitled || !d->path[0]) { dlg_open(DLG_SAVE_AS); return; }
    if (doc_save(d, NULL)) { app_status("Saved %s", d->name); if (G.proj.loaded) project_rescan(&G.proj); }
    else app_status("Could not save %s", d->name);
}

/* ---------------------------------------------------------------- */
/* commands                                                          */
/* ---------------------------------------------------------------- */
static void insert_snippet(const char *text) {
    Doc *d = app_doc();
    if (!d) return;
    size_t l = buf_line_of(&d->b, d->caret), a = buf_line_start(&d->b, l), i = a;
    while (i < d->caret && (buf_at(&d->b, i) == ' ' || buf_at(&d->b, i) == '\t')) ++i;
    size_t ind = i - a;
    char out[1024]; size_t k = 0;
    for (const char *p = text; *p && k < sizeof out - 80; ++p) {
        out[k++] = *p;
        if (*p == '\n' && p[1]) for (size_t j = 0; j < ind; ++j) out[k++] = buf_at(&d->b, a + j);
    }
    doc_insert_at_caret(d, out, k);
    ed_ensure_caret_visible(d);
}

static void goto_problem(int dir) {
    if (!G.ndiag) { app_status("No problems"); return; }
    G.prob_sel = (G.prob_sel + dir + G.ndiag) % G.ndiag;
    app_open_file(G.diags[G.prob_sel].path, G.diags[G.prob_sel].line);
}

static void open_find(bool replace) {
    Doc *d = app_doc();
    if (!d) return;
    if (doc_has_sel(d)) {
        size_t len; char *t = doc_sel_text(d, &len);
        if (t && len < sizeof G.find.what && !strchr(t, '\n')) snprintf(G.find.what, sizeof G.find.what, "%s", t);
        free(t);
    }
    G.find_open = true; G.find_replace = replace; G.find_focus = 0; G.find_kbd = true;
    G.find.count = find_count(d, &G.find);
    G.caret_t = cos_time_ms();
}

static void any_dirty_exit(void) {
    for (int i = 0; i < G.ndocs; ++i) if (G.docs[i]->dirty) { dlg_confirm_exit(); return; }
    G.quit = true;
}

void app_do(int cmd) {
    Doc *d = app_doc();
    G.dirty_frame = true;
    if (cmd >= CMD_OPEN_RECENT && cmd < CMD_OPEN_RECENT + 6) { int i = cmd - CMD_OPEN_RECENT; if (i < G.nrecent) app_open_path(G.recent[i]); return; }
    switch (cmd) {
        case CMD_NEW: new_untitled(); break;
        case CMD_NEW_PROJECT: dlg_open(DLG_NEW_PROJECT); break;
        case CMD_OPEN: G.dlg_arg = 0; dlg_open(DLG_OPEN); break;
        case CMD_OPEN_FOLDER: G.dlg_arg = 1; dlg_open(DLG_OPEN); break;
        case CMD_SAVE: save_doc(d); break;
        case CMD_SAVE_ALL: { int n = 0; for (int i = 0; i < G.ndocs; ++i) if (G.docs[i]->dirty && !G.docs[i]->untitled && doc_save(G.docs[i], NULL)) ++n; app_status("Saved %d file%s", n, n == 1 ? "" : "s"); break; }
        case CMD_SAVE_AS: if (d) dlg_open(DLG_SAVE_AS); break;
        case CMD_CLOSE_TAB: app_close_tab(G.cur, false); break;
        case CMD_EXIT: any_dirty_exit(); break;

        case CMD_UNDO: if (d) { doc_undo(d); ed_ensure_caret_visible(d); } break;
        case CMD_REDO: if (d) { doc_redo(d); ed_ensure_caret_visible(d); } break;
        case CMD_CUT: if (d) { doc_cut_copy(d, true); ed_ensure_caret_visible(d); } break;
        case CMD_COPY: if (d) doc_cut_copy(d, false); break;
        case CMD_PASTE: if (d) { doc_paste(d); ed_ensure_caret_visible(d); } break;
        case CMD_SELECT_ALL: if (d) doc_select_all(d); break;
        case CMD_FIND: open_find(false); break;
        case CMD_REPLACE: open_find(true); break;
        case CMD_FIND_PROJECT: G.sidebar_open = true; G.view = VIEW_SEARCH; G.psearch_focus = true; G.caret_t = cos_time_ms(); break;
        case CMD_TOGGLE_COMMENT: if (d) doc_toggle_comment(d); break;
        case CMD_INDENT: if (d) doc_indent(d, false); break;
        case CMD_OUTDENT: if (d) doc_indent(d, true); break;
        case CMD_DUP_LINE: if (d) doc_duplicate_line(d); break;

        case CMD_VIEW_SIDEBAR: G.sidebar_open = !G.sidebar_open; break;
        case CMD_VIEW_PANEL: G.panel_open = !G.panel_open; break;
        case CMD_ZOOM_IN: if (G.code_px < 20) { ++G.code_px; app_settings_save(); } break;
        case CMD_ZOOM_OUT: if (G.code_px > 10) { --G.code_px; app_settings_save(); } break;
        case CMD_THEME_DARK: app_apply_theme(true); app_settings_save(); break;
        case CMD_THEME_LIGHT: app_apply_theme(false); app_settings_save(); break;

        case CMD_GOTO_LINE: if (d) dlg_open(DLG_GOTO_LINE); break;
        case CMD_GOTO_FILE: G.dlg_arg = 0; dlg_open(DLG_GOTO_FILE); break;
        case CMD_GOTO_SYMBOL: if (d) { G.dlg_arg = 1; dlg_open(DLG_GOTO_FILE); } break;
        case CMD_NEXT_PROBLEM: goto_problem(1); break;
        case CMD_PREV_PROBLEM: goto_problem(-1); break;

        case CMD_RUN: build_start(true); break;
        case CMD_BUILD: build_start(false); break;
        case CMD_STOP: build_stop(); term_stop(); break;
        case CMD_CLEAN: build_clean(); break;
        case CMD_BUILD_OBJ: build_quick(QUICK_OBJ); break;
        case CMD_BUILD_PREPROCESS: build_quick(QUICK_PREPROCESS); break;
        case CMD_BUILD_SHARED: build_quick(QUICK_SHARED); break;
        case CMD_CFG_DEBUG: snprintf(G.bld.cfg, sizeof G.bld.cfg, "Debug"); break;
        case CMD_CFG_RELEASE: snprintf(G.bld.cfg, sizeof G.bld.cfg, "Release"); break;
        case CMD_CFG_TOGGLE: snprintf(G.bld.cfg, sizeof G.bld.cfg, "%s", !strcmp(G.bld.cfg, "Debug") ? "Release" : "Debug"); break;

        case CMD_OPEN_TERMINAL: G.panel_open = true; G.panel_tab = PANEL_TERMINAL; G.term_focus = true; G.caret_t = cos_time_ms(); break;
        case CMD_SETTINGS: dlg_open(DLG_SETTINGS); break;
        case CMD_SNIPPET_MAIN: insert_snippet("int main(void) {\n    return 0;\n}\n"); break;
        case CMD_SNIPPET_WINDOW:
            insert_snippet("cos_win_info_t wi;\nint64_t h = cos_win2_create(\"Window\", 320, 200, &wi);\nif (h <= 0) return 1;\ncui_canvas c;\ncui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);\n");
            break;
        case CMD_SNIPPET_LOOP:
            insert_snippet("cos_win_event_t ev;\nwhile (cos_win2_wait(h, &ev, 16) >= 0) {\n    if (ev.type == COS_EV_CLOSE) break;\n}\n");
            break;
        case CMD_SDK_REF: {
            static const char *const cand[] = { "/system/sdk/include/cos.h", "/usr/include/cos.h", "/bin/cos.h" };
            bool ok = false;
            for (int i = 0; i < 3 && !ok; ++i) if (fs_exists(cand[i])) { app_open_file(cand[i], 0); ok = true; }
            if (!ok) app_status("The SDK headers are not installed on this system");
            break; }
        case CMD_SHORTCUTS: dlg_open(DLG_SHORTCUTS); break;
        case CMD_ABOUT: dlg_open(DLG_ABOUT); break;

        case CMD_NEXT_TAB: if (G.ndocs > 1) G.cur = (G.cur + 1) % G.ndocs; break;
        case CMD_PREV_TAB: if (G.ndocs > 1) G.cur = (G.cur + G.ndocs - 1) % G.ndocs; break;
        case CMD_NAV_BACK: if (G.nav_i > 1) { nav_push(); if (G.nav_i > 1) { --G.nav_i; int i = G.nav_i - 1; char p[256]; snprintf(p, sizeof p, "%s", G.nav[i].path); size_t cr = G.nav[i].caret; int keep = G.nav_i; app_open_file(p, 0); G.nav_i = keep; Doc *nd = app_doc(); if (nd && cr <= buf_len(&nd->b)) { nd->caret = nd->anchor = cr; ed_center_caret(nd); } } } break;
        case CMD_NAV_FWD: if (G.nav_i < G.nav_n) { int i = G.nav_i; char p[256]; snprintf(p, sizeof p, "%s", G.nav[i].path); size_t cr = G.nav[i].caret; int keep = G.nav_i + 1; app_open_file(p, 0); G.nav_i = keep; Doc *nd = app_doc(); if (nd && cr <= buf_len(&nd->b)) { nd->caret = nd->anchor = cr; ed_center_caret(nd); } } break;
        case CMD_QUICKFIX:
            if (d) {
                int line, col; doc_line_col(d, d->caret, &line, &col);
                int di = ed_diag_on_line(d, line);
                if (di >= 0 && G.diags[di].fix_ch) {
                    char msg[64]; snprintf(msg, sizeof msg, "Inserted '%c'", G.diags[di].fix_ch);
                    diag_apply_fix(d, &G.diags[di]);
                    G.diags[di] = G.diags[--G.ndiag];              /* the fixed problem goes away; the next build re-checks */
                    G.tip_visible = false;
                    app_status("%s", msg);
                } else app_status("No quick fix here");
            }
            break;
        case CMD_PANEL_CLEAR: if (G.panel_tab == PANEL_OUTPUT) app_output_clear(); else if (G.panel_tab == PANEL_TERMINAL) term_clear(); else G.ndiag = 0; break;
        case CMD_PANEL_COPY: if (G.out) { clip_set(G.out, G.out_len); app_status("Output copied"); } break;
        case CMD_NEW_FILE_HERE: dlg_new_entry(false); break;
        case CMD_NEW_FOLDER_HERE: dlg_new_entry(true); break;
        case CMD_REFRESH: if (G.proj.loaded) project_rescan(&G.proj); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- */
/* events                                                            */
/* ---------------------------------------------------------------- */
static bool global_shortcut(const cos_win_event_t *ev) {
    bool sh = (ev->mods & COS_MOD_SHIFT) != 0, ctrl = (ev->mods & COS_MOD_CTRL) != 0, alt = (ev->mods & COS_MOD_ALT) != 0;
    if (ev->special >= COS_KEY_F1 && ev->special <= COS_KEY_F12) {
        switch (ev->special - COS_KEY_F1 + 1) {
            case 5: app_do(sh ? CMD_STOP : CMD_RUN); return true;
            case 7: app_do(CMD_BUILD); return true;
            case 8: app_do(sh ? CMD_PREV_PROBLEM : CMD_NEXT_PROBLEM); return true;
            case 1: app_do(CMD_SHORTCUTS); return true;
        }
        return false;
    }
    if (alt && !ctrl) {
        if (ev->special == COS_KEY_ENTER) { app_do(CMD_QUICKFIX); return true; }
        if (ev->special == COS_KEY_LEFT) { app_do(CMD_NAV_BACK); return true; }
        if (ev->special == COS_KEY_RIGHT) { app_do(CMD_NAV_FWD); return true; }
        return false;
    }
    if (!ctrl) return false;
    if (ev->special == COS_KEY_PGUP) { app_do(CMD_PREV_TAB); return true; }
    if (ev->special == COS_KEY_PGDN) { app_do(CMD_NEXT_TAB); return true; }
    if (ev->special == COS_KEY_TAB) { app_do(sh ? CMD_PREV_TAB : CMD_NEXT_TAB); return true; }
    if (ev->special != COS_KEY_NONE) return false;
    char c = ev->ascii;
    if (c >= 'A' && c <= 'Z') c += 32;
    bool editor_focus = !G.psearch_focus && !(G.term_focus && G.panel_open && G.panel_tab == PANEL_TERMINAL) && !G.find_kbd;
    switch (c) {
        case 'n': app_do(sh ? CMD_NEW_PROJECT : CMD_NEW); return true;
        case 'o': app_do(sh ? CMD_GOTO_SYMBOL : CMD_OPEN); return true;
        case 's': app_do(alt ? CMD_SAVE_ALL : (sh ? CMD_SAVE_AS : CMD_SAVE)); return true;
        case 'w': app_do(CMD_CLOSE_TAB); return true;
        case 'f': app_do(sh ? CMD_FIND_PROJECT : CMD_FIND); return true;
        case 'h': app_do(CMD_REPLACE); return true;
        case 'g': app_do(CMD_GOTO_LINE); return true;
        case 'p': app_do(CMD_GOTO_FILE); return true;
        case 'b': app_do(CMD_VIEW_SIDEBAR); return true;
        case 'j': app_do(CMD_VIEW_PANEL); return true;
        case 'k': app_do(CMD_SHORTCUTS); return true;
        case '+': case '=': app_do(CMD_ZOOM_IN); return true;
        case '-': case '_': app_do(CMD_ZOOM_OUT); return true;
        default: break;
    }
    if (!editor_focus) return false;
    switch (c) {
        case 'z': app_do(sh ? CMD_REDO : CMD_UNDO); return true;
        case 'y': app_do(CMD_REDO); return true;
        case 'x': app_do(CMD_CUT); return true;
        case 'c': app_do(CMD_COPY); return true;
        case 'v': app_do(CMD_PASTE); return true;
        case 'a': app_do(CMD_SELECT_ALL); return true;
        case 'd': app_do(CMD_DUP_LINE); return true;
        case '/': app_do(CMD_TOGGLE_COMMENT); return true;
        default: break;
    }
    return false;
}

void app_event(const cos_win_event_t *ev) {
    switch (ev->type) {
        case COS_EV_CLOSE: app_do(CMD_EXIT); break;
        case COS_EV_KEY: {
            G.caret_t = cos_time_ms();
            if (G.dlg) { dlg_key(ev); break; }
            if (ev->special == COS_KEY_ESC) {
                if (G.ctx_open) { G.ctx_open = false; G.dirty_frame = true; break; }
                if (G.open_menu >= 0) { menu_close(); break; }
            }
            if (G.open_menu >= 0) menu_close();
            if (global_shortcut(ev)) break;
            if (G.find_open && G.find_kbd && ed_find_key(ev)) break;
            if (ui_key(ev)) break;
            if (ev->special == COS_KEY_ESC && G.find_open) { G.find_open = false; G.find_kbd = false; G.dirty_frame = true; break; }
            ed_key(ev);
            break; }
        case COS_EV_MOUSE_MOVE:
            G.mx = ev->x; G.my = ev->y;
            if ((ev->buttons & COS_MOUSE_BTN_LEFT) && G.drag_kind) ui_mouse_drag(ev->x, ev->y);
            else ui_mouse_move(ev->x, ev->y);
            break;
        case COS_EV_MOUSE_DOWN:
            G.mx = ev->x; G.my = ev->y; G.mdown = true;
            { bool in_find = G.find_open && ed_find_click(ev->x, ev->y);
              if (in_find) { G.find_kbd = true; G.dirty_frame = true; break; }
              if (G.find_open && pt_in(ev->x, ev->y, L.main_x, L.ed_y, L.main_w, L.ed_h)) G.find_kbd = false; }
            ui_mouse_down(ev->x, ev->y, ev->button, ev->mods);
            break;
        case COS_EV_MOUSE_UP:
            G.mdown = false;
            ui_mouse_up(ev->x, ev->y);
            break;
        case COS_EV_WHEEL: ui_wheel(G.mx, G.my, ev->wheel); break;
        case COS_EV_OPEN: case COS_EV_DROP: {
            char p[256];
            if (cos_win2_get_path(G.win, p, sizeof p) >= 0 && p[0]) { app_open_path(p); app_status("Opened %s", path_base(p)); }
            break; }
        default: break;
    }
}

/* per-loop housekeeping */
void app_tick(void) {
    static int last_phase = -1;
    static bool had_status = false;
    uint64_t now = cos_time_ms();
    build_poll();
    term_tick();
    int phase = (int)(((now - G.caret_t) / 530) & 1);
    if (phase != last_phase) { last_phase = phase; G.dirty_frame = true; }
    bool st = G.status_msg[0] && now - G.status_t < 4500;
    if (st != had_status) { had_status = st; G.dirty_frame = true; }
    if (G.bld.st == B_RUNNING || G.bld.st == B_COMPILING || G.job.active) G.dirty_frame = true;      /* live elapsed state / output */
}

void app_frame(void) {
    ui_draw_all();
    cos_win2_present(G.win);
    G.dirty_frame = false;
}

/* ---------------------------------------------------------------- */
/* startup                                                           */
/* ---------------------------------------------------------------- */
void app_init(const char *arg) {
    memset(&G, 0, sizeof G);
    G.sidebar_open = true; G.panel_open = true;
    G.side_w = SIDE_W_DEF; G.panel_h = PANEL_H_DEF;
    G.view = VIEW_EXPLORER; G.panel_tab = PANEL_OUTPUT;
    G.tab_width = 4; G.dark = true; G.save_before_run = true; G.code_px = 12;
    G.open_menu = -1; G.menu_hover = -1; G.hover_tab = -1; G.hover_btn = -1;
    G.syms_doc = -1; G.tip_diag = -1; G.cur = -1;
    G.find.icase = true;
    G.out = (char *)malloc(OUT_MAX + 1);
    if (G.out) G.out[0] = 0;
    G.out_follow = true;
    G.caret_t = cos_time_ms();
    build_init();

    const char *demo = cos_getenv("STUDIO_DEMO");
    G.demo = demo && demo[0] == '1';
    if (!G.demo) app_settings_load();
    app_apply_theme(G.dark);

    G.win = cos_win2_create("C-OS Studio", WIN_W, WIN_H, &G.info);
    if (G.win <= 0) { cos_printf("studio: cannot create a window\n"); cos_exit(1); }
    cui_canvas_init(&G.cv, G.info.pixels, G.info.width, G.info.height, G.info.stride);

    studio_ensure_dirs();
    if (G.demo) app_demo_setup();
    else if (arg && arg[0]) app_open_path(arg);
    else if (fs_is_dir(studio_deliverables())) project_open(&G.proj, studio_deliverables());   /* the Explorer starts on what Studio made */
    term_init();
    G.dirty_frame = true;
}

int main(int argc, char **argv) {
    app_init(argc > 1 ? argv[1] : NULL);
    while (!G.quit) {
        cos_win_event_t ev;
        memset(&ev, 0, sizeof ev);
        int r = cos_win2_wait(G.win, &ev, 30);
        if (r < 0) break;                                   /* the window is gone */
        if (r > 0) {
            do { app_event(&ev); memset(&ev, 0, sizeof ev); } while (!G.quit && cos_win2_poll(G.win, &ev) > 0);
        }
        app_tick();
        if (G.dirty_frame) app_frame();
    }
    if (!G.demo) app_settings_save();
    cos_win2_close(G.win);
    return 0;
}
