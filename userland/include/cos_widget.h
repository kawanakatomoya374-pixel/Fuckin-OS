/* cos_widget.h - retained-mode widget toolkit for C-OS ring-3 applications.
 *
 * What it is for: apps describe a tree of widgets (panel, label, button, checkbox, text box, list, slider) and
 * react to callbacks; the toolkit owns the event loop, hover/press/focus state, layout and painting. It is built
 * so that a GUI living entirely in ring 3 costs almost nothing per interaction:
 *
 *  - Damage tracking. A widget that changes state (hover, press, new text...) invalidates only its own rectangle.
 *    The frame repaints just those rectangles - the widget tree is drawn clipped to each damaged rectangle, so
 *    nothing outside it is touched - and hands exactly those rectangles to the kernel with
 *    cos_win2_present_rects(), which copies only those rows and makes the compositor's frame itself partial. An
 *    idle app does no work at all: it sleeps in the kernel until an event arrives (no polling, no timers unless
 *    you ask for one).
 *  - Setters compare before they invalidate, so assigning an unchanged value is free.
 *  - Events are drained in one batch per wake-up and mouse-move events are coalesced, so a burst of motion is one
 *    hover update and one repaint, not one per event.
 *  - Widgets are plain structs with a small vtable; there is no allocation on the event or paint paths.
 *
 * Minimal program:
 *
 *     cw_app *app = cw_app_create("Demo", 360, 200);
 *     cw_widget *root = cw_app_root(app);
 *     cw_panel_set_layout(root, CW_VBOX, 16, 10);
 *     cw_label_create(root, "Hello", 16, CW_TEXT);
 *     cw_button_create(root, "Quit", on_quit, app);
 *     return cw_app_run(app);
 */
#ifndef COS_WIDGET_H
#define COS_WIDGET_H
#include <stdint.h>
#include <stdbool.h>
#include "cos.h"
#include "cos_ui.h"

typedef struct cw_widget cw_widget;
typedef struct cw_app cw_app;
typedef struct { int x, y, w, h; } cw_rect;

typedef void (*cw_click_fn)(cw_widget *w, void *user);
typedef void (*cw_value_fn)(cw_widget *w, int value, void *user);
typedef void (*cw_text_fn)(cw_widget *w, const char *text, void *user);
typedef void (*cw_timer_fn)(cw_app *app, void *user);

/* colours of the built-in theme */
#define CW_BG        0xF4F6FA
#define CW_PANEL     0xFFFFFF
#define CW_TEXT      0x1E2430
#define CW_MUTED     0x6B7488
#define CW_ACCENT    0x3A6EE6
#define CW_ACCENT_HI 0x5B8CF0
#define CW_ACCENT_LO 0x2B57C2
#define CW_FACE      0xE4E7EE
#define CW_FACE_HI   0xD6DBE6
#define CW_BORDER    0xBEC4D2

enum { CW_VISIBLE = 1, CW_ENABLED = 2, CW_HOVER = 4, CW_PRESSED = 8, CW_FOCUS = 16, CW_FOCUSABLE = 32 };
typedef enum { CW_NONE = 0, CW_VBOX, CW_HBOX } cw_layout;

typedef struct {
    void (*draw)(cw_widget *w, cui_canvas *c, int ox, int oy);                   /* ox,oy: widget origin on the canvas */
    bool (*event)(cw_widget *w, const cos_win_event_t *ev, int lx, int ly);      /* lx,ly: pointer in widget space; true = consumed */
    void (*layout)(cw_widget *w);
    void (*destroy)(cw_widget *w);
} cw_vtable;

struct cw_widget {
    const cw_vtable *vt;
    cw_app *app;
    cw_widget *parent, *first_child, *last_child, *next;
    int x, y, w, h;              /* relative to the parent */
    int weight;                  /* box layout: share of the space left over after fixed-size children (0 = keep h/w) */
    uint32_t flags;
    void *user;
};

/* ---- application ---- */
cw_app *cw_app_create(const char *title, int w, int h);
cw_widget *cw_app_root(cw_app *app);
int  cw_app_run(cw_app *app);                 /* event loop; returns when the window is closed or cw_app_quit() is called */
void cw_app_quit(cw_app *app);
void cw_app_set_timer(cw_app *app, uint32_t interval_ms, cw_timer_fn fn, void *user);   /* 0 = off; the only thing that wakes an idle app */
void cw_app_layout(cw_app *app);              /* re-run layout (done automatically before the first frame) */
void cw_app_focus(cw_app *app, cw_widget *w);
/* counters since start, for measuring: frames painted, rectangles presented, pixels presented */
void cw_app_stats(const cw_app *app, uint32_t *frames, uint32_t *rects, uint64_t *pixels);

/* ---- damage ---- */
void cw_invalidate(cw_widget *w);             /* the widget's whole rectangle */
void cw_invalidate_rect(cw_app *app, int x, int y, int w, int h);   /* window coordinates */
cw_rect cw_widget_abs_rect(const cw_widget *w);

/* ---- generic widget helpers ---- */
void cw_widget_destroy(cw_widget *w);         /* removes it (and its children) from the tree */
void cw_widget_set_visible(cw_widget *w, bool visible);
void cw_widget_set_enabled(cw_widget *w, bool enabled);
void cw_widget_set_geometry(cw_widget *w, int x, int y, int w_, int h_);
void cw_widget_set_weight(cw_widget *w, int weight);

/* ---- panel (also the root) ---- */
cw_widget *cw_panel_create(cw_widget *parent, uint32_t bg /* 0xFFFFFFFF = transparent */);
void cw_panel_set_layout(cw_widget *panel, cw_layout mode, int padding, int gap);
void cw_panel_set_bg(cw_widget *panel, uint32_t bg);

/* ---- label ---- */
cw_widget *cw_label_create(cw_widget *parent, const char *text, int font_px, uint32_t color);
void cw_label_set_text(cw_widget *label, const char *text);

/* ---- button ---- */
cw_widget *cw_button_create(cw_widget *parent, const char *text, cw_click_fn on_click, void *user);
void cw_button_set_text(cw_widget *button, const char *text);

/* ---- checkbox ---- */
cw_widget *cw_checkbox_create(cw_widget *parent, const char *text, bool checked, cw_value_fn on_change, void *user);
bool cw_checkbox_get(const cw_widget *cb);
void cw_checkbox_set(cw_widget *cb, bool checked);

/* ---- single-line text box ---- */
cw_widget *cw_textbox_create(cw_widget *parent, const char *text, int max_len, cw_text_fn on_change, cw_text_fn on_enter, void *user);
const char *cw_textbox_get(const cw_widget *tb);
void cw_textbox_set(cw_widget *tb, const char *text);

/* ---- list ---- */
cw_widget *cw_list_create(cw_widget *parent, cw_value_fn on_select, void *user);
void cw_list_add(cw_widget *list, const char *item);
void cw_list_clear(cw_widget *list);
int  cw_list_selected(const cw_widget *list);          /* -1 = none */
void cw_list_select(cw_widget *list, int index);

/* ---- slider ---- */
cw_widget *cw_slider_create(cw_widget *parent, int min, int max, int value, cw_value_fn on_change, void *user);
int  cw_slider_get(const cw_widget *s);
void cw_slider_set(cw_widget *s, int value);

#endif
