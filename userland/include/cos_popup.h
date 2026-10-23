/* cos_popup.h - the right-click (context) menu shared by the C-OS apps.
 *
 * An app fills a menu with cpop_add / cpop_sep, opens it at the pointer with cpop_show, draws it last (on top of everything) with
 * cpop_draw, and routes the pointer and keys through cpop_mouse_down / cpop_mouse_move / cpop_key while it is open. Labels are copied, so
 * the caller may build them in temporary buffers. The menu keeps itself inside the window.
 *
 *   cpop_clear(&m); cpop_add(&m, "Copy", "Ctrl+C", CMD_COPY, can_copy, false); cpop_sep(&m); ...
 *   cpop_show(&m, ev->x, ev->y, WIN_W, WIN_H, f, fs);
 *   -- in the mouse handler: if (m.open) { int c = cpop_mouse_down(&m, x, y); if (c >= 0) run(c); return; }
 *   -- in the key handler:   if (m.open) { int c = cpop_key(&m, special); if (c != CPOP_NONE) { if (c >= 0) run(c); return; } }
 *   -- last in the draw:     cpop_draw(&m, canvas, &theme);
 */
#ifndef COS_POPUP_H
#define COS_POPUP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"

#define CPOP_MAX 28
#define CPOP_NONE (-1)      /* cpop_mouse_down / cpop_key: not handled by the menu */
#define CPOP_HANDLED (-2)   /* handled, nothing chosen (menu may still be open) */

typedef struct { uint32_t panel, border, text, muted, accent, accent_text; } cpop_theme;
typedef struct { char label[72]; char key[24]; int cmd; uint8_t enabled, sep, check; } cpop_item;
typedef struct {
    bool open;
    int x, y, w, h, n, hover;
    cpop_item it[CPOP_MAX];
    cui_font *f, *fs;
} cpop_menu;

static inline void cpop_log_num(int v) { char b[16]; int n = 0; if (v == 0) b[n++] = '0'; else { char t[16]; int k = 0; while (v > 0) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) b[n++] = t[--k]; } b[n] = 0; cos_puts(b); }
static inline void cpop_log_chose(const cpop_item *i) { cos_puts("[CTXMENU] chose "); cos_puts(i->label); cos_puts("\n"); }
static inline void cpop_clear(cpop_menu *m) { m->open = false; m->n = 0; m->hover = -1; }
static inline void cpop_add(cpop_menu *m, const char *label, const char *key, int cmd, bool enabled, bool checked) {
    if (m->n >= CPOP_MAX) return;
    cpop_item *i = &m->it[m->n++]; memset(i, 0, sizeof *i);
    strncpy(i->label, label ? label : "", sizeof i->label - 1);
    strncpy(i->key, key ? key : "", sizeof i->key - 1);
    i->cmd = cmd; i->enabled = enabled; i->check = checked;
}
static inline void cpop_sep(cpop_menu *m) {
    if (m->n == 0 || m->it[m->n - 1].sep || m->n >= CPOP_MAX) return;   /* no leading or doubled separators */
    cpop_item *i = &m->it[m->n++]; memset(i, 0, sizeof *i); i->sep = 1;
}
static inline void cpop_trim(cpop_menu *m) { while (m->n > 0 && m->it[m->n - 1].sep) --m->n; }

#define CPOP_ROW 28
#define CPOP_SEP 9
static inline int cpop_item_y(const cpop_menu *m, int i) {
    int cy = m->y + 5;
    for (int k = 0; k < i; ++k) cy += m->it[k].sep ? CPOP_SEP : CPOP_ROW;
    return cy;
}
/* Opens the menu with the top-left corner at (x, y), moved so that it fits in a ww x wh window. f / fs: label and shortcut fonts. */
static inline void cpop_show(cpop_menu *m, int x, int y, int ww, int wh, cui_font *f, cui_font *fs) {
    cpop_trim(m);
    m->f = f; m->fs = fs;
    int mw = 150;
    for (int i = 0; i < m->n; ++i) {
        if (m->it[i].sep) continue;
        int iw = cui_text_width(f, m->it[i].label) + (m->it[i].key[0] ? cui_text_width(fs, m->it[i].key) + 44 : 34) + 22;
        if (iw > mw) mw = iw;
    }
    int hh = 10;
    for (int i = 0; i < m->n; ++i) hh += m->it[i].sep ? CPOP_SEP : CPOP_ROW;
    m->w = mw; m->h = hh;
    if (x + mw > ww - 4) x = ww - mw - 4;
    if (y + hh > wh - 4) y = y - hh > 4 ? y - hh : wh - hh - 4;      /* flip above the pointer when there is no room below */
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    m->x = x; m->y = y;
    m->hover = -1;
    m->open = m->n > 0;
    if (m->open) { cos_puts("[CTXMENU] open items="); cpop_log_num(m->n); cos_puts(" first="); cos_puts(m->it[0].label); cos_puts("\n"); }   /* what the tests read */
}
static inline void cpop_close(cpop_menu *m) { m->open = false; m->hover = -1; }
static inline bool cpop_inside(const cpop_menu *m, int x, int y) { return x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h; }
/* the item index under (x, y), -1 for none (separators and outside) */
static inline int cpop_item_at(const cpop_menu *m, int x, int y) {
    if (!cpop_inside(m, x, y)) return -1;
    int cy = m->y + 5;
    for (int i = 0; i < m->n; ++i) {
        int ih = m->it[i].sep ? CPOP_SEP : CPOP_ROW;
        if (y >= cy && y < cy + ih) return m->it[i].sep ? -1 : i;
        cy += ih;
    }
    return -1;
}
static inline bool cpop_mouse_move(cpop_menu *m, int x, int y) {
    if (!m->open) return false;
    int h = cpop_item_at(m, x, y);
    m->hover = h;
    return true;
}
/* A button press while the menu is open. Returns the chosen command (>= 0), CPOP_HANDLED when the press was used (a disabled item,
 * a separator, or a click outside which only closes the menu). */
