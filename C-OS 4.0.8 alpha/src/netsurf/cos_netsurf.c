/*
 * cos_netsurf.c - the top of the C-OS NetSurf frontend: one-time
 * engine init, and cos_netsurf_load_url_sync(), a synchronous
 * "load this URL, run it through the real HTML/CSS/JS pipeline,
 * and wait" driver.
 *
 * This is deliberately not a port of any upstream frontend (see
 * cos_fetch.c's header comment for why) - it hand-writes the small
 * amount of glue that desktop/netsurf.c + desktop/browser_window.c
 * provide upstream, scoped to exactly what's needed to drive the
 * pipeline ported so far. In particular, the CONTENT_MSG_GETTHREAD
 * handler below is the piece PORTING_NOTES.md identified as the
 * precise remaining gap between "the QuickJS backend is correctly
 * wired" and "a <script> tag on a real page actually executes
 * through it" - answering that message is genuinely all that was
 * missing.
 *
 * What this does NOT do yet: draw anything (Tier B - box model,
 * layout, redraw - and a real plotter don't exist yet), or fetch
 * over a real network (only the data: scheme is registered so far -
 * see cos_fetch.c).
 *
 * C-OS 4.0.8 alpha additions:
 *   - cos_netsurf_load_url_sync_nowait(): fire-and-forget variant for
 *     GUI callers that want to trigger the pipeline without blocking
 *     the render loop.
 *   - cos_netsurf_is_ready(): query whether the engine has been
 *     initialised (safe to call from any GUI code).
 *   - cos_netsurf_eval_script(): run an arbitrary JS snippet through
 *     the shared QuickJS heap/context, for the browser address-bar
 *     javascript: URL scheme and the shell "js" command.
 */
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "content/content.h"
#include "content/hlcache.h"
#include "content/content_factory.h"
#include "content/fetchers.h"
#include "content/fetchers/data.h"
#include "content/fetchers/resource.h"
#include "content/handlers/html/html.h"
#include "content/handlers/css/css.h"
#include "content/handlers/image/bmp.h"
#include "content/handlers/image/svg.h"
#include "content/handlers/image/ico.h"
#include "javascript/content.h"
#include "javascript/js.h"

#include "cos_netsurf.h"
#include "cos_netsurf_internal.h"
#include "serial.h"
#include "task.h"
#include "cos_netsurf_browser.h"

/* ---- C-OS QuickJS port helpers (from third_party/quickjs/quickjs_port.h) ---- */
#include "quickjs_port.h"

static bool g_netsurf_ready = false;

/* One JS heap for the lifetime of this driver, shared by every page
 * loaded through it - matches "one heap per browser window" from
 * js.h's contract, since this driver only ever has one logical
 * "window" open at a time. */
static jsheap *g_js_heap = NULL;

/* Shared JSContext for direct script evaluation (javascript: URLs,
 * shell "js" command). Created lazily on first use. */
static JSContext *g_js_ctx = NULL;

/* cos_nsoptions.c: desktop/options.h defaults JavaScript to off. */
extern void cos_netsurf_enable_javascript(void);
extern void cos_netsurf_configure_constrained_browser_profile(void);
extern nserror cos_netsurf_png_init(void);
extern nserror cos_netsurf_jpeg_init(void);
extern nserror cos_fetch_file_register(void);

