/*
 * cos_js_web_api.h - bounded browser Web API bridge for C-OS QuickJS.
 *
 * This interface is intentionally small: it exposes a safe owner-thread
 * pump for fetch/XMLHttpRequest completion, origin binding and storage
 * lifetime management.  Page JavaScript must never call kernel HTTP or
 * filesystem primitives directly.
 */
#ifndef COS_JS_WEB_API_H
#define COS_JS_WEB_API_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install fetch, XMLHttpRequest, localStorage and sessionStorage in ctx. */
void cos_js_web_install(JSContext *ctx);

/* Bind the normalized origin and page URL after NetSurf has parsed a document. */
void cos_js_web_set_origin(JSContext *ctx, const char *url);

/* Resolve a navigation/fetch URL against the currently committed document URL.
 * Both helpers return false for an opaque/missing browsing context and never
 * perform I/O. */
bool cos_js_web_get_page_url(JSContext *ctx, char *out, size_t out_size);
bool cos_js_web_resolve_page_url(JSContext *ctx, const char *input,
                                 char *out, size_t out_size);

/* Execute at most one bounded transport completion batch on the GUI owner. */
void cos_js_pump_web_requests(void);

/* Cancel every queued operation and discard session storage before ctx dies. */
void cos_js_cancel_context_web_state(JSContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* COS_JS_WEB_API_H */
