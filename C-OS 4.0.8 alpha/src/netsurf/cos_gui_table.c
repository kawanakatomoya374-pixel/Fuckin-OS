/*
 * cos_gui_table.c - provides `guit`, the global frontend-operations
 * table (`struct netsurf_table *guit`, declared extern in
 * desktop/gui_internal.h) that NetSurf's own code throughout
 * content/ calls through for anything platform-specific. Upstream,
 * a real frontend (frontends/framebuffer, frontends/monkey, ...)
 * assembles this in its own gui_factory-equivalent from tables it
 * implements; this is C-OS's version of that assembly, scoped to
 * exactly what Tier A (see PORTING_NOTES.md) actually calls through:
 *
 *   - misc->schedule: mandatory. A real implementation would let
 *     content register "call me again in N ms" timers (hlcache's
 *     periodic cleanup, some CSS reflow debouncing). There's no
 *     event loop to drive that here yet (this is the same
 *     synchronous-driver stage described in cos_netsurf.c), so it's
 *     a no-op - correct as long as nothing depends on the callback
 *     actually firing later, which nothing does yet: hlcache is
 *     configured with bg_clean_time = 0 (see cos_netsurf.c).
 *   - llcache: content/no_backing_store.c (part of this build)
 *     already provides a complete, real "no persistent cache"
 *     implementation as `null_llcache_table` - used directly rather
 *     than reimplemented.
 *   - utf8: C-OS's local encoding already *is* UTF-8 (there's no
 *     legacy-encoding legacy to support), so "conversion" is
 *     correctly just a copy - not a stub standing in for real
 *     conversion, an actually-correct implementation for this
 *     platform.
 *
 * window/download/clipboard/fetch/file/search/search_web/bitmap/
 * layout are left NULL. Several are documented upstream as
 * mandatory, but nothing in Tier A dereferences a second level
 * through any of them (verified by grepping every `guit->` use
 * across every Tier A source file); they matter once Tier B (box
 * model/layout/redraw) and window management exist and get linked
 * in, at which point the corresponding table here needs a real
 * implementation before that code can run without crashing on a
 * NULL sub-table dereference - this file's job at that point is to
 * grow, not to be rewritten.
 */
#include <string.h>
#include "types.h"

#include "utils/errors.h"
#include "desktop/gui_table.h"
#include "netsurf/misc.h"
#include "netsurf/utf8.h"
#include "netsurf/fetch.h"
#include "content/backing_store.h"

#include "memory.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "netsurf/window.h"

extern struct gui_bitmap_table *cos_netsurf_bitmap_table(void);
extern struct gui_layout_table *cos_netsurf_layout_table(void);
extern struct gui_window_table *cos_netsurf_window_table(void);
extern void gui_request_redraw(void);

/* The C-OS port drives NetSurf synchronously rather than through a host GUI
 * event loop.  Keep scheduled callbacks in a small fixed queue and let the
 * synchronous loader explicitly pump it after fetch polling. */
#define COS_NS_SCHEDULE_CAPACITY 64

typedef struct {
    void (*callback)(void *p);
    void *payload;
} cos_ns_scheduled_t;

static cos_ns_scheduled_t g_schedule_queue[COS_NS_SCHEDULE_CAPACITY];
static size_t g_schedule_count = 0;

static nserror cos_gui_schedule(int t, void (*callback)(void *p), void *p)
{
    if (callback == NULL) return NSERROR_BAD_PARAMETER;

    if (t < 0) {
        /* NetSurf cancels by callback/payload identity. */
        for (size_t i = 0; i < g_schedule_count; ) {
            if (g_schedule_queue[i].callback == callback &&
                g_schedule_queue[i].payload == p) {
                g_schedule_queue[i] = g_schedule_queue[g_schedule_count - 1];
                --g_schedule_count;
            } else {
                ++i;
            }
        }
        return NSERROR_OK;
    }

    /* Time is advanced by the synchronous driver, so every non-negative
     * interval is deferred to its next pump rather than discarded. */
    if (g_schedule_count >= COS_NS_SCHEDULE_CAPACITY) return NSERROR_NOMEM;
    g_schedule_queue[g_schedule_count].callback = callback;
    g_schedule_queue[g_schedule_count].payload = p;
    ++g_schedule_count;
    return NSERROR_OK;
}

void cos_netsurf_schedule_pump(void)
{
    /* A zero-delay NetSurf callback is not an instruction to drain every
     * subsequently-created conversion task in the current GUI update.  Doing
     * so can keep HTML/CSS conversion inside one callback chain indefinitely,
     * starving the next HTTP poll after an external stylesheet is queued.
     *
     * Execute exactly one callback per cooperative GUI pass.  Remaining work
     * requests another frame, so HTML, CSS, image fetches and input advance
     * fairly without needing the currently unsafe permanent fetch worker. */
    if (g_schedule_count == 0) return;

    cos_ns_scheduled_t entry = g_schedule_queue[0];
    for (size_t i = 1; i < g_schedule_count; ++i) {
        g_schedule_queue[i - 1] = g_schedule_queue[i];
    }
    --g_schedule_count;
    entry.callback(entry.payload);

    if (g_schedule_count != 0) {
        gui_request_redraw();
    }
}

static struct gui_misc_table cos_misc_table = {
    .schedule = cos_gui_schedule,
};