nserror cos_netsurf_init(void)
{
    if (g_netsurf_ready) {
        return NSERROR_OK;
    }

    js_initialise();

    cos_gui_table_init();
    /* Enable the upstream HTML handler's standard script path.  Each
     * browser_window owns a QuickJS heap, and CONTENT_MSG_GETTHREAD is handled
     * by browser_window.c, so ordinary <script> elements run through the same
     * NetSurf 3.11 lifecycle as HTML, CSS, forms and links.  DOM mutation is
     * deliberately best-effort in this compact frontend; parser/layout output
     * remains usable even when a third-party script depends on an unavailable
     * browser API. */
    cos_netsurf_enable_javascript();
    cos_netsurf_configure_constrained_browser_profile();

    /* C-OS 4.0.8 fix: corestrings_init() populates the corestring_lwc_*
     * / corestring_dom_* / corestring_nsurl_* interned-string globals
     * that fetch_data_register() below, html_init()/nscss_init(), and
     * (later) real HTML parsing all depend on. Upstream calls this
     * first thing in desktop/netsurf.c's netsurf_init() - that file
     * isn't part of this build, so nothing was ever calling it here.
     * Without it, every corestring_* pointer is NULL, and
     * fetch_data_register() fails immediately: lwc_string_ref(NULL)
     * safely returns NULL, but fetcher_add(NULL, ...) then rejects it
     * with NSERROR_BAD_PARAMETER. Confirmed against qemu_serial.log
     * from the prior boot test - "[NetSurf] init failed in nowait
     * path" traced back to exactly this, meaning even the data: URI
     * path never actually worked. */
    nserror err = corestrings_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] corestrings_init failed\n");
        return err;
    }

    /* Also upstream-mandatory, and also never called anywhere in this
     * build: these register the HTML and CSS content handlers with
     * content_factory. Without them content_factory_create_content()
     * has no handler for "text/html" or "text/css" and every fetch -
     * data:, and now http(s): - fails to become a content object
     * regardless of whether the underlying bytes arrived fine. */
    err = nscss_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] nscss_init failed\n");
        return err;
    }

    err = html_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] html_init failed\n");
        return err;
    }

    /* Register text/javascript and application/javascript as CONTENT_JS.
     * html/script.c then resolves both inline and fetched scripts to js_exec,
     * whose C-OS backend is the linked QuickJS implementation. */
    err = javascript_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] javascript_init failed\n");
        return err;
    }

    err = cos_netsurf_png_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] PNG handler init failed\n");
        return err;
    }

    /* The C-OS baseline JPEG decoder is exposed through a stateless NetSurf
     * content handler; unsupported progressive JPEGs fail visibly rather than
     * being presented as fabricated image data. */
    err = cos_netsurf_jpeg_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] JPEG handler init failed\n");
        return err;
    }

    /* BMP uses the fully vendored libnsbmp decoder and the standard NetSurf
     * image content lifecycle, including the GUI bitmap plotter. */
    err = nsbmp_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] BMP handler init failed\n");
        return err;
    }

    /* Register the upstream vector and multi-image icon handlers. SVG uses
     * the vendored libsvgtiny path/shape renderer, while ICO reuses libnsbmp
     * and the frontend bitmap API. Both are generic MIME handlers, never URL
     * allowlists, and participate in normal llcache/content lifecycle. */
    err = svg_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] SVG handler init failed\n");
        return err;
    }
    err = nsico_init();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] ICO handler init failed\n");
        return err;
    }

    err = fetcher_init();
    if (err != NSERROR_OK) {
        return err;
    }

    err = fetch_data_register();
    if (err != NSERROR_OK) {
        return err;
    }

    /* file: maps C-OS storage paths to the normal NetSurf content lifecycle,
     * so locally created/downloaded HTML and scripts execute through the same
     * HTML/CSS/QuickJS pipeline as HTTP pages. */
    err = cos_fetch_file_register();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] fetch_file_register failed\n");
        return err;
    }

    /* resource: provides the built-in CSS required when an HTML content
     * object is created.  The C-OS gui fetch table serves these resources
     * from static memory. */
    err = fetch_resource_register();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] fetch_resource_register failed\n");
        return err;
    }

    /* Real network fetcher - see cos_fetch_http.c. Registers both
     * "http" and "https"; backed by kernel/drivers/http.c's existing
     * TCP/TLS client (the same one gui_apps_browser.c already uses).
     * If COS_ENABLE_NETWORK is 0 (see kernel/drivers/net.c), the
     * underlying http_get()/http_post() calls fail cleanly at fetch
     * time with a network error - registration itself doesn't touch
     * the network and always succeeds. */
    err = fetch_http_register();
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] fetch_http_register failed\n");
        return err;
    }

    struct hlcache_parameters params;
    memset(&params, 0, sizeof(params));
    params.bg_clean_time = 0; /* no periodic cleanup - nothing drives
                                * a timer for it yet */
    /* A browser document can now retain up to 10MiB of HTML input and page
     * resources commonly include CSS, scripts, and images.  Give NetSurf's
     * standard llcache a 128MiB shared resource budget; this is separate from
     * the 24MiB per-QuickJS-runtime execution ceiling. */
    params.llcache.limit = 128u * 1024u * 1024u;   /* 128MiB RAM cache */
    params.llcache.hysteresis = 16u * 1024u * 1024u;
    params.llcache.minimum_lifetime = 0;
    params.llcache.minimum_bandwidth = 0;
    params.llcache.maximum_bandwidth = 0;
    params.llcache.time_quantum = 100;
    params.llcache.fetch_attempts = 1;
    params.llcache.store.path = NULL; /* no backing store - see
                                        * content/no_backing_store.c,
                                        * which this links against */

    err = hlcache_initialise(&params);
    if (err != NSERROR_OK) {
        return err;
    }

    g_netsurf_ready = true;
    serial_puts("[NetSurf] engine initialised (content core + html/css/png/jpeg/bmp "
                "handlers + QuickJS backend + data:/file:/http:/https: "
                "fetchers; llcache=128MiB, QuickJS runtime=24MiB)\n");

    /* A tiny side-effect-free evaluation verifies that the QuickJS bridge is
     * usable when the browser first starts, rather than merely linked. */
    cos_netsurf_eval_script("1 + 2");
    return NSERROR_OK;
}