static inline int cpop_mouse_down(cpop_menu *m, int x, int y) {
    if (!m->open) return CPOP_NONE;
    int i = cpop_item_at(m, x, y);
    if (i >= 0 && m->it[i].enabled) { int c = m->it[i].cmd; cpop_log_chose(&m->it[i]); cpop_close(m); return c; }
    if (cpop_inside(m, x, y)) return CPOP_HANDLED;
    cpop_close(m);
    return CPOP_HANDLED;
}
static inline int cpop_next(const cpop_menu *m, int from, int dir) {
    for (int k = 1; k <= m->n; ++k) {
        int i = ((from + dir * k) % m->n + m->n) % m->n;
        if (!m->it[i].sep && m->it[i].enabled) return i;
    }
    return -1;
}
/* Up / Down / Home / End / Enter / Esc. Returns the chosen command, CPOP_HANDLED for a key the menu used (every key is swallowed
 * while the menu is open so that nothing is typed behind it), CPOP_NONE when the menu is closed. */
static inline int cpop_key(cpop_menu *m, int special) {
    if (!m->open) return CPOP_NONE;
    if (special == COS_KEY_ESC) { cpop_close(m); return CPOP_HANDLED; }
    if (special == COS_KEY_DOWN) { m->hover = cpop_next(m, m->hover < 0 ? -1 : m->hover, 1); return CPOP_HANDLED; }
    if (special == COS_KEY_UP) { m->hover = cpop_next(m, m->hover < 0 ? 0 : m->hover, -1); return CPOP_HANDLED; }
    if (special == COS_KEY_HOME) { m->hover = cpop_next(m, -1, 1); return CPOP_HANDLED; }
    if (special == COS_KEY_END) { m->hover = cpop_next(m, 0, -1); return CPOP_HANDLED; }
    if (special == COS_KEY_ENTER) {
        if (m->hover >= 0 && m->it[m->hover].enabled) { int c = m->it[m->hover].cmd; cpop_log_chose(&m->it[m->hover]); cpop_close(m); return c; }
        return CPOP_HANDLED;
    }
    return CPOP_HANDLED;
}
static inline void cpop_draw(const cpop_menu *m, cui_canvas *c, const cpop_theme *t) {
    if (!m->open) return;
    cui_unclip(c);
    cui_round_rect_alpha(c, m->x - 1, m->y + 3, m->w + 2, m->h + 2, 9, 0x000000, 70);
    cui_round_rect(c, m->x - 1, m->y - 1, m->w + 2, m->h + 2, 8, t->border);
    cui_round_rect(c, m->x, m->y, m->w, m->h, 7, t->panel);
    int cy = m->y + 5;
    int fh = cui_font_height(m->f);
    for (int i = 0; i < m->n; ++i) {
        const cpop_item *it = &m->it[i];
        if (it->sep) { cui_fill(c, m->x + 10, cy + 4, m->w - 20, 1, t->border); cy += CPOP_SEP; continue; }
        bool hv = i == m->hover && it->enabled;
        if (hv) cui_round_rect(c, m->x + 5, cy, m->w - 10, CPOP_ROW - 2, 5, t->accent);
        uint32_t col = !it->enabled ? cui_mix(t->muted, t->panel, 90) : hv ? t->accent_text : t->text;
        if (it->check) {
            cui_line(c, (float)(m->x + 13), (float)(cy + 13), (float)(m->x + 17), (float)(cy + 17), 1.8f, col);
            cui_line(c, (float)(m->x + 17), (float)(cy + 17), (float)(m->x + 24), (float)(cy + 8), 1.8f, col);
        }
        cui_text(c, m->f, m->x + 32, cy + (CPOP_ROW - 2 - fh) / 2, it->label, col);
        if (it->key[0]) cui_text(c, m->fs, m->x + m->w - 14 - cui_text_width(m->fs, it->key), cy + (CPOP_ROW - 2 - cui_font_height(m->fs)) / 2, it->key,
                                 hv ? cui_mix(t->accent_text, t->accent, 40) : t->muted);
        cy += CPOP_ROW;
    }
}

#endif
