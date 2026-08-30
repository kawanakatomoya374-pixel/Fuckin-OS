/* C-OS bridge to the upstream NetSurf desktop/browser_window frontend. */
#undef PLOT_FONT_FAMILY_SANS_SERIF

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory.h"
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/mouse.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "content/content.h"
#include "content/hlcache.h"
#include "html/private.h"
#include "timer.h"

#include "cos_netsurf.h"
#include "cos_netsurf_browser.h"
#include "quickjs_port.h"

extern void cos_netsurf_schedule_pump(void);
extern void gui_request_redraw(void);
extern const struct plotter_table *cos_netsurf_plotter_table(void);
extern void cos_netsurf_plot_stats_reset(void);
extern void cos_netsurf_plot_stats_get(uint32_t *rects, uint32_t *texts,
                                       uint32_t *bitmaps);
extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t n);
extern bool cos_netsurf_window_take_invalidated(void);
extern void cos_netsurf_window_force_invalidate(void);
extern void cos_netsurf_window_set_viewport(int width, int height);
extern void cos_netsurf_window_get_scroll_offsets(int *scroll_x, int *scroll_y);
extern bool cos_netsurf_window_scroll_by(int delta_x, int delta_y,
                                          int content_width, int content_height);

#define COS_NS_QUEUED_URL_MAX 2048
/* NetSurf fetch identities intentionally omit a fragment because it is never
 * sent on HTTP. JavaScript Location, in contrast, must retain that fragment.
 * Keep the committed navigation spelling separately and return it only for
 * the matching active HTML content. */
static char g_cos_ns_committed_location_url[COS_NS_QUEUED_URL_MAX];

/* The NetSurf JS backend receives an opaque content pointer from
 * browser_window.  Centralise the private HTML cast here, where the frontend
 * already includes html/private.h, so the backend itself stays isolated from
 * the full HTML/CSS include graph. */
struct dom_document *cos_netsurf_html_document_from_content(void *content)
{
    html_content *htmlc = (html_content *)content;
    return htmlc != NULL ? htmlc->document : NULL;
}

const char *cos_netsurf_content_url_from_content(void *content)
{
    struct content *c = (struct content *)content;
    nsurl *url = c != NULL ? content_get_url(c) : NULL;
    const char *content_url = url != NULL ? nsurl_access(url) : NULL;
    if (content_url != NULL && g_cos_ns_committed_location_url[0] != '\0') {
        const char *fragment = strchr(g_cos_ns_committed_location_url, '#');
        size_t base_len = fragment != NULL ?
            (size_t)(fragment - g_cos_ns_committed_location_url) :
            strlen(g_cos_ns_committed_location_url);
        if (strlen(content_url) == base_len &&
            strncmp(content_url, g_cos_ns_committed_location_url, base_len) == 0) {
            return g_cos_ns_committed_location_url;
        }
    }
    return content_url;
}

static struct browser_window *g_cos_ns_bw;
#define COS_NS_DOCUMENT_TITLE_MAX 256
/* A JavaScript Location assignment occurs on the QuickJS/NetSurf callback
 * stack. Destroying/navigating browser_window from that stack is re-entrant,
 * so hold one normalized target and consume it at the next owner redraw. */
static bool g_cos_ns_queued_navigation_pending;
static char g_cos_ns_queued_navigation_url[COS_NS_QUEUED_URL_MAX];
/* Browser-window title calculation is normally performed at conversion end.
 * A later QuickJS document.title assignment has no automatic upstream title
 * callback, so retain that standards-visible DOM state explicitly until the
 * next navigation replaces it. */
static char g_cos_ns_script_document_title[COS_NS_DOCUMENT_TITLE_MAX];
/* Set by the synchronous QuickJS/libdom bridge.  It is consumed only from the
 * next GUI redraw, after the DOM event/script stack has unwound. */
static bool g_cos_ns_dom_rebuild_pending;
static bool g_cos_ns_dom_rebuild_inflight;
/* One-shot trace after deferred rebox completion; remove once the residual
 * text/plotter gap is resolved. */
static bool g_cos_ns_dom_rebuild_paint_diag;
/* Set by html_box_convert_done while NetSurf's schedule pump is active;
 * consumed by the GUI before its BitBlt cache decision. */