/* Returns true if cos_netsurf_init() has completed successfully.
 * Safe to call from any GUI code without side effects. */
bool cos_netsurf_is_ready(void)
{
    return g_netsurf_ready;
}

struct cos_page_load_state {
    bool done;
    bool error;
    char error_msg[256];
};

static nserror cos_content_callback(hlcache_handle *handle,
                                     const hlcache_event *event, void *pw)
{
    struct cos_page_load_state *state = (struct cos_page_load_state *)pw;

    switch (event->type) {
    case CONTENT_MSG_GETTHREAD:
        /* The one piece PORTING_NOTES.md called out as missing:
         * answer the html content handler's request for somewhere to
         * run its <script> tags. js_newheap()/js_newthread() are the
         * QuickJS backend in
         * content/handlers/javascript/quickjs/quickjs.c - both
         * genuinely create a runtime/context, not stubs. */
        if (g_js_heap == NULL) {
            if (js_newheap(0, &g_js_heap) != NSERROR_OK) {
                serial_puts("[NetSurf] failed to create JS heap\n");
                break;
            }
        }
        {
            jsthread *thread = NULL;
            if (js_newthread(g_js_heap, NULL, NULL, &thread) == NSERROR_OK) {
                *(event->data.jsthread) = thread;
                serial_puts("[NetSurf] JS thread created for page - "
                            "<script> tags will now execute\n");
            } else {
                serial_puts("[NetSurf] failed to create JS thread\n");
            }
        }
        break;

    case CONTENT_MSG_LOG:
        serial_puts("[NetSurf/console] ");
        if (event->data.log.msg != NULL) {
            serial_puts(event->data.log.msg);
        }
        serial_puts("\n");
        break;

    case CONTENT_MSG_DONE:
        state->done = true;
        serial_puts("[NetSurf] page load complete\n");
        break;

    case CONTENT_MSG_ERROR:
        state->done = true;
        state->error = true;
        if (event->data.errordata.errormsg != NULL) {
            cos_strlcpy(state->error_msg, event->data.errordata.errormsg,
                        sizeof(state->error_msg));
        } else {
            cos_strlcpy(state->error_msg, "(no message)", sizeof(state->error_msg));
        }
        serial_puts("[NetSurf] page load error: ");
        serial_puts(state->error_msg);
        serial_puts("\n");
        break;

    default:
        /* CONTENT_MSG_STATUS, CONTENT_MSG_READY, CONTENT_MSG_REDRAW,
         * etc. - all fine to ignore for a driver that isn't drawing
         * anything yet (Tier B - see PORTING_NOTES.md). */
        break;
    }

    return NSERROR_OK;
}

/* Loads `url_string` (any scheme a fetcher is registered for - just
 * data: today, e.g. "data:text/html,<script>console.log(1+1)</script>")
 * through the real content pipeline (fetch -> parse into a real DOM
 * via libhubbub+libdom -> style via libcss -> execute <script> tags
 * via the QuickJS backend), driving cos_fetch_poll_all() synchronously
 * until the page finishes loading or errors. Logs progress and any
 * console.log()/script output to the serial console. Returns
 * NSERROR_OK if the page reached CONTENT_MSG_DONE, the content
 * pipeline's own error code (not necessarily this function's own
 * fault) if it reached CONTENT_MSG_ERROR, or a bad-parameter/timeout
 * error for problems in this function itself.
 *
 * There is deliberately no visual result: Tier B (box model, layout,
 * redraw) isn't ported yet, so this proves and exercises the parse +
 * style + script pipeline, not rendering. */
