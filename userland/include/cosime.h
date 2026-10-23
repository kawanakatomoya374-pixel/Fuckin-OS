/* cosime.h - the C-OS Japanese IME for ring-3 programs.
 *
 * Romaji -> hiragana, then kana -> kanji conversion with the Mozc open-source dictionary (IPAdic based) and its
 * connection-cost matrix: a Viterbi search over a word lattice, split into bunsetsu with candidate lists.
 * The dictionary is /system/ime/cosime.dic (built by tools/build_ime_dict.py); without it the IME still types kana.
 *
 * Usage: feed every key event to cosime_key(); when it returns COSIME_COMMIT insert `commit` into your text; draw
 * cosime_text() (the composition) at the caret and, while cosime_state() == COSIME_CONVERTING, the candidate list.
 * Keys while composing: Space convert / next candidate, Up/Down candidates, 1-9 pick, Left/Right focus a bunsetsu,
 * Shift+Left/Right resize it, Enter commit, Esc cancel, Backspace edit, F6 hiragana, F7 katakana. */
#ifndef COSIME_H
#define COSIME_H
#include <stdbool.h>
#include <stddef.h>
#include "cos.h"

typedef struct cosime cosime;
enum { COSIME_IDLE = 0, COSIME_COMPOSING = 1, COSIME_CONVERTING = 2 };
enum { COSIME_PASS = 0, COSIME_CONSUMED = 1, COSIME_COMMIT = 2 };   /* flags: COMMIT = insert `commit`; CONSUMED = the key is used up (otherwise handle it too) */

cosime *cosime_new(void);
int  cosime_load(cosime *m, const char *path);       /* 0 on success; slow (decompresses ~55 MB), call once */
bool cosime_loaded(const cosime *m);
void cosime_set_enabled(cosime *m, bool on);          /* turning it off cancels a composition */
bool cosime_enabled(const cosime *m);

/* Returns COSIME_PASS when the key is not for the IME (the caller handles it as usual). */
int  cosime_key(cosime *m, const cos_win_event_t *ev, char *commit, size_t cap);

int  cosime_state(const cosime *m);
const char *cosime_text(cosime *m);                   /* the whole composition (kana + pending romaji, or the chosen candidates) */
int  cosime_nseg(const cosime *m);                    /* converting: number of bunsetsu */
int  cosime_focus(const cosime *m);
const char *cosime_seg_text(const cosime *m, int i);  /* the candidate currently chosen for bunsetsu i */
int  cosime_ncand(const cosime *m);                   /* candidates of the focused bunsetsu */
const char *cosime_cand(const cosime *m, int i);
int  cosime_cand_sel(const cosime *m);
#endif
