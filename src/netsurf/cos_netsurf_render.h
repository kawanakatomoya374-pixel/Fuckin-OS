/*
 * cos_netsurf_render.h - GUI-facing declarations for real-DOM-backed
 * page rendering (cos_netsurf_render.c).
 *
 * This is a SEPARATE file from cos_netsurf.h on purpose: cos_netsurf.h
 * needs netsurf/utils/errors.h for its nserror-returning declarations,
 * which only resolves under COS_NETSURF_INCLUDES (the build rule
 * used for files under src/netsurf/) - a plain kernel/GUI file like
 * gui_apps_browser.c, built with the general $(INCLUDES) only, cannot
 * include it (this was tried and confirmed to fail: "fatal error:
 * utils/errors.h: No such file or directory"). Everything here uses
 * only plain C types (<stddef.h>'s size_t, int, char, enum) so GUI
 * code can include this header with no extra include-path dependency
 * at all.
 *
 * Closes the gap PORTING_NOTES.md described as item 3 ("wire
 * gui_apps_browser.c to call into the real engine instead of a
 * hand-rolled parser") - see gui_apps_browser.c's
 * browser_load_via_real_netsurf() for the caller side of this.
 */
#ifndef COS_NETSURF_RENDER_H
#define COS_NETSURF_RENDER_H

#include <stddef.h>

/* Evaluates `script` through the shared QuickJS runtime and logs the
 * result (or exception) to the serial console. Implemented in
 * cos_netsurf.c; declared here (rather than only in cos_netsurf.h,
 * which GUI code can't include - see above) because it shares none of
 * that header's nserror/NetSurf-header dependency and
 * gui_apps_browser.c's "javascript:" URL handling needs it. */
void cos_netsurf_eval_script(const char *script);

#define COS_NS_LINE_TEXT_MAX 256
#define COS_NS_LINE_HREF_MAX 256

typedef enum {
    COS_NS_LINE_TITLE,    /* the page's <title>, if any - at most one,
                            * always the first line if present */
    COS_NS_LINE_HEADING,  /* heading_level gives 1-6 */
    COS_NS_LINE_TEXT,      /* ordinary paragraph/block text */
    COS_NS_LINE_LINK,      /* text = link label, href = target URL */
    COS_NS_LINE_BLANK,     /* spacing only, e.g. for <hr> */
} cos_ns_line_kind_t;

typedef struct {
    cos_ns_line_kind_t kind;
    int heading_level;  /* 1-6 when kind == COS_NS_LINE_HEADING, else 0 */
    char text[COS_NS_LINE_TEXT_MAX];
    char href[COS_NS_LINE_HREF_MAX];  /* only set when kind == COS_NS_LINE_LINK */
} cos_ns_render_line_t;

/* Loads `url_string` through the real NetSurf content pipeline (real
 * data:/http:/https: fetch, real HTML5 parse via libhubbub into a
 * real DOM via libdom, real CSS association via libcss, real
 * <script> execution via the QuickJS backend) and walks the
 * resulting DOM tree to extract a simple sequence of display lines
 * (title/headings/paragraphs/links) into `out_lines` (capped at
 * `max_lines`).
 *
 * This is genuinely the real engine's parse output - not a
 * hand-rolled HTML-tag stripper - but it is NOT Tier B (see
 * PORTING_NOTES.md): there is no CSS box model or layout, so the
 * result is a plain top-to-bottom list of lines, not a visually laid
 * out page. Whether a page loads at all also still depends on
 * whatever cos_fetch_http.c's underlying transport can reach - see
 * that file and kernel/drivers/net.c re: COS_ENABLE_NETWORK.
 *
 * Returns the number of lines written on success (0 is a valid
 * result - a real page with no visible text/links is not an error),
 * or a negative value on failure, in which case `out_error` (if
 * non-NULL) is filled with a short human-readable reason and
 * `out_lines` is untouched - the caller should show that error
 * rather than treat out_lines as valid. */
int cos_netsurf_render_page(const char *url_string,
                             cos_ns_render_line_t *out_lines, int max_lines,
                             char *out_error, size_t error_sz);

#endif /* COS_NETSURF_RENDER_H */
