/*
 * cos_netsurf_tierb_stubs.c - link-satisfying stubs for the Tier B
 * (box model / layout / redraw) and desktop/ (selection, textarea,
 * scrollbar, browser_window, imagemap) symbols that
 * content/handlers/html/html.c, dom_event.c, and css.c (all real,
 * unmodified upstream Tier A files) reference but that this port
 * doesn't implement - see PORTING_NOTES.md.
 *
 * Why this file exists now: html_init()/nscss_init() were never
 * actually called anywhere in this build until the 2026-08-08 fix in
 * cos_netsurf.c (see PORTING_NOTES.md) - so html.o's own symbol
 * table, including everything below, was never actually pulled into
 * a real link attempt before. This isn't new breakage; it's a
 * pre-existing gap that simply had nothing forcing it to surface
 * until html_init() started being called for real.
 *
 * Two different kinds of stub live here, and it matters which is
 * which:
 *
 *   1. Functions actually reached during a normal content lifecycle
 *      in THIS pipeline (fetch -> parse -> cos_netsurf_render.c reads
 *      the DOM -> release) even with no box tree or window ever
 *      created - html_open/html_close/html_create_html_data/
 *      html_destroy/html_stop/html_finish_conversion/
 *      html_box_convert_done all call into some of these
 *      unconditionally. These need real, careful "do nothing, but
 *      correctly" behaviour - see the comment on dom_to_box() below
 *      in particular, which is the one that matters most: get its
 *      contract wrong and every page load silently fails.
 *   2. Functions only reachable via a real plotter/box tree/window -
 *      mouse tracking, keypress handling, redraw, text selection
 *      dragging, scrollbar dragging. Nothing in this build ever
 *      constructs a box tree or opens a NetSurf-side window (see (1):
 *      dom_to_box() deliberately never builds one), so these
 *      genuinely cannot be called yet. Still need to exist and behave
 *      safely for whoever eventually wires up interaction, but a
 *      minimal "did nothing / not supported" answer is correct, not
 *      just convenient.
 *
 * Every stub here follows cos_urldb_stub.c's precedent: an honest
 * "don't have one / didn't do it" answer, never a fabricated success
 * that could mislead a future caller. Consulted the real declaration
 * in every header cited below before writing each one - not guessed
 * signatures.
 */
/* COS_NETSURF_INCLUDES defines -DPLOT_FONT_FAMILY_SANS_SERIF=1 (for
 * files elsewhere in this module that need it as a plain macro
 * without including netsurf/plot_style.h). That header - pulled in
 * transitively below via netsurf/browser_window.h, needed for the
 * browser_window_*() stubs - declares PLOT_FONT_FAMILY_SANS_SERIF as
 * an *enum value* instead; with the macro still active the
 * preprocessor rewrites that declaration into "1 = 0" and fails to
 * parse. Undefining it here, before any #include, lets the real enum
 * declaration through; nothing in this file needs the macro's value. */
#undef PLOT_FONT_FAMILY_SANS_SERIF

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "utils/errors.h"
#include "utils/libdom.h"            /* dom_node etc. */

#include "netsurf/browser_window.h"
#include "netsurf/url_db.h"          /* struct url_data (not used
                                       * directly here - see
                                       * cos_urldb_stub.c - but pulled
                                       * in transitively by some of the
                                       * headers below) */

#include "content/fetch.h"
#include "content/handlers/html/private.h"      /* html_forms_get_forms;
                                                   * also declares the
                                                   * html_content typedef
                                                   * and html_drag_type/
                                                   * html_selection_type/
                                                   * html_focus_type
                                                   * enums that
                                                   * interaction.h below
                                                   * needs already
                                                   * visible - must come
                                                   * before it. */
#include "content/handlers/html/box.h"          /* box_construct_complete_cb */
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/html/object.h"
#include "content/handlers/html/textselection.h"
#include "content/handlers/html/interaction.h"
#include "content/handlers/html/imagemap.h"
#include "content/handlers/html/form_internal.h"
#include "content/textsearch.h"