nserror cos_netsurf_load_url_sync(const char *url_string)
{
    if (!g_netsurf_ready) {
        nserror err = cos_netsurf_init();
        if (err != NSERROR_OK) {
            return err;
        }
    }

    nsurl *url = NULL;
    nserror err = nsurl_create(url_string, &url);
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] bad URL\n");
        return err;
    }

    struct cos_page_load_state state;
    memset(&state, 0, sizeof(state));

    hlcache_handle *handle = NULL;
    err = hlcache_handle_retrieve(url, 0, NULL, NULL,
                                   cos_content_callback, &state,
                                   NULL, CONTENT_ANY, &handle);
    nsurl_unref(url);

    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] hlcache_handle_retrieve failed\n");
        return err;
    }

    /* Synchronous wait: poll every registered fetcher until the
     * content callback above sees DONE or ERROR. Bounded so a
     * misbehaving/unregistered scheme can't hang this call forever -
     * data: URIs (the only scheme registered so far) resolve within
     * their first poll, so in practice this loop runs once. */
    const int max_polls = 10000;
    int polls = 0;
    while (!state.done && polls < max_polls) {
        cos_fetch_poll_all();
        cos_netsurf_schedule_pump();
        /* This legacy synchronous diagnostic API is never used by the GUI,
         * but it must still not monopolise a CPU while a transport worker is
         * waiting for DNS/TCP/TLS. */
        thread_yield();
        polls++;
    }

    if (handle != NULL) {
        hlcache_handle_release(handle);
    }

    if (!state.done) {
        serial_puts("[NetSurf] timed out waiting for page load\n");
        return NSERROR_TIMEOUT;
    }

    return state.error ? NSERROR_UNKNOWN : NSERROR_OK;
}

/* Same synchronous load as cos_netsurf_load_url_sync() above, for
 * callers that need to inspect the *result* afterward - e.g. walk its
 * real DOM tree - rather than just exercising the pipeline. The two
 * are kept separate rather than adding an out-parameter to the
 * original: every existing caller of cos_netsurf_load_url_sync()
 * wants the release-immediately behaviour, and this function's
 * different ownership contract (see below) is easy to get wrong by
 * accident if it were the same entry point.
 *
 * On NSERROR_OK, `*out_handle` is a live, retrieved hlcache_handle
 * that the caller now owns and MUST release with exactly one
 * hlcache_handle_release() call once done with it (see
 * cos_netsurf_render.c for the intended caller). On any other
 * return value, `*out_handle` is NULL and nothing needs releasing -
 * this function has already cleaned up internally. `out_error`, if
 * non-NULL, is filled with a short human-readable failure reason
 * (truncated to `error_sz`); it is left untouched on success. */
nserror cos_netsurf_load_url_sync_for_render(const char *url_string,
                                              hlcache_handle **out_handle,
                                              char *out_error,
                                              size_t error_sz)
{
    *out_handle = NULL;

    if (!g_netsurf_ready) {
        nserror err = cos_netsurf_init();
        if (err != NSERROR_OK) {
            if (out_error != NULL) {
                cos_strlcpy(out_error,
                    "NetSurf engine failed to initialise", error_sz);
            }
            return err;
        }
    }

    nsurl *url = NULL;
    nserror err = nsurl_create(url_string, &url);
    if (err != NSERROR_OK) {
        if (out_error != NULL) {
            cos_strlcpy(out_error, "malformed URL", error_sz);
        }
        return err;
    }

    struct cos_page_load_state state;
    memset(&state, 0, sizeof(state));

    hlcache_handle *handle = NULL;
    err = hlcache_handle_retrieve(url, 0, NULL, NULL,
                                   cos_content_callback, &state,
                                   NULL, CONTENT_ANY, &handle);
    nsurl_unref(url);

    if (err != NSERROR_OK) {
        if (out_error != NULL) {
            cos_strlcpy(out_error, "could not start fetch", error_sz);
        }
        return err;
    }

    const int max_polls = 10000;
    int polls = 0;
    while (!state.done && polls < max_polls) {
        cos_fetch_poll_all();
        cos_netsurf_schedule_pump();
        /* This legacy synchronous diagnostic API is never used by the GUI,
         * but it must still not monopolise a CPU while a transport worker is
         * waiting for DNS/TCP/TLS. */
        thread_yield();
        polls++;
    }

    if (!state.done) {
        if (handle != NULL) {
            hlcache_handle_release(handle);
        }
        if (out_error != NULL) {
            cos_strlcpy(out_error, "timed out waiting for page load",
                        error_sz);
        }
        return NSERROR_TIMEOUT;
    }

    if (state.error) {
        if (handle != NULL) {
            hlcache_handle_release(handle);
        }
        if (out_error != NULL) {
            cos_strlcpy(out_error, state.error_msg, error_sz);
        }
        return NSERROR_UNKNOWN;
    }

    *out_handle = handle;
    return NSERROR_OK;
}