static bool g_cos_ns_dom_rebuild_complete_pending;
static unsigned int g_cos_ns_reformat_warmup;
/* Track LOADING -> READY/DONE so the first fully constructed box tree is
 * reformatted exactly once even if an upstream invalidation edge was consumed
 * during the last loading-frame redraw. */
static bool g_cos_ns_content_was_loading;
static bool g_cos_ns_content_status_valid;
static content_status g_cos_ns_last_content_status;
static unsigned int g_cos_ns_redraw_diag_budget;
/* Keep the cooperative fetch/JS pump active every GUI pass, but rate-limit
 * expensive layout while a document is still streaming.  Real invalidations
 * and status transitions remain immediate; this only prevents a 1000Hz GUI
 * loop from repeatedly rebuilding the same partial box tree. */
#define COS_NS_LOADING_REFORMAT_TICKS 33u
static uint64_t g_cos_ns_next_loading_reformat_tick;
static int g_cos_ns_viewport_width = 760;
static int g_cos_ns_viewport_height = 500;
/* Performance timing is deliberately quiet while work is in progress. A
 * single result is emitted after a request has both finished reception and
 * reached its first post-layout framebuffer redraw. PIT ticks are milliseconds
 * (TIMER_TICKS_PER_SEC == 1000). */
static uint64_t g_cos_ns_navigation_start_tick;
static uint64_t g_cos_ns_receive_complete_tick;
static bool g_cos_ns_display_timing_pending;

static void cos_ns_browser_error(char *dst, size_t dst_size, const char *msg)
{
    if (dst == NULL || dst_size == 0) return;
    size_t i = 0;
    if (msg != NULL) {
        while (msg[i] != '\0' && i + 1 < dst_size) { dst[i] = msg[i]; ++i; }
    }
    dst[i] = '\0';
}

void cos_netsurf_browser_close(void)
{
    g_cos_ns_script_document_title[0] = '\0';
    g_cos_ns_queued_navigation_pending = false;
    g_cos_ns_queued_navigation_url[0] = '\0';
    g_cos_ns_committed_location_url[0] = '\0';
    if (g_cos_ns_bw != NULL) {
        browser_window_destroy(g_cos_ns_bw);
        g_cos_ns_bw = NULL;
        g_cos_ns_content_was_loading = false;
        g_cos_ns_content_status_valid = false;
        g_cos_ns_dom_rebuild_pending = false;
        g_cos_ns_dom_rebuild_inflight = false;
        g_cos_ns_dom_rebuild_paint_diag = false;
        g_cos_ns_dom_rebuild_complete_pending = false;
        g_cos_ns_next_loading_reformat_tick = 0;
    }
}

/* Called only from the GUI/QuickJS owner thread after a real libdom mutation.
 * Do not reformat re-entrantly while a script is on the NetSurf parser stack;
 * request a bounded reformat on the next browser redraw instead. */
void cos_netsurf_browser_notify_dom_mutation(void)
{
    if (g_cos_ns_bw == NULL) return;

    /* Parser-executed scripts commonly mutate the DOM before the first
     * dom_to_box conversion has run. That initial conversion already consumes
     * the live modified DOM; scheduling a second destructive rebox at this
     * point unnecessarily tears down CSS/node state and used to collapse every
     * text box at the viewport origin. Reserve full rebox work for mutations
     * made after the top-level HTML content has reached READY/DONE. */
    struct hlcache_handle *content = browser_window_get_content(g_cos_ns_bw);
    bool initial_conversion_pending =
        (content == NULL || content_get_status(content) == CONTENT_STATUS_LOADING);
    if (!initial_conversion_pending) {
        g_cos_ns_dom_rebuild_pending = true;
        g_cos_ns_reformat_warmup = 1;
    }
    g_cos_ns_next_loading_reformat_tick = 0;
    /* This must precede the browser app's cache-generation check. Otherwise
     * the requested redraw would BitBlt the old viewport and never reach the
     * initial conversion or the later deferred rebuild below. */
    cos_netsurf_window_force_invalidate();
    gui_request_redraw();
}

