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

#include "serial.h"
#include "timer.h"
#include "memory.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "netsurf/window.h"
#include "netsurf/download.h"
#include "desktop/download.h"
#include "fs.h"
#include "timer.h"
extern void gui_notify_simple(const char* msg);

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

    /* Defence in depth against a real, upstream-acknowledged NetSurf risk:
     * html_destroy() cancels its box-conversion callback on the way down,
     * but html.c's own comment at that call site says outright, "Unable
     * to cancel conversion context, browser may crash" - the original
     * authors already knew a content object can be torn down (most
     * plausibly reached from an OOM-triggered navigate-away, which is
     * exactly the sequence that surfaced this: a large real page
     * exhausting the heap, then the error-recovery path running) while a
     * callback still referencing it sits in the queue. If that happens,
     * `entry.callback`/`entry.payload` point at memory that something
     * else may since have reused for unrelated data, and calling through
     * it is calling through whatever bytes now happen to live there.
     *
     * A wild call like that does not fail cleanly: the CPU raises #UD
     * (invalid opcode) or #GP the moment it tries to execute non-code
     * bytes, or - worse - if those bytes happen to decode as SOME
     * instruction stream, runs briefly before faulting somewhere
     * unrelated. Confirmed directly: a real 745KB Wikipedia page,
     * exercised through this exact path, produced "[MEMORY] Out of
     * memory", then a content teardown, then "Unhandled interrupt: 6"
     * with the faulting RIP landing inside the kernel heap array itself
     * - not in any function, because it was never a function to begin
     * with.
     *
     * This does not fix the use-after-free - that is an upstream
     * NetSurf/libdom lifetime question this session does not have the
     * scope to resolve, and the comment already on record shows even
     * NetSurf's own authors treated it as a known risk rather than a
     * solved one. What this DOES fix is turning "the kernel halts" into
     * "one scheduled callback is skipped and logged", by checking that
     * the address is actually inside compiled kernel code (the linker's
     * _text_start.._text_end, see linker.ld) before ever calling through
     * it. A skipped callback can mean a page is left half-converted;
     * that is a vastly better failure than every other process, every
     * open window and everything else this OS is doing at the time all
     * going down with it. */
    extern const uint8_t _text_start[];
    extern const uint8_t _text_end[];
    uintptr_t target = (uintptr_t)(void *)entry.callback;
    if (target < (uintptr_t)_text_start || target >= (uintptr_t)_text_end) {
        serial_puts("[SCHED] refusing to call a scheduled callback outside "
                    "kernel code (0x");
        serial_puthex((uint64_t)target);
        serial_puts(") - the content that scheduled it was very likely "
                    "already destroyed; see cos_netsurf_schedule_pump()\n");
        if (g_schedule_count != 0) {
            gui_request_redraw();
        }
        return;
    }

    /* One callback per pass bounds the callback *count*, not the work each
     * callback performs. Report any single callback that overruns a frame,
     * with its code address so nm can name it. */
    uint64_t cos_cb_t0 = get_timer_ticks();
    entry.callback(entry.payload);
    uint64_t cos_cb_ms = get_timer_ticks() - cos_cb_t0;
    if (cos_cb_ms > 25u) {
        serial_puts("[NSPERF] slow scheduled callback ms=");
        serial_putdec(cos_cb_ms);
        serial_puts(" fn=0x");
        serial_puthex((uint64_t)(uintptr_t)entry.callback);
        serial_puts("\n");
    }

    if (g_schedule_count != 0) {
        /* THIS, not mouse movement, is the main reason a fully idle
         * desktop with no visible page open still ran full recomposition
         * on every single frame: any recurring scheduled item - and
         * NetSurf always has at least one somewhere in its lifetime
         * (html_css_process_modified_styles reschedules itself every
         * 1000ms in content/handlers/html/css.c until every stylesheet
         * is done; there are others) - kept g_schedule_count non-zero
         * essentially permanently, and "there is more work to check
         * later" was being treated as "the screen must be redrawn now".
         * Those are different things: cos_netsurf_browser_poll() (see
         * cos_netsurf_browser.c) already calls this function every
         * frame UNCONDITIONALLY, full-redraw or not, so throttling this
         * signal cannot delay the scheduled work itself running - only
         * how often its mere presence forces an expensive full
         * recomposition (measured elsewhere in this investigation at
         * ~14,500+ kcycles for the desktop-layer blit alone) to check
         * for a visible change that, most of the time, is not there
         * yet.
         *
         * A callback that DOES produce something worth showing still
         * gets it shown promptly: this throttle bounds "redraw due to
         * pending work" to roughly once per second, not "never" - well
         * under anything a person would perceive as unresponsive for
         * BACKGROUND work (a direct user action like a click has its
         * own separate, unthrottled trigger), and a large reduction
         * from every single frame at whatever the frame rate otherwise
         * is. Widened from an initial 250ms after measuring that this
         * codebase's NetSurf integration keeps at least one item
         * scheduled almost continuously during ordinary page activity -
         * not a single periodic task but the natural result of normal
         * browsing - so a short window was still firing often enough to
         * keep the cursor-only fast path (gui.h) from accumulating
         * meaningful use during sustained mouse movement. */
        static uint64_t last_pending_redraw_ms = 0;
        uint64_t now_ms = get_timer_ticks();
        if (now_ms - last_pending_redraw_ms >= 1000u) {
            last_pending_redraw_ms = now_ms;
            gui_request_redraw();
        }
    }
}