/* Fire-and-forget variant for GUI callers that want to kick off the
 * NetSurf pipeline without blocking the render loop. Initialises the
 * engine if needed, then starts the load. Any parse/script output
 * goes to the serial console as usual. Errors are logged but not
 * returned - callers wanting the actual visible result should use
 * cos_netsurf_render_page() (cos_netsurf_render.c) instead, which
 * blocks until the page is ready. */
void cos_netsurf_load_url_sync_nowait(const char *url_string)
{
    if (!url_string || !url_string[0]) {
        return;
    }

    serial_puts("[NetSurf] GUI triggered pipeline for: ");
    serial_puts(url_string);
    serial_puts("\n");

    /* Initialise lazily - safe to call multiple times. */
    if (!g_netsurf_ready) {
        nserror err = cos_netsurf_init();
        if (err != NSERROR_OK) {
            serial_puts("[NetSurf] init failed in nowait path\n");
            return;
        }
    }

    /* This public GUI entry point must never call either synchronous helper
     * above. It owns no completion callback or display handle, so route it to
     * the persistent upstream browser window; GUI lifecycle polling performs
     * fetch, parse, layout and redraw incrementally on later frames. */
    char error[128];
    if (!cos_netsurf_browser_open(url_string, 760, 500, error, sizeof(error))) {
        serial_puts("[NetSurf] nowait navigation failed: ");
        serial_puts(error[0] ? error : "unknown error");
        serial_puts("\n");
    }
}

/* Evaluates `script` through the shared QuickJS runtime and logs the
 * result (or exception) to the serial console. Intended for the
 * browser address-bar "javascript:" URL scheme and the shell "js"
 * command. Initialises the engine if needed. */
void cos_netsurf_eval_script(const char *script)
{
    if (!script || !script[0]) {
        return;
    }

    /* Ensure the engine is up. */
    if (!g_netsurf_ready) {
        nserror err = cos_netsurf_init();
        if (err != NSERROR_OK) {
            serial_puts("[NetSurf/JS] engine init failed\n");
            return;
        }
    }

    /* Lazily create the shared evaluation context. */
    if (g_js_ctx == NULL) {
        if (g_js_heap == NULL) {
            if (js_newheap(0, &g_js_heap) != NSERROR_OK) {
                serial_puts("[NetSurf/JS] failed to create JS heap\n");
                return;
            }
        }
        jsthread *t = NULL;
        if (js_newthread(g_js_heap, NULL, NULL, &t) != NSERROR_OK) {
            serial_puts("[NetSurf/JS] failed to create JS thread\n");
            return;
        }
        /* Extract the raw JSContext from the jsthread struct.
         * jsthread is defined in content/handlers/javascript/quickjs/
         * quickjs.c with `JSContext *ctx` as its first member, so a
         * cast to JSContext** gives us the pointer directly. */
        g_js_ctx = *((JSContext **)t);
        if (g_js_ctx == NULL) {
            serial_puts("[NetSurf/JS] direct JS context unavailable\n");
            return;
        }
        /* This capability is intentionally confined to the explicit local
         * javascript:/shell context.  Page contexts are created independently
         * by the NetSurf backend and never receive `OS.drawRect`. */
        cos_js_enable_privileged_os_api(g_js_ctx);
    }

    serial_puts("[NetSurf/JS] eval: ");
    serial_puts(script);
    serial_puts("\n");

    cos_js_eval_and_report(g_js_ctx, script, "javascript:");
}
