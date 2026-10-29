/* cosime_ui.h - Japanese input for any ring-3 program, on top of cosime.h.
 *
 * Add it to a program in four steps:
 *   1. link userland/lib/cosime.c and userland/lib/cosime_ui.c;
 *   2. imeui_set_redraw(fn) once, with a function that redraws and presents the window (used while the dictionary loads);
 *   3. feed every COS_EV_KEY to imeui_key() first:
 *          char commit[1024]; int r = imeui_key(&ev, commit, sizeof commit);
 *          if (r & COSIME_COMMIT) insert_text(commit);          // the converted text
 *          if (r & COSIME_CONSUMED) continue;                    // the IME used the key
 *   4. at the end of your drawing call imeui_draw(&canvas, font, caret_x, caret_y, line_height): the composition and the
 *      candidate list float next to the caret, and imeui_indicator() shows the あ / A state wherever you like.
 * Ctrl+Space (and Ctrl+\ for keyboards without it) switches Japanese input on and off. The dictionary
 * (/system/ime/cosime.dic) loads the first time a letter is typed with the IME on; until then, and without the file, kana typing works. */
#ifndef COSIME_UI_H
#define COSIME_UI_H
#include "cosime.h"
#include "cos_ui.h"

void imeui_set_redraw(void (*redraw)(void));
bool imeui_on(void);
void imeui_set(bool on);
void imeui_toggle(void);
bool imeui_composing(void);                                   /* a composition is in progress */
int  imeui_key(const cos_win_event_t *ev, char *commit, size_t cap);
int  imeui_key_gated(const cos_win_event_t *ev, bool active, char *commit, size_t cap);   /* active=false: only the on/off key is looked at */
void imeui_cancel(void);                                      /* drop the composition (a click moved the caret, ...) */
void imeui_draw(cui_canvas *c, cui_font *f, int caret_x, int caret_y, int line_h);
bool imeui_last_rect(int *x, int *y, int *w, int *h);          /* the rectangle the last imeui_draw() covered (false: it drew nothing) - for programs whose pixels are rewritten from outside */
size_t imeui_utf8_prev(const char *s, size_t pos);                  /* byte offset of the character before pos (for Backspace) */
void imeui_indicator(cui_canvas *c, cui_font *f, int x, int y);   /* a small あ chip while Japanese input is on */
#endif
