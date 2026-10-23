/* main.c - Py2C Studio: the Python-to-C workbench of C-OS.
 *
 *   doc.c / hl.c / view.c   documents (undo, UTF-8), highlighting, the text view and the one-line field
 *   run.c                   Convert (Python -> C, C -> Python), build with tcc.c-os against /system/sdk/lib/libp2crt.a, run with captured output
 *   ui.c                    menus, toolbar, tabs, explorer, find bar, panels, dialogs, mouse and keyboard routing
 *   plugins.c               plugins (cos_plugin_py2c.h)
 * This file: the application object, commands, configuration and the event loop. */
#include "p2.h"

App A;

void app_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(A.status, sizeof A.status, fmt, ap); va_end(ap);
    A.status_t = cos_time_ms(); A.dirty = true;
}
void slog(const char *fmt, ...) { char b[400], l[420]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); snprintf(l, sizeof l, "[PY2C] %s\n", b); cos_puts(l); }
Doc *app_doc(void) { return A.cur >= 0 && A.cur < A.ndocs ? A.docs[A.cur] : NULL; }

/* ---------------------------------------------------------------- configuration */
static void cfg_line(const char *k, const char *v) {
    if (!strcmp(k, "ui_mode")) A.opt.ui_mode = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "dark")) A.opt.dark = atoi(v) != 0;
    else if (!strcmp(k, "comments")) A.opt.comments = atoi(v) != 0;
    else if (!strcmp(k, "strict_c11")) A.opt.strict_c11 = atoi(v) != 0;
    else if (!strcmp(k, "fallback")) A.opt.fallback = atoi(v) != 0;
    else if (!strcmp(k, "embed")) snprintf(A.opt.embed, sizeof A.opt.embed, "%s", v);
    else if (!strcmp(k, "autosave_run")) A.opt.autosave_run = atoi(v) != 0;
    else if (!strcmp(k, "sidebar")) A.opt.show_sidebar = atoi(v) != 0;
    else if (!strcmp(k, "panel")) A.opt.show_panel = atoi(v) != 0;
    else if (!strcmp(k, "split_c")) A.opt.split_c = atoi(v) != 0;
    else if (!strcmp(k, "code_px")) { int n = atoi(v); if (n >= 9 && n <= 28) A.opt.code_px = n; }
    else if (!strcmp(k, "side_w")) { int n = atoi(v); if (n >= 140 && n <= 600) A.side_w = n; }
    else if (!strcmp(k, "panel_h")) { int n = atoi(v); if (n >= 80 && n <= 700) A.panel_h = n; }
    else if (!strcmp(k, "cpane_w")) { int n = atoi(v); if (n >= 200 && n <= 1200) A.cpane_w = n; }
    else if (!strcmp(k, "dir")) snprintf(A.dir, sizeof A.dir, "%s", v);
    else if (!strncmp(k, "recent", 6)) { int i = atoi(k + 6); if (i >= 0 && i < 6 && v[0]) { snprintf(A.recent[i], sizeof A.recent[i], "%s", v); if (i + 1 > A.nrecent) A.nrecent = i + 1; } }
}
void app_config_load(void) {
    static char buf[4096];
    ssize_t n = cos_read_file(CONFIG_PATH, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    if (buf[0] == '0' || buf[0] == '1') { if (!strchr(buf, '=')) { A.opt.ui_mode = buf[0] == '1'; return; } }      /* the first versions stored only the layout */
    for (char *l = buf; *l; ) {
        char *e = l; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        char *eq = strchr(l, '=');
        if (eq) { *eq = 0; cfg_line(l, eq + 1); }
        *e = save; l = *e ? e + 1 : e;
    }
}
void app_config_save(void) {
    char b[2400]; size_t o = 0;
    o += (size_t)snprintf(b + o, sizeof b - o, "ui_mode=%d\ndark=%d\ncomments=%d\nstrict_c11=%d\nfallback=%d\nembed=%s\nautosave_run=%d\nsidebar=%d\npanel=%d\nsplit_c=%d\ncode_px=%d\nside_w=%d\npanel_h=%d\ncpane_w=%d\ndir=%s\n",
        A.opt.ui_mode, A.opt.dark, A.opt.comments, A.opt.strict_c11, A.opt.fallback, A.opt.embed, A.opt.autosave_run, A.opt.show_sidebar, A.opt.show_panel, A.opt.split_c, A.opt.code_px, A.side_w, A.panel_h, A.cpane_w, A.dir);
    for (int i = 0; i < A.nrecent && o + 300 < sizeof b; ++i) o += (size_t)snprintf(b + o, sizeof b - o, "recent%d=%s\n", i, A.recent[i]);
    cos_write_file(CONFIG_PATH, b, o);
}

/* ---------------------------------------------------------------- documents */
static const char STARTER[] = "# Py2C Studio\n# F5 converts this Python file to C, F6 builds it with tcc and runs it.\n\ndef greet(name):\n    return \"Hello, \" + name + \" from C-OS!\"\n\nprint(greet(\"world\"))\nfor i in range(3):\n    print(\"line\", i)\n";
static int add_doc(Doc *d) {
    if (A.ndocs >= MAX_DOCS) { doc_free(d); app_status("Too many open files (%d)", MAX_DOCS); return -1; }
    A.docs[A.ndocs] = d; A.cur = A.ndocs++; A.focus = 0; A.ed.d = d;
    d->err_line = 0; A.nprobs = 0; A.conv_ok = -1;
    app_relayout(); A.dirty = true; return A.cur;
}
void app_new(const char *text, const char *name) {
    Doc *d = doc_new(NULL, L_PY);
    if (!d) return;
    if (name && name[0]) { snprintf(d->name, sizeof d->name, "%s", name); d->lang = lang_of_name(name); }
    const char *t = text ? text : STARTER;
    doc_set_text(d, t, strlen(t));
    if (!text) { d->caret = d->anchor = d->len; }
    d->untitled = true;
    if (add_doc(d) >= 0) { view_ensure_caret(&A.ed); app_status("New file"); }
}
static void recent_add(const char *path) {
    int at = -1; for (int i = 0; i < A.nrecent; ++i) if (!strcmp(A.recent[i], path)) at = i;
    if (at < 0) { if (A.nrecent < 6) at = A.nrecent++; else at = 5; }
    for (int i = at; i > 0; --i) memcpy(A.recent[i], A.recent[i - 1], sizeof A.recent[0]);
    snprintf(A.recent[0], sizeof A.recent[0], "%s", path);
}
void app_open(const char *path) {
    for (int i = 0; i < A.ndocs; ++i) if (!strcmp(A.docs[i]->path, path)) { tab_switch(i); A.focus = 0; return; }
    Doc *d = doc_new(NULL, L_TEXT);
    if (!d) return;
    if (!doc_load(d, path)) { doc_free(d); app_status("Could not open %s", path); return; }
    d->untitled = false;
    if (add_doc(d) < 0) return;
    recent_add(path);
    snprintf(A.dir, sizeof A.dir, "%s", path); { char *s = strrchr(A.dir, '/'); if (s && s != A.dir) *s = 0; else strcpy(A.dir, "/"); }
    explorer_refresh(); app_config_save();
    app_status("Opened %s (%d lines)", d->name, (int)doc_lines(d)); slog("opened %s", path);
}
void app_close_tab(int i, bool force) {
    if (i < 0 || i >= A.ndocs) return;
    Doc *d = A.docs[i];
    if (!force && d->dirty) {
        A.pending_action = 1; A.pending_doc = i; A.cur = i; app_relayout();
        dlg_open(DLG_CONFIRM); snprintf(A.dlg_msg, sizeof A.dlg_msg, "Save changes to %s?", d->name); return;
    }
    doc_free(d);
    for (int k = i; k + 1 < A.ndocs; ++k) A.docs[k] = A.docs[k + 1];
    --A.ndocs;
    if (A.cur > i || A.cur >= A.ndocs) --A.cur;
    if (A.cur < 0 && A.ndocs) A.cur = 0;
    A.nprobs = 0; A.conv_ok = -1; A.focus = 0;
    app_relayout(); A.dirty = true;
}
void app_save(Doc *d, bool as) {
    if (!d) return;
    if (as || d->untitled || !d->path[0]) { dlg_open(DLG_SAVE_AS); return; }
    if (doc_save(d, NULL)) { app_status("Saved %s", d->name); slog("saved %s", d->path); explorer_refresh(); }
    else app_status("Could not save %s", d->name);
}
void app_set_panel(int tab) {
    A.panel_tab = tab; A.opt.show_panel = true;
    if (tab == PANEL_LANG) panel_fill_lang();
    A.pv.d = tab == PANEL_PROBLEMS ? NULL : A.panel_docs[tab];
    if (tab == PANEL_INPUT) A.focus = 2;
    app_relayout();
}

/* ---------------------------------------------------------------- commands */
static View *focus_view(void) { return A.focus == 1 ? &A.cv_view : A.focus == 2 ? &A.pv : &A.ed; }
static void scan_examples(void) {
    cos_dirent_ex_t d[40]; int n = cos_listdir_ex("/system/sdk/examples/py2c", d, 40); A.nex = 0;
    if (n < 0) return;
    for (int i = 0; i < n && A.nex < 24; ++i) if (!d[i].is_dir && lang_of_name(d[i].name) == L_PY) snprintf(A.ex_names[A.nex++], sizeof A.ex_names[0], "%s", d[i].name);
    for (int i = 0; i < A.nex; ++i) for (int j = i + 1; j < A.nex; ++j) if (strcmp(A.ex_names[j], A.ex_names[i]) < 0) { char t[48]; memcpy(t, A.ex_names[i], 48); memcpy(A.ex_names[i], A.ex_names[j], 48); memcpy(A.ex_names[j], t, 48); }
}
static void exit_check(void) {
    for (int i = 0; i < A.ndocs; ++i) if (A.docs[i]->dirty && !A.docs[i]->readonly) {
        A.pending_action = 2; A.pending_doc = i; A.cur = i; app_relayout();
        dlg_open(DLG_CONFIRM); snprintf(A.dlg_msg, sizeof A.dlg_msg, "Save changes to %s before closing?", A.docs[i]->name); return;
    }
    A.quit = true;
}
void app_do(int cmd) {
    Doc *d = app_doc();
    A.dirty = true;
    if (cmd >= CMD_EXAMPLE && cmd < CMD_EXAMPLE + 24) { open_example(cmd - CMD_EXAMPLE); return; }
    if (cmd >= CMD_RECENT && cmd < CMD_RECENT + 6) { int i = cmd - CMD_RECENT; if (i < A.nrecent) { char p[256]; snprintf(p, sizeof p, "%s", A.recent[i]); app_open(p); } return; }
    View *fv = focus_view();
    switch (cmd) {
        case CMD_NEW: app_new(NULL, NULL); break;
        case CMD_OPEN: dlg_open(DLG_OPEN); break;
        case CMD_SAVE: app_save(d, false); break;
        case CMD_SAVE_AS: app_save(d, true); break;
        case CMD_SAVE_ALL: { int n = 0; for (int i = 0; i < A.ndocs; ++i) if (A.docs[i]->dirty && !A.docs[i]->untitled && doc_save(A.docs[i], NULL)) ++n; app_status("Saved %d file%s", n, n == 1 ? "" : "s"); break; }
        case CMD_CLOSE_TAB: app_close_tab(A.cur, false); break;
        case CMD_EXIT: exit_check(); break;
        case CMD_UNDO: if (d && fv->d == d) { doc_undo(d); view_ensure_caret(&A.ed); } else if (fv->d && fv->editable) { doc_undo(fv->d); } break;
        case CMD_REDO: if (d && fv->d == d) { doc_redo(d); view_ensure_caret(&A.ed); } else if (fv->d && fv->editable) { doc_redo(fv->d); } break;
        case CMD_CUT: if (fv->d) { view_cut(fv); view_ensure_caret(fv); } break;
        case CMD_COPY: if (fv->d) view_copy(fv); break;
        case CMD_PASTE: if (fv->d && fv->editable) { view_paste(fv); view_ensure_caret(fv); } break;
        case CMD_SELECT_ALL: if (fv->d) view_select_all(fv); break;
        case CMD_FIND: find_open(false); break;
        case CMD_REPLACE: find_open(true); break;
        case CMD_FIND_NEXT: if (!A.find_f.len) find_open(false); else find_step(false); break;
        case CMD_FIND_PREV: if (!A.find_f.len) find_open(false); else find_step(true); break;
        case CMD_GOTO_LINE: if (d) dlg_open(DLG_GOTO); break;
        case CMD_COMMENT: if (d) { view_comment_toggle(&A.ed); view_ensure_caret(&A.ed); } break;
        case CMD_INDENT: if (d) view_indent(&A.ed, false); break;
        case CMD_OUTDENT: if (d) view_indent(&A.ed, true); break;
        case CMD_DUP_LINE: if (d) { cos_win_event_t e; memset(&e, 0, sizeof e); e.type = COS_EV_KEY; e.mods = COS_MOD_CTRL; e.ascii = 'd'; view_key(&A.ed, &e, NULL); } break;
        case CMD_VIEW_SIDEBAR: A.opt.show_sidebar = !A.opt.show_sidebar; if (A.opt.show_sidebar && !A.opt.ui_mode) A.opt.ui_mode = 1; app_relayout(); app_config_save(); break;
        case CMD_VIEW_PANEL: A.opt.show_panel = !A.opt.show_panel; app_relayout(); app_config_save(); break;
        case CMD_VIEW_CPANE: A.opt.split_c = !A.opt.split_c; app_relayout(); app_config_save(); break;
        case CMD_ZOOM_IN: if (A.opt.code_px < 28) { A.opt.code_px++; fonts_set_code_px(A.opt.code_px); app_relayout(); app_config_save(); } break;
        case CMD_ZOOM_OUT: if (A.opt.code_px > 9) { A.opt.code_px--; fonts_set_code_px(A.opt.code_px); app_relayout(); app_config_save(); } break;
        case CMD_THEME_DARK: app_apply_theme(true); app_config_save(); break;
        case CMD_THEME_LIGHT: app_apply_theme(false); app_config_save(); break;
        case CMD_UI_STUDIO: A.opt.ui_mode = 1; app_relayout(); app_config_save(); break;
        case CMD_UI_CLASSIC: A.opt.ui_mode = 0; app_relayout(); app_config_save(); break;
        case CMD_CONVERT: conv_run(); break;
        case CMD_RUN: run_start(true); break;
        case CMD_BUILD: run_start(false); break;
        case CMD_STOP: run_stop(); break;
        case CMD_SAVE_C: conv_save_c(); break;
        case CMD_OPEN_IN_STUDIO: run_open_studio(); break;
        case CMD_AST: conv_ast(); break;
        case CMD_OPTIONS: dlg_open(DLG_OPTIONS); break;
        case CMD_PANEL_OUTPUT: app_set_panel(PANEL_OUTPUT); break;
        case CMD_PANEL_PROBLEMS: if (A.opt.show_panel && A.panel_tab == PANEL_PROBLEMS) { A.opt.show_panel = false; app_relayout(); } else app_set_panel(PANEL_PROBLEMS); break;
        case CMD_PANEL_INPUT: app_set_panel(PANEL_INPUT); break;
        case CMD_PANEL_AST: if (!A.panel_docs[PANEL_AST]->len) conv_ast(); else app_set_panel(PANEL_AST); break;
        case CMD_PANEL_LANG: app_set_panel(PANEL_LANG); break;
        case CMD_PANEL_CLEAR:
            if (A.panel_tab == PANEL_OUTPUT) panel_out_clear(); else if (A.panel_tab == PANEL_PROBLEMS) { A.nprobs = 0; if (d) d->err_line = 0; }
            else if (A.panel_tab == PANEL_INPUT) doc_set_text(A.panel_docs[PANEL_INPUT], "", 0);
            break;
        case CMD_PANEL_COPY: {
            Doc *pd = A.panel_docs[A.panel_tab];
            if (A.panel_tab == PANEL_PROBLEMS) { char b[4096]; size_t o = 0; for (int i = 0; i < A.nprobs && o + 300 < sizeof b; ++i) o += (size_t)snprintf(b + o, sizeof b - o, "line %d, col %d: %s\n", A.probs[i].line, A.probs[i].col, A.probs[i].msg); cos_clip_set(b, o); app_status("Copied the problems"); }
            else if (pd) { cos_clip_set(pd->t, pd->len); app_status("Copied %d bytes", (int)pd->len); }
            break; }
        case CMD_NEXT_TAB: if (A.ndocs > 1) tab_switch((A.cur + 1) % A.ndocs); break;
        case CMD_PREV_TAB: if (A.ndocs > 1) tab_switch((A.cur + A.ndocs - 1) % A.ndocs); break;
        case CMD_PLUGINS: dlg_open(DLG_PLUGINS); break;
        case CMD_ABOUT: dlg_open(DLG_ABOUT); break;
        case CMD_OPEN_SRC: run_open_source(false); break;
        case CMD_BUILD_SRC: run_open_source(true); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- the event loop */
static void redraw_now(void) { ui_draw(); cos_win2_present(A.win); }
static void create_panels(void) {
    for (int i = 0; i < PANEL_N; ++i) {
        if (i == PANEL_PROBLEMS) continue;
        Doc *d = doc_new("", L_TEXT); d->untitled = false; snprintf(d->name, sizeof d->name, "%s", i == PANEL_OUTPUT ? "output" : i == PANEL_INPUT ? "input" : i == PANEL_AST ? "ast" : "language");
        d->readonly = i != PANEL_INPUT;
        A.panel_docs[i] = d;
    }
    A.cdoc = doc_new("", L_C); A.cdoc->readonly = true; A.cdoc->untitled = false; snprintf(A.cdoc->name, sizeof A.cdoc->name, "converted.c");
}
static void handle_event(cos_win_event_t *ev) {
    switch (ev->type) {
        case COS_EV_CLOSE: app_do(CMD_EXIT); break;
        case COS_EV_KEY: {
            bool active = A.dlg || A.focus == 0 || A.focus == 3 || (A.focus == 2 && A.panel_tab == PANEL_INPUT);
            char commit[1024]; commit[0] = 0;
            int ir = imeui_key_gated(ev, active, commit, sizeof commit);
            if (ir & COSIME_COMMIT) { cos_win_event_t e0; memset(&e0, 0, sizeof e0); e0.type = COS_EV_KEY; ui_key(&e0, commit); }
            if (ir & COSIME_CONSUMED) { A.dirty = true; break; }
            ui_key(ev, NULL); A.dirty = true;
            break; }
        case COS_EV_MOUSE_DOWN: ui_mouse(0, ev); break;
        case COS_EV_MOUSE_MOVE: ui_mouse(1, ev); break;
        case COS_EV_MOUSE_UP: ui_mouse(2, ev); break;
        case COS_EV_WHEEL: ui_wheel(ev->wheel, (ev->mods & COS_MOD_SHIFT) != 0); A.dirty = true; break;
        case COS_EV_OPEN: case COS_EV_DROP: { char p[256]; if (cos_win2_get_path(A.win, p, sizeof p) >= 0 && p[0]) app_open(p); break; }
        default: break;
    }
}
int main(int argc, char **argv) {
    memset(&A, 0, sizeof A);
    A.opt.ui_mode = 1; A.opt.dark = true; A.opt.autosave_run = true; A.opt.show_sidebar = true; A.opt.show_panel = true; A.opt.split_c = true; A.opt.code_px = 13;
    A.side_w = 230; A.panel_h = 200; A.cpane_w = 460; A.panel_tab = PANEL_OUTPUT; A.cur = -1; A.conv_ok = -1; A.menu_open = -1;
    snprintf(A.dir, sizeof A.dir, "/desktop");
    app_config_load();
    A.opt.split_c = A.opt.split_c;
    T = A.opt.dark ? &THEME_DARK : &THEME_LIGHT;
    int32_t sw = 1024, sh = 768; cos_win2_screen_size(&sw, &sh);
    int ww = sw - 30, wh = sh - 48 - 20 - 36; if (ww > 1400) ww = 1400; if (wh > 900) wh = 900; if (ww < 640) ww = 640; if (wh < 420) wh = 420;
    A.win = cos_win2_create("Py2C Studio", ww, wh, &A.wi);
    if (A.win <= 0) { cos_printf("py2c: cannot create a window\n"); return 1; }
    cui_canvas_init(&A.cv, A.wi.pixels, A.wi.width, A.wi.height, A.wi.stride);
    ui_init(); create_panels(); plug_init(); imeui_set_redraw(redraw_now);
    { cos_stat_ex_t st; if (cos_stat_ex(A.dir, &st) != 0 || !st.is_dir) snprintf(A.dir, sizeof A.dir, "/"); }
    explorer_refresh(); scan_examples();
    A.caret_t = cos_time_ms();
    app_relayout();
    slog("started");
    if (argc > 1 && argv[1][0]) app_open(argv[1]); else app_new(NULL, NULL);
    char last_title[160] = "";
    uint64_t last_phase = 0; cos_win_event_t pending_move; bool have_move = false;
    while (!A.quit) {
        cos_win_event_t ev; memset(&ev, 0, sizeof ev);
        int r = cos_win2_wait(A.win, &ev, A.bs == B_RUNNING || A.bs == B_COMPILING ? 40 : 120);
        if (r < 0) break;
        if (r > 0) {
            do {
                if (ev.type == COS_EV_MOUSE_MOVE) { pending_move = ev; have_move = true; }
                else { if (have_move) { handle_event(&pending_move); have_move = false; } handle_event(&ev); }
                memset(&ev, 0, sizeof ev);
            } while (!A.quit && cos_win2_poll(A.win, &ev) > 0);
            if (have_move) { handle_event(&pending_move); have_move = false; }
        }
        run_poll(); plug_poll();
        uint64_t now = cos_time_ms();
        uint64_t ph = (now - A.caret_t) / 530;
        if (ph != last_phase) { last_phase = ph; if (A.focus == 0 || A.focus == 2 || A.focus == 3 || A.dlg) A.dirty = true; }
        if (A.status[0] && now - A.status_t > 6000 && now - A.status_t < 6200) A.dirty = true;
        if (run_busy()) A.dirty = true;
        Doc *d = app_doc();
        char title[160]; snprintf(title, sizeof title, "%s%s - Py2C Studio", d ? d->name : "Py2C Studio", d && d->dirty ? " *" : "");
        if (strcmp(title, last_title)) { snprintf(last_title, sizeof last_title, "%s", title); cos_win2_set_title(A.win, title); }
        if (A.dirty) { ui_draw(); cos_win2_present(A.win); }
    }
    if (A.bs == B_RUNNING || A.bs == B_COMPILING) run_stop();
    app_config_save();
    plug_shutdown();
    cos_win2_close(A.win);
    return 0;
}
