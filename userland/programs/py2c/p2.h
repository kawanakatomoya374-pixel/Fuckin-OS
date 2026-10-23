/* p2.h - Py2C Studio: the Python-to-C workbench of C-OS (editor, converter, build and run, plugins).
 * Shared types and prototypes. See main.c for the overview. */
#ifndef P2_H
#define P2_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include "cos.h"
#include "cos_ui.h"

/* SYS_WRITE_FILE refuses a length of 0, so an empty file is made by removing and re-creating it */
static inline ssize_t p2_write_file(const char *path, const void *buf, size_t len) {
    if (len) return cos_write_file(path, buf, len);
    cos_unlink(path); int fd = cos_open(path, COS_O_WRONLY_CREATE);
    if (fd < 0) return -1;
    cos_close(fd); return 0;
}
#include "cosime_ui.h"

/* ---------------------------------------------------------------- theme */
typedef struct {
    uint32_t bg, panel, side, bar, border, text, dim, faint, accent, on_accent, sel, curline, gutter, field, btn, btn_hi, tab_on, err, warn, ok;
    uint32_t kw, str, num, cmt, fn, deco, type, builtin;
} Theme;
extern const Theme THEME_DARK, THEME_LIGHT;
extern const Theme *T;

/* ---------------------------------------------------------------- text documents */
enum { L_TEXT, L_PY, L_C };
typedef struct { size_t pos; char *del; size_t dlen; char *ins; size_t ilen; uint32_t group; size_t cb, ca; uint64_t t; } UndoOp;
typedef struct Doc {
    char path[256], name[64];
    char *t; size_t len, cap;
    size_t caret, anchor;
    int want_col;                  /* the column an Up/Down keeps (in characters) */
    bool dirty, untitled, readonly, crlf;
    int lang;
    uint32_t ver;                  /* +1 on every change */
    size_t *ls; size_t nl, ls_cap; uint32_t ls_ver;      /* line starts (rebuilt when ver moved) */
    uint8_t *lst; size_t lst_cap; uint32_t lst_ver;      /* per line: highlighter state at its start */
    UndoOp *ops; int nops, ops_cap, head; uint32_t group, next_group; size_t undo_bytes;
    uint64_t mtime;
    int top, left;                 /* scroll: first line, pixels */
    int err_line;                  /* 1-based line of the last conversion error, 0 = none */
} Doc;

Doc  *doc_new(const char *path, int lang);
void  doc_free(Doc *d);
bool  doc_load(Doc *d, const char *path);
bool  doc_save(Doc *d, const char *path);
void  doc_set_text(Doc *d, const char *s, size_t n);          /* replaces everything, clears undo, not dirty */
void  doc_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t ilen, bool merge);
void  doc_group_begin(Doc *d); void doc_group_end(Doc *d);
void  doc_undo(Doc *d); void doc_redo(Doc *d);
size_t doc_lines(Doc *d);
size_t doc_line_start(Doc *d, size_t line);
size_t doc_line_end(Doc *d, size_t line);                     /* excludes the newline */
size_t doc_line_of(Doc *d, size_t pos);
int   doc_col_of(Doc *d, size_t pos);                          /* column in characters (tab = 4) */
size_t doc_pos_at_col(Doc *d, size_t line, int col);
bool  doc_has_sel(const Doc *d);
void  doc_sel(const Doc *d, size_t *a, size_t *b);
char *doc_sel_text(Doc *d, size_t *len);                       /* malloc'd; NULL when nothing is selected */
size_t u8_prev(const char *s, size_t i); size_t u8_next(const char *s, size_t n, size_t i);
int   lang_of_name(const char *name);
void  doc_name_from_path(Doc *d);

/* ---------------------------------------------------------------- highlighting */
enum { K_TEXT, K_KW, K_STR, K_NUM, K_CMT, K_FN, K_DECO, K_TYPE, K_BUILTIN, K_PUNCT };
typedef struct { int start, len, kind; } Tok;
/* tokenises one line; *state is the carry between lines (0 none, 1 inside ''' , 2 inside """, 3 inside a C block comment) */
int   hl_line(int lang, const char *s, int n, int *state, Tok *out, int max);
void  hl_prepare(Doc *d);                                     /* fills d->lst up to date */

/* ---------------------------------------------------------------- fonts and metrics */
extern cui_font *g_mono, *g_ui, *g_ui_bold, *g_ui_small;
extern int g_code_px, g_cw, g_lh;
void  fonts_set_code_px(int px);

