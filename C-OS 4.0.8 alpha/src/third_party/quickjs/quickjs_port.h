/**
 * quickjs_port.h - public interface to the C-OS <-> QuickJS embedding
 * bridge implemented in quickjs_port.c.
 *
 * C-OS 4.0.8 alpha: extended with console.log and COS global API.
 *
 * Any C-OS translation unit that wants to run real JavaScript (the
 * NetSurf `javascript/quickjs` content handler, the browser's inline
 * <script> execution, a future shell "js" command, ...) should go
 * through these entry points rather than calling JS_NewRuntime2/
 * JS_NewContext directly, so the allocator wiring, memory limits,
 * and built-in globals (console, COS) stay in exactly one place.
 */
#ifndef COS_QUICKJS_PORT_H
#define COS_QUICKJS_PORT_H

#include "quickjs.h"
#include <stdbool.h>
#include <stddef.h>

/* Per-JSRuntime limit for untrusted page scripts.  This is intentionally
 * separate from NetSurf's shared resource/cache budget: JavaScript has a
 * deterministic 64MiB ceiling even when HTML/CSS/image caches are larger. */
#define COS_QUICKJS_RUNTIME_MEMORY_LIMIT_BYTES (64u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/* Creates a JSRuntime whose allocator is backed by kmalloc/krealloc/
 * kfree (the same kernel heap everything else in C-OS uses), with the
 * COS_QUICKJS_RUNTIME_MEMORY_LIMIT_BYTES (64MiB) bounded memory limit so a
 * runaway script cannot exhaust the shared browser/kernel allocation pool.
 * Returns NULL on allocation failure. */
JSRuntime* cos_js_new_runtime(void);

/* Creates a JSContext with the full standard set of intrinsics
 * (Object/Array/String/Math/JSON/RegExp/Promise/Map/Set/Date/...)
 * already installed - everything QuickJS itself provides out of the
 * box, independent of any DOM/browser bindings.
 *
 * C-OS 4.0.8 alpha: also installs:
 *   - console.log/warn/error/info  (route to serial console)
 *   - COS.version, COS.osName, COS.print(), COS.getMemInfo()
 * automatically in every new context. */
JSContext* cos_js_new_context(JSRuntime* rt);

/* Associates the page-local native DOM document with a QuickJS context.
 * The pointer is opaque to this embedding layer and remains owned by NetSurf. */
void cos_js_bind_document(JSContext *ctx, void *document);

/* Synchronise standard read-only page URL fields (document.URL and
 * location.href/protocol/host/hostname/pathname/search/hash/origin) with the
 * document URL that NetSurf is loading. This never performs navigation; page
 * navigation remains owned by the browser window. */
void cos_js_set_page_location(JSContext *ctx, const char *url);

/* Evaluates `script` as a top-level global-scope program and reports
 * the result value (or a thrown exception, with message and stack
 * trace when present) to the serial console. Intended as a standalone
 * smoke test / future REPL building block - not used by page script
 * execution, which wants silent success and only cares about
 * exceptions (see cos_js_eval_quiet below). */
void cos_js_eval_and_report(JSContext* ctx, const char* script, const char* filename);

/* Evaluates `script` (script_len bytes, not required to be
 * NUL-terminated) as a top-level global-scope program. Any thrown
 * exception (with stack trace, when available) is logged to the
 * serial console the same way cos_js_eval_and_report does, but the
 * *result* value of a successful run is not printed - this is the
 * variant real page/content script execution wants, where the point
 * is side effects (DOM mutation, console.log, ...), not the
 * completion value. Returns true if the script ran without throwing,
 * false if it threw (already reported) or could not be parsed. */
bool cos_js_eval_quiet(JSContext *ctx, const char *script, size_t script_len, const char *filename);

/* Dispatches a browser-style named event to listeners registered by the
 * compatibility window/document objects. */
bool cos_js_dispatch_event(JSContext *ctx, const char *event_type);
/* Dispatches a GUI-originated keydown event to the active page window. */
bool cos_js_dispatch_window_keydown(uint32_t key);
/* Dispatches an event to listeners registered on a real libdom target node. */
bool cos_js_dispatch_dom_event(JSContext *ctx, void *target_node, const char *event_type);
/* Dispatches a real libdom event through the active page context and walks
 * target ancestors so a listener on an element receives a child text click. */
bool cos_js_dispatch_bound_dom_event(void *target_node, const char *event_type);

/* Install the privileged `OS` drawing capability into an explicitly trusted
 * direct-evaluation context. This is deliberately not called for NetSurf page
 * contexts, so remote web content cannot draw arbitrary desktop overlays. */
void cos_js_enable_privileged_os_api(JSContext *ctx);

/* Release every bounded web timer owned by a context before that context is
 * destroyed. NetSurf's QuickJS backend must call this during thread teardown. */
void cos_js_cancel_context_timers(JSContext *ctx);
/* Release queued fetch/XMLHttpRequest work and page-session storage before
 * the owning page context is destroyed. */
void cos_js_cancel_context_web_state(JSContext *ctx);
/* Execute a bounded number of Promise/microtask jobs on the active page
 * context. Network completion resolves promises before this is called. */
void cos_js_pump_pending_jobs(void);
/* Drop cached libdom-to-JavaScript wrapper references before JS_FreeContext(). */
void cos_js_release_context_dom_wrappers(JSContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* COS_QUICKJS_PORT_H */