#include "desktop/selection.h"
#include "desktop/textarea.h"
#include "desktop/scrollbar.h"
#include "desktop/system_colour.h"

#include "utils/utf8.h"
#include "utils/string.h"            /* squash_whitespace - the
                                       * "utils/" prefix matters: bare
                                       * "string.h" would resolve to
                                       * C-OS's own kernel string.h
                                       * instead (-Isrc/include comes
                                       * first on this build's search
                                       * path) - see cos_netsurf.h's
                                       * comments for the same gotcha
                                       * elsewhere in this module. */

/* ============================================================
 * 1a. dom_to_box() / cancel_dom_to_box() - box tree construction.
 *
 * THE ONE THAT MATTERS MOST. html_finish_conversion() (html.c) calls
 * dom_to_box() unconditionally when parsing finishes, and treats
 * anything other than NSERROR_OK as a hard content error
 * (content_set_error() - which this build's cos_content_callback(),
 * cos_netsurf.c, sees as CONTENT_MSG_ERROR and reports up as a failed
 * page load). dom_to_box() is documented (box_construct.h) as
 * asynchronous: it returns a status immediately and reports the real
 * completion later via `cb`. This stub returns NSERROR_OK and calls
 * `cb` synchronously with success=true before returning - i.e. "box
 * conversion complete, trivially, with an empty box tree" - which is
 * what lets html_box_convert_done() (html.c) proceed through
 * imagemap_extract() (below) to content_set_ready()/
 * html_proceed_to_done()/content_set_done(), which is what actually
 * fires CONTENT_MSG_DONE and lets a page load finish. Traced this
 * whole chain by reading html.c directly before writing this -
 * getting it wrong here means every page load hangs or errors out
 * silently.
 * ============================================================ */
#if 0 /* Superseded by upstream box_construct.c in Tier B step 1. */
nserror dom_to_box(struct dom_node *n, struct html_content *c,
                    box_construct_complete_cb cb,
                    void **box_conversion_context)
{
    (void)n;
    if (box_conversion_context != NULL) {
        *box_conversion_context = NULL;
    }
    if (cb != NULL) {
        cb(c, true);
    }
    return NSERROR_OK;
}

nserror cancel_dom_to_box(void *box_conversion_context)
{
    (void)box_conversion_context;
    return NSERROR_OK;
}
#endif

/* ============================================================
 * 1b. imagemap_extract() - called by html_box_convert_done() right
 * after a successful dom_to_box(), same "must return NSERROR_OK or
 * the page load fails" contract as above. imagemap_get()/_destroy()
 * are the ordinary "no imagemap" / "nothing to free" cases - never
 * actually invoked in this pipeline since nothing ever extracts one.
 * ============================================================ */
#if 0 /* Superseded by upstream imagemap.c. */
nserror imagemap_extract(struct html_content *c)
{
    (void)c;
    return NSERROR_OK;
}

struct nsurl *imagemap_get(struct html_content *c, const char *key,
                            unsigned long x, unsigned long y,
                            unsigned long click_x, unsigned long click_y,
                            const char **target)
{
    (void)c; (void)key; (void)x; (void)y;
    (void)click_x; (void)click_y; (void)target;
    return NULL;
}

void imagemap_destroy(struct html_content *c)
{
    (void)c;
}
#endif

/* ============================================================
 * 1c. html_forms_get_forms() / form_free() / form_gadget_*() -
 * called from html_begin_conversion() (populates htmlc->forms) and
 * html_destroy()/dom_event.c. Returning NULL here just means "no
 * forms found", which every caller already handles (html.c walks
 * htmlc->forms with a plain `for (f = forms; f != NULL; ...)`, so
 * NULL is a normal empty case, not a special one to guard).
 * ============================================================ */
#if 0 /* Superseded by upstream forms.c and form.c. */
struct form *html_forms_get_forms(const char *docenc,
                                   dom_html_document *doc)
{
    (void)docenc; (void)doc;
    return NULL;
}

void form_free(struct form *form)
{
    (void)form;
}