/* ---------------------------------------------------------------- the text view (editor / read-only panes) */
typedef struct {
    Doc *d;
    int x, y, w, h;
    bool gutter, editable, focus;
    int lang;
    bool drag; int click_n; uint64_t click_t; int click_x, click_y;
    int text_x;                    /* where text starts (after the gutter) */
    int vis_lines;
    bool wrap_hint;
    bool mark_err;
    /* find highlighting */
    const char *find; size_t find_len; bool find_icase;
} View;
void  view_layout(View *v);
void  view_draw(cui_canvas *c, View *v);
bool  view_key(View *v, const cos_win_event_t *ev, const char *commit);   /* true when used */
bool  view_mouse(View *v, int type, int x, int y, int buttons, int mods);
void  view_wheel(View *v, int dy, bool shift);
void  view_ensure_caret(View *v);
void  view_center_caret(View *v);
void  view_goto_line(View *v, int line);
int   view_x_of(Doc *d, size_t pos);                           /* pixel x of pos inside its line, without scroll */
void  view_caret_xy(View *v, int *x, int *y);                  /* screen position of the caret (for the IME window) */
void  view_select_all(View *v);
void  view_copy(View *v); void view_cut(View *v); void view_paste(View *v);
void  view_comment_toggle(View *v);
void  view_indent(View *v, bool out);
long  find_next(Doc *d, const char *what, size_t wl, size_t from, bool icase, bool backwards);

/* ---------------------------------------------------------------- one-line text field */
typedef struct { char text[260]; int len, pos, sel; bool focus; } Field;
void  field_set(Field *f, const char *s);
bool  field_key(Field *f, const cos_win_event_t *ev, const char *commit);   /* edits; true when it handled the key */
void  field_draw(cui_canvas *c, Field *f, int x, int y, int w, int h, bool numeric_hint);
int   field_hit(Field *f, int fx, int mx);                      /* caret index at mouse x */

/* ---------------------------------------------------------------- the application */
#define MAX_DOCS 12
#define MAX_PROBLEMS 64
typedef struct { int line, col; char msg[160]; int sev; } Problem;
typedef enum { B_IDLE, B_CONVERTING, B_COMPILING, B_RUNNING, B_EXITED, B_FAILED, B_KILLED } BuildState;
enum { PANEL_OUTPUT, PANEL_PROBLEMS, PANEL_INPUT, PANEL_AST, PANEL_LANG, PANEL_N };
enum { DLG_NONE, DLG_OPEN, DLG_SAVE_AS, DLG_GOTO, DLG_OPTIONS, DLG_ABOUT, DLG_CONFIRM, DLG_PLUGINS, DLG_SAVE_C };

typedef struct {
    bool comments, strict_c11, fallback;       /* conversion options (--comments --c11 --fallback) */
    char embed[48];                            /* --embed-entry NAME, "" = int main() */
    bool autosave_run, show_sidebar, show_panel, dark, split_c;
    int ui_mode;                               /* 1 = Studio layout (rail, explorer), 0 = classic */
    int code_px;
} Options;

typedef struct {
    int64_t win; cos_win_info_t wi; cui_canvas cv;
    int W, H;
    Doc *docs[MAX_DOCS]; int ndocs, cur;
    Doc *cdoc;                                  /* what the last conversion made (C, or Python for a C source) */
    char cdoc_for[256];
    Doc *panel_docs[PANEL_N];                   /* output, (problems drawn from the list), input, ast, language */
    View ed, cv_view, pv;                       /* the editor, the converted text, the bottom text */
    Options opt;
    int panel_tab, side_w, panel_h, cpane_w;    /* layout */
    int drag_split;                             /* 0 none, 1 sidebar, 2 panel, 3 C pane */
    int menu_open, menu_hover;
    int mx, my; bool mdown;
    int focus;                                  /* 0 editor, 1 C pane, 2 bottom text, 3 field (find / dialog), 4 sidebar */
    /* find bar */
    bool find_open, find_replace; Field find_f, repl_f; bool find_icase; int find_focus; int find_count; int find_idx;
    /* status */
    char status[160]; uint64_t status_t;
    /* conversion + build state */
    BuildState bs; int64_t pid; uint64_t t0, t1; int exit_code; char out_path[96], exe_path[160]; size_t tail_pos; char line_acc[400]; int line_len;
    Problem probs[MAX_PROBLEMS]; int nprobs, prob_sel, prob_scroll;
    int conv_ok;                                /* -1 never, 0 failed, 1 ok */
    uint64_t conv_ms; size_t conv_bytes;
    /* sidebar explorer */
    char dir[256]; cos_dirent_ex_t *ents; int nents; int side_scroll; int side_hover; uint64_t dir_gen;
    bool side_examples;
    char recent[6][256]; int nrecent;
    char ex_names[24][48]; int nex;
    /* dialogs */
    int dlg; char dlg_title[64]; char dlg_msg[200]; int dlg_arg;
    Field dlg_f1, dlg_f2; int dlg_focus; char bdir[256]; cos_dirent_ex_t *bents; int nbents; int bscroll, bsel;
    int pending_action, pending_doc;           /* what continues after a Save: 0 none, 1 close tab pending_doc, 2 exit */
    bool overwrite_ok;
    /* misc */
    bool quit, dirty; uint64_t caret_t;
    bool ime_have; int ime_x, ime_y;
    char tip[96]; int tip_x, tip_y;
} App;
extern App A;

