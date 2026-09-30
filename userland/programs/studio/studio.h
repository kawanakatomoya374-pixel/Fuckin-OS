/* studio.h - C-OS Studio: shared types and module interfaces.
 *
 * Studio is a ring-3 .c-os program (window API v2 + cos_ui). It is built
 * twice: for the OS with tools/cos-cc, and for the host with plain gcc
 * against studio/host/host_shim.c (which implements the handful of cos_*
 * calls Studio uses on top of POSIX) so the editor can be driven by a
 * script and screenshotted without booting anything.
 */
#ifndef STUDIO_H
#define STUDIO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cos.h"
#include "cos_ui.h"

/* ------------------------------------------------------------------ */
/* geometry (client area 960x640, see design doc 8.1)                  */
/* ------------------------------------------------------------------ */
#define WIN_W        960
#define WIN_H        640
#define MENU_H       24
#define TOOL_Y       25
#define TOOL_H       33
#define MAIN_Y       59
#define ACT_W        40
#define SIDE_W_DEF   196
#define SIDE_W_MIN   120
#define SIDE_W_MAX   400
#define TAB_H        30
#define GUTTER_W     50
#define PANEL_H_DEF  150
#define PANEL_H_MIN  80
#define PANEL_H_MAX  360
#define PANEL_HDR_H  28
#define STATUS_H     24
#define CODE_PAD_X   12
#define LINE_H_F     17.6f
#define CELL_W_F     7.25f

/* ------------------------------------------------------------------ */
/* theme                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t bg, side, bar, border, act_bg, gutter, panel_bg;
    uint32_t text, dim, faint, accent, on_accent;
    uint32_t curline, errline, sel, err, warn, ok;
    uint32_t btn, btn_hi, field, tip_bg, tab_active, row_sel, bar2, folder, thumb;
    uint32_t kw, type, fn, str, num, cmt, pp;
} Theme;
extern const Theme THEME_DARK, THEME_LIGHT;
extern const Theme *T;

/* ------------------------------------------------------------------ */
/* text buffer (gap buffer) + undo                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buf;
    size_t gs, ge, cap;          /* gap = [gs, ge) */
    uint32_t version;            /* +1 on every change */
    uint32_t *ls;                /* line start offsets */
    size_t nl, ls_cap;
    uint32_t ls_ver;
} Buffer;

void   buf_init(Buffer *b);
void   buf_free(Buffer *b);
size_t buf_len(const Buffer *b);
char   buf_at(const Buffer *b, size_t i);
void   buf_insert(Buffer *b, size_t pos, const char *s, size_t n);
void   buf_delete(Buffer *b, size_t pos, size_t n);
void   buf_copy(const Buffer *b, size_t pos, size_t n, char *out);
const char *buf_text(Buffer *b);                 /* contiguous text, not NUL-terminated */
size_t buf_lines(Buffer *b);
size_t buf_line_start(Buffer *b, size_t line);
size_t buf_line_end(Buffer *b, size_t line);     /* excludes '\n' (and a trailing '\r') */
size_t buf_line_of(Buffer *b, size_t pos);

typedef struct { size_t pos; char *del; size_t dlen; char *ins; size_t ilen; uint64_t t; uint32_t group; uint8_t solo; size_t caret_before, caret_after; } UndoOp;
typedef struct { UndoOp *ops; size_t n, cap, head; uint32_t next_group, cur_group; } UndoStack;

/* ------------------------------------------------------------------ */
/* syntax highlighting                                                 */
/* ------------------------------------------------------------------ */
enum { TK_TEXT, TK_KW, TK_TYPE, TK_FN, TK_STR, TK_NUM, TK_CMT, TK_PP, TK_PUNCT };
typedef struct { int start, len, kind; } Tok;
/* Tokenises one line (given whether it starts inside a block comment).
 * Returns the token count and updates *in_comment for the next line. */
int hl_line(const char *s, int n, bool *in_comment, Tok *out, int max);