/* Defaults for the OPTIONAL gui_misc_table entries.
 *
 * "Optional" in NetSurf's headers means "the frontend need not implement
 * it", NOT "the core checks for NULL before calling it". Upstream
 * guarantees non-NULL a different way: desktop/gui_factory.c's
 * verify_misc_register() walks the table at registration time and
 * substitutes a gui_default_* stub into every unset slot, so by the time
 * the core runs, every pointer is callable. The core then calls them
 * unconditionally - e.g. browser_window.c:343 and :3588 both do a bare
 * `guit->misc->launch_url(url)` with no NULL test.
 *
 * C-OS does not build gui_factory.c (it assembles this table itself, see
 * the file header), so that substitution never happened and the unset
 * slots stayed NULL. That is a latent wild call through a null/garbage
 * pointer for every one of them.
 *
 * This was not hypothetical. It crashed: a real page exhausted the heap,
 * the fetch failed, navigate_internal_real() reached its
 * "no fetcher for this URL" path and called launch_url - offset 0x10 in
 * this struct, confirmed by disassembling the faulting `call *0x10(%rax)`
 * - and the CPU jumped through the NULL slot into unrelated memory and
 * executed it as code until it hit a #GP. The stack-overflow fix earlier
 * in this session is what made it reachable at all: before that, the
 * pipeline died of stack corruption long before it ever got this far.
 *
 * The bodies below intentionally mirror gui_factory.c's own defaults
 * rather than inventing behaviour - returning the same error codes the
 * core already expects to handle. */
static nserror cos_gui_default_launch_url(struct nsurl *url)
{
    /* Exactly upstream's gui_default_launch_url: the core uses this
     * return to report "nothing can fetch this", which is the correct
     * and expected outcome for a scheme C-OS has no fetcher for. */
    (void)url;
    return NSERROR_NO_FETCH_HANDLER;
}

static void cos_gui_default_quit(void)
{
    /* Upstream's default is likewise empty. C-OS's desktop owns process
     * lifetime; NetSurf asking to quit must not tear anything down. */
}

static nserror cos_gui_default_login(struct nsurl *url, const char *realm,
                                     const char *username, const char *password,
                                     nserror (*cb)(struct nsurl *url,
                                                   const char *realm,
                                                   const char *username,
                                                   const char *password,
                                                   void *pw),
                                     void *cbpw)
{
    /* Upstream returns NSERROR_NOT_IMPLEMENTED here, and its header is
     * explicit that on a non-OK return the frontend must NOT invoke the
     * callback - so this deliberately does not call cb. */
    (void)url; (void)realm; (void)username; (void)password;
    (void)cb; (void)cbpw;
    return NSERROR_NOT_IMPLEMENTED;
}

static void cos_gui_default_pdf_password(char **owner_pass, char **user_pass,
                                         char *path)
{
    /* Upstream's default sets no password. Both out-params are cleared
     * rather than left untouched: the caller reads them on return, and
     * leaving a caller's uninitialised pointer alone is how a stack
     * value gets freed as if it were a heap string. */
    (void)path;
    if (owner_pass != NULL) *owner_pass = NULL;
    if (user_pass != NULL) *user_pass = NULL;
}

static nserror cos_gui_default_present_cookies(const char *search_term)
{
    (void)search_term;
    return NSERROR_NOT_IMPLEMENTED;
}

