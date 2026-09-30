/* files.h - "Files", the C-OS File Manager as a ring-3 .c-os program.
 *
 * It replaces the in-kernel file manager (src/gui/file_manager). The feature set is
 * the same - folder tree, places bar, breadcrumb, Details/List/Icons/Thumbnails views,
 * sorting, hidden files, preview pane, search, bookmarks, cut/copy/paste/delete/rename,
 * new folder/file, context menus, English/Japanese - redrawn with TrueType text and
 * anti-aliased vector icons instead of the old bitmap font, and extended with rubber-band
 * selection, drag and drop, a background copy/move/delete job with progress, conflict
 * handling, a properties dialog and a system-wide clipboard. */
#ifndef FILES_H
#define FILES_H
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

#define WIN_W      960
#define WIN_H      620
#define MENU_H     26
#define TOOL_Y     26
#define TOOL_H     44
#define PLACES_Y   70
#define PLACES_H   34
#define ADDR_Y     104
#define ADDR_H     36
#define BODY_Y     140
#define STATUS_H   28
#define TREE_W     220
#define PREV_W     264
#define HDR_H      28
#define ROW_H      26

#define MAX_ENT    1024
#define MAX_TREE   500
#define MAX_MARK   16
#define MAX_HIST   40
#define MAX_THUMB  96
#define MAX_TASKS  6000
#define PATHN      512

enum { V_DETAILS, V_LIST, V_ICONS, V_THUMBS };
enum { K_FOLDER, K_IMAGE, K_AUDIO, K_VIDEO, K_CODE, K_ARCHIVE, K_EXEC, K_TEXT, K_DOC, K_OTHER };
enum { S_NAME, S_SIZE, S_DATE, S_TYPE };

typedef struct { char name[256]; uint64_t size, mtime; uint8_t is_dir, attr, kind, sel; } Ent;
typedef struct { char path[PATHN]; char name[96]; int depth; bool open; } TNode;
typedef struct { char name[48]; char path[256]; bool builtin; } Mark;
typedef struct { char path[PATHN]; uint64_t mtime; int w, h; uint32_t *px; uint64_t used; uint8_t state; } Thumb;   /* state: 0 free, 1 ready, 2 failed */
typedef struct { int x, y, w, h; } R;

typedef struct { uint32_t bg, panel, side, tool, border, text, muted, accent, hover, select, danger, warn; } Pal;
extern Pal P;

/* ---- clipboard, jobs ---- */
enum { OP_MKDIR, OP_COPY, OP_DEL };
typedef struct { char src[PATHN]; char dst[PATHN]; uint64_t size; uint8_t op, is_dir; } Task;
typedef struct {
    int mode;                    /* 0 idle, 1 copy, 2 move, 3 delete */
    Task *t; int nt, cap, cur;
    uint64_t total, done;
    int in_fd, out_fd; uint64_t off;
    int policy;                  /* 0 ask, 1 replace all, 2 skip all, 3 keep both */
    bool waiting;                /* a conflict is waiting for the user */
    bool cancel;
    int errors, files_done, files_total;
    char cur_name[256];
    char conflict_dst[PATHN];
    char dstdir[PATHN];
    uint64_t t0;
} Job;

/* ---- dialogs ---- */
enum { D_NONE, D_INPUT, D_DELETE, D_PROPS, D_CONFLICT, D_PROGRESS, D_ABOUT };
enum { IN_RENAME, IN_NEWFOLDER, IN_NEWFILE };

typedef struct {
    bool jp, dark;
    int64_t win; cos_win_info_t info; cui_canvas cv;

    char path[PATHN];
    Ent *all; int nall;
    int *vis; int nvis;
    bool truncated;

    int view, sort, sort_asc;
    bool show_hidden, preview, tree_vis;
    char search[96]; bool search_focus;

    char hist[MAX_HIST][PATHN]; int hn, hi;

    TNode tree[MAX_TREE]; int ntree; char topen[64][PATHN]; int ntopen; int tree_scroll;
    Mark marks[MAX_MARK]; int nmarks;

    int scroll, focus, anchor;
    int hover;                    /* visible index under the pointer, or -1 */
    int mx, my; bool mdown; uint8_t mbtn;
    uint64_t click_t; int click_x, click_y, click_vi;
    int drag_kind;                /* 0 none, 1 rubber band, 2 file drag, 3 scrollbar, 4 splitter (unused) */
    int drag_x0, drag_y0, drag_pending; bool drag_copy;
    int scroll_drag_off;
    char ta[64]; int ta_n; uint64_t ta_t;   /* type-ahead */

    /* address bar */
    bool addr_edit; char addr[PATHN];

    /* menus */
    int menu_open, menu_hover;
    bool ctx_open; int ctx_x, ctx_y, ctx_hover, ctx_kind;   /* 0 item, 1 background, 2 place chip, 3 tree node */
    int ctx_target;
    int tip_btn; uint64_t tip_t;

    /* dialogs */
    int dlg, dlg_in; char dlg_text[256]; char dlg_target[PATHN];
    int dlg_sel0, dlg_sel1;   /* selection range over dlg_text, sel0==sel1 means no selection (caret at sel0) */
    int dlg_btn_hover;
    bool dlg_all;
    uint64_t props_files, props_dirs, props_bytes; Ent props_ent; char props_path[PATHN];

    Job job;

    char status[160]; uint64_t status_t;
    uint64_t sp_total, sp_free; uint64_t sp_t;

    Thumb thumbs[MAX_THUMB]; uint64_t thumb_clock;
    Thumb pv; int pv_ent_hash;
    char pv_text[1400]; int pv_text_n; char pv_text_path[PATHN];

    bool quit, dirty;
    uint64_t caret_t;
} App;
extern App G;