/* ------------------------------------------------------------------ */
/* documents                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int  line;              /* 1-based line the squiggle/marker is drawn on */
    int  rep_line;          /* 1-based line the COMPILER reported (differs for "expected" errors) */
    int  col, col_end;      /* 0-based columns of the squiggle */
    int  sev;               /* 0 error, 1 warning */
    char fix_ch;            /* quick fix: insert this character at (line, col_end_of_text); 0 if none */
    char file[256];
    char path[256];         /* resolved path of the file, if known */
    char msg[160];
} Diag;

typedef struct Doc {
    char path[256];
    char name[64];
    Buffer b;
    UndoStack u;
    size_t caret, anchor;
    int    want_col;
    int    scroll_y;         /* pixels */
    int    scroll_x;         /* pixels */
    bool   dirty, crlf, untitled, center_req;
    uint64_t mtime_check;
    uint8_t *hs;             /* per-line "starts in block comment" */
    size_t   hs_n, hs_valid;
    uint32_t hs_ver;
} Doc;

Doc  *doc_new(const char *path);
bool  doc_load(Doc *d, const char *path);
bool  doc_save(Doc *d, const char *path);
void  doc_free(Doc *d);

/* editing (all go through the undo stack) */
void  doc_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t ilen, bool merge);
void  doc_insert_at_caret(Doc *d, const char *s, size_t n);
void  doc_type_char(Doc *d, char c);
void  doc_backspace(Doc *d, bool word);
void  doc_delete(Doc *d, bool word);
void  doc_newline(Doc *d);
void  doc_indent(Doc *d, bool outdent);
void  doc_toggle_comment(Doc *d);
void  doc_duplicate_line(Doc *d);
void  doc_move_line(Doc *d, int dir);
void  doc_group_begin(Doc *d);        /* several edits -> one undo step */
void  doc_group_end(Doc *d);
bool  doc_undo(Doc *d);
bool  doc_redo(Doc *d);
void  doc_cut_copy(Doc *d, bool cut);
void  doc_paste(Doc *d);
bool  doc_has_sel(const Doc *d);
void  doc_sel_range(const Doc *d, size_t *a, size_t *b);
char *doc_sel_text(Doc *d, size_t *len);

/* movement; `sel` extends the selection */
void  doc_move_left(Doc *d, bool sel, bool word);
void  doc_move_right(Doc *d, bool sel, bool word);
void  doc_move_vert(Doc *d, int lines, bool sel);
void  doc_move_home(Doc *d, bool sel);
void  doc_move_end(Doc *d, bool sel);
void  doc_move_doc_edge(Doc *d, bool end, bool sel);
void  doc_select_all(Doc *d);
void  doc_select_word_at(Doc *d, size_t pos);
void  doc_select_line_at(Doc *d, size_t pos);
void  doc_goto_line(Doc *d, int line, int col);       /* 1-based */
void  doc_line_col(Doc *d, size_t pos, int *line, int *col);   /* 1-based line, 1-based col */
size_t doc_pos_from_line_col(Doc *d, int line, int col);       /* 0-based line/col */
size_t doc_prev_cp(Doc *d, size_t pos);
size_t doc_next_cp(Doc *d, size_t pos);
int   doc_visual_col(Doc *d, size_t line, size_t pos);
bool  doc_match_bracket(Doc *d, size_t pos, size_t *a, size_t *b);
void  doc_hs_sync(Doc *d, size_t upto_line);           /* make block-comment state valid */
bool  doc_line_in_comment(Doc *d, size_t line);

/* clipboard (Studio-internal; the OS has no clipboard syscall yet) */
void  clip_set(const char *s, size_t n);
const char *clip_get(size_t *n);

/* ------------------------------------------------------------------ */
/* find / replace                                                      */
/* ------------------------------------------------------------------ */
typedef struct { char what[128], with[128]; bool icase, word; int count, index; } FindState;
long  find_next(Doc *d, const FindState *f, size_t from, bool backwards);   /* -1 if none */
int   find_count(Doc *d, const FindState *f);
int   find_replace_all(Doc *d, const FindState *f);
void  search_project(const char *what, bool icase);    /* fills G.hits */

/* completion */
void  comp_update(Doc *d);
void  comp_accept(Doc *d);

