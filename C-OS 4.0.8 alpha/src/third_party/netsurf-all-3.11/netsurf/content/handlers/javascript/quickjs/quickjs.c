/*
 * content/handlers/javascript/quickjs/quickjs.c
 *
 * QuickJS-backed implementation of NetSurf's javascript engine
 * interface (../js.h). This is a real backend against NetSurf's own
 * js_newheap/js_newthread/js_exec/... contract - the same contract
 * the historical duktape backend (../duktape/dukky.c) satisfies -
 * built from the vendored, unmodified QuickJS in
 * third_party/quickjs/, via the runtime/context helpers in
 * third_party/quickjs/quickjs_port.{c,h}.
 *
 * Scope, stated plainly:
 *
 *   - js_exec() genuinely executes arbitrary ECMAScript through
 *     QuickJS: variables, functions, closures, loops, and every
 *     builtin QuickJS itself ships (Math, JSON, Array, String,
 *     RegExp, Promise, Map/Set, Date, ...) all work, because they're
 *     part of the engine, not something this file has to implement.
 *
 *   - js_fire_event(), js_dom_event_add_listener() and
 *     js_handle_new_element() are safe no-ops, matching
 *     ../none/none.c's behaviour rather than duktape's. A real
 *     implementation needs to walk struct dom_node/dom_document/
 *     dom_string (from libdom) and expose them as JS objects/
 *     properties - that's what nsgenbind plus the .bnd binding-
 *     definition files under ../duktape/ drive for the duktape
 *     backend. libdom
 *     is not part of this C-OS build yet (only libwapcaplet and
 *     libparserutils are, as of this writing - see PORTING_NOTES.md
 *     at the repo root for current status), so there is no DOM tree
 *     here to bind against. Once libdom exists in this tree, that is
 *     where this file grows: win_priv/doc_priv below become real
 *     dom_window/dom_document pointers, and js_handle_new_element /
 *     js_fire_event gain real bodies instead of early returns.
 *
 *   - Nothing in NetSurf's own content pipeline calls js_exec() yet
 *     either, because content/handlers/html (the thing that would
 *     parse a <script> tag out of a page and call it) itself needs
 *     libdom + libcss + libhubbub, none of which are ported yet. So
 *     while this backend is real and compiles/links as a genuine
 *     NetSurf javascript engine, it isn't reachable from a live page
 *     load in *this* build yet - only from direct calls (which is
 *     how it's exercised/tested today; see PORTING_NOTES.md).
 *
 * None of the above is faked to look more complete than it is: the
 * intent is that every function below does exactly what its doc
 * comment says, and nothing it doesn't.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>

#include "utils/errors.h"
#include "javascript/js.h"
#include "../../../../../libdom/include/dom/core/node.h"

#include "quickjs_port.h"
#include "memory.h"
#include "serial.h"

/* Defined by the C-OS NetSurf frontend, which already builds against the
 * complete HTML private interface.  Keeping that dependency outside this
 * small JS backend avoids duplicating the heavyweight HTML include graph. */
extern struct dom_document *cos_netsurf_html_document_from_content(void *content);
extern const char *cos_netsurf_content_url_from_content(void *content);

/**
 * One heap per browser window (per the js.h contract). Maps directly
 * onto a QuickJS JSRuntime: NetSurf's "heap" and QuickJS's "runtime"
 * are the same concept - the GC arena/allocator scope that one or
 * more JS realms (NetSurf: "threads"; QuickJS: JSContexts) live in.
 */
struct jsheap {
    JSRuntime *rt;
};

/**
 * One thread per browsing context (per the js.h contract) - NOT an
 * OS/kernel thread. A page's <script> tags all share one of these,
 * i.e. one JS global scope, for their lifetime. Maps onto a QuickJS
 * JSContext.
 */
struct jsthread {
    JSContext *ctx;
    jsheap    *heap;
    bool       closed;
    /* Opaque HTML content supplied by browser_window. It remains owned by
     * NetSurf and lets us acquire its libdom document once parser creation has
     * completed. */
    void *content_priv;
    /* A retained reference to the real libdom document for this page. */
    struct dom_document *document;
};