void cos_netsurf_browser_dom_rebuild_complete(void)
{
    if (g_cos_ns_bw == NULL) return;
    g_cos_ns_dom_rebuild_pending = false;
    g_cos_ns_dom_rebuild_inflight = false;
    g_cos_ns_dom_rebuild_paint_diag = true;
    /* Called inside NetSurf's conversion callback: do not touch the frontend
     * window or trigger cache-generation work re-entrantly here. The browser
     * GUI consumes this flag before its next BitBlt cache decision. */
    g_cos_ns_dom_rebuild_complete_pending = true;
    gui_request_redraw();
}

bool cos_netsurf_browser_take_dom_rebuild_complete(void)
{
    bool pending = g_cos_ns_dom_rebuild_complete_pending;
    g_cos_ns_dom_rebuild_complete_pending = false;
    return pending;
}

void cos_netsurf_browser_poll(void)
{
    /* NetSurf conversion, fetch and scheduled callbacks advance cooperatively
     * inside the GUI loop; none of these calls blocks on a host event loop. */
    cos_fetch_poll_all();
    cos_netsurf_schedule_pump();
}

static bool cos_netsurf_browser_navigate(const char *url,
                                         const char *post_urlenc,
                                         int viewport_width,
                                         int viewport_height,
                                         char *error,
                                         size_t error_size)
{
    if (url == NULL || url[0] == '\0') {
        cos_ns_browser_error(error, error_size, "URL is empty");
        return false;
    }
    /* A document navigation starts a new title lifecycle. */
    g_cos_ns_script_document_title[0] = '\0';
    if (viewport_width < 1 || viewport_height < 1) {
        cos_ns_browser_error(error, error_size, "invalid viewport");
        return false;
    }
    if (cos_netsurf_init() != NSERROR_OK) {
        cos_ns_browser_error(error, error_size, "NetSurf initialization failed");
        return false;
    }

    nsurl *nsurl = NULL;
    nserror err = nsurl_create(url, &nsurl);
    if (err != NSERROR_OK || nsurl == NULL) {
        cos_ns_browser_error(error, error_size, "invalid NetSurf URL");
        return false;
    }

    g_cos_ns_viewport_width = viewport_width;
    g_cos_ns_viewport_height = viewport_height;

    bool created_browser_window = false;
    if (g_cos_ns_bw == NULL) {
        /* Create once and preserve the browsing context.  Creating a browser
         * window already starts a GET for `nsurl`; do not immediately submit
         * the same GET a second time, since that re-entrant path can corrupt
         * the compact port's in-flight content state. */
        err = browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
                                    nsurl, NULL, NULL, &g_cos_ns_bw);
        if (err != NSERROR_OK || g_cos_ns_bw == NULL) {
            nsurl_unref(nsurl);
            g_cos_ns_bw = NULL;
            cos_ns_browser_error(error, error_size, "browser window creation failed");
            return false;
        }
        created_browser_window = true;
    }

    /* A newly created window already has its GET target.  POST still needs an
     * explicit navigation; for an initial GET, wait for the normal poll path.
     * Existing windows use browser_window_navigate for every new URL. */
    if (post_urlenc != NULL) {
        err = browser_window_navigate(g_cos_ns_bw, nsurl, NULL,
                                      BW_NAVIGATE_HISTORY,
                                      (char *)post_urlenc, NULL, NULL);
    } else if (!created_browser_window && g_cos_ns_bw != NULL) {
        err = browser_window_navigate(g_cos_ns_bw, nsurl, NULL,
                                      BW_NAVIGATE_HISTORY,
                                      NULL, NULL, NULL);
    }
    nsurl_unref(nsurl);
    if (err != NSERROR_OK) {
        cos_ns_browser_error(error, error_size, "browser navigation failed");
        return false;
    }

    size_t committed_len = 0;
    while (url[committed_len] != '\0' &&
           committed_len + 1 < sizeof(g_cos_ns_committed_location_url)) {
        ++committed_len;
    }
    if (url[committed_len] == '\0') {
        memcpy(g_cos_ns_committed_location_url, url, committed_len + 1);
    } else {
        /* Preserve navigation correctness over a non-standard oversized URL;
         * the content URL remains the safe script-visible fallback. */
        g_cos_ns_committed_location_url[0] = '\0';
    }

    g_cos_ns_reformat_warmup = 8;
    g_cos_ns_redraw_diag_budget = 12;
    g_cos_ns_content_was_loading = false;
    g_cos_ns_content_status_valid = false;
    g_cos_ns_next_loading_reformat_tick = 0;
    g_cos_ns_navigation_start_tick = get_timer_ticks();
    g_cos_ns_receive_complete_tick = 0;
    g_cos_ns_display_timing_pending = true;
    cos_netsurf_window_set_viewport(g_cos_ns_viewport_width,
                                   g_cos_ns_viewport_height);
    /* Do not execute a network poll from the mouse/key handler.  In the
     * compact synchronous fetch backend, that could parse an entire response
     * and schedule CSS conversion before control returned to the GUI lifecycle,
     * leaving the just-created subresource fetch without a presentable frame.
     * Defer the first poll to cos_netsurf_browser_redraw(), which advances one
     * cooperative batch in a normal GUI frame and keeps input responsive. */
    gui_request_redraw();
    cos_ns_browser_error(error, error_size, "");
    return true;
}