void form_gadget_update_value(struct form_control *control, char *value)
{
    (void)control; (void)value;
}

void form_gadget_sync_with_dom(struct form_control *control)
{
    (void)control;
}
#endif

/* ============================================================
 * 1d. selection_*() - called unconditionally from html_open()/
 * html_close()/html_create_html_data()/html_destroy(). Since
 * selection_create() below returns NULL, every other selection_*()
 * here defensively treats a NULL `s`/`sel` as "nothing to do" rather
 * than dereferencing it - real NetSurf callers likely always check,
 * but there's no reason to depend on that being true everywhere.
 * ============================================================ */
struct selection *selection_create(struct content *c)
{
    (void)c;
    return NULL;
}

void selection_init(struct selection *s)
{
    (void)s;
}

void selection_reinit(struct selection *s)
{
    (void)s;
}

bool selection_clear(struct selection *s, bool redraw)
{
    (void)s; (void)redraw;
    return false;
}

void selection_destroy(struct selection *s)
{
    (void)s;
}

char *selection_get_copy(struct selection *s)
{
    (void)s;
    return NULL;
}

/* ============================================================
 * 1e. layout_document() - content/handlers/html/layout.h. Called from
 * html_reformat() (html.c), which is only invoked once real layout is
 * requested (e.g. a browser_window telling its content "you now have
 * these pixel dimensions, lay yourself out") - never reached in this
 * pipeline for the same reason as 2b/2c below (no browser_window, no
 * plotter, nothing ever asks for a reformat). Missed this one in the
 * first pass at this file - the assembler-substitute link check that
 * caught it is recorded in PORTING_NOTES.md.
 * ============================================================ */
#if 0 /* Superseded by upstream layout.c in Tier B layout cutover. */
bool layout_document(struct html_content *content, int width, int height)
{
    (void)content; (void)width; (void)height;
    return true;
}
#endif

/* ============================================================
 * 2a. html_object_*() - embedded-object (<img>/<object>/<embed>)
 * lifecycle, called unconditionally from html_open()/html_close()/
 * html_destroy()/html_stop()/html_box_convert_done()/
 * html_finish_conversion(). No-ops: this stub's dom_to_box() never
 * discovers any embeddable objects in the first place (there is no
 * box tree to find <img> boxes in), so there is nothing for these to
 * actually open/close/abort/free - but they still need to exist and
 * return success so html.c's own lifecycle functions complete
 * normally instead of erroring out.
 * ============================================================ */
#if 0 /* Superseded by upstream content/handlers/html/object.c. */
nserror html_object_open_objects(struct html_content *html,
                                  struct browser_window *bw)
{
    (void)html; (void)bw;
    return NSERROR_OK;
}

nserror html_object_close_objects(struct html_content *html)
{
    (void)html;
    return NSERROR_OK;
}

nserror html_object_abort_objects(struct html_content *html)
{
    (void)html;
    return NSERROR_OK;
}

nserror html_object_free_objects(struct html_content *html)
{
    (void)html;
    return NSERROR_OK;
}

bool html_fetch_object(struct html_content *c, struct nsurl *url,
                        struct box *box, content_type permitted_types,
                        bool background)
{
    (void)c; (void)url; (void)box; (void)permitted_types; (void)background;
    return false;
}
#endif

/* ============================================================
 * 2b. Mouse/keyboard/redraw/textselection - only reachable through a
 * real plotter and open browser_window, neither of which this build
 * creates (cos_netsurf_render.c reads the DOM directly - see that
 * file). Genuinely dead code in this pipeline today; kept minimal and
 * clearly-labelled rather than fleshed out speculatively.
 * ============================================================ */
#if 0 /* Superseded by upstream interaction.c. */
nserror html_mouse_track(struct content *c, struct browser_window *bw,
                          browser_mouse_state mouse, int x, int y)
{
    (void)c; (void)bw; (void)mouse; (void)x; (void)y;
    return NSERROR_OK;
}