/* ------------------------------------------------------------------ */
/* project + file system                                               */
/* ------------------------------------------------------------------ */
#define MAX_NODES 512
typedef struct { char name[64]; char path[256]; int depth; bool is_dir, open; } Node;
typedef struct {
    char root[256], name[64], target[64], cflags[128];
    char defines[192];      /* "FOO,BAR=1,BAZ=\"x\"" - comma separated, become -DFOO -DBAR=1 -DBAZ="x" */
    char includes[192];     /* "vendor,../shared/include" - comma separated, relative to root unless absolute */
    char libs[192];         /* "vendor/libfoo.a,extra.o" - comma separated, relative to root unless absolute */
    char type[8];           /* "exe" (default), "lib" (static .a), or "shared" (.c-osll) */
    bool warn_all, werror;
    Node nodes[MAX_NODES];
    int  nnodes;
    bool loaded;
} Project;

bool  fs_exists(const char *path);
bool  fs_is_dir(const char *path);
char *fs_read_all(const char *path, size_t *len);
bool  fs_write_all(const char *path, const void *data, size_t len);
void  path_join(char *out, size_t cap, const char *a, const char *b);
const char *path_base(const char *p);
void  path_dir(const char *p, char *out, size_t cap);
bool  has_ext(const char *name, const char *ext);

bool  project_open(Project *p, const char *root);
void  project_rescan(Project *p);
void  project_toggle(Project *p, int idx);
bool  project_create(const char *root_parent, const char *name, int templ, char *out_main, size_t cap);
bool  project_find_root(const char *file, char *root, size_t cap);   /* walks up looking for cosproj.txt */
int   project_sources(Project *p, char out[][256], int max);

/* ------------------------------------------------------------------ */
/* C declaration index (index.c): functions, globals, typedefs,        */
/* struct/union/enum tags and members, macros, enum constants -        */
/* gathered from the current file and everything it #includes.         */
/* ------------------------------------------------------------------ */
enum { DK_FUNC, DK_VAR, DK_TYPEDEF, DK_STRUCT, DK_UNION, DK_ENUM, DK_MACRO, DK_ENUMCONST, DK_MEMBER };
#define COMP_MAX 24
void index_rebuild(Doc *d, Project *proj);
void index_ensure_fresh(Doc *d, Project *proj);
int  index_find_prefix(const char *prefix, size_t plen, const void **out_decls, int max);
int  index_members(const char *tag, const char *prefix, size_t plen, const void **out_decls, int max);
const char *index_resolve_tag(const char *type_text);
const char *index_var_type(const char *name);
const char *index_func_params(const char *name);
int  index_count(void);
uint8_t index_kind_at(int i);
const char *index_name_at(int i);
const char *index_detail_at(int i);
char *index_clean_range(Doc *d, size_t a, size_t b);   /* comment/string-stripped copy of buf[a,b); caller frees */
const char *decl_name(const void *p);
const char *decl_detail(const void *p);
uint8_t     decl_kind(const void *p);
void  comp_update_auto(Doc *d, bool force);
void  sig_update(Doc *d);

typedef struct { char kind[8]; char name[64]; int line; } Sym;
int   outline_scan(Doc *d, Sym *out, int max);

/* ------------------------------------------------------------------ */
/* build / run                                                         */
/* ------------------------------------------------------------------ */
typedef enum { B_IDLE, B_SAVING, B_COMPILING, B_RUNNING, B_EXITED, B_FAILED, B_KILLED } BuildState;
typedef struct {
    BuildState st;
    int64_t pid;
    uint64_t t0, t1;
    int exit_code;
    int nerr, nwarn;
    char out_path[128];       /* where the child's stdout/stderr is written */
    char exe_path[128];
    char src[256];
    size_t tail_pos;          /* how much of out_path was already shown */
    char line_acc[512];       /* partial line being assembled */
    int  line_len;
    char cfg[16];             /* "Debug" / "Release" */
    bool build_only;          /* F7: compile but do not run */
} Build;

int  diag_parse_line(const char *line, Diag *out);    /* file:line: error: msg -> 1 error, 2 warning, 0 no */
void diag_refine(Diag *d);                            /* find the real squiggle position */
bool diag_apply_fix(Doc *d, const Diag *g);