bool cos_netsurf_browser_open(const char *url, int viewport_width,
                              int viewport_height, char *error,
                              size_t error_size)
{
    return cos_netsurf_browser_navigate(url, NULL, viewport_width,
                                        viewport_height, error, error_size);
}

bool cos_netsurf_browser_open_post(const char *url, const char *post_urlenc,
                                   int viewport_width, int viewport_height,
                                   char *error, size_t error_size)
{
    if (post_urlenc == NULL) {
        cos_ns_browser_error(error, error_size, "POST data is empty");
        return false;
    }
    return cos_netsurf_browser_navigate(url, post_urlenc, viewport_width,
                                        viewport_height, error, error_size);
}

bool cos_netsurf_browser_queue_navigation(const char *url)
{
    if (url == NULL || url[0] == '\0') return false;
    size_t n = 0;
    while (url[n] != '\0' && n + 1 < sizeof(g_cos_ns_queued_navigation_url)) ++n;
    if (url[n] != '\0') return false;
    memcpy(g_cos_ns_queued_navigation_url, url, n + 1);
    g_cos_ns_queued_navigation_pending = true;
    gui_request_redraw();
    return true;
}

bool cos_netsurf_browser_redraw(int origin_x, int origin_y,
                                int viewport_width, int viewport_height)
{
    if (g_cos_ns_queued_navigation_pending) {
        char url[COS_NS_QUEUED_URL_MAX];
        char error[128];
        memcpy(url, g_cos_ns_queued_navigation_url, sizeof(url));
        g_cos_ns_queued_navigation_pending = false;
        g_cos_ns_queued_navigation_url[0] = '\0';
        if (!cos_netsurf_browser_navigate(url, NULL, g_cos_ns_viewport_width,
                                          g_cos_ns_viewport_height,
                                          error, sizeof(error))) {
            serial_puts("[QJS/Location] queued navigation failed: ");
            serial_puts(error);
            serial_puts("\n");
        }
        /* Navigation invalidates all prior content; let the normally scheduled
         * follow-up frame run its first cooperative fetch/present batch. */
        return g_cos_ns_bw != NULL;
    }
    if (g_cos_ns_bw == NULL || viewport_width < 1 || viewport_height < 1) {
        return false;
    }
    bool viewport_changed = (viewport_width != g_cos_ns_viewport_width ||
                             viewport_height != g_cos_ns_viewport_height);
    if (viewport_changed) {
        g_cos_ns_viewport_width = viewport_width;
        g_cos_ns_viewport_height = viewport_height;
    }

    cos_netsurf_window_set_viewport(viewport_width, viewport_height);
    cos_netsurf_browser_poll();

    /* browser_window_reformat() must follow (not precede) cooperative fetch
     * polling: an HTML document has no boxes until content_open invalidates
     * its gui_window.  Reformat only for a viewport change or that explicit
     * upstream invalidation, retaining the BitBlt-friendly static redraw path
     * during idle frames. */
    bool invalidated = cos_netsurf_window_take_invalidated();
    /* A network document can remain CONTENT_STATUS_LOADING for many GUI
     * frames after the first poll (especially on Wikipedia with several
     * subresources).  The old eight-frame warmup could therefore expire while
     * the old start page was still the only laid-out content.  Keep asking
     * NetSurf to reformat until the top-level handle reaches READY/DONE; this
     * is cheap during loading and guarantees the first usable HTML layout is
     * presented as soon as the fetch completes. */
    bool content_loading = false;
    bool content_status_changed = false;
    struct hlcache_handle *content = browser_window_get_content(g_cos_ns_bw);
    if (content != NULL) {
        content_status status = content_get_status(content);
        if (g_cos_ns_redraw_diag_budget != 0) {
            serial_puts("[NSDRAW] poll content status=");
            serial_putdec((uint64_t)status);
            serial_puts("\n");
            --g_cos_ns_redraw_diag_budget;
        }
        content_loading = (status == CONTENT_STATUS_LOADING);
        content_status_changed = !g_cos_ns_content_status_valid ||
                                 status != g_cos_ns_last_content_status;
        g_cos_ns_last_content_status = status;
        g_cos_ns_content_status_valid = true;
    } else {
        if (g_cos_ns_redraw_diag_budget != 0) {
            serial_puts("[NSDRAW] poll has no content\n");
            --g_cos_ns_redraw_diag_budget;
        }
        g_cos_ns_content_status_valid = false;
    }
    /* DOM mutation needs a new box tree, not merely html_reformat() over the
     * previous tree.  This runs after cos_netsurf_browser_poll(), while no JS
     * callback is active, and retries on the following GUI pass if a previous
     * box conversion is still yielding. */
    bool dom_rebuild_active = false;
    if (content != NULL && content_get_type(content) == CONTENT_HTML) {
        html_content *htmlc = (html_content *)hlcache_handle_get_content(content);
        dom_rebuild_active = (htmlc != NULL && htmlc->box_conversion_context != NULL);
        /* The scheduled conversion finished between GUI passes. Its status is
         * still DONE, so it emits no ordinary content invalidation; explicitly
         * advance the frontend generation before drawing the new box tree. */
        if (g_cos_ns_dom_rebuild_inflight && !dom_rebuild_active) {
            g_cos_ns_dom_rebuild_inflight = false;
            g_cos_ns_dom_rebuild_paint_diag = true;
            cos_netsurf_window_force_invalidate();
            invalidated = true;
        }
        if (g_cos_ns_dom_rebuild_pending && !dom_rebuild_active &&
            html_rebuild_layout_after_dom_mutation(htmlc)) {
            g_cos_ns_dom_rebuild_pending = false;
            g_cos_ns_dom_rebuild_inflight = true;
            dom_rebuild_active = true;
            /* Invalidate the frontend's cached viewport before the rebuilt
             * boxes can be painted; this uses the normal paint generation. */
            cos_netsurf_window_force_invalidate();
            invalidated = true;
        }
    }

    bool content_became_ready = (g_cos_ns_content_was_loading && !content_loading &&
                                 content != NULL);
    if (content_became_ready && g_cos_ns_receive_complete_tick == 0) {
        g_cos_ns_receive_complete_tick = get_timer_ticks();
    }
    g_cos_ns_content_was_loading = content_loading;
    /* gui_lifecycle drives cos_netsurf_browser_poll() even on BitBlt-only
     * idle passes, so a loading document does not need to force full scene
     * composition every tick.  Let adapter invalidation/status transitions
     * request immediate paint; otherwise reformat at a bounded 30Hz cadence. */
    bool loading_reformat_due = false;
    if (content_loading) {
        uint64_t now = get_timer_ticks();
        if (g_cos_ns_next_loading_reformat_tick == 0 ||
            now >= g_cos_ns_next_loading_reformat_tick) {
            loading_reformat_due = true;
            g_cos_ns_next_loading_reformat_tick = now + COS_NS_LOADING_REFORMAT_TICKS;
        }
    } else {
        g_cos_ns_next_loading_reformat_tick = 0;
    }
    if (invalidated || content_became_ready || content_status_changed) {
        gui_request_redraw();
    }
    if (!dom_rebuild_active &&
        (viewport_changed || invalidated || g_cos_ns_reformat_warmup != 0 ||
         loading_reformat_due || content_became_ready || content_status_changed)) {
        browser_window_reformat(g_cos_ns_bw, false, viewport_width, viewport_height);
        if ((content_became_ready || content_status_changed) && content != NULL) {
            int extent_w = 0, extent_h = 0;
            (void)browser_window_get_extents(g_cos_ns_bw, true, &extent_w, &extent_h);
            serial_puts("[NSDRAW] content ready; extent=");
            serial_putdec((uint64_t)extent_w);
            serial_puts("x");
            serial_putdec((uint64_t)extent_h);
            serial_puts(" viewport=");
            serial_putdec((uint64_t)viewport_width);
            serial_puts("x");
            serial_putdec((uint64_t)viewport_height);
            serial_puts("\n");
        }
        if (g_cos_ns_reformat_warmup != 0) {
            --g_cos_ns_reformat_warmup;
        }
        (void)cos_netsurf_window_take_invalidated();
    }
    /* dom_to_box is scheduled cooperatively.  Its old box context has been
     * freed, so html_redraw() must not observe layout == NULL in this frame.
     * Ask for another GUI pass; the schedule pump at its start will finish (or
     * continue) conversion and the normal invalidation path will then paint. */
    if (dom_rebuild_active) {
        gui_request_redraw();
        return false;
    }

    struct rect clip = {
        .x0 = origin_x,
        .y0 = origin_y,
        .x1 = origin_x + viewport_width,
        .y1 = origin_y + viewport_height
    };
    struct redraw_context ctx = {
        .interactive = true,
        .background_images = true,
        .plot = cos_netsurf_plotter_table(),
        .priv = NULL
    };
    /* The C-OS gui_window owns root-document scrolling. Keep the clip fixed
     * to the visible GUI canvas but translate NetSurf's content origin by the
     * frontend scroll offsets before plotting. */
    int scroll_x = 0, scroll_y = 0;
    cos_netsurf_window_get_scroll_offsets(&scroll_x, &scroll_y);
    cos_netsurf_plot_stats_reset();
    bool drawn = browser_window_redraw(g_cos_ns_bw,
                                       origin_x - scroll_x,
                                       origin_y - scroll_y,
                                       &clip, &ctx);
    if (drawn && g_cos_ns_display_timing_pending &&
        g_cos_ns_receive_complete_tick != 0) {
        uint64_t now = get_timer_ticks();
        uint64_t receive_to_display = now - g_cos_ns_receive_complete_tick;
        uint64_t navigation_to_display = now - g_cos_ns_navigation_start_tick;
        serial_puts("[NetSurf] 受信から完全表示まで ");
        serial_putdec(receive_to_display);
        serial_puts("ms (ナビゲーション開始から ");
        serial_putdec(navigation_to_display);
        serial_puts("ms)\\n");
        g_cos_ns_display_timing_pending = false;
    }
    if (content_became_ready || content_status_changed ||
        g_cos_ns_dom_rebuild_paint_diag) {
        uint32_t rects = 0, texts = 0, bitmaps = 0;
        cos_netsurf_plot_stats_get(&rects, &texts, &bitmaps);
        serial_puts(g_cos_ns_dom_rebuild_paint_diag ?
                    "[NSDRAW] DOM-rebox redraw result=" :
                    "[NSDRAW] status redraw result=");
        serial_putdec(drawn ? 1 : 0);
        serial_puts(" plots rect/text/bitmap=");
        serial_putdec(rects);
        serial_puts("/");
        serial_putdec(texts);
        serial_puts("/");
        serial_putdec(bitmaps);
        serial_puts("\n");
        g_cos_ns_dom_rebuild_paint_diag = false;
    }
    return drawn;
}