nserror html_mouse_action(struct content *c, struct browser_window *bw,
                           browser_mouse_state mouse, int x, int y)
{
    (void)c; (void)bw; (void)mouse; (void)x; (void)y;
    return NSERROR_OK;
}

bool html_keypress(struct content *c, uint32_t key)
{
    (void)c; (void)key;
    return false;
}
#endif

#if 0 /* Superseded by upstream redraw.c in Tier B redraw cutover. */
bool html_redraw(struct content *c, struct content_redraw_data *data,
                  const struct rect *clip,
                  const struct redraw_context *ctx)
{
    (void)c; (void)data; (void)clip; (void)ctx;
    return true;
}

/* Global, not a function - see content/handlers/html/private.h:
 * "extern bool html_redraw_debug;". html.c's debug-dump keyboard
 * shortcut toggles this; nothing reads it back since html_redraw()
 * above never does a real redraw. */
bool html_redraw_debug = false;
#endif

#if 1 /* textselection.c is deferred; preserve safe no-selection semantics. */
nserror html_textselection_redraw(struct content *c, unsigned start_idx,
                                   unsigned end_idx)
{
    (void)c; (void)start_idx; (void)end_idx;
    return NSERROR_OK;
}

nserror html_textselection_copy(struct content *c, unsigned start_idx,
                                 unsigned end_idx,
                                 struct selection_string *selstr)
{
    (void)c; (void)start_idx; (void)end_idx; (void)selstr;
    return NSERROR_OK;
}

nserror html_textselection_get_end(struct content *c, unsigned *end_idx)
{
    (void)c;
    if (end_idx != NULL) {
        *end_idx = 0;
    }
    return NSERROR_OK;
}
#endif

/* ============================================================
 * 2c. textarea_*() / scrollbar_*() - form widget and scrollbar
 * interaction, same "no box tree, so nothing ever creates one of
 * these to act on" reasoning as 2b above.
 * ============================================================ */
#if 0 /* Superseded by upstream desktop/textarea.c and scrollbar.c. */
bool textarea_scroll(struct textarea *ta, int scrx, int scry)
{
    (void)ta; (void)scrx; (void)scry;
    return false;
}

bool textarea_clear_selection(struct textarea *ta)
{
    (void)ta;
    return false;
}

char *textarea_get_selection(struct textarea *ta)
{
    (void)ta;
    return NULL;
}

bool scrollbar_scroll(struct scrollbar *s, int change)
{
    (void)s; (void)change;
    return false;
}

/* The first box-tree cutover has no native scrollbar object yet. A zero
 * offset is the correct non-scrolled coordinate system and lets upstream
 * box_inspect calculate DOM box positions without fabricating a scrollbar. */
int scrollbar_get_offset(struct scrollbar *s)
{
    (void)s;
    return 0;
}
#endif

/* ============================================================
 * 2d. box_*() inspection helpers - box.h/box_inspect.h/
 * box_construct.h. No box tree ever exists in this pipeline (see
 * 1a), so "not found"/"do nothing" is always the correct answer, not
 * an approximation of one.
 * ============================================================ */
#if 0 /* Superseded by upstream box_inspect.c / box_construct.c. */
void box_coords(struct box *box, int *x, int *y)
{
    (void)box;
    if (x != NULL) { *x = 0; }
    if (y != NULL) { *y = 0; }
}

struct box *box_at_point(const css_unit_ctx *unit_len_ctx, struct box *box,
                          const int x, const int y, int *box_x, int *box_y)
{
    (void)unit_len_ctx; (void)box; (void)x; (void)y;
    (void)box_x; (void)box_y;
    return NULL;
}

void box_dump(FILE *stream, struct box *box, unsigned int depth, bool style)
{
    (void)stream; (void)box; (void)depth; (void)style;
}

struct box *box_find_by_id(struct box *box, lwc_string *id)
{
    (void)box; (void)id;
    return NULL;
}

struct box *box_for_node(struct dom_node *node)
{
    (void)node;
    return NULL;
}
#endif