static struct gui_misc_table cos_misc_table = {
    .schedule = cos_gui_schedule,
    /* Every remaining member is filled in explicitly - see the note
     * above. Leaving any of them NULL is a wild call waiting for the
     * right page to trigger it, not a harmless "unimplemented". */
    .quit = cos_gui_default_quit,
    .launch_url = cos_gui_default_launch_url,
    .login = cos_gui_default_login,
    .pdf_password = cos_gui_default_pdf_password,
    .present_cookies = cos_gui_default_present_cookies,
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
    /* Deliberately NULL, unlike the gui_misc_table entries above.
     *
     * Upstream's verify_fetch_register() would substitute
     * gui_default_mimetype() here, and C-OS skips that verifier - but
     * this particular slot is only ever called from
     * content/fetchers/curl.c (two call sites), and C-OS does not build
     * curl.c at all; its fetchers are cos_fetch_*.c. So unlike
     * launch_url - which the core calls from browser_window.c and DID
     * crash this system - there is no reachable path to this one.
     *
     * Left NULL rather than given a stub so that if curl.c is ever
     * added to the build, this faults immediately and obviously instead
     * of silently returning a wrong MIME type for every upload. */
    .mimetype = NULL,
};

/* ---- downloads: a page whose content NetSurf cannot show (a zip, a PDF, a program ...) is saved to /Downloads ------------------------------------
 * create() picks a file name from the response (made safe for FAT and exFAT, and unique: "name (1).zip"), data() appends, done() closes and tells the
 * user, error() removes the part that was written. A full volume stops the download with an error instead of leaving a half file. */
/* The downloads window (userland/programs/dlprogress) shows what this table says: the kernel writes /tmp/.cosdl ("ID<TAB>STATE<TAB>GOT<TAB>TOTAL<TAB>NAME" per download, STATE 0 running, 1 done, 2 failed,
 * 3 cancelled) every 400 ms and at every change; the window's Cancel button writes the id to /tmp/.cosdl.cancel, which is read at the same moments. */