void cos_netsurf_browser_click(int x, int y)
{
    if (g_cos_ns_bw == NULL) return;
    /* GUI callers supply viewport-local coordinates.  browser_window input
     * uses document coordinates, whereas redraw translates document space by
     * (origin - scroll), so apply the same root-scroll inverse exactly once. */
    int scroll_x = 0, scroll_y = 0;
    cos_netsurf_window_get_scroll_offsets(&scroll_x, &scroll_y);
    x += scroll_x;
    y += scroll_y;
    /* Synchronise hover/hit-test state at the same document coordinate before
     * dispatching a completed click. */
    browser_window_mouse_track(g_cos_ns_bw, 0, x, y);
    browser_window_mouse_click(g_cos_ns_bw,
                               BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_CLICK_1,
                               x, y);
    cos_netsurf_browser_poll();
}

void cos_netsurf_browser_track(int x, int y)
{
    if (g_cos_ns_bw == NULL) return;
    int scroll_x = 0, scroll_y = 0;
    cos_netsurf_window_get_scroll_offsets(&scroll_x, &scroll_y);
    browser_window_mouse_track(g_cos_ns_bw, 0, x + scroll_x, y + scroll_y);
}

bool cos_netsurf_browser_scroll_at(int x, int y, int delta_x, int delta_y)
{
    if (g_cos_ns_bw == NULL || (delta_x == 0 && delta_y == 0)) return false;

    int scroll_x = 0, scroll_y = 0;
    cos_netsurf_window_get_scroll_offsets(&scroll_x, &scroll_y);
    /* First let NetSurf route a wheel request to a nested scrollable element.
     * Coordinates are in document space, so account for the C-OS frontend
     * root-scroll offset before asking the upstream engine. */
    bool consumed = browser_window_scroll_at_point(g_cos_ns_bw,
                                                   x + scroll_x, y + scroll_y,
                                                   delta_x, delta_y);
    if (!consumed) {
        /* A frontend-managed root browser window intentionally has no core
         * scrollbar (bw->scroll_y is NULL).  Its GUI window owns the root
         * offset, so move that offset against the real NetSurf content extent. */
        int content_width = 0, content_height = 0;
        if (browser_window_get_extents(g_cos_ns_bw, true,
                                       &content_width, &content_height) == NSERROR_OK) {
            serial_puts("[NetSurf] root scroll extent=");
            serial_putdec((uint64_t)content_width);
            serial_puts("x");
            serial_putdec((uint64_t)content_height);
            serial_puts(" offset=");
            serial_putdec((uint64_t)scroll_x);
            serial_puts(",");
            serial_putdec((uint64_t)scroll_y);
            serial_puts("\n");
            consumed = cos_netsurf_window_scroll_by(delta_x, delta_y,
                                                     content_width, content_height);
        }
    }
    if (consumed) {
        serial_puts("[NetSurf] scroll consumed dx=");
        serial_putdec((uint64_t)(delta_x < 0 ? -delta_x : delta_x));
        serial_puts(" dy=");
        serial_putdec((uint64_t)(delta_y < 0 ? -delta_y : delta_y));
        serial_puts("\n");
        gui_request_redraw();
    }
    return consumed;
}