/* ------------------------------------------------------------------ */
/* make + terminal                                                     */
/* ------------------------------------------------------------------ */
#define MAKE_MAXARGV 48
typedef struct {
    char  show[512];                 /* the command as written (after variable expansion) */
    char *argv[MAKE_MAXARGV]; int argc;
    char  output[256];               /* the file after `-o`, if any */
    bool  silent, ignore_err, last_of_target;
    char  sigtarget[128]; uint64_t sig;
} MakeStep;
typedef struct { MakeStep *steps; int n, cap; char goal[128]; bool up_to_date; } MakePlan;
bool make_plan(const char *dir, const char *makefile, const char *target, const char *const *overrides, int nov,
               MakePlan *plan, char *err, size_t errcap, void (*msg)(const char *));
void make_plan_free(MakePlan *p);
void make_commit_step(const MakeStep *s);
void make_forget(const char *target);

typedef struct {
    bool active; int64_t pid; char log[64]; size_t tail; char acc[512]; int acc_len; uint64_t t0;
    bool is_make; MakePlan plan; int step; char dir[256];
    int errors; bool stopping;
} TermJob;

/* Studio's own folder: the program lives in it, and everything Studio creates goes into
 * its deliverables/ subfolder. */
const char *studio_home(void);           /* "/C-OS Studio" */
const char *studio_deliverables(void);   /* "/C-OS Studio/deliverables" */
void studio_ensure_dirs(void);

void term_init(void);
void term_print(const char *s, size_t n);
void term_printf(const char *fmt, ...);
void term_key(const cos_win_event_t *ev);
void term_tick(void);
void term_stop(void);
bool term_busy(void);
void term_clear(void);
void term_prompt(char *buf, size_t cap);
int  term_line_count(void);
int  term_line_at(int idx, char *buf, size_t cap);

/* ------------------------------------------------------------------ */
/* application state                                                   */
/* ------------------------------------------------------------------ */
#define MAX_DOCS   16
#define MAX_DIAGS  256
#define OUT_MAX    (256 * 1024)
enum { VIEW_EXPLORER, VIEW_SEARCH, VIEW_PROBLEMS, VIEW_SETTINGS };
enum { PANEL_OUTPUT, PANEL_PROBLEMS, PANEL_TERMINAL };
enum { DLG_NONE, DLG_GOTO_LINE, DLG_GOTO_FILE, DLG_OPEN, DLG_SAVE_AS, DLG_NEW_PROJECT,
       DLG_SETTINGS, DLG_CONFIRM, DLG_ABOUT, DLG_SHORTCUTS, DLG_INPUT };

typedef struct { char label[48]; int id; int key; uint8_t mods; bool sep; } MenuItem;