#define DLN 8
static struct { bool used; int id; char name[100]; unsigned long long got, total; int state; bool cancel; } g_dl[DLN];
static int g_dl_seq; static uint64_t g_dl_last;
extern void cos_dlwin_open(void);
static void dl_publish(void) {
    static char buf[1400]; size_t n = 0;
    for (int i = 0; i < DLN; ++i) if (g_dl[i].used) {
        int k = snprintf(buf + n, sizeof buf - n, "%d\t%d\t%llu\t%llu\t%s\n", g_dl[i].id, g_dl[i].state, g_dl[i].got, g_dl[i].total, g_dl[i].name);
        if (k < 0 || n + (size_t)k >= sizeof buf) break; n += (size_t)k;
    }
    uint64_t sz; bool d; if (!fs_stat_path("/tmp", &sz, &d)) (void)fs_create_dir_at("/", "tmp");
    (void)fs_write_file_at("/tmp", ".cosdl", buf, n);
    g_dl_last = get_timer_ticks();
}
static void dl_poll_cancel(void) {
    char b[64]; uint64_t got = 0;
    if (!fs_read_path_into("/tmp/.cosdl.cancel", b, sizeof b - 1, &got) || !got) return;
    b[got] = 0; (void)fs_delete_file("/tmp/.cosdl.cancel");
    for (const char *p = b; *p; ) { int id = 0; while (*p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0'); if (id) for (int i = 0; i < DLN; ++i) if (g_dl[i].used && g_dl[i].id == id) g_dl[i].cancel = true; while (*p && (*p < '0' || *p > '9')) ++p; }
}

struct gui_download_window {
    int slot;
    download_context *ctx;
    fs_wstream_t *ws;
    char path[300];
    unsigned long long got, total;
    bool failed;
};

static void dl_notify(const char *what, const char *path) {
    char m[420]; snprintf(m, sizeof m, "%s: %s", what, path);
    serial_puts("[DOWNLOAD] "); serial_puts(m); serial_puts("\n");
    gui_notify_simple(m);
}

static void dl_safe_name(const char *in, char *out, size_t cap) {
    size_t n = 0;
    const char *base = in ? in : "";
    for (const char *p = base; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    for (const char *p = base; *p && n + 1 < cap && n < 120; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || strchr("\"*:<>?|", ch)) ch = '_';
        out[n++] = (char)ch;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) --n;       /* FAT drops trailing spaces and dots */
    out[n] = 0;
    if (!n || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, cap, "download");
}

static struct gui_download_window *cos_download_create(download_context *ctx, struct gui_window *parent) {
    (void)parent;
    struct gui_download_window *dw = (struct gui_download_window *)kmalloc(sizeof *dw);
    if (dw) memset(dw, 0, sizeof *dw);
    if (!dw) { download_context_abort(ctx); download_context_destroy(ctx); return NULL; }
    dw->ctx = ctx; dw->total = download_context_get_total_length(ctx);
    char name[130]; dl_safe_name(download_context_get_filename(ctx), name, sizeof name);
    uint64_t sz; bool isdir;
    if (!fs_stat_path("/Downloads", &sz, &isdir)) (void)fs_create_dir_at("/", "Downloads");
    char stem[130], ext[40]; const char *dot = strrchr(name, '.');
    if (dot && dot != name && strlen(dot) < sizeof ext) { size_t sl = (size_t)(dot - name); memcpy(stem, name, sl); stem[sl] = 0; snprintf(ext, sizeof ext, "%s", dot); }
    else { snprintf(stem, sizeof stem, "%s", name); ext[0] = 0; }
    snprintf(dw->path, sizeof dw->path, "/Downloads/%s%s", stem, ext);
    for (int i = 1; i < 1000 && fs_stat_path(dw->path, &sz, &isdir); ++i) snprintf(dw->path, sizeof dw->path, "/Downloads/%s (%d)%s", stem, i, ext);
    dw->ws = fs_wstream_open(dw->path);
    if (!dw->ws) {
        dl_notify("ダウンロードを保存できません / Cannot save the download", dw->path);
        download_context_abort(ctx); download_context_destroy(ctx); kfree(dw); return NULL;
    }
    dw->slot = -1;
    for (int i = 0; i < DLN; ++i) if (!g_dl[i].used) { dw->slot = i; break; }
    if (dw->slot < 0) { for (int i = 0; i < DLN && dw->slot < 0; ++i) if (g_dl[i].state != 0) dw->slot = i; }          /* all slots taken: reuse a finished one */
    if (dw->slot >= 0) { g_dl[dw->slot].used = true; g_dl[dw->slot].id = ++g_dl_seq; g_dl[dw->slot].got = 0; g_dl[dw->slot].total = dw->total; g_dl[dw->slot].state = 0; g_dl[dw->slot].cancel = false; snprintf(g_dl[dw->slot].name, sizeof g_dl[dw->slot].name, "%s", strrchr(dw->path, '/') + 1); dl_publish(); cos_dlwin_open(); }
    return dw;
}

static nserror cos_download_data(struct gui_download_window *dw, const char *data, unsigned int size) {
    if (dw->failed) return NSERROR_SAVE_FAILED;
    if (dw->slot >= 0 && g_dl[dw->slot].cancel) { dw->failed = true; return NSERROR_SAVE_FAILED; }
    if (!fs_wstream_write(dw->ws, data, size)) { dw->failed = true; return NSERROR_SAVE_FAILED; }
    dw->got += size;
    if (dw->slot >= 0) { g_dl[dw->slot].got = dw->got; if (get_timer_ticks() - g_dl_last > 400) { dl_poll_cancel(); dl_publish(); } }
    return NSERROR_OK;
}

static void cos_download_finish(struct gui_download_window *dw, bool ok, const char *why) {
    fs_wstream_close(dw->ws);
    if (dw->slot >= 0) { bool cancelled = g_dl[dw->slot].cancel; g_dl[dw->slot].state = ok ? 1 : cancelled ? 3 : 2; if (ok) g_dl[dw->slot].got = dw->got; dl_publish(); }
    if (ok) dl_notify("ダウンロード完了 / Download finished", dw->path);
    else {
        (void)fs_delete_file(dw->path);                                  /* no half file */
        char m[420]; snprintf(m, sizeof m, "ダウンロード失敗 / Download failed (%s)", why ? why : "?"); dl_notify(m, dw->path);
    }
    download_context_destroy(dw->ctx); kfree(dw);
}
static void cos_download_error(struct gui_download_window *dw, const char *msg) { cos_download_finish(dw, false, msg); }
static void cos_download_done(struct gui_download_window *dw) {
    if (dw->failed) cos_download_finish(dw, false, "disk full or write error");
    else cos_download_finish(dw, true, NULL);
}
static struct gui_download_table cos_download_table = {
    .create = cos_download_create, .data = cos_download_data, .error = cos_download_error, .done = cos_download_done,
};

static struct netsurf_table cos_netsurf_table = {
    .misc = &cos_misc_table,
    .window = NULL,
    .download = &cos_download_table,
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