static nserror cos_utf8_copy(const char *string, size_t len, char **result)
{
    if (string == NULL || result == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    size_t n = (len != 0) ? len : strlen(string);
    char *copy = (char *)kmalloc(n + 1);
    if (copy == NULL) {
        return NSERROR_NOMEM;
    }
    memcpy(copy, string, n);
    copy[n] = '\0';
    *result = copy;
    return NSERROR_OK;
}

static struct gui_utf8_table cos_utf8_table = {
    .utf8_to_local = cos_utf8_copy,
    .local_to_utf8 = cos_utf8_copy,
};

/* The NetSurf HTML handler always boots with resource:default.css and
 * resource:user.css.  Serve compact built-in styles directly from the C-OS
 * frontend so no host filesystem is required. */
static const uint8_t cos_resource_default_css[] =
    /* Core HTML elements normally hidden by NetSurf's UA stylesheet must be
     * explicitly suppressed in the compact C-OS resource bundle. */
    "head,title,base,link,meta,style,script,template{display:none;}"
    "html,body{display:block;margin:8px;font-family:sans-serif;color:#111;background:#fff;}"
    "body,div,p,form,section,main,header,footer{display:block;}"
    "h1{display:block;font-size:1.5em;margin:0.67em 0;}"
    "h2{display:block;font-size:1.25em;margin:0.83em 0;}"
    "p{display:block;margin:1em 0;}"
    "a{color:#06c;text-decoration:underline;cursor:pointer;}"
    "input,textarea,select,button{display:inline-block;font-family:sans-serif;}"
    /* Search engines commonly omit an explicit input size because their
     * production CSS supplies it. Preserve a usable native control when
     * external CSS is deferred or intentionally simplified. */
    "input[type=text],input[type=search],input[name=q],textarea{min-width:320px;min-height:22px;border:1px solid #777;padding:4px;background:#fff;color:#111;}"
    "input[type=submit],button{width:auto;min-width:0;min-height:0;border:1px solid #777;padding:4px 10px;background:#eee;color:#111;}"
    /* Google places its server-rendered fallback content in #yvlrue and
     * initially hides it until a delayed DOM callback runs.  C-OS keeps the
     * content visible so that NetSurf's native layout, forms and links stay
     * usable while QuickJS runs scripts without a full mutable-DOM binding. */
    "#yvlrue{display:block!important;visibility:visible!important;}";
static const uint8_t cos_resource_empty_css[] = "";

static const char *cos_gui_filetype(const char *path)
{
    if (path != NULL && strstr(path, ".css") != NULL) return "text/css";
    if (path != NULL && strstr(path, ".html") != NULL) return "text/html";
    if (path != NULL && strstr(path, ".png") != NULL) return "image/png";
    if (path != NULL && strstr(path, ".ico") != NULL) return "image/x-icon";
    return "application/octet-stream";
}

static struct nsurl *cos_gui_get_resource_url(const char *path)
{
    (void)path;
    /* Returning NULL tells the resource fetcher that this optional asset is
     * unavailable; it will neither dereference a NULL callback nor attempt
     * a host-filesystem fetch. */
    return NULL;
}

static nserror cos_gui_get_resource_data(const char *path,
        const uint8_t **data, size_t *data_len)
{
    if (path == NULL || data == NULL || data_len == NULL) return NSERROR_BAD_PARAMETER;
    if (strcmp(path, "default.css") == 0 || strcmp(path, "internal.css") == 0) {
        *data = cos_resource_default_css;
        *data_len = sizeof(cos_resource_default_css) - 1;
        return NSERROR_OK;
    }
    if (strcmp(path, "user.css") == 0 || strcmp(path, "adblock.css") == 0 ||
        strcmp(path, "quirks.css") == 0) {
        *data = cos_resource_empty_css;
        *data_len = 0;
        return NSERROR_OK;
    }
    return NSERROR_NOT_FOUND;
}

static nserror cos_gui_release_resource_data(const uint8_t *data)
{
    (void)data; /* All resource data is static storage. */
    return NSERROR_OK;
}

static struct gui_fetch_table cos_fetch_table = {
    .filetype = cos_gui_filetype,
    .get_resource_url = cos_gui_get_resource_url,
    .get_resource_data = cos_gui_get_resource_data,
    .release_resource_data = cos_gui_release_resource_data,
    .mimetype = NULL,
};

static struct netsurf_table cos_netsurf_table = {
    .misc = &cos_misc_table,
    .window = NULL,
    .download = NULL,
    .clipboard = NULL,
    .fetch = &cos_fetch_table,
    .file = NULL,
    .utf8 = &cos_utf8_table,
    .search = NULL,
    .search_web = NULL,
    .llcache = NULL, /* filled in by cos_gui_table_init() below, once
                       * null_llcache_table has been through its own
                       * static initialisation - both are statically
                       * initialised globals in different translation
                       * units, so which runs first isn't guaranteed
                       * by C, and both are always non-NULL constants
                       * by the time cos_netsurf_init() actually runs
                       * anyway (see that function). */
    .bitmap = NULL,
    .layout = NULL,
};

struct netsurf_table *guit = &cos_netsurf_table;

void cos_gui_table_init(void)
{
    cos_netsurf_table.llcache = null_llcache_table;
    cos_netsurf_table.bitmap = cos_netsurf_bitmap_table();
    cos_netsurf_table.layout = cos_netsurf_layout_table();
    cos_netsurf_table.window = cos_netsurf_window_table();
}