typedef struct {
    int64_t win;
    cui_canvas cv;
    cos_win_info_t info;

    Project proj;
    Doc *docs[MAX_DOCS];
    int  ndocs, cur;

    Build bld;
    Diag  diags[MAX_DIAGS];
    int   ndiag;

    char *out;                /* OUTPUT panel text */
    size_t out_len;
    int    out_scroll;        /* in lines */
    bool   out_follow;
    int    out_sel_line;

    /* layout state */
    bool sidebar_open, panel_open;
    int  side_w, panel_h;
    int  view;                /* VIEW_* */
    int  panel_tab;
    int  side_scroll;
    int  prob_scroll;

    /* interaction */
    int  mx, my;
    bool mdown;
    int  drag_kind;           /* 0 none, 1 editor select, 2 side splitter, 3 panel splitter, 4 vscroll */
    int  drag_off;
    int  open_menu;           /* -1 none */
    int  menu_hover;
    int  hover_tab, hover_btn;
    uint64_t last_click_t; int last_click_x, last_click_y, click_count;
    uint64_t caret_t;         /* last caret activity (blink) */
    bool tip_visible; int tip_diag; uint64_t tip_t;

    /* find bar */
    bool  find_open, find_replace;
    FindState find;
    int   find_focus;         /* 0 what, 1 with */
    bool  find_kbd;           /* the find bar owns the keyboard */

    /* dialogs */
    int   dlg;
    char  dlg_text[256];
    int   dlg_sel, dlg_scroll;
    char  dlg_title[64], dlg_msg[160];
    int   dlg_action;         /* what CONFIRM/INPUT does on OK */
    char  dlg_dir[256];       /* directory being browsed in OPEN/SAVE_AS */
    Node  *dlg_list; int dlg_nlist;

    /* completion popup */
    bool  comp_open; char comp_items[COMP_MAX][64]; int comp_n, comp_sel; size_t comp_from;
    uint8_t comp_kind[COMP_MAX]; char comp_detail[COMP_MAX][72];
    bool  comp_is_member;         /* true: accepting replaces after the '.'/'->' only, not a whole identifier */
    bool  comp_is_include;        /* true: item text is a filename, insert verbatim, no trailing () logic */

    /* signature help (shown instead of the completion popup when a call's parens are open) */
    bool   sig_open;
    char   sig_name[56];
    char   sig_params[8][48];     /* each formatted "type name" as written in the header */
    int    sig_nparams;
    int    sig_active;            /* index of the parameter the caret is currently in, or -1 */
    size_t sig_paren_pos;         /* position of the '(' - used to anchor the popup like comp_from does */

    /* outline cache */
    Sym   syms[128]; int nsyms; uint32_t syms_ver; int syms_doc;

    /* search-in-project results */
    struct { char path[256]; int line; char text[96]; } *hits; int nhits;
    char  psearch[96]; bool psearch_focus;

    /* settings */
    int   tab_width;
    bool  dark, save_before_run, autosave;
    int   code_px;            /* code font size in the mono font's px units */
    char  status_msg[96]; uint64_t status_t;

    /* test-fixture switches (STUDIO_DEMO) */
    bool  demo;
    bool  demo_extra; int demo_line, demo_c0, demo_c1;

    /* explorer context menu */
    bool  ctx_open; int ctx_x, ctx_y, ctx_node, ctx_hover;

    /* recent files, navigation history */
    char  recent[6][256]; int nrecent;
    struct { char path[256]; size_t caret; } nav[32]; int nav_n, nav_i;

    /* dialogs: second text field, focus, template choice, browser entries */
    char  dlg_text2[256]; int dlg_field, dlg_templ, dlg_arg;
    char  toolbar_q[96];

    int   prob_sel;
    char  term_line[256]; bool term_focus;
    char  cwd[256], cwd_prev[256];
    char  *term; size_t term_len; int term_scroll; bool term_follow;
    char  term_hist[32][256]; int term_hist_n, term_hist_i;
    TermJob job;

    bool  quit;
    bool  dirty_frame;
} App;

extern App G;

/* ---- app / ui ---- */
void app_init(const char *arg);
void app_frame(void);                 /* draw everything */
void app_event(const cos_win_event_t *ev);
void app_tick(void);                  /* per-loop housekeeping (build polling, blink) */
void app_open_path(const char *path);
Doc *app_doc(void);
void app_status(const char *fmt, ...);
void app_do(int cmd);
void app_open_file(const char *path, int line);
void app_close_tab(int i, bool force);
void app_output_clear(void);
void app_output_add(const char *s, size_t n);
void app_output_addf(const char *fmt, ...);
void app_apply_theme(bool dark);
void app_demo_setup(void);
void app_settings_load(void);
void app_settings_save(void);