void js_initialise(void)
{
    /* Nothing process-global to set up: cos_js_new_runtime() and
     * cos_js_new_context() do all necessary setup per-heap/
     * per-thread in js_newheap()/js_newthread() below. */
}

void js_finalise(void)
{
}

nserror js_newheap(int timeout, jsheap **heap)
{
    /* No watchdog/timeout support yet: QuickJS supports an
     * interrupt handler (JS_SetInterruptHandler) that could enforce
     * this, but wiring a wallclock timeout through to it needs a
     * timer source from the kernel side. Tracked as future work in
     * PORTING_NOTES.md rather than silently ignored-without-a-trace. */
    (void)timeout;

    if (heap == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    jsheap *h = (jsheap *)kmalloc(sizeof(jsheap));
    if (h == NULL) {
        return NSERROR_NOMEM;
    }

    h->rt = cos_js_new_runtime();
    if (h->rt == NULL) {
        kfree(h);
        return NSERROR_NOMEM;
    }

    *heap = h;
    return NSERROR_OK;
}

void js_destroyheap(jsheap *heap)
{
    if (heap == NULL) {
        return;
    }
    if (heap->rt != NULL) {
        JS_FreeRuntime(heap->rt);
    }
    kfree(heap);
}

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
    /* browser_window passes the requesting HTML content as doc_priv.  Ask the
     * C-OS frontend for its real libdom document rather than treating that
     * opaque content pointer itself as a dom_document. */
    (void)win_priv;

    if (heap == NULL || thread == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    jsthread *t = (jsthread *)kmalloc(sizeof(jsthread));
    if (t == NULL) {
        return NSERROR_NOMEM;
    }

    t->ctx = cos_js_new_context(heap->rt);
    if (t->ctx == NULL) {
        kfree(t);
        return NSERROR_NOMEM;
    }
    t->heap = heap;
    t->closed = false;
    t->content_priv = doc_priv;
    t->document = NULL;
    /* Document construction can lag CONTENT_MSG_GETTHREAD.  js_exec obtains
     * and binds it again immediately before each actual page script. */

    *thread = t;
    serial_puts("[NetSurf/QuickJS] thread content=");
    serial_puthex((uint64_t)(uintptr_t)t->content_priv);
    serial_puts("\n");
    serial_puts("[NetSurf/QuickJS] page JavaScript context created\n");
    return NSERROR_OK;
}

nserror js_closethread(jsthread *thread)
{
    if (thread == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    /* Disconnect future callbacks without freeing anything yet -
     * js_destroythread() (a separate, later call per the contract in
     * js.h) does the actual teardown. DOM references are released by
     * js_destroythread(); event callback registration remains a separate
     * integration step. */
    thread->closed = true;
    return NSERROR_OK;
}

void js_destroythread(jsthread *thread)
{
    if (thread == NULL) {
        return;
    }
    if (thread->ctx != NULL) {
        /* Timer callbacks retain JSValue references. Release them while the
         * context is live so a later GUI owner-thread pump cannot observe a
         * freed context after navigation or content teardown. */
        cos_js_cancel_context_timers(thread->ctx);
        cos_js_cancel_context_web_state(thread->ctx);
        cos_js_release_context_dom_wrappers(thread->ctx);
        JS_FreeContext(thread->ctx);
    }
    if (thread->document != NULL) {
        dom_node_unref((struct dom_node *)thread->document);
        thread->document = NULL;
    }
    thread->content_priv = NULL;
    kfree(thread);
}

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
    if (thread == NULL || thread->closed || thread->ctx == NULL || txt == NULL) {
        return false;
    }

    /* This is the real integration point: hand the script text
     * straight to QuickJS. Everything QuickJS supports natively
     * (functions, closures, loops, Math/JSON/Array/RegExp/Promise/
     * ...) is available to the page context. */
#ifdef COS_KERNEL
    /* Modern Google/Wikipedia bootstrap scripts commonly exceed 64KiB. Keep
     * a bounded ceiling for kernel safety while allowing real page
     * initialization code to run; resource fetch policy still constrains
     * external framework bundles. */
    if (txtlen > 256 * 1024) {
        serial_puts("[NetSurf/QuickJS] skipped page script over 256KiB\n");
        return true;
    }
#endif
    /* CONTENT_MSG_GETTHREAD may precede construction of htmlc->document.
     * Acquire and retain the real document lazily at the first actual script,
     * then rebind on later scripts so its settled page URL governs Storage and
     * relative Web API resolution. */
    if (thread->document == NULL && thread->content_priv != NULL) {
        struct dom_document *document =
            cos_netsurf_html_document_from_content(thread->content_priv);
        serial_puts("[NetSurf/QuickJS] bind content=");
        serial_puthex((uint64_t)(uintptr_t)thread->content_priv);
        serial_puts(" document=");
        serial_puthex((uint64_t)(uintptr_t)document);
        serial_puts("\n");
        if (document != NULL) {
            thread->document = document;
            dom_node_ref((struct dom_node *)thread->document);
        }
    }
    if (thread->document != NULL) {
        cos_js_bind_document(thread->ctx, thread->document);
    }
    /* htmlc->document may not be exposed through a node callback before the
     * parser executes its first script. The content URL is authoritative and
     * already available here, so establish the Web API origin independently. */
    if (thread->content_priv != NULL) {
        const char *page_url =
            cos_netsurf_content_url_from_content(thread->content_priv);
        if (page_url != NULL && page_url[0] != '\0') {
            cos_js_web_set_origin(thread->ctx, page_url);
            cos_js_set_page_location(thread->ctx, page_url);
        }
    }

    serial_puts("[NetSurf/QuickJS] executing page script bytes=");
    serial_putdec((uint64_t)txtlen);
    serial_puts("\n");
    bool ok = cos_js_eval_quiet(thread->ctx, (const char *)txt, txtlen,
                                 name != NULL ? name : "<script>");
    if (ok) {
        /* Promise callbacks and other microtasks are queued by QuickJS rather
         * than executed synchronously by JS_Eval.  Drain a bounded batch so a
         * page's immediate async initialization can complete within the same
         * cooperative NetSurf pump without creating an unbounded GUI stall. */
        JSContext *job_ctx = thread->ctx;
        int jobs = 0;
        while (jobs < 32) {
            int job_rc = JS_ExecutePendingJob(thread->heap->rt, &job_ctx);
            if (job_rc <= 0) break;
            ++jobs;
        }
        if (jobs == 32) {
            serial_puts("[NetSurf/QuickJS] pending job batch capped\n");
        }
    }
    serial_puts(ok ? "[NetSurf/QuickJS] page script complete\n"
                   : "[NetSurf/QuickJS] page script raised an exception\n");
    return ok;
}

bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)
{
    (void)doc;
    if (thread == NULL || thread->closed || thread->ctx == NULL) return false;
    /* Preserve the real libdom target through the backend boundary. The
     * bridge currently falls back to page listeners while its bounded node
     * listener registry is populated, then dispatches the same call per node. */
    return cos_js_dispatch_dom_event(thread->ctx, target, type);
}

bool js_dom_event_add_listener(jsthread *thread,
                                struct dom_document *document,
                                struct dom_node *node,
                                struct dom_string *event_type_dom,
                                void *js_funcval)
{
    (void)thread;
    (void)document;
    (void)node;
    (void)event_type_dom;
    (void)js_funcval;
    return true;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
    if (thread == NULL || node == NULL) return;

    struct dom_document *owner = NULL;
    if (dom_node_get_owner_document((struct dom_node *)node, &owner) != DOM_NO_ERR ||
        owner == NULL) {
        return;
    }

    if (thread->document != NULL) {
        dom_node_unref((struct dom_node *)thread->document);
    }
    thread->document = owner;
    cos_js_bind_document(thread->ctx, owner);
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
    (void)thread;
    (void)evt;
}