/* ============================================================
 * 2e. browser_window_*() - netsurf/browser_window.h. No NetSurf-side
 * browser_window is ever created in this build (gui_apps_browser.c's
 * WIN_BROWSER window is a C-OS GUI window, not this) - see
 * PORTING_NOTES.md item 1d.
 * ============================================================ */
#if 0 /* Superseded by upstream desktop/browser_window.c. */
float browser_window_get_scale(struct browser_window *bw)
{
    (void)bw;
    return 1.0f;
}

bool browser_window_scroll_at_point(struct browser_window *bw, int x, int y,
                                     int scrx, int scry)
{
    (void)bw; (void)x; (void)y; (void)scrx; (void)scry;
    return false;
}

nserror browser_window_get_features(struct browser_window *bw, int x, int y,
                                     struct browser_window_features *data)
{
    (void)bw; (void)x; (void)y;
    if (data != NULL) {
        memset(data, 0, sizeof(*data));
    }
    return NSERROR_OK;
}
#endif

/* ============================================================
 * 2f. Misc utils. content_textsearch_*() (find-in-page) and
 * libdom_dump_structure() (debug dump, and the one function in
 * utils/libdom.c actually excluded there via #ifndef COS_KERNEL -
 * confirmed by reading that file - so this genuinely has no
 * implementation anywhere in this tree, not even a disabled one) are
 * both UI-only, same reasoning as 2b/2d. ns_system_colour() backs
 * libcss's lookup for obscure CSS system-colour keywords (ButtonFace
 * etc.) - registered as a callback but never actually invoked, since
 * nothing in this pipeline runs libcss selection matching (that
 * normally happens from inside box construction, which dom_to_box()
 * above never really does). CSS_INVALID (not CSS_OK) because this is
 * an honest "don't have this colour", matching the rest of this
 * file's philosophy, not a fabricated success.
 *
 * squash_whitespace() and utf8_from_ucs4() are different: real,
 * simple, well-defined utility functions with no NetSurf-desktop
 * dependency, just implemented in files this build excludes for
 * unrelated reasons (utils/utils.c needs real POSIX filesystem
 * access elsewhere in the same file - see the Makefile's own comment
 * next to NS_UTILS_FILES). squash_whitespace() is a direct,
 * unmodified port of the real utils/utils.c implementation (read
 * before porting, not reinvented) since html_process_title()
 * (dom_event.c) - part of ordinary title parsing, not a UI-only path
 * - depends on it behaving correctly. utf8_from_ucs4() is UI-only
 * (keyboard input, content/handlers/html/html.c's
 * fire_dom_keyboard_event()) but implemented for real anyway (a
 * correct UTF-8 encoder is short and this way nothing here is subtly
 * wrong if some future change does reach it).
 * ============================================================ */
const char *content_textsearch_find_pattern(const char *string, int s_len,
                                             const char *pattern, int p_len,
                                             bool case_sens,
                                             unsigned int *m_len)
{
    (void)string; (void)s_len; (void)pattern; (void)p_len; (void)case_sens;
    if (m_len != NULL) {
        *m_len = 0;
    }
    return NULL;
}

nserror content_textsearch_add_match(struct textsearch_context *context,
                                      unsigned start_idx, unsigned end_idx,
                                      struct box *start_ptr,
                                      struct box *end_ptr)
{
    (void)context; (void)start_idx; (void)end_idx;
    (void)start_ptr; (void)end_ptr;
    return NSERROR_OK;
}

nserror libdom_dump_structure(dom_node *node, FILE *f, int depth)
{
    (void)node; (void)f; (void)depth;
    return NSERROR_OK;
}

css_error ns_system_colour(void *pw, lwc_string *name, css_color *color)
{
    (void)pw; (void)name; (void)color;
    return CSS_INVALID;
}

/* Direct port of utils/utils.c's real squash_whitespace() (that file
 * itself is excluded from this build for unrelated reasons - see the
 * comment above) - same algorithm, same malloc-based ownership
 * (caller frees), same behaviour on allocation failure (returns
 * NULL). Not reinvented. */