#define CONFIG_PATH "/desktop/py2c.conf"
#define OUT_CAP (8 * 1024 * 1024)

/* app.c / main.c */
void  app_status(const char *fmt, ...);
void  slog(const char *fmt, ...);                               /* a line on the serial log (what the tests read) */
Doc  *app_doc(void);
void  app_open(const char *path);
void  app_new(const char *text, const char *name);
void  app_close_tab(int i, bool force);
void  app_save(Doc *d, bool as);
void  app_do(int cmd);
void  app_set_panel(int tab);
void  app_apply_theme(bool dark);
void  app_relayout(void);
void  app_redraw(void);
void  app_config_load(void); void app_config_save(void);
void  panel_out_add(const char *s, size_t n);
void  panel_out_clear(void);
void  explorer_refresh(void);
void  tab_switch(int i);

/* run.c */
void  conv_run(void);                           /* Convert (F5) */
void  conv_save_c(void);
void  run_start(bool exec);                     /* Run (exec=true) or Build (false) */
void  run_stop(void);
void  run_poll(void);
void  run_open_studio(void);
void  run_open_source(bool build);                 /* the Py2C source tree (/apps/Py2C) in C-OS Studio; build: and make it with tcc (make install) */
void  conv_ast(void);
void  panel_fill_lang(void);
bool  run_busy(void);

/* ui.c */
void  ui_draw(void);
bool  ui_key(const cos_win_event_t *ev, const char *commit);
void  ui_mouse(int type, const cos_win_event_t *ev);
void  ui_wheel(int dy, bool shift);
void  dlg_open(int kind);
void  dlg_close(void);
void  ui_init(void);
void  ui_tab_rects(void);
void  open_example(int i);
void  find_step(bool back);
void  find_open(bool replace);

/* commands */
enum {
    CMD_NONE = 0, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_SAVE_ALL, CMD_CLOSE_TAB, CMD_EXIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECT_ALL, CMD_FIND, CMD_REPLACE, CMD_FIND_NEXT, CMD_FIND_PREV, CMD_GOTO_LINE,
    CMD_COMMENT, CMD_INDENT, CMD_OUTDENT, CMD_DUP_LINE,
    CMD_VIEW_SIDEBAR, CMD_VIEW_PANEL, CMD_VIEW_CPANE, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_THEME_DARK, CMD_THEME_LIGHT, CMD_UI_STUDIO, CMD_UI_CLASSIC,
    CMD_CONVERT, CMD_RUN, CMD_BUILD, CMD_STOP, CMD_SAVE_C, CMD_OPEN_IN_STUDIO, CMD_AST, CMD_OPTIONS,
    CMD_PANEL_OUTPUT, CMD_PANEL_PROBLEMS, CMD_PANEL_INPUT, CMD_PANEL_AST, CMD_PANEL_LANG, CMD_PANEL_CLEAR, CMD_PANEL_COPY,
    CMD_NEXT_TAB, CMD_PREV_TAB, CMD_NEXT_PROBLEM, CMD_PREV_PROBLEM, CMD_PLUGINS, CMD_ABOUT, CMD_C_TO_PY, CMD_OPEN_SRC, CMD_BUILD_SRC,
    CMD_EXAMPLE = 100,         /* + index */
    CMD_RECENT = 200,          /* + index */
    CMD_PLUGIN_BASE = 400
};

/* plugins.inc */
void  plug_init(void); void plug_poll(void); void plug_shutdown(void);
#endif