#define L(en, ja) (G.jp ? (ja) : (en))

/* core.c */
void   app_init_state(void);
void   apply_theme(void);
cui_font *FN(int px, bool bold);
void   status(const char *fmt, ...);
void   path_join(char *out, size_t cap, const char *a, const char *b);
const char *path_base(const char *p);
void   path_parent(const char *p, char *out, size_t cap);
void   path_normalize(const char *in, char *out, size_t cap);
bool   is_dir_path(const char *p);
bool   exists_path(const char *p);
int    kind_of(const char *name, bool is_dir);
const char *kind_name(int kind);
void   fmt_size(uint64_t n, char *out, size_t cap);
void   fmt_date(uint64_t t, char *out, size_t cap);
void   dir_load(bool keep_sel);
void   rebuild_view(void);
void   nav_to(const char *path, bool push);
void   nav_back(void); void nav_forward(void); void nav_up(void);
int    sel_count(void);
void   sel_clear(void);
void   sel_set_only(int vi);
void   sel_range(int a, int b, bool add);
Ent   *ent_at(int vi);
void   tree_rebuild(void);
void   tree_toggle(int i);
void   tree_reveal(const char *path);
void   marks_default(void);
void   marks_add(const char *name, const char *path);
void   marks_remove(int i);
void   config_load(void);
void   config_save(void);
void   unique_name(const char *dir, const char *name, char *out, size_t cap);
bool   op_mkdir(const char *name, char *created, size_t cap);
bool   op_newfile(const char *name, char *created, size_t cap);
bool   op_rename(const char *from_path, const char *new_name);
void   clip_put(bool cut);
void   clip_paste(void);
bool   clip_has_files(void);
void   copy_path_text(void);
void   job_start_delete(void);
void   job_start_transfer(int mode, char (*srcs)[PATHN], int n, const char *dstdir);
void   job_tick(void);
void   job_answer(int policy, bool all);
void   job_cancel(void);
void   dir_stats(const char *path, uint64_t *files, uint64_t *dirs, uint64_t *bytes);
void   refresh_space(void);
Thumb *thumb_lookup(const Ent *e);
void   thumb_load_pending(void);
Thumb *preview_image(const Ent *e);
const char *preview_text(const Ent *e);
void   blit_argb(cui_canvas *c, int x, int y, const uint32_t *px, int w, int h);
void   open_entry(int vi);
void   open_path_external(const char *path);
void   fm_set_title(void);
void   dir_poll(void);

/* ui.c */
void   ui_draw(void);
R      center_rect(void);
R      tree_rect(void);
R      prev_rect(void);
R      content_rect(void);
int    hit_item(int x, int y);
bool   item_rect(int vi, R *out);
int    content_height(void);
void   ensure_visible(int vi);
void   draw_icon(int kind, int x, int y, int s);
int    nav_cols(void);
bool   action_enabled(int a);
int    menubar_hit(int x, int y);
int    menu_item_hit(int m, int x, int y, int *index);      /* action, 0 = disabled/separator, -1 = outside */
int    ctx_hit(int x, int y, int *index);
int    tb_hit(int x, int y);
int    place_hit(int x, int y);
int    crumb_hit(int x, int y);                             /* index, -2 = empty part of the bar, -1 = outside */
const char *crumb_path(int i);
bool   search_hit(int x, int y);
bool   addr_hit(int x, int y);
int    tree_hit(int x, int y, bool *on_arrow);
int    header_hit(int x, int y);
bool   scrollbar_hit(int x, int y, int *thumb_y, int *thumb_h);
int    dlg_hit(int x, int y);

/* input.c */
void   input_event(const cos_win_event_t *ev);
void   do_action(int a);
void   open_dialog(int kind);
void   start_rename(int vi);

enum {
    A_NONE, A_OPEN, A_NEW_FOLDER, A_NEW_FILE, A_RENAME, A_DELETE, A_PROPS, A_CLOSE,
    A_CUT, A_COPY, A_PASTE, A_SELECT_ALL, A_COPY_PATH, A_INVERT,
    A_VIEW_DETAILS, A_VIEW_LIST, A_VIEW_ICONS, A_VIEW_THUMBS, A_TOGGLE_PREVIEW, A_TOGGLE_HIDDEN, A_TOGGLE_TREE, A_REFRESH,
    A_SORT_NAME, A_SORT_SIZE, A_SORT_DATE, A_SORT_TYPE, A_SORT_ORDER,
    A_BACK, A_FORWARD, A_UP, A_ADD_MARK, A_REMOVE_MARK, A_FOCUS_ADDR, A_FOCUS_SEARCH,
    A_GO_ROOT, A_GO_STUDIO, A_GO_DESKTOP, A_GO_DOCS, A_GO_MUSIC, A_GO_PICTURES, A_GO_DOWNLOADS, A_GO_BIN,
    A_ABOUT, A_SEP,
    A_OPEN_STUDIO_HERE
};
#endif