char *squash_whitespace(const char *s)
{
    char *c;
    int i = 0, j = 0;

    if (s == NULL) {
        return NULL;
    }

    c = malloc(strlen(s) + 1);
    if (c != NULL) {
        do {
            if (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
                c[j++] = ' ';
                while (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' ||
                       s[i] == '\t') {
                    i++;
                }
            }
            c[j++] = s[i++];
        } while (s[i - 1] != 0);
    }
    return c;
}

/* Standard UTF-8 encoding of a single Unicode code point, per RFC
 * 3629 - writes 1-4 bytes to `s` (caller-owned, must have room) and
 * returns the byte count. UI-only caller today (see file header
 * comment) but a correct, general implementation regardless. */
size_t utf8_from_ucs4(uint32_t c, char *s)
{
    if (s == NULL) {
        return 0;
    }
    if (c < 0x80) {
        s[0] = (char)c;
        return 1;
    } else if (c < 0x800) {
        s[0] = (char)(0xC0 | (c >> 6));
        s[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    } else if (c < 0x10000) {
        s[0] = (char)(0xE0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        s[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    } else {
        s[0] = (char)(0xF0 | (c >> 18));
        s[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        s[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
}

/* Direct, allocation-compatible port of NetSurf utils/utils.c's
 * cnv_space2nbsp(). box_special.c uses this to keep select option labels
 * intact while the standard box tree is constructed. */
char *cnv_space2nbsp(const char *s)
{
    if (s == NULL) return NULL;
    const char *src;
    unsigned int spaces = 0;
    for (src = s; *src != '\0'; ++src) {
        if (*src == ' ' || *src == '\t') ++spaces;
    }
    char *out = malloc((size_t)(src - s) + spaces + 1u);
    if (out == NULL) return NULL;
    char *dst = out;
    for (src = s; *src != '\0'; ++src) {
        if (*src == ' ' || *src == '\t') {
            *dst++ = (char)0xC2;
            *dst++ = (char)0xA0;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
    return out;
}

/* C-OS GUI and NetSurf DOM use UTF-8 end-to-end. For form submission this
 * preserves bytes for UTF-8 documents (including Google's UTF-8 forms) and
 * provides the ownership/error contract expected by form.c. */
nserror utf8_to_enc(const char *string, const char *encname,
                    size_t len, char **result)
{
    (void)encname;
    if (result == NULL || string == NULL) return NSERROR_BAD_PARAMETER;
    if (len == 0) len = strlen(string);
    char *copy = malloc(len + 1u);
    if (copy == NULL) return NSERROR_NOMEM;
    memcpy(copy, string, len);
    copy[len] = '\0';
    *result = copy;
    return NSERROR_OK;
}

/* UTF-8 cursor helpers required by the upstream textarea implementation.
 * They operate on byte offsets while treating malformed leading bytes as a
 * single-byte character, which keeps editing robust for arbitrary content. */
static size_t cos_utf8_step(const char *s, size_t left)
{
    if (s == NULL || left == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && left >= 2 && ((unsigned char)s[1] & 0xC0) == 0x80) return 2;
    if ((c & 0xF0) == 0xE0 && left >= 3 && ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80) return 3;
    if ((c & 0xF8) == 0xF0 && left >= 4 && ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80 && ((unsigned char)s[3] & 0xC0) == 0x80) return 4;
    return 1;
}
size_t utf8_length(const char *s)
{
    if (s == NULL) return 0;
    size_t bytes = strlen(s), off = 0, count = 0;
    while (off < bytes) { off += cos_utf8_step(s + off, bytes - off); ++count; }
    return count;
}
size_t utf8_bounded_length(const char *s, size_t limit)
{
    if (s == NULL) return 0;
    size_t off = 0, count = 0;
    while (off < limit && s[off] != '\0') { off += cos_utf8_step(s + off, limit - off); ++count; }
    return count;
}
size_t utf8_bounded_byte_length(const char *s, size_t limit, size_t chars)
{
    if (s == NULL) return 0;
    size_t off = 0;
    while (off < limit && s[off] != '\0' && chars-- != 0) off += cos_utf8_step(s + off, limit - off);
    return off;
}
size_t utf8_next(const char *s, size_t limit, size_t off)
{
    if (s == NULL || off >= limit || s[off] == '\0') return off;
    size_t step = cos_utf8_step(s + off, limit - off);
    return (off + step <= limit) ? off + step : limit;
}
size_t utf8_prev(const char *s, size_t off)
{
    if (s == NULL || off == 0) return 0;
    --off;
    while (off > 0 && (((unsigned char)s[off] & 0xC0) == 0x80)) --off;
    return off;
}

/* Minimal system palette for CSS system-colour lookups used by native form
 * controls. Values retain NetSurf's XBGR channel ordering. */
nserror ns_system_colour_char(const char *name, colour *out)
{
    if (name == NULL || out == NULL) return NSERROR_BAD_PARAMETER;
    if (strcasecmp(name, "ButtonText") == 0 || strcasecmp(name, "WindowText") == 0) *out = 0x000000;
    else if (strcasecmp(name, "Highlight") == 0) *out = 0xFF9933;
    else if (strcasecmp(name, "HighlightText") == 0) *out = 0xFFFFFF;
    else if (strcasecmp(name, "GrayText") == 0) *out = 0x808080;
    else if (strcasecmp(name, "Scrollbar") == 0 || strcasecmp(name, "ButtonFace") == 0) *out = 0xD9D9D9;
    else *out = 0xFFFFFF;
    return NSERROR_OK;
}

/* These interfaces are replaced by interaction.c and browser_window.c in the
 * next cutover. Keeping them explicit prevents the newly-real box/form tree
 * from silently using an unsafe fake drag or selection implementation. */
#if 0 /* Superseded by upstream interaction.c. */
void html_overflow_scroll_callback(void *client_data, struct scrollbar_msg_data *data)
{ (void)client_data; (void)data; }
void html_set_drag_type(html_content *html, html_drag_type type,
                        union html_drag_owner owner, const struct rect *rect)
{ (void)html; (void)type; (void)owner; (void)rect; }
void html_set_selection(html_content *html, html_selection_type type,
                        union html_selection_owner owner, bool read_only)
{ (void)html; (void)type; (void)owner; (void)read_only; }
void html_set_focus(html_content *html, html_focus_type type,
                    union html_focus_owner owner, bool hide_caret,
                    int x, int y, int height, const struct rect *clip)
{ (void)html; (void)type; (void)owner; (void)hide_caret; (void)x; (void)y; (void)height; (void)clip; }
#endif
#if 0 /* Superseded by upstream desktop/browser_window.c. */
void browser_window_set_drag_type(struct browser_window *bw,
                                  browser_drag_type type, const struct rect *rect)
{ (void)bw; (void)type; (void)rect; }
nserror browser_window_navigate(struct browser_window *bw, struct nsurl *url,
        struct nsurl *referrer, enum browser_window_nav_flags flags,
        char *post_urlenc, struct fetch_multipart_data *post_multipart,
        struct hlcache_handle *parent)
{
    (void)bw; (void)url; (void)referrer; (void)flags;
    (void)post_urlenc; (void)post_multipart; (void)parent;
    return NSERROR_NOT_IMPLEMENTED;
}
#endif

/* redraw.c exposes these printing-mode switches as extern globals. C-OS's
 * browser frontend is a display plotter, never a print renderer. */
bool html_redraw_printing = false;
bool html_redraw_printing_border = false;
bool html_redraw_printing_top_cropped = false;

/* Highlighting is intentionally inactive until find-in-page and selection
 * ownership are connected to the C-OS GUI. This preserves ordinary redraw. */
bool selection_highlighted(const struct selection *s, unsigned start,
                           unsigned end, unsigned *start_idx,
                           unsigned *end_idx)
{
    (void)s; (void)start; (void)end; (void)start_idx; (void)end_idx;
    return false;
}
bool content_textsearch_ishighlighted(struct textsearch_context *textsearch,
        unsigned start_offset, unsigned end_offset,
        unsigned *start_idx, unsigned *end_idx)
{
    (void)textsearch; (void)start_offset; (void)end_offset;
    (void)start_idx; (void)end_idx;
    return false;
}

/* Iframes are parsed into boxes; nested browser windows are not created in
 * the initial display cutover, so these operations remain benign no-ops. */
#if 0 /* Superseded by upstream desktop/browser_window.c. */
void browser_window_set_position(struct browser_window *bw, int x, int y)
{ (void)bw; (void)x; (void)y; }
void browser_window_set_dimensions(struct browser_window *bw, int width, int height)
{ (void)bw; (void)width; (void)height; }
void browser_window_reformat(struct browser_window *bw, bool background,
                             int width, int height)
{ (void)bw; (void)background; (void)width; (void)height; }
bool browser_window_redraw(struct browser_window *bw, int x, int y,
                           const struct rect *clip,
                           const struct redraw_context *ctx)
{ (void)bw; (void)x; (void)y; (void)clip; (void)ctx; return true; }
#endif

/* C-OS fetchers do not use NetSurf's curl-based content/fetch.c. These
 * allocation-compatible helpers preserve the multipart ownership contract
 * used by browser_window.c and form.c until native multipart upload is added. */
nserror fetch_multipart_data_new_kv(struct fetch_multipart_data **list,
                                    const char *name, const char *value)
{
    if (list == NULL || name == NULL || value == NULL) return NSERROR_BAD_PARAMETER;
    for (struct fetch_multipart_data *p = *list; p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0) return NSERROR_OK;
    }
    struct fetch_multipart_data *item = calloc(1, sizeof(*item));
    if (item == NULL) return NSERROR_NOMEM;
    item->name = strdup(name);
    item->value = strdup(value);
    if (item->name == NULL || item->value == NULL) {
        if (item->name) free(item->name);
        if (item->value) free(item->value);
        free(item);
        return NSERROR_NOMEM;
    }
    item->next = *list;
    *list = item;
    return NSERROR_OK;
}
const char *fetch_multipart_data_find(const struct fetch_multipart_data *list,
                                      const char *name)
{
    if (name == NULL) return NULL;
    for (const struct fetch_multipart_data *p = list; p != NULL; p = p->next) {
        if (p->name != NULL && strcmp(p->name, name) == 0) return p->value;
    }
    return NULL;
}

/* C-OS does not yet expose a NetSurf-specific caret overlay or history/hotlist
 * panel. These retain browser_window lifecycle correctness without creating
 * hidden state outside the C-OS GUI. */
/* browser_window_place_caret/remove_caret are supplied by the upstream
 * desktop/textinput.c implementation enabled for standard form editing. */
nserror global_history_add(struct nsurl *url)
{ return (url != NULL) ? NSERROR_OK : NSERROR_BAD_PARAMETER; }
void hotlist_update_url(struct nsurl *url)
{ (void)url; }

/* Selection UI is deliberately deferred; interaction.c still needs these
 * operations to answer consistently while ordinary links and form widgets
 * remain fully interactive. */
bool selection_active(struct selection *s) { (void)s; return false; }
bool selection_dragging(struct selection *s) { (void)s; return false; }
bool selection_dragging_start(struct selection *s) { (void)s; return false; }
void selection_drag_end(struct selection *s) { (void)s; }
void selection_select_all(struct selection *s) { (void)s; }
void selection_set_position(struct selection *s, unsigned start, unsigned end)
{ (void)s; (void)start; (void)end; }
bool selection_click(struct selection *s, struct browser_window *top,
                     browser_mouse_state mouse, unsigned idx)
{ (void)s; (void)top; (void)mouse; (void)idx; return false; }
void selection_track(struct selection *s, browser_mouse_state mouse, unsigned idx)
{ (void)s; (void)mouse; (void)idx; }
bool selection_copy_to_clipboard(struct selection *s) { (void)s; return false; }