/* ---- layout (recomputed every frame from the window size and panel state) ---- */
typedef struct { int x, y, w, h; } Rect;
typedef struct {
    int W, H;
    int side_x, side_w;          /* sidebar (side_w == 0 when hidden) */
    int main_x, main_w;          /* tabs + editor + panel column */
    int tab_y, ed_y, ed_h;
    int panel_y, panel_h;        /* panel_h == 0 when hidden; panel_y is the separator line */
    int status_y;
} Layout;
extern Layout L;
void layout_compute(void);
static inline bool pt_in(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

/* ---- edview.c: the editor widget ---- */
float ed_line_h(void);
float ed_cell_w(void);
Rect  ed_rect(void);                                 /* text + gutter area */
void  ed_draw(void);
void  ed_ensure_caret_visible(Doc *d);
void  ed_scroll_by(int dy_px, int dx_px);
size_t ed_pos_at(int x, int y);
bool  ed_mouse_down(int x, int y, uint8_t button, uint8_t mods);
void  ed_mouse_drag(int x, int y);
void  ed_mouse_move(int x, int y);
bool  ed_key(const cos_win_event_t *ev);
bool  ed_find_click(int x, int y);
bool  ed_find_key(const cos_win_event_t *ev);
void  ed_draw_tooltip(void);
int   ed_diag_on_line(const Doc *d, int line1);
int   ed_max_scroll(Doc *d);
void  ed_center_caret(Doc *d);

/* ---- fonts ---- */
cui_font *font_ui(int px);
cui_font *font_ui_bold(int px);
cui_font *font_code_px(int px, bool bold);
cui_font *font_code(void);
cui_font *font_code_bold(void);

/* ---- build.c ---- */
void build_init(void);
void build_start(bool run_after);
void build_stop(void);
void build_poll(void);
void build_clean(void);
enum { QUICK_OBJ, QUICK_PREPROCESS, QUICK_SHARED };
void build_quick(int mode);   /* one-shot action on the active file: -c, -E, or -shared */
const char *build_state_text(char *buf, size_t cap);

/* ---- dialogs.c ---- */
void dlg_open(int kind);
void dlg_close(void);
void dlg_draw(void);
bool dlg_key(const cos_win_event_t *ev);
bool dlg_mouse(int x, int y, uint8_t button, bool down);
void ctx_draw(void);
bool ctx_mouse(int x, int y, bool down);
void ctx_open_at(int x, int y, int node);
void dlg_confirm(const char *title, const char *msg, int action, int arg);

/* ---- ui.c (chrome) ---- */
void ui_draw_all(void);
bool ui_mouse_down(int x, int y, uint8_t button, uint8_t mods);
void ui_mouse_up(int x, int y);
void ui_mouse_drag(int x, int y);
void ui_mouse_move(int x, int y);
void ui_wheel(int x, int y, int delta);
bool ui_key(const cos_win_event_t *ev);
void ui_field(int x, int y, int w, int h, const char *text, bool focus, const char *hint);
void ui_button(int x, int y, int w, int h, const char *label, bool primary, bool hover);
int  menu_shortcut_text(int key, uint8_t mods, char *out, size_t cap);
void menu_close(void);

/* commands */
enum {
    CMD_NONE = 0,
    CMD_NEW, CMD_NEW_PROJECT, CMD_OPEN, CMD_OPEN_FOLDER, CMD_SAVE, CMD_SAVE_ALL, CMD_SAVE_AS, CMD_CLOSE_TAB, CMD_EXIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECT_ALL, CMD_FIND, CMD_REPLACE, CMD_FIND_PROJECT,
    CMD_TOGGLE_COMMENT, CMD_INDENT, CMD_OUTDENT, CMD_DUP_LINE,
    CMD_VIEW_SIDEBAR, CMD_VIEW_PANEL, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_THEME_DARK, CMD_THEME_LIGHT,
    CMD_GOTO_LINE, CMD_GOTO_FILE, CMD_GOTO_SYMBOL, CMD_NEXT_PROBLEM, CMD_PREV_PROBLEM,
    CMD_RUN, CMD_BUILD, CMD_STOP, CMD_CLEAN, CMD_CFG_DEBUG, CMD_CFG_RELEASE,
    CMD_BUILD_OBJ, CMD_BUILD_PREPROCESS, CMD_BUILD_SHARED,
    CMD_OPEN_TERMINAL, CMD_SETTINGS, CMD_SNIPPET_MAIN, CMD_SNIPPET_WINDOW, CMD_SNIPPET_LOOP,
    CMD_SDK_REF, CMD_SHORTCUTS, CMD_ABOUT,
    CMD_NEXT_TAB, CMD_PREV_TAB, CMD_FOCUS_EXPLORER, CMD_FOCUS_SEARCH, CMD_FOCUS_PROBLEMS,
    CMD_NAV_BACK, CMD_NAV_FWD, CMD_QUICKFIX, CMD_PANEL_CLEAR, CMD_PANEL_COPY, CMD_CFG_TOGGLE,
    CMD_NEW_FILE_HERE, CMD_NEW_FOLDER_HERE, CMD_RENAME, CMD_DELETE, CMD_REFRESH,
    CMD_OPEN_RECENT = 200            /* + index */
};

#endif
