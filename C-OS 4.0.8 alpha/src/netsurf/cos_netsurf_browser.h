/* C-OS bridge to the upstream NetSurf browser_window frontend. */
#ifndef COS_NETSURF_BROWSER_H
#define COS_NETSURF_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates or replaces the single C-OS browser tab and begins loading url. */
bool cos_netsurf_browser_open(const char *url, int viewport_width,
                              int viewport_height, char *error,
                              size_t error_size);

/* Opens or navigates the standard NetSurf window with URL-encoded POST data. */
bool cos_netsurf_browser_open_post(const char *url, const char *post_urlenc,
                                   int viewport_width, int viewport_height,
                                   char *error, size_t error_size);

/* Queue a script-originated GET for the GUI owner to commit after the current
 * QuickJS stack has unwound. It validates/copies only; network I/O is deferred
 * to the following normal Browser redraw. */
bool cos_netsurf_browser_queue_navigation(const char *url);

/* Advances C-OS fetchers and scheduled NetSurf work. */
void cos_netsurf_browser_poll(void);

/* Queues one safe browser_window_reformat after a real QuickJS/libdom mutation. */
void cos_netsurf_browser_notify_dom_mutation(void);

/* Called by the HTML handler when a deferred QuickJS/libdom mutation rebox
 * has completed; invalidates cached pixels and schedules a clean redraw. */
void cos_netsurf_browser_dom_rebuild_complete(void);

/* Consumes a completed deferred rebox notification before GUI cache reuse. */
bool cos_netsurf_browser_take_dom_rebuild_complete(void);

/* Redraws the active NetSurf page at the supplied C-OS canvas origin. */
bool cos_netsurf_browser_redraw(int origin_x, int origin_y,
                                int viewport_width, int viewport_height);

/* Delivers C-OS canvas coordinates to the page viewport. */
void cos_netsurf_browser_click(int x, int y);
void cos_netsurf_browser_track(int x, int y);
/* Scrolls the deepest NetSurf scrollable object at the canvas point.
 * Returns true only when the scroll request was consumed. */
bool cos_netsurf_browser_scroll_at(int x, int y, int delta_x, int delta_y);

/* Delivers a NetSurf key code to focused form controls. */
bool cos_netsurf_browser_keypress(uint32_t key);

/* Copies the currently committed standard NetSurf URL into dst. */
bool cos_netsurf_browser_get_url(char *dst, size_t dst_size);

/* Copies the current document title into dst.  This is updated by normal
 * NetSurf page lifecycle callbacks, including a QuickJS document.title write. */
bool cos_netsurf_browser_get_title(char *dst, size_t dst_size);

/* Records a script-driven document.title update and invalidates the Browser
 * chrome.  The DOM bridge calls this after changing the bound libdom document. */
void cos_netsurf_browser_set_document_title(const char *title);

/* Releases the active standard NetSurf browser window. */
void cos_netsurf_browser_close(void);

/* Monotonic visible-viewport generation.  It changes whenever NetSurf
 * invalidates page pixels and lets the GUI safely reuse its cached BitBlt
 * surface only while the rendered result remains current. */
uint32_t cos_netsurf_window_paint_generation(void);

#endif