bool cos_netsurf_browser_keypress(uint32_t key)
{
    if (g_cos_ns_bw == NULL) return false;
    /* Route text through the desktop text-input layer rather than directly
     * into content. browser_window_key_press maintains focused form state,
     * textarea editing and Enter-driven form submission. */
    bool handled = browser_window_key_press(g_cos_ns_bw, key);
    /* Page-level shortcuts (for example calculator keypads) are independent
     * of focused form editing.  Dispatch after NetSurf has updated its form
     * state so listener code observes the current input value. */
    (void)cos_js_dispatch_window_keydown(key);
    cos_netsurf_browser_poll();
    return handled;
}

bool cos_netsurf_browser_get_url(char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0 || g_cos_ns_bw == NULL) return false;
    nsurl *url = browser_window_access_url(g_cos_ns_bw);
    const char *text = url ? nsurl_access(url) : NULL;
    if (text == NULL || text[0] == '\0') {
        dst[0] = '\0';
        return false;
    }
    size_t i = 0;
    while (text[i] != '\0' && i + 1 < dst_size) {
        dst[i] = text[i];
        ++i;
    }
    dst[i] = '\0';
    return true;
}

bool cos_netsurf_browser_get_title(char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0 || g_cos_ns_bw == NULL) return false;
    const char *title = g_cos_ns_script_document_title[0] != '\0'
                        ? g_cos_ns_script_document_title
                        : browser_window_get_title(g_cos_ns_bw);
    if (title == NULL || title[0] == '\0') {
        dst[0] = '\0';
        return false;
    }
    size_t i = 0;
    while (title[i] != '\0' && i + 1 < dst_size) {
        dst[i] = title[i];
        ++i;
    }
    dst[i] = '\0';
    return true;
}

void cos_netsurf_browser_set_document_title(const char *title)
{
    size_t i = 0;
    if (title != NULL) {
        while (title[i] != '\0' && i + 1 < sizeof(g_cos_ns_script_document_title)) {
            g_cos_ns_script_document_title[i] = title[i];
            ++i;
        }
    }
    g_cos_ns_script_document_title[i] = '\0';
    /* Title-bar paint is part of the GUI frame, not the NetSurf viewport;
     * request it explicitly without trying to re-enter a content redraw. */
    gui_request_redraw();
}
