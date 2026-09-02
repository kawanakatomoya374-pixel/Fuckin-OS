/**
 * quickjs_port.c - the C-OS <-> QuickJS embedding bridge.
 *
 * C-OS 4.0.8 alpha: full QuickJS + NetSurf binding layer.
 *
 * This file provides:
 *   1. The JSMallocFunctions hook wired to kmalloc/krealloc/kfree.
 *   2. cos_js_new_runtime() / cos_js_new_context() - the canonical
 *      entry points for creating a JS runtime/context anywhere in
 *      C-OS (NetSurf JS backend, shell "js" command, ...).
 *   3. cos_js_eval_and_report() / cos_js_eval_quiet() - script
 *      evaluation helpers used by both the NetSurf backend and the
 *      direct-eval path in cos_netsurf.c.
 *   4. cos_js_install_console() - installs a minimal `console`
 *      object with console.log/warn/error/info that route to the
 *      serial console. Called automatically by cos_js_new_context()
 *      so every context gets it for free.
 *   5. cos_js_install_cos_api() - installs a `COS` global object
 *      exposing C-OS kernel services to scripts:
 *        COS.version     - version string ("C-OS 4.0.8 alpha")
 *        COS.print(s)    - serial_puts wrapper
 *        COS.getMemInfo() - returns {used, total} from kernel memory
 *      Called automatically by cos_js_new_context().
 *
 * The bridge now exposes the page-bound libdom tree, bounded DOM mutation,
 * selector queries, classList/style accessors, event dispatch, timers, and
 * the separate origin-aware Web API module (fetch, XMLHttpRequest,
 * localStorage, and sessionStorage). Unsupported features remain explicitly
 * bounded rather than pretending to be a complete hosted browser; see
 * validation/browser/quickjs_dom_storage_selftest.html for the executable
 * compatibility smoke test.
 */
#include "quickjs_port.h"
#include "memory.h"
#include "serial.h"
#include "http.h"
#include "timer.h"
#include "cos_version.h"
#include "cos_js_os_api.h"
#include "cos_js_scheduler.h"
#include "cos_js_web_api.h"
#include "vga.h"
#include "../netsurf-all-3.11/libdom/include/dom/events/event.h"
#include "../netsurf-all-3.11/libdom/include/dom/events/event_listener.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/document.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/element.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/node.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/nodelist.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/namednodemap.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/string.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/text.h"
#include "../netsurf-all-3.11/libdom/include/dom/core/comment.h"
#include "../netsurf-all-3.11/libdom/include/dom/html/html_document.h"
#include "../netsurf-all-3.11/libdom/include/dom/html/html_input_element.h"
#include "../netsurf-all-3.11/libdom/include/dom/html/html_text_area_element.h"
#include <string.h>
#include <stdbool.h>

/* Declared here rather than including GUI-private state in this portable
 * embedding unit. The callback only requests a later owner-thread redraw;
 * it never presents or re-enters GUI composition synchronously. Needed by
 * both the real DOM mutation primitives below (a script that edits the page
 * should cause it to repaint) and the privileged OS.drawRect debug API
 * further down. */
extern void gui_request_redraw(void);
extern void cos_netsurf_browser_notify_dom_mutation(void);
extern void cos_netsurf_browser_set_document_title(const char *title);
extern bool cos_netsurf_browser_queue_navigation(const char *url);
static void cos_dom_mutation_observer_notify(void);
static void cos_dom_mutation_observer_notify_one(void);
static JSValue cos_web_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_set_inner_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_get_named_attr(JSContext *ctx, JSValueConst this_val, const char *attr_name);
static int cos_dom_last_mutation_kind;
static char cos_dom_last_attribute_name[96];
static char cos_dom_last_old_value[512];
static bool cos_dom_last_has_old_value;
static inline void cos_dom_remember_old_value(const char *value)
{
    if (value == NULL) { cos_dom_last_old_value[0] = '\0'; cos_dom_last_has_old_value = false; return; }
    size_t len = strlen(value); if (len >= sizeof(cos_dom_last_old_value)) len = sizeof(cos_dom_last_old_value) - 1;
    memcpy(cos_dom_last_old_value, value, len); cos_dom_last_old_value[len] = '\0'; cos_dom_last_has_old_value = true;
}
static inline void cos_dom_remember_attribute_name(const char *name)
{
    if (name == NULL) { cos_dom_last_attribute_name[0] = '\0'; return; }
    size_t len = strlen(name); if (len >= sizeof(cos_dom_last_attribute_name)) len = sizeof(cos_dom_last_attribute_name) - 1;
    memcpy(cos_dom_last_attribute_name, name, len); cos_dom_last_attribute_name[len] = '\0';
}

/* Snapshot the old attribute value without creating a QuickJS string.  The
 * returned libdom string is copied while it is owned, then released before
 * any mutation notification can enter the JS microtask machinery. */
static inline void cos_dom_snapshot_attribute(dom_element *element, const char *name_c)
{
    cos_dom_remember_old_value(NULL);
    if (element == NULL || name_c == NULL) return;
    dom_string *name = NULL, *value = NULL; bool present = false;
    if (dom_string_create((const uint8_t *)name_c, strlen(name_c), &name) == DOM_NO_ERR &&
        name != NULL && dom_element_has_attribute(element, name, &present) == DOM_NO_ERR && present &&
        dom_element_get_attribute(element, name, &value) == DOM_NO_ERR && value != NULL) {
        cos_dom_remember_old_value(dom_string_data(value));
        dom_string_unref(value);
    }
    if (name != NULL) dom_string_unref(name);
}
/* Snapshot CharacterData before libdom mutates it. The dom_string is
 * copied while owned and released before JS notification/microtasks run;
 * this avoids retaining a libdom reference across QuickJS callbacks. */
static inline void cos_dom_snapshot_character_data(dom_node *node)
{
    cos_dom_remember_old_value(NULL);
    if (node == NULL) return;
    dom_string *value = NULL;
    if (dom_characterdata_get_data((dom_characterdata *)node, &value) == DOM_NO_ERR && value != NULL) {
        size_t len = dom_string_byte_length(value);
        if (len >= sizeof(cos_dom_last_old_value)) len = sizeof(cos_dom_last_old_value) - 1;
        memcpy(cos_dom_last_old_value, dom_string_data(value), len);
        cos_dom_last_old_value[len] = '\0';
        cos_dom_last_has_old_value = true;
        dom_string_unref(value);
    }
}
static inline void cos_dom_notify_mutation(void)
{
    gui_request_redraw();
        cos_netsurf_browser_notify_dom_mutation();
    cos_dom_last_mutation_kind = 0;
    cos_dom_mutation_observer_notify();
}
/* ---- Real libdom Element wrapper ---- */
static JSClassID cos_dom_element_class_id;
/* C-OS owns one foreground NetSurf page context; this is a borrowed pointer
 * cleared before its JSContext is released. */
static JSContext *cos_js_active_page_context;
static inline void cos_dom_notify_attribute_mutation(void)
{
    gui_request_redraw();
    cos_netsurf_browser_notify_dom_mutation();
    cos_dom_last_mutation_kind = 1;
    cos_dom_mutation_observer_notify();
}
static inline void cos_dom_notify_character_data_mutation(void)
{
    gui_request_redraw();
    cos_netsurf_browser_notify_dom_mutation();
    cos_dom_last_mutation_kind = 2;
    cos_dom_mutation_observer_notify();
}
/* JavaScript requires repeated DOM lookups of the same native node to produce
 * the same object (`document.getElementById(x) === document.getElementById(x)`).
 * Keep the cache page-context scoped and release it before JS_FreeContext(). */
#define COS_DOM_WRAPPER_CACHE_MAX 4096
struct cos_dom_wrapper_cache_entry {
    JSContext *ctx;
    dom_node *node;
    JSValue wrapper;
    bool used;
};
static struct cos_dom_wrapper_cache_entry
    cos_dom_wrapper_cache[COS_DOM_WRAPPER_CACHE_MAX];

/* A second, minimal class for Text nodes (document.createTextNode(), and
 * anything a real element's DOM mutation methods hand back to script). Text
 * nodes are leaf CharacterData nodes in libdom: they do not implement
 * dom_element_vtable, so reaching an element-only method (e.g.
 * dom_element_get_attribute) through a Text node's vtable pointer is
 * undefined behaviour. A distinct class id/proto makes that impossible from
 * script, rather than merely unlikely. */
static JSClassID cos_dom_text_class_id;
static JSClassID cos_dom_comment_class_id;
/* DocumentFragment is a real libdom container node. Keeping a distinct
 * wrapper class prevents element-only attribute APIs from being called on it
 * while still exposing standard tree mutation and child traversal methods. */
static JSClassID cos_dom_fragment_class_id;

bool cos_js_dispatch_dom_event(JSContext *ctx, void *target_node, const char *event_type);

static void cos_dom_element_finalizer(JSRuntime *rt, JSValueConst val)
{
    (void)rt;
    dom_node *node = (dom_node *)JS_GetOpaque(val, cos_dom_element_class_id);
    if (node != NULL) dom_node_unref(node);
}

static void cos_dom_text_finalizer(JSRuntime *rt, JSValueConst val)
{
    (void)rt;
    dom_node *node = (dom_node *)JS_GetOpaque(val, cos_dom_text_class_id);
    if (node != NULL) dom_node_unref(node);
}
static void cos_dom_fragment_finalizer(JSRuntime *rt, JSValueConst val)
{
    (void)rt;
    dom_node *node = (dom_node *)JS_GetOpaque(val, cos_dom_fragment_class_id);
    if (node != NULL) dom_node_unref(node);
}
static void cos_dom_comment_finalizer(JSRuntime *rt, JSValueConst val)
{
    (void)rt;
    dom_node *node = (dom_node *)JS_GetOpaque(val, cos_dom_comment_class_id);
    if (node != NULL) dom_node_unref(node);
}

static dom_element *cos_dom_unwrap_element(JSContext *ctx, JSValueConst val)
{
    return (dom_element *)JS_GetOpaque2(ctx, val, cos_dom_element_class_id);
}

/* Accepts either wrapper class and returns the underlying dom_node* - the
 * common base that every generic mutation primitive (appendChild/
 * removeChild/insertBefore/parentNode/textContent) actually operates on.
 * Returns NULL for anything else, including plain objects and the inert
 * bootstrap-only placeholders from cos_web_new_element(). */
static dom_node *cos_dom_unwrap_any_node(JSContext *ctx, JSValueConst val)
{
    (void)ctx;
    dom_node *node = (dom_node *)JS_GetOpaque(val, cos_dom_element_class_id);
    if (node == NULL) node = (dom_node *)JS_GetOpaque(val, cos_dom_text_class_id);
    if (node == NULL) node = (dom_node *)JS_GetOpaque(val, cos_dom_fragment_class_id);
    if (node == NULL) node = (dom_node *)JS_GetOpaque(val, cos_dom_comment_class_id);
    return node;
}

static JSValue cos_dom_element_get_attribute(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_NULL;
    const char *name_c = JS_ToCString(ctx, argv[0]);
    if (name_c == NULL) return JS_EXCEPTION;
    dom_string *name = NULL;
    dom_string *value = NULL;
    JSValue result = JS_NULL;
    if (dom_string_create((const uint8_t *)name_c, strlen(name_c), &name) == DOM_NO_ERR &&
        dom_element_get_attribute(element, name, &value) == DOM_NO_ERR &&
        value != NULL) {
        result = JS_NewStringLen(ctx, dom_string_data(value),
                                 dom_string_byte_length(value));
        dom_string_unref(value);
    }
    if (name != NULL) dom_string_unref(name);
    JS_FreeCString(ctx, name_c);
    return result;
}

static JSValue cos_dom_element_has_attribute(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_FALSE;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_FALSE;
    const char *name_c = JS_ToCString(ctx, argv[0]);
    if (name_c == NULL) return JS_EXCEPTION;
    dom_string *name = NULL;
    bool present = false;
    if (dom_string_create((const uint8_t *)name_c, strlen(name_c), &name) == DOM_NO_ERR) {
        (void)dom_element_has_attribute(element, name, &present);
    }
    if (name != NULL) dom_string_unref(name);
    JS_FreeCString(ctx, name_c);
    return JS_NewBool(ctx, present);
}

static JSValue cos_dom_element_remove_attribute(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_ThrowTypeError(ctx, "removeAttribute called on a non-DOM element");
    const char *name_c = JS_ToCString(ctx, argv[0]);
    if (name_c == NULL) return JS_EXCEPTION;
    cos_dom_remember_attribute_name(name_c);
    cos_dom_snapshot_attribute(element, name_c);
    dom_string *name = NULL;
    if (dom_string_create((const uint8_t *)name_c, strlen(name_c), &name) == DOM_NO_ERR) {
        (void)dom_element_remove_attribute(element, name);
    }
    if (name != NULL) dom_string_unref(name);
    JS_FreeCString(ctx, name_c);
    cos_dom_notify_attribute_mutation();
    return JS_UNDEFINED;
}
static JSValue cos_dom_element_set_attribute(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_UNDEFINED;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_EXCEPTION;
    const char *name_c = JS_ToCString(ctx, argv[0]);
    const char *value_c = JS_ToCString(ctx, argv[1]);
    if (name_c == NULL || value_c == NULL) {
        if (name_c != NULL) cos_dom_remember_attribute_name(name_c);
        if (name_c != NULL) JS_FreeCString(ctx, name_c);
        if (value_c != NULL) JS_FreeCString(ctx, value_c);
        return JS_EXCEPTION;
    }
    cos_dom_remember_attribute_name(name_c);
    cos_dom_snapshot_attribute(element, name_c);
    dom_string *name = NULL; dom_string *value = NULL;
    if (dom_string_create((const uint8_t *)name_c, strlen(name_c), &name) == DOM_NO_ERR &&
        dom_string_create((const uint8_t *)value_c, strlen(value_c), &value) == DOM_NO_ERR) {
        (void)dom_element_set_attribute(element, name, value);
    }
    if (name != NULL) dom_string_unref(name);
    if (value != NULL) dom_string_unref(value);
    JS_FreeCString(ctx, name_c);
    JS_FreeCString(ctx, value_c);
    cos_dom_notify_attribute_mutation();
    return JS_UNDEFINED;
}
static bool cos_dom_ascii_equal_fold(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool cos_dom_is_tag(dom_element *element, const char *wanted)
{
    dom_string *name = NULL;
    bool match = false;
    if (element != NULL && dom_node_get_local_name((dom_node *)element, &name) == DOM_NO_ERR && name != NULL) {
        /* HTML DOM normalises local names differently across parser paths
         * (notably uppercase in the live parser).  Form bridge dispatch must
         * therefore follow HTML's ASCII case-insensitive tag comparison. */
        match = cos_dom_ascii_equal_fold((const char *)dom_string_data(name), wanted);
        dom_string_unref(name);
    }
    return match;
}

static JSValue cos_dom_element_get_value(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_NULL;
    dom_string *value = NULL;
    dom_exception err;
    if (cos_dom_is_tag(element, "input")) {
        err = dom_html_input_element_get_value((dom_html_input_element *)element, &value);
    } else if (cos_dom_is_tag(element, "textarea")) {
        err = dom_html_text_area_element_get_value((dom_html_text_area_element *)element, &value);
    } else {
        return JS_UNDEFINED;
    }
    if (err != DOM_NO_ERR || value == NULL) return JS_NewString(ctx, "");
    JSValue result = JS_NewStringLen(ctx, dom_string_data(value), dom_string_byte_length(value));
    dom_string_unref(value);
    return result;
}

static JSValue cos_dom_element_set_value(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_EXCEPTION;
    const char *value_c = JS_ToCString(ctx, argv[0]);
    if (value_c == NULL) return JS_EXCEPTION;
    dom_string *value = NULL;
    dom_exception err = dom_string_create((const uint8_t *)value_c, strlen(value_c), &value);
    if (err == DOM_NO_ERR && value != NULL) {
        if (cos_dom_is_tag(element, "input")) {
            err = dom_html_input_element_set_value((dom_html_input_element *)element, value);
        } else if (cos_dom_is_tag(element, "textarea")) {
            err = dom_html_text_area_element_set_value((dom_html_text_area_element *)element, value);
        }
        dom_string_unref(value);
    }
    JS_FreeCString(ctx, value_c);
    return err == DOM_NO_ERR ? JS_UNDEFINED : JS_EXCEPTION;
}

/* Boolean reflected attributes are standard Web API properties rather than
 * arbitrary JS expandos. Keeping disabled in libdom makes programmatic UI
 * state, CSS selectors and input activation agree. */
static JSValue cos_dom_element_get_disabled(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_FALSE;
    dom_string *name = NULL;
    bool present = false;
    if (dom_string_create((const uint8_t *)"disabled", 8, &name) == DOM_NO_ERR && name != NULL) {
        (void)dom_element_has_attribute(element, name, &present);
        dom_string_unref(name);
    }
    return JS_NewBool(ctx, present);
}

static JSValue cos_dom_element_set_disabled(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_ThrowTypeError(ctx, "disabled called on a non-DOM element");
    dom_string *name = NULL, *value = NULL;
    if (dom_string_create((const uint8_t *)"disabled", 8, &name) != DOM_NO_ERR || name == NULL) {
        return JS_EXCEPTION;
    }
    if (JS_ToBool(ctx, argv[0])) {
        if (dom_string_create((const uint8_t *)"", 0, &value) == DOM_NO_ERR && value != NULL) {
            (void)dom_element_set_attribute(element, name, value);
            dom_string_unref(value);
        }
    } else {
        (void)dom_element_remove_attribute(element, name);
    }
    dom_string_unref(name);
    cos_dom_notify_mutation();
    return JS_UNDEFINED;
}

/* HTMLElement.click() shares the same activation path as visible button input. */
static JSValue cos_dom_element_click(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_element *element = cos_dom_unwrap_element(ctx, this_val);
    if (element == NULL) return JS_ThrowTypeError(ctx, "click called on a non-DOM element");
    JSValue disabled = cos_dom_element_get_disabled(ctx, this_val, 0, NULL);
    int is_disabled = JS_ToBool(ctx, disabled);
    JS_FreeValue(ctx, disabled);
    if (is_disabled <= 0) (void)cos_js_dispatch_dom_event(ctx, element, "click");
    return JS_UNDEFINED;
}

/* Forward declarations used by the style/classList bridge below. */
static JSValue cos_dom_element_get_named_attr(JSContext *ctx, JSValueConst this_val,
                                              const char *attr_name);
static JSValue cos_dom_element_set_named_attr(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              const char *attr_name);

/* Inline CSS and class-token helpers. The initial browser bridge returned an
 * empty plain object for `element.style`, so assignments appeared successful
 * but never affected the live DOM. These helpers instead reflect the style and
 * class attributes and always request a real redraw. */
static const char *cos_dom_style_property_name(int magic)
{
    static const char *const names[] = {
        "display", "visibility", "color", "background-color",
        "font-size", "width", "height", "margin", "padding",
        "position", "top", "right", "bottom", "left", "opacity",
        "border", "border-radius", "text-align", "font-weight",
        "line-height", "overflow", "z-index"
    };
    return (magic >= 0 && magic < (int)(sizeof(names) / sizeof(names[0])))
        ? names[magic] : "";
}

static bool cos_dom_style_name_equal(const char *a, size_t a_len, const char *b)
{
    size_t i = 0;
    if (a == NULL || b == NULL) return false;
    while (i < a_len && b[i] != '\0') {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        ++i;
    }
    return i == a_len && b[i] == '\0';
}

static void cos_dom_get_style_value(dom_element *element, const char *name,
                                    char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (element == NULL || name == NULL) return;
    dom_string *attr_name = NULL;
    dom_string *style = NULL;
    if (dom_string_create((const uint8_t *)"style", 5, &attr_name) != DOM_NO_ERR ||
        dom_element_get_attribute(element, attr_name, &style) != DOM_NO_ERR || style == NULL) {
        if (attr_name != NULL) dom_string_unref(attr_name);
        return;
    }
    const char *text = (const char *)dom_string_data(style);
    size_t length = dom_string_byte_length(style);
    size_t p = 0;
    while (p < length) {
        while (p < length && (text[p] == ';' || text[p] == ' ' || text[p] == '\t')) ++p;
        size_t key_start = p;
        while (p < length && text[p] != ':' && text[p] != ';') ++p;
        size_t key_end = p;
        if (p >= length || text[p] != ':') {
            while (p < length && text[p] != ';') ++p;
            continue;
        }
        ++p;
        while (p < length && (text[p] == ' ' || text[p] == '\t')) ++p;
        size_t value_start = p;
        while (p < length && text[p] != ';') ++p;
        size_t value_end = p;
        while (key_end > key_start && (text[key_end - 1] == ' ' || text[key_end - 1] == '\t')) --key_end;
        while (value_end > value_start && (text[value_end - 1] == ' ' || text[value_end - 1] == '\t')) --value_end;
        if (cos_dom_style_name_equal(text + key_start, key_end - key_start, name)) {
            size_t n = value_end - value_start;
            if (n >= out_size) n = out_size - 1;
            memcpy(out, text + value_start, n);
            out[n] = '\0';
        }
    }
    dom_string_unref(style);
    dom_string_unref(attr_name);
}

static JSValue cos_dom_style_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic,
                                 JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv;
    dom_element *element = cos_dom_unwrap_element(ctx, func_data[0]);
    char value[256];
    cos_dom_get_style_value(element, cos_dom_style_property_name(magic), value, sizeof(value));
    return JS_NewString(ctx, value);
}

static JSValue cos_dom_style_set(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic,
                                 JSValueConst *func_data)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    dom_element *element = cos_dom_unwrap_element(ctx, func_data[0]);
    if (element == NULL) return JS_EXCEPTION;
    const char *value = JS_ToCString(ctx, argv[0]);
    if (value == NULL) return JS_EXCEPTION;
    char old_style[768];
    char merged[1024];
    cos_dom_get_style_value(element, "__all__", old_style, sizeof(old_style));
    /* `__all__` is not a CSS property, so fetch the raw attribute explicitly. */
    dom_string *attr_name = NULL;
    dom_string *attr_value = NULL;
    old_style[0] = '\0';
    if (dom_string_create((const uint8_t *)"style", 5, &attr_name) == DOM_NO_ERR &&
        dom_element_get_attribute(element, attr_name, &attr_value) == DOM_NO_ERR && attr_value != NULL) {
        size_t n = dom_string_byte_length(attr_value);
        if (n >= sizeof(old_style)) n = sizeof(old_style) - 1;
        memcpy(old_style, dom_string_data(attr_value), n);
        old_style[n] = '\0';
        dom_string_unref(attr_value);
    }
    if (attr_name != NULL) dom_string_unref(attr_name);
    size_t used = 0;
    while (old_style[used] != '\0' && used + 1 < sizeof(merged)) { merged[used] = old_style[used]; ++used; }
    if (used > 0 && merged[used - 1] != ';' && used + 1 < sizeof(merged)) merged[used++] = ';';
    const char *property = cos_dom_style_property_name(magic);
    for (size_t i = 0; property[i] != '\0' && used + 1 < sizeof(merged); ++i) merged[used++] = property[i];
    if (used + 1 < sizeof(merged)) merged[used++] = ':';
    for (size_t i = 0; value[i] != '\0' && used + 1 < sizeof(merged); ++i) merged[used++] = value[i];
    if (used + 1 < sizeof(merged)) merged[used++] = ';';
    merged[used] = '\0';
    JS_FreeCString(ctx, value);
    dom_string *name = NULL;
    dom_string *style = NULL;
    if (dom_string_create((const uint8_t *)"style", 5, &name) == DOM_NO_ERR &&
        dom_string_create((const uint8_t *)merged, strlen(merged), &style) == DOM_NO_ERR) {
        (void)dom_element_set_attribute(element, name, style);
    }
    if (name != NULL) dom_string_unref(name);
    if (style != NULL) dom_string_unref(style);
    cos_dom_notify_mutation();
    return JS_UNDEFINED;
}

static bool cos_dom_class_contains(const char *classes, const char *token)
{
    if (classes == NULL || token == NULL || token[0] == '\0') return false;
    size_t token_len = strlen(token);
    for (size_t p = 0; classes[p] != '\0';) {
        while (classes[p] == ' ' || classes[p] == '\t' || classes[p] == '\n') ++p;
        size_t start = p;
        while (classes[p] != '\0' && classes[p] != ' ' && classes[p] != '\t' && classes[p] != '\n') ++p;
        if (p - start == token_len && memcmp(classes + start, token, token_len) == 0) return true;
    }
    return false;
}

static JSValue cos_dom_class_list_contains(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv, int magic,
                                           JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 1) return JS_FALSE;
    const char *token = JS_ToCString(ctx, argv[0]);
    JSValue value = cos_dom_element_get_named_attr(ctx, func_data[0], "class");
    const char *classes = JS_ToCString(ctx, value);
    bool found = cos_dom_class_contains(classes, token);
    if (classes != NULL) JS_FreeCString(ctx, classes);
    JS_FreeValue(ctx, value);
    if (token != NULL) JS_FreeCString(ctx, token);
    return JS_NewBool(ctx, found);
}

static JSValue cos_dom_class_list_add(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic,
                                      JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    dom_element *element = cos_dom_unwrap_element(ctx, func_data[0]);
    if (element == NULL) return JS_EXCEPTION;
    JSValue current = cos_dom_element_get_named_attr(ctx, func_data[0], "class");
    const char *classes = JS_ToCString(ctx, current);
    char out[512];
    size_t used = 0;
    if (classes != NULL) while (classes[used] != '\0' && used + 1 < sizeof(out)) { out[used] = classes[used]; ++used; }
    for (int a = 0; a < argc; ++a) {
        const char *token = JS_ToCString(ctx, argv[a]);
        if (token != NULL && token[0] != '\0' && !cos_dom_class_contains(out, token)) {
            if (used > 0 && used + 1 < sizeof(out)) out[used++] = ' ';
            for (size_t i = 0; token[i] != '\0' && used + 1 < sizeof(out); ++i) out[used++] = token[i];
        }
        if (token != NULL) JS_FreeCString(ctx, token);
    }
    out[used] = '\0';
    if (classes != NULL) JS_FreeCString(ctx, classes);
    JS_FreeValue(ctx, current);
    JSValue replacement = JS_NewString(ctx, out);
    JSValueConst args[1] = { replacement };
    JSValue result = cos_dom_element_set_named_attr(ctx, func_data[0], 1, args, "class");
    JS_FreeValue(ctx, replacement);
    return result;
}

static JSValue cos_dom_class_list_remove(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic,
                                         JSValueConst *func_data);

static JSValue cos_dom_class_list_toggle(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic,
                                         JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 1) return JS_FALSE;
    const char *token = JS_ToCString(ctx, argv[0]);
    if (token == NULL || token[0] == '\0') { if (token != NULL) JS_FreeCString(ctx, token); return JS_FALSE; }
    JSValue current = cos_dom_element_get_named_attr(ctx, func_data[0], "class");
    const char *classes = JS_ToCString(ctx, current);
    bool present = cos_dom_class_contains(classes, token);
    bool add = argc < 2 ? !present : JS_ToBool(ctx, argv[1]);
    JS_FreeValue(ctx, current);
    if (classes != NULL) JS_FreeCString(ctx, classes);
    JSValue arg = JS_NewString(ctx, token);
    JSValue result = add
        ? cos_dom_class_list_add(ctx, JS_UNDEFINED, 1, (JSValueConst *)&arg, 0, func_data)
        : cos_dom_class_list_remove(ctx, JS_UNDEFINED, 1, (JSValueConst *)&arg, 0, func_data);
    JS_FreeValue(ctx, arg);
    JS_FreeCString(ctx, token);
    JS_FreeValue(ctx, result);
    return JS_NewBool(ctx, add);
}

static JSValue cos_dom_class_list_remove(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic,
                                         JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    dom_element *element = cos_dom_unwrap_element(ctx, func_data[0]);
    if (element == NULL) return JS_EXCEPTION;
    JSValue current = cos_dom_element_get_named_attr(ctx, func_data[0], "class");
    const char *classes = JS_ToCString(ctx, current);
    char out[512]; size_t used = 0; size_t p = 0;
    while (classes != NULL && classes[p] != '\0') {
        while (classes[p] == ' ' || classes[p] == '\t' || classes[p] == '\n') ++p;
        size_t start = p;
        while (classes[p] != '\0' && classes[p] != ' ' && classes[p] != '\t' && classes[p] != '\n') ++p;
        size_t len = p - start; bool remove = false;
        for (int a = 0; a < argc; ++a) {
            const char *token = JS_ToCString(ctx, argv[a]);
            if (token != NULL && strlen(token) == len && memcmp(token, classes + start, len) == 0) remove = true;
            if (token != NULL) JS_FreeCString(ctx, token);
        }
        if (len > 0 && !remove) {
            if (used > 0 && used + 1 < sizeof(out)) out[used++] = ' ';
            for (size_t i = 0; i < len && used + 1 < sizeof(out); ++i) out[used++] = classes[start + i];
        }
    }
    out[used] = '\0';
    if (classes != NULL) JS_FreeCString(ctx, classes);
    JS_FreeValue(ctx, current);
    JSValue replacement = JS_NewString(ctx, out);
    JSValueConst args[1] = { replacement };
    JSValue result = cos_dom_element_set_named_attr(ctx, func_data[0], 1, args, "class");
    JS_FreeValue(ctx, replacement);
    return result;
}

static void cos_dom_define_bound_accessor(JSContext *ctx, JSValueConst obj, const char *name,
                                          int magic, JSValue element)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue data_get[1] = { JS_DupValue(ctx, element) };
    JSValue data_set[1] = { JS_DupValue(ctx, element) };
    JSValue getter = JS_NewCFunctionData(ctx, cos_dom_style_get, 0, magic, 1, data_get);
    JSValue setter = JS_NewCFunctionData(ctx, cos_dom_style_set, 1, magic, 1, data_set);
    if (JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter); JS_FreeValue(ctx, setter);
    }
    JS_FreeValue(ctx, data_get[0]);
    JS_FreeValue(ctx, data_set[0]);
    JS_FreeAtom(ctx, atom);
}

static int cos_dom_style_magic_from_name(JSContext *ctx, JSValueConst value)
{
    const char *name = JS_ToCString(ctx, value);
    if (name == NULL) return -1;
    int result = -1;
    for (int i = 0; i < 22; ++i) {
        const char *canonical = cos_dom_style_property_name(i);
        if (strcmp(name, canonical) == 0 ||
            (strcmp(name, "backgroundColor") == 0 && i == 3) ||
            (strcmp(name, "fontSize") == 0 && i == 4)) {
            result = i;
            break;
        }
    }
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue cos_dom_style_get_css_text(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    JSValue name = JS_NewString(ctx, "style");
    JSValue result = cos_dom_element_get_attribute(ctx, func_data[0], 1, &name);
    JS_FreeValue(ctx, name);
    return result;
}

static JSValue cos_dom_style_set_css_text(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 1) return JS_UNDEFINED;
    JSValue name = JS_NewString(ctx, "style");
    JSValue args[2] = { name, JS_DupValue(ctx, argv[0]) };
    JSValue result = cos_dom_element_set_attribute(ctx, func_data[0], 2, args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    return result;
}

static JSValue cos_dom_style_get_property(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 1) return JS_NewString(ctx, "");
    int index = cos_dom_style_magic_from_name(ctx, argv[0]);
    if (index < 0) return JS_NewString(ctx, "");
    return cos_dom_style_get(ctx, this_val, 0, NULL, index, func_data);
}

static JSValue cos_dom_style_set_property(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 2) return JS_UNDEFINED;
    int index = cos_dom_style_magic_from_name(ctx, argv[0]);
    if (index < 0) return JS_UNDEFINED;
    return cos_dom_style_set(ctx, this_val, 1, &argv[1], index, func_data);
}

static JSValue cos_dom_style_remove_property(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv,
                                             int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    if (argc < 1) return JS_NewString(ctx, "");
    int index = cos_dom_style_magic_from_name(ctx, argv[0]);
    if (index < 0) return JS_NewString(ctx, "");
    JSValue old = cos_dom_style_get(ctx, this_val, 0, NULL, index, func_data);
    JSValue empty = JS_NewString(ctx, "");
    (void)cos_dom_style_set(ctx, this_val, 1, &empty, index, func_data);
    JS_FreeValue(ctx, empty);
    return old;
}

static JSValue cos_dom_style_item(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t index = 0;
    if (argc < 1 || JS_ToUint32(ctx, &index, argv[0]) || index >= 22u)
        return JS_NewString(ctx, "");
    return JS_NewString(ctx, cos_dom_style_property_name((int)index));
}

static JSValue cos_dom_text_split_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_get_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_substring_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_append_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_insert_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_delete_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_characterdata_replace_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_web_create_text_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_get_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_node_get_parent_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
bool cos_js_dispatch_bound_dom_event(void *target_node, const char *event_type);
static JSValue cos_dom_element_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_get_elements_by_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_node_get_relative(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
static JSValue cos_dom_element_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_insert_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_element_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static JSValue cos_dom_element_append_variadic(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    for (int i = 0; i < argc; ++i) {
        JSValue child = JS_IsString(argv[i]) ? cos_web_create_text_node(ctx, this_val, 1, &argv[i])
                                             : JS_DupValue(ctx, argv[i]);
        if (JS_IsException(child)) return child;
        JSValue args[1] = { child };
        JSValue result = cos_dom_element_append_child(ctx, this_val, 1, args);
        JS_FreeValue(ctx, child);
        if (JS_IsException(result)) return result;
        JS_FreeValue(ctx, result);
    }
    return JS_UNDEFINED;
}

static JSValue cos_dom_element_prepend_variadic(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv)
{
    JSValue ref = cos_dom_node_get_relative(ctx, this_val, 1, NULL, 1);
    if (JS_IsException(ref)) return ref;
    for (int i = argc - 1; i >= 0; --i) {
        JSValue child = JS_IsString(argv[i]) ? cos_web_create_text_node(ctx, this_val, 1, &argv[i])
                                             : JS_DupValue(ctx, argv[i]);
        if (JS_IsException(child)) { JS_FreeValue(ctx, ref); return child; }
        JSValue args[2] = { child, JS_DupValue(ctx, ref) };
        JSValue result = cos_dom_element_insert_before(ctx, this_val, 2, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
        if (JS_IsException(result)) { JS_FreeValue(ctx, ref); return result; }
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, ref);
    return JS_UNDEFINED;
}

static JSValue cos_dom_element_insert_relative(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv, int after)
{
    JSValue parent = cos_dom_node_get_parent_node(ctx, this_val, 0, NULL);
    if (!JS_IsObject(parent)) { JS_FreeValue(ctx, parent); return JS_UNDEFINED; }
    JSValue ref = after ? cos_dom_node_get_relative(ctx, this_val, 0, NULL, 4)
                        : JS_DupValue(ctx, this_val);
    for (int i = 0; i < argc; ++i) {
        JSValue child = JS_IsString(argv[i]) ? cos_web_create_text_node(ctx, this_val, 1, &argv[i])
                                             : JS_DupValue(ctx, argv[i]);
        if (JS_IsException(child)) { JS_FreeValue(ctx, ref); JS_FreeValue(ctx, parent); return child; }
        JSValue result;
        if (JS_IsNull(ref) || JS_IsUndefined(ref)) {
            result = cos_dom_element_append_child(ctx, parent, 1, &child);
        } else {
            JSValue args[2] = { child, JS_DupValue(ctx, ref) };
            result = cos_dom_element_insert_before(ctx, parent, 2, args);
            JS_FreeValue(ctx, args[1]);
        }
        JS_FreeValue(ctx, child);
        if (JS_IsException(result)) { JS_FreeValue(ctx, ref); JS_FreeValue(ctx, parent); return result; }
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, ref); JS_FreeValue(ctx, parent); return JS_UNDEFINED;
}

static JSValue cos_dom_element_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return cos_dom_element_insert_relative(ctx, this_val, argc, argv, 0); }

static JSValue cos_dom_element_after(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return cos_dom_element_insert_relative(ctx, this_val, argc, argv, 1); }

static JSValue cos_dom_element_replace_with(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue parent = cos_dom_node_get_parent_node(ctx, this_val, 0, NULL);
    if (!JS_IsObject(parent)) { JS_FreeValue(ctx, parent); return JS_UNDEFINED; }
    JSValue ref = cos_dom_node_get_relative(ctx, this_val, 0, NULL, 4);
    JSValue old_args[1] = { JS_DupValue(ctx, this_val) };
    JSValue removed = cos_dom_element_remove_child(ctx, parent, 1, old_args);
    JS_FreeValue(ctx, old_args[0]); JS_FreeValue(ctx, removed);
    JSValue result = cos_dom_element_insert_relative(ctx, JS_IsNull(ref) ? parent : parent,
                                                     argc, argv, JS_IsNull(ref) ? 1 : 1);
    JS_FreeValue(ctx, ref); JS_FreeValue(ctx, parent); return result;
}

static JSValue cos_dom_element_get_attribute_names(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    dom_namednodemap *map = NULL; dom_ulong length = 0;
    JSValue result = JS_NewArray(ctx);
    if (node == NULL || dom_node_get_attributes(node, &map) != DOM_NO_ERR || map == NULL) return result;
    if (dom_namednodemap_get_length(map, &length) == DOM_NO_ERR) {
        for (dom_ulong i = 0; i < length; ++i) {
            dom_node *attr = NULL; dom_string *name = NULL;
            if (dom_namednodemap_item(map, i, &attr) == DOM_NO_ERR && attr != NULL &&
                dom_node_get_node_name(attr, &name) == DOM_NO_ERR && name != NULL) {
                JS_SetPropertyUint32(ctx, result, (uint32_t)i,
                    JS_NewStringLen(ctx, dom_string_data(name), dom_string_byte_length(name)));
                dom_string_unref(name);
            }
            if (attr != NULL) dom_node_unref(attr);
        }
    }
    dom_namednodemap_unref(map); return result;
}

static void cos_js_define_readonly_accessor(JSContext *ctx, JSValueConst obj,
                                            const char *name, JSCFunction *getter);
static JSValue cos_dom_element_get_inner_html(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv);

static JSValue cos_dom_mutation_observer_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "MutationObserver.observe requires a Node target");
    bool child_list = false, attributes = false, character_data = false, subtree = false;
    bool attribute_old_value = false, character_data_old_value = false, has_attribute_filter = false;
    JSValue attribute_filter = JS_NewArray(ctx);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue child = JS_GetPropertyStr(ctx, argv[1], "childList");
        JSValue attrs = JS_GetPropertyStr(ctx, argv[1], "attributes");
        JSValue chars = JS_GetPropertyStr(ctx, argv[1], "characterData");
        JSValue sub = JS_GetPropertyStr(ctx, argv[1], "subtree");
        JSValue filter = JS_GetPropertyStr(ctx, argv[1], "attributeFilter");
        JSValue attr_old = JS_GetPropertyStr(ctx, argv[1], "attributeOldValue");
        JSValue char_old = JS_GetPropertyStr(ctx, argv[1], "characterDataOldValue");
        child_list = JS_ToBool(ctx, child); attributes = JS_ToBool(ctx, attrs);
        character_data = JS_ToBool(ctx, chars); subtree = JS_ToBool(ctx, sub);
        attribute_old_value = JS_ToBool(ctx, attr_old); character_data_old_value = JS_ToBool(ctx, char_old);
        if (JS_IsArray(filter)) { JS_FreeValue(ctx, attribute_filter); attribute_filter = filter; has_attribute_filter = true; } else JS_FreeValue(ctx, filter);
        JS_FreeValue(ctx, child); JS_FreeValue(ctx, attrs); JS_FreeValue(ctx, chars); JS_FreeValue(ctx, sub); JS_FreeValue(ctx, attr_old); JS_FreeValue(ctx, char_old);
        if (!child_list && !attributes && !character_data)
            return JS_ThrowTypeError(ctx, "MutationObserver options select no mutation types");
        if ((attribute_old_value || has_attribute_filter) && !attributes)
            return JS_ThrowTypeError(ctx, "MutationObserver attribute options require attributes");
        if (character_data_old_value && !character_data)
            return JS_ThrowTypeError(ctx, "MutationObserver characterDataOldValue requires characterData");
    } else {
        return JS_ThrowTypeError(ctx, "MutationObserver.observe requires options");
    }
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_childList", JS_NewBool(ctx, child_list));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributes", JS_NewBool(ctx, attributes));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_characterData", JS_NewBool(ctx, character_data));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_subtree", JS_NewBool(ctx, subtree));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributeOldValue", JS_NewBool(ctx, attribute_old_value));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_characterDataOldValue", JS_NewBool(ctx, character_data_old_value));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributeFilter", attribute_filter);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_target", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_observing", JS_NewBool(ctx, true));
    return JS_UNDEFINED;
}

static JSValue cos_dom_mutation_observer_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_observing", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_target", JS_NULL);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_childList", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributes", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_characterData", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_subtree", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributeOldValue", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_characterDataOldValue", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_attributeFilter", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_records", JS_NewArray(ctx));
    return JS_UNDEFINED;
}

static JSValue cos_dom_mutation_observer_take_records(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue records = JS_GetPropertyStr(ctx, this_val, "_cos_records");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_records", JS_NewArray(ctx));
    return records;
}

static JSValue cos_dom_range_set_boundary_relative(JSContext *ctx, JSValueConst this_val, JSValueConst node, bool start_boundary, bool after)
{
    JSValue parent = JS_GetPropertyStr(ctx, node, "parentNode");
    if (!JS_IsObject(parent)) { JS_FreeValue(ctx, parent); return JS_ThrowDOMException(ctx, "InvalidStateError", "Node has no parent"); }
    JSValue children = JS_GetPropertyStr(ctx, parent, "childNodes");
    JSValue length = JS_GetPropertyStr(ctx, children, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, length);
    uint32_t index = n;
    for (uint32_t i = 0; i < n; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, children, i);
        bool same = JS_IsStrictEqual(ctx, item, node);
        JS_FreeValue(ctx, item);
        if (same) { index = i + (after ? 1u : 0u); break; }
    }
    JS_FreeValue(ctx, length); JS_FreeValue(ctx, children);
    if (index > n) { JS_FreeValue(ctx, parent); return JS_ThrowDOMException(ctx, "NotFoundError", "Node is not a child of its parent"); }
    if (start_boundary) {
        JS_SetPropertyStr(ctx, (JSValue)this_val, "startContainer", JS_DupValue(ctx, parent));
        JS_SetPropertyStr(ctx, (JSValue)this_val, "startOffset", JS_NewUint32(ctx, index));
    } else {
        JS_SetPropertyStr(ctx, (JSValue)this_val, "endContainer", JS_DupValue(ctx, parent));
        JS_SetPropertyStr(ctx, (JSValue)this_val, "endOffset", JS_NewUint32(ctx, index));
    }
    JS_FreeValue(ctx, parent);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_set_start_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setStartBefore requires a Node");
    return cos_dom_range_set_boundary_relative(ctx, this_val, argv[0], true, false);
}
static JSValue cos_dom_range_set_start_after(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setStartAfter requires a Node");
    return cos_dom_range_set_boundary_relative(ctx, this_val, argv[0], true, true);
}
static JSValue cos_dom_range_set_end_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setEndBefore requires a Node");
    return cos_dom_range_set_boundary_relative(ctx, this_val, argv[0], false, false);
}
static JSValue cos_dom_range_set_end_after(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setEndAfter requires a Node");
    return cos_dom_range_set_boundary_relative(ctx, this_val, argv[0], false, true);
}
static JSValue cos_dom_range_set_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setStart requires a Node and offset");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startContainer", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startOffset", JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_set_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.setEnd requires a Node and offset");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endContainer", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endOffset", JS_DupValue(ctx, argv[1]));
    JSValue start = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "collapsed", JS_NewBool(ctx, JS_IsStrictEqual(ctx, start, argv[0])));
    JS_FreeValue(ctx, start);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_collapse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    bool to_start = argc == 0 || JS_ToBool(ctx, argv[0]);
    const char *from = to_start ? "startContainer" : "endContainer";
    const char *offset = to_start ? "startOffset" : "endOffset";
    JSValue node = JS_GetPropertyStr(ctx, this_val, from);
    JSValue pos = JS_GetPropertyStr(ctx, this_val, offset);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startContainer", JS_DupValue(ctx, node));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endContainer", JS_DupValue(ctx, node));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startOffset", JS_DupValue(ctx, pos));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endOffset", JS_DupValue(ctx, pos));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "collapsed", JS_TRUE);
    JS_FreeValue(ctx, node); JS_FreeValue(ctx, pos);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_select_node_contents(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.selectNodeContents requires a Node");
    JSValue length = JS_GetPropertyStr(ctx, argv[0], "textContent");
    JSValue zero = JS_NewInt32(ctx, 0);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startContainer", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endContainer", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "startOffset", zero);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endOffset", JS_GetPropertyStr(ctx, length, "length"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "collapsed", JS_FALSE);
    JS_FreeValue(ctx, length);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue node = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JSValue text = JS_GetPropertyStr(ctx, node, "textContent");
    JS_FreeValue(ctx, node);
    return text;
}
static JSValue cos_dom_range_delete_contents(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue start = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JSValue end = JS_GetPropertyStr(ctx, this_val, "endContainer");
    if (!JS_IsStrictEqual(ctx, start, end)) { JS_FreeValue(ctx, start); JS_FreeValue(ctx, end); return JS_UNDEFINED; }
    JSValue text = JS_GetPropertyStr(ctx, start, "textContent");
    JSValue so = JS_GetPropertyStr(ctx, this_val, "startOffset");
    JSValue eo = JS_GetPropertyStr(ctx, this_val, "endOffset");
    int32_t a = 0, b = 0; JS_ToInt32(ctx, &a, so); JS_ToInt32(ctx, &b, eo);
    size_t n = 0; const char *s = JS_ToCStringLen(ctx, &n, text);
    if (s != NULL) {
        if (a < 0) a = 0; if (b < a) b = a; if ((size_t)b > n) b = (int32_t)n;
        size_t out_n = n - (size_t)(b - a);
        char *out = (char *)js_malloc(ctx, out_n + 1);
        if (out != NULL) {
            memcpy(out, s, (size_t)a); memcpy(out + a, s + b, n - (size_t)b); out[out_n] = '\0';
            JSValue replacement = JS_NewStringLen(ctx, out, out_n); JS_SetPropertyStr(ctx, start, "textContent", replacement); js_free(ctx, out);
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, text); JS_FreeValue(ctx, so); JS_FreeValue(ctx, eo);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endContainer", JS_DupValue(ctx, start));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "endOffset", JS_GetPropertyStr(ctx, this_val, "startOffset"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "collapsed", JS_TRUE);
    JS_FreeValue(ctx, start); JS_FreeValue(ctx, end);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_insert_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Range.insertNode requires a Node");
    JSValue container = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JSValue offset_value = JS_GetPropertyStr(ctx, this_val, "startOffset");
    JSValue children = JS_GetPropertyStr(ctx, container, "childNodes");
    JSValue length = JS_GetPropertyStr(ctx, children, "length");
    uint32_t offset = 0, count = 0;
    JS_ToUint32(ctx, &offset, offset_value); JS_ToUint32(ctx, &count, length);
    JSValue result = JS_EXCEPTION;
    if (offset >= count) {
        JSValue append = JS_GetPropertyStr(ctx, container, "appendChild");
        result = JS_Call(ctx, append, container, 1, argv);
        JS_FreeValue(ctx, append);
    } else {
        JSValue reference = JS_GetPropertyUint32(ctx, children, offset);
        JSValue insert = JS_GetPropertyStr(ctx, container, "insertBefore");
        JSValue args[2] = { argv[0], reference };
        result = JS_Call(ctx, insert, container, 2, args);
        JS_FreeValue(ctx, insert); JS_FreeValue(ctx, reference);
    }
    JS_FreeValue(ctx, length); JS_FreeValue(ctx, children); JS_FreeValue(ctx, offset_value); JS_FreeValue(ctx, container);
    if (JS_IsException(result)) return result;
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_clone_contents(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue fragment = JS_NewObject(ctx);
    JSValue text = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JSValue content = JS_GetPropertyStr(ctx, text, "textContent");
    JS_SetPropertyStr(ctx, fragment, "textContent", content);
    JS_FreeValue(ctx, text);
    return fragment;
}
static JSValue cos_dom_range_extract_contents(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue fragment = JS_NewObject(ctx);
    JSValue text = JS_GetPropertyStr(ctx, this_val, "startContainer");
    JSValue content = JS_GetPropertyStr(ctx, text, "textContent");
    JS_SetPropertyStr(ctx, fragment, "textContent", content);
    JS_FreeValue(ctx, text);
    JSValue deleter = JS_GetPropertyStr(ctx, this_val, "deleteContents");
    JSValue result = JS_Call(ctx, deleter, this_val, 0, NULL);
    JS_FreeValue(ctx, deleter); JS_FreeValue(ctx, result);
    return fragment;
}
static JSValue cos_dom_range_detach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_detached", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue cos_dom_range_compare_boundary_points(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "Range.compareBoundaryPoints requires a Range");
    int32_t how = 0;
    if (argc > 0 && JS_ToInt32(ctx, &how, argv[0]) < 0) return JS_EXCEPTION;
    if (how < 0 || how > 3) return JS_ThrowDOMException(ctx, "NotSupportedError", "Invalid comparison mode");
    const char *left_node_name = (how == 2 || how == 3) ? "endContainer" : "startContainer";
    const char *left_offset_name = (how == 2 || how == 3) ? "endOffset" : "startOffset";
    const char *right_node_name = (how == 1 || how == 2) ? "endContainer" : "startContainer";
    const char *right_offset_name = (how == 1 || how == 2) ? "endOffset" : "startOffset";
    JSValue left_node = JS_GetPropertyStr(ctx, this_val, left_node_name);
    JSValue right_node = JS_GetPropertyStr(ctx, argv[1], right_node_name);
    JSValue left_offset = JS_GetPropertyStr(ctx, this_val, left_offset_name);
    JSValue right_offset = JS_GetPropertyStr(ctx, argv[1], right_offset_name);
    int result = 0;
    if (!JS_IsStrictEqual(ctx, left_node, right_node)) {
        JS_FreeValue(ctx, left_node); JS_FreeValue(ctx, right_node); JS_FreeValue(ctx, left_offset); JS_FreeValue(ctx, right_offset);
        return JS_ThrowDOMException(ctx, "WrongDocumentError", "Boundary points are in different trees");
    }
    int32_t a = 0, b = 0; JS_ToInt32(ctx, &a, left_offset); JS_ToInt32(ctx, &b, right_offset);
    if (a < b) result = -1; else if (a > b) result = 1;
    JS_FreeValue(ctx, left_node); JS_FreeValue(ctx, right_node); JS_FreeValue(ctx, left_offset); JS_FreeValue(ctx, right_offset);
    return JS_NewInt32(ctx, result);
}
static JSValue cos_dom_range_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue copy = JS_NewObject(ctx);
    const char *props[] = { "startContainer", "startOffset", "endContainer", "endOffset", "collapsed" };
    for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); ++i) {
        JSValue value = JS_GetPropertyStr(ctx, this_val, props[i]);
        JS_SetPropertyStr(ctx, copy, props[i], value);
    }
    JS_SetPropertyStr(ctx, copy, "setStart", JS_NewCFunction(ctx, cos_dom_range_set_start, "setStart", 2));
    JS_SetPropertyStr(ctx, copy, "setEnd", JS_NewCFunction(ctx, cos_dom_range_set_end, "setEnd", 2));
    JS_SetPropertyStr(ctx, copy, "setStartBefore", JS_NewCFunction(ctx, cos_dom_range_set_start_before, "setStartBefore", 1));
    JS_SetPropertyStr(ctx, copy, "setStartAfter", JS_NewCFunction(ctx, cos_dom_range_set_start_after, "setStartAfter", 1));
    JS_SetPropertyStr(ctx, copy, "setEndBefore", JS_NewCFunction(ctx, cos_dom_range_set_end_before, "setEndBefore", 1));
    JS_SetPropertyStr(ctx, copy, "setEndAfter", JS_NewCFunction(ctx, cos_dom_range_set_end_after, "setEndAfter", 1));
    JS_SetPropertyStr(ctx, copy, "collapse", JS_NewCFunction(ctx, cos_dom_range_collapse, "collapse", 1));
    JS_SetPropertyStr(ctx, copy, "selectNodeContents", JS_NewCFunction(ctx, cos_dom_range_select_node_contents, "selectNodeContents", 1));
    JS_SetPropertyStr(ctx, copy, "cloneRange", JS_NewCFunction(ctx, cos_dom_range_clone, "cloneRange", 0));
    JS_SetPropertyStr(ctx, copy, "toString", JS_NewCFunction(ctx, cos_dom_range_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, copy, "deleteContents", JS_NewCFunction(ctx, cos_dom_range_delete_contents, "deleteContents", 0));
    JS_SetPropertyStr(ctx, copy, "insertNode", JS_NewCFunction(ctx, cos_dom_range_insert_node, "insertNode", 1));
    JS_SetPropertyStr(ctx, copy, "cloneContents", JS_NewCFunction(ctx, cos_dom_range_clone_contents, "cloneContents", 0));
    JS_SetPropertyStr(ctx, copy, "extractContents", JS_NewCFunction(ctx, cos_dom_range_extract_contents, "extractContents", 0));
    JS_SetPropertyStr(ctx, copy, "detach", JS_NewCFunction(ctx, cos_dom_range_detach, "detach", 0));
    JS_SetPropertyStr(ctx, copy, "compareBoundaryPoints", JS_NewCFunction(ctx, cos_dom_range_compare_boundary_points, "compareBoundaryPoints", 2));
    return copy;
}
static JSValue cos_dom_range_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue range = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, range, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, range, "setStart", JS_NewCFunction(ctx, cos_dom_range_set_start, "setStart", 2));
    JS_SetPropertyStr(ctx, range, "setEnd", JS_NewCFunction(ctx, cos_dom_range_set_end, "setEnd", 2));
    JS_SetPropertyStr(ctx, range, "setStartBefore", JS_NewCFunction(ctx, cos_dom_range_set_start_before, "setStartBefore", 1));
    JS_SetPropertyStr(ctx, range, "setStartAfter", JS_NewCFunction(ctx, cos_dom_range_set_start_after, "setStartAfter", 1));
    JS_SetPropertyStr(ctx, range, "setEndBefore", JS_NewCFunction(ctx, cos_dom_range_set_end_before, "setEndBefore", 1));
    JS_SetPropertyStr(ctx, range, "setEndAfter", JS_NewCFunction(ctx, cos_dom_range_set_end_after, "setEndAfter", 1));
    JS_SetPropertyStr(ctx, range, "collapse", JS_NewCFunction(ctx, cos_dom_range_collapse, "collapse", 1));
    JS_SetPropertyStr(ctx, range, "selectNodeContents", JS_NewCFunction(ctx, cos_dom_range_select_node_contents, "selectNodeContents", 1));
    JS_SetPropertyStr(ctx, range, "cloneRange", JS_NewCFunction(ctx, cos_dom_range_clone, "cloneRange", 0));
        JS_SetPropertyStr(ctx, range, "toString", JS_NewCFunction(ctx, cos_dom_range_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, range, "deleteContents", JS_NewCFunction(ctx, cos_dom_range_delete_contents, "deleteContents", 0));
    JS_SetPropertyStr(ctx, range, "insertNode", JS_NewCFunction(ctx, cos_dom_range_insert_node, "insertNode", 1));
    JS_SetPropertyStr(ctx, range, "cloneContents", JS_NewCFunction(ctx, cos_dom_range_clone_contents, "cloneContents", 0));
    JS_SetPropertyStr(ctx, range, "extractContents", JS_NewCFunction(ctx, cos_dom_range_extract_contents, "extractContents", 0));
    JS_SetPropertyStr(ctx, range, "detach", JS_NewCFunction(ctx, cos_dom_range_detach, "detach", 0));
    JS_SetPropertyStr(ctx, range, "compareBoundaryPoints", JS_NewCFunction(ctx, cos_dom_range_compare_boundary_points, "compareBoundaryPoints", 2));
    JS_SetPropertyStr(ctx, range, "_cos_detached", JS_FALSE);
    return range;
}
static JSValue cos_dom_selection_remove_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue cos_dom_selection_remove_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Selection.removeRange requires a Range");
    JSValue current = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    bool same = JS_IsStrictEqual(ctx, current, argv[0]);
    JS_FreeValue(ctx, current);
    if (same) return cos_dom_selection_remove_all(ctx, this_val, 0, NULL);
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_contains_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Selection.containsNode requires a Node");
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_FALSE; }
    JSValue start = JS_GetPropertyStr(ctx, range, "startContainer");
    JSValue end = JS_GetPropertyStr(ctx, range, "endContainer");
    bool contained = JS_IsStrictEqual(ctx, start, argv[0]) && JS_IsStrictEqual(ctx, end, argv[0]);
    JS_FreeValue(ctx, start); JS_FreeValue(ctx, end); JS_FreeValue(ctx, range);
    return JS_NewBool(ctx, contained);
}
static JSValue cos_dom_selection_remove_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_range", JS_NULL);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "rangeCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", JS_NULL);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", JS_NULL);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "none"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_add_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Selection.addRange requires a Range");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_range", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "rangeCount", JS_NewInt32(ctx, 1));
    JSValue start_node = JS_GetPropertyStr(ctx, argv[0], "startContainer");
    JSValue end_node = JS_GetPropertyStr(ctx, argv[0], "endContainer");
    JSValue start_offset = JS_GetPropertyStr(ctx, argv[0], "startOffset");
    JSValue end_offset = JS_GetPropertyStr(ctx, argv[0], "endOffset");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", start_node);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", end_node);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", start_offset);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", end_offset);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "forward"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_GetPropertyStr(ctx, argv[0], "collapsed"));
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_get_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue count = JS_GetPropertyStr(ctx, this_val, "rangeCount");
    int32_t n = 0; JS_ToInt32(ctx, &n, count); JS_FreeValue(ctx, count);
    int32_t index = argc > 0 ? 0 : -1; if (argc > 0) JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0 || index >= n) return JS_ThrowRangeError(ctx, "Selection range index out of bounds");
    return JS_GetPropertyStr(ctx, this_val, "_cos_range");
}
static JSValue cos_dom_selection_extend(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Selection.extend requires a Node");
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_ThrowDOMException(ctx, "InvalidStateError", "Selection has no range"); }
    JSValue method = JS_GetPropertyStr(ctx, range, "setEnd");
    JSValue offset = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_NewInt32(ctx, 0);
    JSValue args[2] = { argv[0], offset };
    JSValue result = JS_Call(ctx, method, range, 2, args);
    JS_FreeValue(ctx, method); JS_FreeValue(ctx, offset);
    if (JS_IsException(result)) { JS_FreeValue(ctx, range); return result; }
    JS_FreeValue(ctx, result);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_GetPropertyStr(ctx, range, "collapsed"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "forward"));
    JS_FreeValue(ctx, range);
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_set_base_and_extent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 3 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[2])) return JS_ThrowTypeError(ctx, "Selection.setBaseAndExtent requires Nodes and offsets");
    JSValue range = cos_dom_range_constructor(ctx, JS_UNDEFINED, 0, NULL);
    JSValue start = JS_GetPropertyStr(ctx, range, "setStart");
    JSValue end = JS_GetPropertyStr(ctx, range, "setEnd");
    JSValue start_args[2] = { argv[0], argv[1] };
    JSValue end_args[2] = { argv[2], argc > 3 ? argv[3] : JS_NewInt32(ctx, 0) };
    JSValue result = JS_Call(ctx, start, range, 2, start_args); JS_FreeValue(ctx, start);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); JS_FreeValue(ctx, end); JS_FreeValue(ctx, range); return JS_EXCEPTION; }
    JS_FreeValue(ctx, result); result = JS_Call(ctx, end, range, 2, end_args); JS_FreeValue(ctx, end);
    if (argc < 4) JS_FreeValue(ctx, end_args[1]);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); JS_FreeValue(ctx, range); return JS_EXCEPTION; }
    JS_FreeValue(ctx, result);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_range", range);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "rangeCount", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", JS_DupValue(ctx, argv[2]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", argc > 3 ? JS_DupValue(ctx, argv[3]) : JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "forward"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_GetPropertyStr(ctx, range, "collapsed"));
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_collapse_to_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_ThrowDOMException(ctx, "InvalidStateError", "Selection has no range"); }
    JSValue method = JS_GetPropertyStr(ctx, range, "collapse");
    JSValue start = JS_NewBool(ctx, true);
    JSValue result = JS_Call(ctx, method, range, 1, &start);
    JS_FreeValue(ctx, method); JS_FreeValue(ctx, start);
    if (JS_IsException(result)) { JS_FreeValue(ctx, range); return result; }
    JS_FreeValue(ctx, result);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", JS_GetPropertyStr(ctx, range, "startContainer"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", JS_GetPropertyStr(ctx, range, "startOffset"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", JS_GetPropertyStr(ctx, range, "startContainer"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", JS_GetPropertyStr(ctx, range, "startOffset"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "none"));
    JS_FreeValue(ctx, range); return JS_UNDEFINED;
}
static JSValue cos_dom_selection_collapse_to_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_ThrowDOMException(ctx, "InvalidStateError", "Selection has no range"); }
    JSValue method = JS_GetPropertyStr(ctx, range, "collapse");
    JSValue end = JS_NewBool(ctx, false);
    JSValue result = JS_Call(ctx, method, range, 1, &end);
    JS_FreeValue(ctx, method); JS_FreeValue(ctx, end);
    if (JS_IsException(result)) { JS_FreeValue(ctx, range); return result; }
    JS_FreeValue(ctx, result);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", JS_GetPropertyStr(ctx, range, "startContainer"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", JS_GetPropertyStr(ctx, range, "startOffset"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", JS_GetPropertyStr(ctx, range, "startContainer"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", JS_GetPropertyStr(ctx, range, "startOffset"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "none"));
    JS_FreeValue(ctx, range); return JS_UNDEFINED;
}
static JSValue cos_dom_selection_delete_from_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_UNDEFINED; }
    JSValue method = JS_GetPropertyStr(ctx, range, "deleteContents");
    JSValue result = JS_Call(ctx, method, range, 0, NULL);
    JS_FreeValue(ctx, method); JS_FreeValue(ctx, range);
    if (JS_IsException(result)) return result;
    JS_FreeValue(ctx, result);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue range = JS_GetPropertyStr(ctx, this_val, "_cos_range");
    if (!JS_IsObject(range)) { JS_FreeValue(ctx, range); return JS_NewString(ctx, ""); }
    JSValue method = JS_GetPropertyStr(ctx, range, "toString");
    JSValue result = JS_Call(ctx, method, range, 0, NULL);
    JS_FreeValue(ctx, method); JS_FreeValue(ctx, range); return result;
}
static JSValue cos_dom_selection_collapse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Selection.collapse requires a Node");
    JSValue range = cos_dom_range_constructor(ctx, JS_UNDEFINED, 0, NULL);
    JSValue default_offset = JS_UNDEFINED;
    if (argc < 2) default_offset = JS_NewInt32(ctx, 0);
    JSValue args[2] = { argv[0], argc > 1 ? argv[1] : default_offset };
    JSValue set = JS_GetPropertyStr(ctx, range, "setStart"); JSValue r = JS_Call(ctx, set, range, 2, args);
    JS_FreeValue(ctx, set); JS_FreeValue(ctx, r); JS_FreeValue(ctx, default_offset);
    JSValue collapse = JS_GetPropertyStr(ctx, range, "collapse"); r = JS_Call(ctx, collapse, range, 0, NULL);
    JS_FreeValue(ctx, collapse); JS_FreeValue(ctx, r);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_range", range);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "rangeCount", JS_NewInt32(ctx, 1));
    JSValue node = JS_GetPropertyStr(ctx, range, "startContainer");
    JSValue offset = JS_GetPropertyStr(ctx, range, "startOffset");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorNode", JS_DupValue(ctx, node));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusNode", node);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "anchorOffset", JS_DupValue(ctx, offset));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "focusOffset", offset);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "direction", JS_NewString(ctx, "none"));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "isCollapsed", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue cos_dom_selection_new(JSContext *ctx)
{
    JSValue selection = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, selection, "_cos_range", JS_NULL);
    JS_SetPropertyStr(ctx, selection, "rangeCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, selection, "addRange", JS_NewCFunction(ctx, cos_dom_selection_add_range, "addRange", 1));
    JS_SetPropertyStr(ctx, selection, "getRangeAt", JS_NewCFunction(ctx, cos_dom_selection_get_range, "getRangeAt", 1));
    JS_SetPropertyStr(ctx, selection, "removeRange", JS_NewCFunction(ctx, cos_dom_selection_remove_range, "removeRange", 1));
    JS_SetPropertyStr(ctx, selection, "containsNode", JS_NewCFunction(ctx, cos_dom_selection_contains_node, "containsNode", 2));
    JS_SetPropertyStr(ctx, selection, "removeAllRanges", JS_NewCFunction(ctx, cos_dom_selection_remove_all, "removeAllRanges", 0));
    JS_SetPropertyStr(ctx, selection, "collapse", JS_NewCFunction(ctx, cos_dom_selection_collapse, "collapse", 2));
    JS_SetPropertyStr(ctx, selection, "extend", JS_NewCFunction(ctx, cos_dom_selection_extend, "extend", 2));
    JS_SetPropertyStr(ctx, selection, "setBaseAndExtent", JS_NewCFunction(ctx, cos_dom_selection_set_base_and_extent, "setBaseAndExtent", 4));
    JS_SetPropertyStr(ctx, selection, "collapseToStart", JS_NewCFunction(ctx, cos_dom_selection_collapse_to_start, "collapseToStart", 0));
    JS_SetPropertyStr(ctx, selection, "collapseToEnd", JS_NewCFunction(ctx, cos_dom_selection_collapse_to_end, "collapseToEnd", 0));
    JS_SetPropertyStr(ctx, selection, "deleteFromDocument", JS_NewCFunction(ctx, cos_dom_selection_delete_from_document, "deleteFromDocument", 0));
    JS_SetPropertyStr(ctx, selection, "toString", JS_NewCFunction(ctx, cos_dom_selection_to_string, "toString", 0));
    return selection;
}
static JSValue cos_dom_get_selection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue selection = JS_GetPropertyStr(ctx, global, "__cos_selection");
    if (!JS_IsObject(selection)) {
        JS_FreeValue(ctx, selection); selection = cos_dom_selection_new(ctx);
        JS_SetPropertyStr(ctx, global, "__cos_selection", JS_DupValue(ctx, selection));
    }
    JS_FreeValue(ctx, global); return selection;
}

static JSValue cos_dom_mutation_observer_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "MutationObserver callback must be a function");
    JSValue observer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, observer, "_cos_callback", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, observer, "_cos_records", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, observer, "_cos_observing", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, observer, "_cos_target", JS_NULL);
    JS_SetPropertyStr(ctx, observer, "_cos_childList", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_attributes", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_characterData", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_subtree", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_attributeOldValue", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_characterDataOldValue", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "_cos_attributeFilter", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, observer, "_cos_job_pending", JS_FALSE);
    JS_SetPropertyStr(ctx, observer, "observe", JS_NewCFunction(ctx, cos_dom_mutation_observer_observe, "observe", 2));
    JS_SetPropertyStr(ctx, observer, "disconnect", JS_NewCFunction(ctx, cos_dom_mutation_observer_disconnect, "disconnect", 0));
    JS_SetPropertyStr(ctx, observer, "takeRecords", JS_NewCFunction(ctx, cos_dom_mutation_observer_take_records, "takeRecords", 0));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__cos_mutation_observers");
    if (!JS_IsArray(registry)) {
        JS_FreeValue(ctx, registry);
        registry = JS_NewArray(ctx);
    }
    JSValue rlen = JS_GetPropertyStr(ctx, registry, "length");
    uint32_t ri = 0;
    (void)JS_ToUint32(ctx, &ri, rlen);
    JS_FreeValue(ctx, rlen);
    JS_SetPropertyUint32(ctx, registry, ri, JS_DupValue(ctx, observer));
    JS_SetPropertyStr(ctx, global, "__cos_mutation_observers", registry);
    /* Keep the legacy slot until notify is converted to iterate the registry. */
    JS_SetPropertyStr(ctx, global, "__cos_mutation_observer", JS_DupValue(ctx, observer));
    JS_FreeValue(ctx, global);
    return observer;
}

static void cos_js_report_pending_exception(JSContext *ctx);

static JSValue cos_dom_mutation_observer_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    JSValue observer = argv[0];
    JSValue records = argv[1];
    JSValue callback = JS_GetPropertyStr(ctx, observer, "_cos_callback");
    if (JS_IsFunction(ctx, callback)) {
        JSValue args[2] = { records, observer };
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
        if (JS_IsException(result)) cos_js_report_pending_exception(ctx);
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, callback);
    JS_SetPropertyStr(ctx, (JSValue)observer, "_cos_job_pending", JS_FALSE);
    JS_SetPropertyStr(ctx, (JSValue)observer, "_cos_records", JS_NewArray(ctx));
    return JS_UNDEFINED;
}

static void cos_dom_mutation_observer_notify_one(void)
{
    JSContext *ctx = cos_js_active_page_context;
    if (ctx == NULL) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue observer = JS_GetPropertyStr(ctx, global, "__cos_mutation_observer");
    JSValue observing = JS_GetPropertyStr(ctx, observer, "_cos_observing");
    bool active = JS_ToBool(ctx, observing); JS_FreeValue(ctx, observing);
    const char *option_name = cos_dom_last_mutation_kind == 1 ? "_cos_attributes" :
                               (cos_dom_last_mutation_kind == 2 ? "_cos_characterData" : "_cos_childList");
    JSValue option = JS_GetPropertyStr(ctx, observer, option_name);
    bool wants_mutation = JS_ToBool(ctx, option); JS_FreeValue(ctx, option);
    if (cos_dom_last_mutation_kind == 1 && wants_mutation) {
        JSValue filter = JS_GetPropertyStr(ctx, observer, "_cos_attributeFilter");
        JSValue length = JS_GetPropertyStr(ctx, filter, "length"); uint32_t count = 0; JS_ToUint32(ctx, &count, length);
        JS_FreeValue(ctx, length);
        if (count > 0) {
            wants_mutation = false;
            for (uint32_t i = 0; i < count; ++i) {
                JSValue name = JS_GetPropertyUint32(ctx, filter, i);
                const char *name_c = JS_ToCString(ctx, name);
                if (name_c != NULL && cos_dom_ascii_equal_fold(name_c, cos_dom_last_attribute_name)) wants_mutation = true;
                if (name_c != NULL) JS_FreeCString(ctx, name_c); JS_FreeValue(ctx, name);
                if (wants_mutation) break;
            }
        }
        JS_FreeValue(ctx, filter);
    }
    if (active && wants_mutation && JS_IsObject(observer)) {
        JSValue record = JS_NewObject(ctx);
        const char *record_type = cos_dom_last_mutation_kind == 1 ? "attributes" :
                                  (cos_dom_last_mutation_kind == 2 ? "characterData" : "childList");
        JS_SetPropertyStr(ctx, record, "type", JS_NewString(ctx, record_type));
        if (cos_dom_last_mutation_kind == 1) {
            JS_SetPropertyStr(ctx, record, "attributeName", JS_NewString(ctx, cos_dom_last_attribute_name));
            JSValue old_option = JS_GetPropertyStr(ctx, observer, "_cos_attributeOldValue");
            if (JS_ToBool(ctx, old_option) && cos_dom_last_has_old_value)
                JS_SetPropertyStr(ctx, record, "oldValue", JS_NewString(ctx, cos_dom_last_old_value));
            else
                JS_SetPropertyStr(ctx, record, "oldValue", JS_NULL);
            JS_FreeValue(ctx, old_option);
        } else if (cos_dom_last_mutation_kind == 2) {
            JSValue old_option = JS_GetPropertyStr(ctx, observer, "_cos_characterDataOldValue");
            if (JS_ToBool(ctx, old_option) && cos_dom_last_has_old_value)
                JS_SetPropertyStr(ctx, record, "oldValue", JS_NewString(ctx, cos_dom_last_old_value));
            else
                JS_SetPropertyStr(ctx, record, "oldValue", JS_NULL);
            JS_FreeValue(ctx, old_option);
        }
        JS_SetPropertyStr(ctx, record, "addedNodes", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, record, "removedNodes", JS_NewArray(ctx));
        JSValue target = JS_GetPropertyStr(ctx, observer, "_cos_target");
        JS_SetPropertyStr(ctx, record, "target", target);
        JSValue records = JS_GetPropertyStr(ctx, observer, "_cos_records");
        if (!JS_IsArray(records)) { JS_FreeValue(ctx, records); records = JS_NewArray(ctx); }
        JSValue length = JS_GetPropertyStr(ctx, records, "length"); uint32_t index = 0;
        (void)JS_ToUint32(ctx, &index, length); JS_FreeValue(ctx, length);
        JS_SetPropertyUint32(ctx, records, index, JS_DupValue(ctx, record));
        JSValue pending = JS_GetPropertyStr(ctx, observer, "_cos_job_pending");
        bool queued = JS_ToBool(ctx, pending); JS_FreeValue(ctx, pending);
        if (!queued) {
            JS_SetPropertyStr(ctx, observer, "_cos_job_pending", JS_TRUE);
            JSValue args[2] = { JS_DupValue(ctx, observer), JS_DupValue(ctx, records) };
            if (JS_EnqueueJob(ctx, cos_dom_mutation_observer_job, 2, args) < 0) {
                JS_FreeValue(ctx, args[0]);
                JS_FreeValue(ctx, args[1]);
                JS_SetPropertyStr(ctx, observer, "_cos_job_pending", JS_FALSE);
            }
        }
        /* SetPropertyStr takes ownership of records; do not free it again here. */
        JS_SetPropertyStr(ctx, observer, "_cos_records", records);
        JS_FreeValue(ctx, record);
    }
    JS_FreeValue(ctx, observer); JS_FreeValue(ctx, global);
}

static void cos_dom_mutation_observer_notify(void)
{
    JSContext *ctx = cos_js_active_page_context;
    if (ctx == NULL) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__cos_mutation_observers");
    if (JS_IsArray(registry)) {
        JSValue length = JS_GetPropertyStr(ctx, registry, "length");
        uint32_t count = 0;
        (void)JS_ToUint32(ctx, &count, length);
        JS_FreeValue(ctx, length);
        for (uint32_t i = 0; i < count; ++i) {
            JSValue observer = JS_GetPropertyUint32(ctx, registry, i);
            if (JS_IsObject(observer)) {
                JS_SetPropertyStr(ctx, global, "__cos_mutation_observer", JS_DupValue(ctx, observer));
                cos_dom_mutation_observer_notify_one();
            }
            JS_FreeValue(ctx, observer);
        }
    } else {
        cos_dom_mutation_observer_notify_one();
    }
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);
}

struct cos_dom_serial_buffer {
    JSContext *ctx;
    char *data;
    size_t length;
    size_t capacity;
};
static bool cos_dom_serial_append(struct cos_dom_serial_buffer *buf, const char *text, size_t length)
{
    if (length == 0) return true;
    size_t needed = buf->length + length + 1;
    if (needed > buf->capacity) {
        size_t capacity = buf->capacity ? buf->capacity : 256;
        while (capacity < needed) capacity *= 2;
        char *grown = js_realloc(buf->ctx, buf->data, capacity);
        if (grown == NULL) return false;
        buf->data = grown; buf->capacity = capacity;
    }
    memcpy(buf->data + buf->length, text, length); buf->length += length; buf->data[buf->length] = '\0';
    return true;
}
static bool cos_dom_serial_append_escaped(struct cos_dom_serial_buffer *buf, const char *text, bool attribute)
{
    if (text == NULL) return true;
    for (const char *p = text; *p != '\0'; ++p) {
        const char *replacement = NULL;
        if (*p == '&') replacement = "&amp;";
        else if (*p == '<') replacement = "&lt;";
        else if (*p == '>') replacement = "&gt;";
        else if (attribute && *p == '"') replacement = "&quot;";
        if (replacement != NULL) { if (!cos_dom_serial_append(buf, replacement, strlen(replacement))) return false; }
        else if (!cos_dom_serial_append(buf, p, 1)) return false;
    }
    return true;
}
static bool cos_dom_serialize_node(JSContext *ctx, JSValueConst value, struct cos_dom_serial_buffer *buf, bool include_self)
{
    JSValue type_value = JS_GetPropertyStr(ctx, value, "nodeType"); int32_t type = 0; JS_ToInt32(ctx, &type, type_value); JS_FreeValue(ctx, type_value);
    if (type == 8) {
        JSValue text = JS_GetPropertyStr(ctx, value, "textContent"); const char *text_c = JS_ToCString(ctx, text);
        bool ok = cos_dom_serial_append(buf, "<!--", 4) && text_c != NULL &&
                  cos_dom_serial_append(buf, text_c, strlen(text_c)) &&
                  cos_dom_serial_append(buf, "-->", 3);
        if (text_c != NULL) JS_FreeCString(ctx, text_c); JS_FreeValue(ctx, text); return ok;
    }
    if (type == 3 || type == 4) {
        JSValue text = JS_GetPropertyStr(ctx, value, "textContent"); const char *text_c = JS_ToCString(ctx, text);
        bool ok = cos_dom_serial_append_escaped(buf, text_c, false);
        if (text_c != NULL) JS_FreeCString(ctx, text_c); JS_FreeValue(ctx, text); return ok;
    }
    if (type != 1 && type != 9 && type != 11) return true;
    JSValue tag = JS_GetPropertyStr(ctx, value, "tagName"); const char *tag_c = JS_ToCString(ctx, tag);
    bool element = type == 1 && tag_c != NULL; bool ok = true;
    if (element && include_self) {
        ok = cos_dom_serial_append(buf, "<", 1) && cos_dom_serial_append(buf, tag_c, strlen(tag_c));
        JSValue names = cos_dom_element_get_attribute_names(ctx, value, 0, NULL); JSValue len = JS_GetPropertyStr(ctx, names, "length"); uint32_t count = 0; JS_ToUint32(ctx, &count, len);
        JS_FreeValue(ctx, len);
        for (uint32_t i = 0; ok && i < count; ++i) {
            JSValue name = JS_GetPropertyUint32(ctx, names, i); JSValue val = cos_dom_element_get_attribute(ctx, value, 1, &name);
            const char *name_c = JS_ToCString(ctx, name); const char *val_c = JS_ToCString(ctx, val);
            ok = name_c != NULL && cos_dom_serial_append(buf, " ", 1) && cos_dom_serial_append(buf, name_c, strlen(name_c)) && cos_dom_serial_append(buf, "=\"", 2) && cos_dom_serial_append_escaped(buf, val_c, true) && cos_dom_serial_append(buf, "\"", 1);
            if (name_c != NULL) JS_FreeCString(ctx, name_c); if (val_c != NULL) JS_FreeCString(ctx, val_c); JS_FreeValue(ctx, name); JS_FreeValue(ctx, val);
        }
        JS_FreeValue(ctx, names); ok = ok && cos_dom_serial_append(buf, ">", 1);
    }
    JSValue children = JS_GetPropertyStr(ctx, value, "childNodes"); JSValue len = JS_GetPropertyStr(ctx, children, "length"); uint32_t count = 0; JS_ToUint32(ctx, &count, len); JS_FreeValue(ctx, len);
    for (uint32_t i = 0; ok && i < count; ++i) { JSValue child = JS_GetPropertyUint32(ctx, children, i); ok = cos_dom_serialize_node(ctx, child, buf, true); JS_FreeValue(ctx, child); }
    JS_FreeValue(ctx, children);
    if (element && include_self) { const char *void_tag = (!strcmp(tag_c, "AREA") || !strcmp(tag_c, "BASE") || !strcmp(tag_c, "BR") || !strcmp(tag_c, "COL") || !strcmp(tag_c, "EMBED") || !strcmp(tag_c, "HR") || !strcmp(tag_c, "IMG") || !strcmp(tag_c, "INPUT") || !strcmp(tag_c, "LINK") || !strcmp(tag_c, "META") || !strcmp(tag_c, "PARAM") || !strcmp(tag_c, "SOURCE") || !strcmp(tag_c, "TRACK") || !strcmp(tag_c, "WBR")) ? tag_c : NULL; if (void_tag == NULL) ok = ok && cos_dom_serial_append(buf, "</", 2) && cos_dom_serial_append(buf, tag_c, strlen(tag_c)) && cos_dom_serial_append(buf, ">", 1); }
    if (tag_c != NULL) JS_FreeCString(ctx, tag_c); JS_FreeValue(ctx, tag); return ok;
}
static JSValue cos_dom_element_get_outer_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv; struct cos_dom_serial_buffer buf = { ctx, NULL, 0, 0 };
    if (!cos_dom_serialize_node(ctx, this_val, &buf, true)) { if (buf.data != NULL) js_free(ctx, buf.data); return JS_EXCEPTION; }
    JSValue result = JS_NewStringLen(ctx, buf.data != NULL ? buf.data : "", buf.length); if (buf.data != NULL) js_free(ctx, buf.data); return result;
}

static JSValue cos_dom_xml_serializer_serialize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "serializeToString requires a Node");
    struct cos_dom_serial_buffer buf = { ctx, NULL, 0, 0 };
    if (!cos_dom_serialize_node(ctx, argv[0], &buf, true)) { if (buf.data != NULL) js_free(ctx, buf.data); return JS_EXCEPTION; }
    JSValue result = JS_NewStringLen(ctx, buf.data != NULL ? buf.data : "", buf.length); if (buf.data != NULL) js_free(ctx, buf.data); return result;
}
static JSValue cos_dom_xml_serializer_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue serializer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, serializer, "serializeToString", JS_NewCFunction(ctx, cos_dom_xml_serializer_serialize, "serializeToString", 1));
    return serializer;
}
static bool cos_dom_parser_supported_mime(const char *mime)
{
    return mime != NULL && (strcmp(mime, "text/html") == 0 ||
           strcmp(mime, "text/xml") == 0 || strcmp(mime, "application/xml") == 0 ||
           strcmp(mime, "application/xhtml+xml") == 0 || strcmp(mime, "image/svg+xml") == 0);
}
static JSValue cos_dom_parser_parse_from_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "parseFromString requires input and type");
    JSValue input_string = JS_ToString(ctx, argv[0]);
    JSValue mime_string = JS_ToString(ctx, argv[1]);
    if (JS_IsException(input_string) || JS_IsException(mime_string)) {
        JS_FreeValue(ctx, input_string); JS_FreeValue(ctx, mime_string); return JS_EXCEPTION;
    }
    const char *markup = JS_ToCString(ctx, input_string); const char *mime = JS_ToCString(ctx, mime_string);
    if (markup == NULL || mime == NULL) {
        if (markup != NULL) JS_FreeCString(ctx, markup); if (mime != NULL) JS_FreeCString(ctx, mime);
        JS_FreeValue(ctx, input_string); JS_FreeValue(ctx, mime_string); return JS_EXCEPTION;
    }
    if (!cos_dom_parser_supported_mime(mime)) {
        JS_FreeCString(ctx, markup); JS_FreeCString(ctx, mime);
        JS_FreeValue(ctx, input_string); JS_FreeValue(ctx, mime_string);
        return JS_ThrowTypeError(ctx, "Unsupported DOMParser MIME type");
    }
    JSValue tag = JS_NewString(ctx, "body"); JSValue body = cos_web_create_element(ctx, JS_UNDEFINED, 1, &tag); JS_FreeValue(ctx, tag);
    JSValue html = JS_NewString(ctx, markup);
    JSValue parse_result = cos_dom_element_set_inner_html(ctx, body, 1, &html);
    JS_FreeValue(ctx, parse_result); JS_FreeValue(ctx, html);
    JSValue result = JS_NewObject(ctx); JS_SetPropertyStr(ctx, result, "body", JS_DupValue(ctx, body)); JS_SetPropertyStr(ctx, result, "documentElement", JS_DupValue(ctx, body)); JS_SetPropertyStr(ctx, result, "contentType", JS_NewString(ctx, mime)); JS_SetPropertyStr(ctx, result, "toString", JS_NewCFunction(ctx, cos_dom_xml_serializer_serialize, "toString", 0));
    JS_FreeValue(ctx, body); JS_FreeCString(ctx, markup); JS_FreeCString(ctx, mime); JS_FreeValue(ctx, input_string); JS_FreeValue(ctx, mime_string); return result;
}
static JSValue cos_dom_parser_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue parser = JS_NewObject(ctx); JS_SetPropertyStr(ctx, parser, "parseFromString", JS_NewCFunction(ctx, cos_dom_parser_parse_from_string, "parseFromString", 2)); return parser;
}
static JSValue cos_dom_exception_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv; JSValue name = JS_GetPropertyStr(ctx, this_val, "name"); JSValue message = JS_GetPropertyStr(ctx, this_val, "message");
    const char *name_c = JS_ToCString(ctx, name); const char *message_c = JS_ToCString(ctx, message); char buffer[512];
    int n = snprintf(buffer, sizeof(buffer), "%s: %s", name_c != NULL ? name_c : "Error", message_c != NULL ? message_c : "");
    if (name_c != NULL) JS_FreeCString(ctx, name_c); if (message_c != NULL) JS_FreeCString(ctx, message_c); JS_FreeValue(ctx, name); JS_FreeValue(ctx, message);
    if (n < 0) return JS_EXCEPTION; if ((size_t)n >= sizeof(buffer)) n = (int)sizeof(buffer) - 1; return JS_NewStringLen(ctx, buffer, (size_t)n);
}
static int cos_dom_exception_legacy_code(const char *name)
{
    if (name == NULL) return 0;
    if (strcmp(name, "IndexSizeError") == 0) return 1;
    if (strcmp(name, "DOMStringSizeError") == 0) return 2;
    if (strcmp(name, "HierarchyRequestError") == 0) return 3;
    if (strcmp(name, "WrongDocumentError") == 0) return 4;
    if (strcmp(name, "InvalidCharacterError") == 0) return 5;
    if (strcmp(name, "NoModificationAllowedError") == 0) return 7;
    if (strcmp(name, "NotFoundError") == 0) return 8;
    if (strcmp(name, "NotSupportedError") == 0) return 9;
    if (strcmp(name, "InUseAttributeError") == 0) return 10;
    if (strcmp(name, "InvalidStateError") == 0) return 11;
    if (strcmp(name, "SyntaxError") == 0) return 12;
    if (strcmp(name, "InvalidModificationError") == 0) return 13;
    if (strcmp(name, "NamespaceError") == 0) return 14;
    if (strcmp(name, "InvalidAccessError") == 0) return 15;
    if (strcmp(name, "TypeMismatchError") == 0) return 17;
    if (strcmp(name, "SecurityError") == 0) return 18;
    if (strcmp(name, "NetworkError") == 0) return 19;
    if (strcmp(name, "AbortError") == 0) return 20;
    if (strcmp(name, "URLMismatchError") == 0) return 21;
    if (strcmp(name, "QuotaExceededError") == 0) return 22;
    if (strcmp(name, "TimeoutError") == 0) return 23;
    if (strcmp(name, "InvalidNodeTypeError") == 0) return 24;
    if (strcmp(name, "DataCloneError") == 0) return 25;
    return 0;
}
static JSValue cos_dom_exception_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; JSValue message = argc > 0 ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, ""); JSValue name = argc > 1 ? JS_ToString(ctx, argv[1]) : JS_NewString(ctx, "Error");
    if (JS_IsException(message) || JS_IsException(name)) { JS_FreeValue(ctx, message); JS_FreeValue(ctx, name); return JS_EXCEPTION; }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "message", message);
    JS_SetPropertyStr(ctx, result, "name", name);
    const char *name_c = JS_ToCString(ctx, name);
    JS_SetPropertyStr(ctx, result, "code", JS_NewInt32(ctx, cos_dom_exception_legacy_code(name_c)));
    if (name_c != NULL) JS_FreeCString(ctx, name_c);
    JS_SetPropertyStr(ctx, result, "toString", JS_NewCFunction(ctx, cos_dom_exception_to_string, "toString", 0));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    if (JS_IsObject(proto)) JS_SetPrototype(ctx, result, proto);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor); JS_FreeValue(ctx, global);
    return result;
}
static JSValue cos_dom_owner_document(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_GetPropertyStr(ctx, global, "document");
    JS_FreeValue(ctx, global);
    return document;
}

static JSValue cos_dom_node_is_equal_node(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    dom_node *other = argc > 0 ? cos_dom_unwrap_any_node(ctx, argv[0]) : NULL;
    bool equal = false;
    if (node != NULL && other != NULL) (void)dom_node_is_equal(node, other, &equal);
    return JS_NewBool(ctx, equal);
}

static JSValue cos_dom_node_compare_position(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    dom_node *other = argc > 0 ? cos_dom_unwrap_any_node(ctx, argv[0]) : NULL;
    uint16_t position = 0;
    if (node == NULL || other == NULL || dom_node_compare_document_position(node, other, &position) != DOM_NO_ERR)
        return JS_NewUint32(ctx, 0);
    return JS_NewUint32(ctx, position);
}

static JSValue cos_dom_node_normalize(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node != NULL && dom_node_normalize(node) != DOM_NO_ERR)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue cos_dom_node_is_same_node(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, argc > 0 && JS_IsStrictEqual(ctx, this_val, argv[0]));
}

static JSValue cos_dom_node_contains(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_FALSE;
    dom_node *root = cos_dom_unwrap_any_node(ctx, this_val);
    dom_node *needle = cos_dom_unwrap_any_node(ctx, argv[0]);
    if (root == NULL || needle == NULL) return JS_FALSE;
    if (root == needle) return JS_TRUE;
    dom_node *cur = needle;
    bool found = false;
    for (unsigned depth = 0; cur != NULL && depth < 4096u; ++depth) {
        dom_node *parent = NULL;
        if (dom_node_get_parent_node(cur, &parent) != DOM_NO_ERR || parent == NULL) break;
        if (parent == root) { found = true; dom_node_unref(parent); break; }
        if (cur != needle) dom_node_unref(cur);
        cur = parent;
    }
    return JS_NewBool(ctx, found);
}

static JSValue cos_dom_element_closest(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "closest requires a selector");
    JSValue current = JS_DupValue(ctx, this_val);
    for (unsigned depth = 0; depth < 4096u && JS_IsObject(current); ++depth) {
        JSValue matched = cos_dom_element_matches(ctx, current, 1, argv);
        if (JS_IsException(matched)) { JS_FreeValue(ctx, current); return matched; }
        if (JS_ToBool(ctx, matched)) { JS_FreeValue(ctx, matched); return current; }
        JS_FreeValue(ctx, matched);
        JSValue parent = cos_dom_node_get_parent_node(ctx, current, 0, NULL);
        JS_FreeValue(ctx, current); current = parent;
    }
    JS_FreeValue(ctx, current);
    return JS_NULL;
}

static JSValue cos_dom_class_list_value(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv,
                                        int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return cos_dom_element_get_class_name(ctx, func_data[0], 0, NULL);
}

static JSValue cos_dom_class_list_length(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv,
                                         int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    JSValue value = cos_dom_element_get_class_name(ctx, func_data[0], 0, NULL);
    const char *s = JS_ToCString(ctx, value); uint32_t count = 0; bool in_token = false;
    if (s != NULL) {
        for (const char *p = s; *p; ++p) {
            bool space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f';
            if (space) in_token = false; else if (!in_token) { in_token = true; ++count; }
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, value); return JS_NewUint32(ctx, count);
}

static JSValue cos_dom_document_get_elements_by_class_name(JSContext *ctx, JSValueConst this_val,
                                                           int argc, JSValueConst *argv)
{
    (void)this_val; JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_GetPropertyStr(ctx, global, "document");
    JSValue result = cos_dom_element_get_elements_by_class_name(ctx, document, argc, argv);
    JS_FreeValue(ctx, document); JS_FreeValue(ctx, global); return result;
}

static void cos_dom_install_element_web_properties(JSContext *ctx, JSValue element)
{
    JSValue style = JS_NewObject(ctx);
    static const char *const style_names[] = {
        "display", "visibility", "color", "backgroundColor", "fontSize",
        "width", "height", "margin", "padding", "position", "top",
        "right", "bottom", "left", "opacity", "border", "borderRadius",
        "textAlign", "fontWeight", "lineHeight", "overflow", "zIndex"
    };
    for (int i = 0; i < (int)(sizeof(style_names) / sizeof(style_names[0])); ++i) {
        cos_dom_define_bound_accessor(ctx, style, style_names[i], i, element);
    }
    JSValue style_data[1] = { JS_DupValue(ctx, element) };
    JS_SetPropertyStr(ctx, style, "getPropertyValue",
                      JS_NewCFunctionData2(ctx, cos_dom_style_get_property, "getPropertyValue", 1, 0, 1, style_data));
    JS_SetPropertyStr(ctx, style, "setProperty",
                      JS_NewCFunctionData2(ctx, cos_dom_style_set_property, "setProperty", 2, 0, 1, style_data));
    JS_SetPropertyStr(ctx, style, "removeProperty",
                      JS_NewCFunctionData2(ctx, cos_dom_style_remove_property, "removeProperty", 1, 0, 1, style_data));
    JS_SetPropertyStr(ctx, style, "item",
                      JS_NewCFunction(ctx, cos_dom_style_item, "item", 1));
    JS_SetPropertyStr(ctx, style, "length", JS_NewInt32(ctx, 22));
    JSAtom css_atom = JS_NewAtom(ctx, "cssText");
    JSValue css_get = JS_NewCFunctionData2(ctx, cos_dom_style_get_css_text, "cssText", 0, 0, 1, style_data);
    JSValue css_set = JS_NewCFunctionData2(ctx, cos_dom_style_set_css_text, "cssText", 1, 0, 1, style_data);
    if (JS_DefinePropertyGetSet(ctx, style, css_atom, css_get, css_set,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, css_get); JS_FreeValue(ctx, css_set);
    }
    JS_FreeAtom(ctx, css_atom);
    JS_FreeValue(ctx, style_data[0]);
    JS_SetPropertyStr(ctx, element, "style", style);

    JSValue class_list = JS_NewObject(ctx);
    JSValue data[1] = { JS_DupValue(ctx, element) };
    JS_SetPropertyStr(ctx, class_list, "contains", JS_NewCFunctionData(ctx, cos_dom_class_list_contains, 1, 0, 1, data));
    JS_SetPropertyStr(ctx, class_list, "add", JS_NewCFunctionData(ctx, cos_dom_class_list_add, 1, 0, 1, data));
    JS_SetPropertyStr(ctx, class_list, "remove", JS_NewCFunctionData(ctx, cos_dom_class_list_remove, 1, 0, 1, data));
    JS_SetPropertyStr(ctx, class_list, "toggle", JS_NewCFunctionData(ctx, cos_dom_class_list_toggle, 2, 0, 1, data));
    JS_SetPropertyStr(ctx, class_list, "value", JS_NewCFunctionData2(ctx, cos_dom_class_list_value, "value", 0, 0, 1, data));
    JS_SetPropertyStr(ctx, class_list, "length", JS_NewCFunctionData2(ctx, cos_dom_class_list_length, "length", 0, 0, 1, data));
    JS_FreeValue(ctx, data[0]);
    JS_SetPropertyStr(ctx, element, "classList", class_list);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue make_dataset = JS_GetPropertyStr(ctx, global, "__cos_make_dataset");
    if (JS_IsFunction(ctx, make_dataset)) {
        JSValue arg = JS_DupValue(ctx, element);
        JSValue dataset = JS_Call(ctx, make_dataset, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (!JS_IsException(dataset)) JS_SetPropertyStr(ctx, element, "dataset", dataset);
        else JS_FreeValue(ctx, dataset);
    }
    JS_FreeValue(ctx, make_dataset);
    JS_FreeValue(ctx, global);
}

/* tagName is a one-time snapshot: it is spec-read-only and an element's tag
 * never changes after creation, so there is nothing to keep live. textContent
 * is deliberately NOT set as an own property here any more - it is installed
 * as a live get/set accessor on the shared proto below (see
 * cos_dom_install_element_proto). An own data property of the same name
 * would shadow that accessor on every instance and silently turn
 * `el.textContent = x` back into a no-op JS-side property write that never
 * reaches libdom, exactly the bug this pass fixes. */
static JSValue cos_dom_wrap_element(JSContext *ctx, dom_element *element)
{
    if (element == NULL) return JS_NULL;
    for (size_t i = 0; i < COS_DOM_WRAPPER_CACHE_MAX; ++i) {
        if (cos_dom_wrapper_cache[i].used && cos_dom_wrapper_cache[i].ctx == ctx &&
            cos_dom_wrapper_cache[i].node == (dom_node *)element) {
            return JS_DupValue(ctx, cos_dom_wrapper_cache[i].wrapper);
        }
    }

    JSValue obj = JS_NewObjectClass(ctx, cos_dom_element_class_id);
    if (JS_IsException(obj)) return obj;
    dom_node_ref((dom_node *)element);
    JS_SetOpaque(obj, element);
    dom_string *tag = NULL;
    if (dom_element_get_tag_name(element, &tag) == DOM_NO_ERR && tag != NULL) {
        JS_SetPropertyStr(ctx, obj, "tagName", JS_NewString(ctx, dom_string_data(tag)));
        dom_string_unref(tag);
    }
    cos_dom_install_element_web_properties(ctx, obj);
    for (size_t i = 0; i < COS_DOM_WRAPPER_CACHE_MAX; ++i) {
        if (!cos_dom_wrapper_cache[i].used) {
            cos_dom_wrapper_cache[i].used = true;
            cos_dom_wrapper_cache[i].ctx = ctx;
            cos_dom_wrapper_cache[i].node = (dom_node *)element;
            cos_dom_wrapper_cache[i].wrapper = JS_DupValue(ctx, obj);
            break;
        }
    }
    return obj;
}

void cos_js_release_context_dom_wrappers(JSContext *ctx)
{
    if (ctx == NULL) return;
    if (cos_js_active_page_context == ctx) cos_js_active_page_context = NULL;
    for (size_t i = 0; i < COS_DOM_WRAPPER_CACHE_MAX; ++i) {
        if (cos_dom_wrapper_cache[i].used && cos_dom_wrapper_cache[i].ctx == ctx) {
            JS_FreeValue(ctx, cos_dom_wrapper_cache[i].wrapper);
            memset(&cos_dom_wrapper_cache[i], 0, sizeof(cos_dom_wrapper_cache[i]));
        }
    }
}

/* Wraps a real libdom Text node (from document.createTextNode(), or handed
 * back through a real element's child-node methods) as a JS value using the
 * separate, minimal text-node class - see cos_dom_text_class_id above for
 * why this must not share the element class/proto. */
static JSValue cos_dom_wrap_text_node(JSContext *ctx, dom_text *text_node)
{
    if (text_node == NULL) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, cos_dom_text_class_id);
    if (JS_IsException(obj)) return obj;
    dom_node_ref((dom_node *)text_node);
    JS_SetOpaque(obj, text_node);
    JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#text"));
    return obj;
}

static JSValue cos_dom_wrap_comment_node(JSContext *ctx, dom_comment *comment)
{
    if (comment == NULL) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, cos_dom_comment_class_id);
    if (JS_IsException(obj)) return obj;
    dom_node_ref((dom_node *)comment);
    JS_SetOpaque(obj, comment);
    return obj;
}
static JSValue cos_dom_wrap_fragment(JSContext *ctx, dom_node *fragment)
{
    if (fragment == NULL) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, cos_dom_fragment_class_id);
    if (JS_IsException(obj)) return obj;
    dom_node_ref(fragment);
    JS_SetOpaque(obj, fragment);
    JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#document-fragment"));
    JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, DOM_DOCUMENT_FRAGMENT_NODE));
    return obj;
}

static void cos_dom_register_class(JSRuntime *rt)
{
    if (cos_dom_element_class_id == JS_INVALID_CLASS_ID) {
        JS_NewClassID(rt, &cos_dom_element_class_id);
    }
    if (!JS_IsRegisteredClass(rt, cos_dom_element_class_id)) {
        static const JSClassDef def = {
            .class_name = "COSDOMElement",
            .finalizer = cos_dom_element_finalizer,
            .gc_mark = NULL,
            .call = NULL,
            .exotic = NULL
        };
        JS_NewClass(rt, cos_dom_element_class_id, &def);
    }
    if (cos_dom_text_class_id == JS_INVALID_CLASS_ID) {
        JS_NewClassID(rt, &cos_dom_text_class_id);
    }
    if (!JS_IsRegisteredClass(rt, cos_dom_text_class_id)) {
        static const JSClassDef text_def = {
            .class_name = "COSDOMText",
            .finalizer = cos_dom_text_finalizer,
            .gc_mark = NULL,
            .call = NULL,
            .exotic = NULL
        };
        JS_NewClass(rt, cos_dom_text_class_id, &text_def);
    }
    if (cos_dom_comment_class_id == JS_INVALID_CLASS_ID) {
        JS_NewClassID(rt, &cos_dom_comment_class_id);
    }
    if (!JS_IsRegisteredClass(rt, cos_dom_comment_class_id)) {
        static const JSClassDef comment_def = {
            .class_name = "COSDOMComment",
            .finalizer = cos_dom_comment_finalizer,
            .gc_mark = NULL,
            .call = NULL,
            .exotic = NULL
        };
        JS_NewClass(rt, cos_dom_comment_class_id, &comment_def);
    }
    if (cos_dom_fragment_class_id == JS_INVALID_CLASS_ID) {
        JS_NewClassID(rt, &cos_dom_fragment_class_id);
    }
    if (!JS_IsRegisteredClass(rt, cos_dom_fragment_class_id)) {
        static const JSClassDef fragment_def = {
            .class_name = "COSDOMDocumentFragment",
            .finalizer = cos_dom_fragment_finalizer,
            .gc_mark = NULL,
            .call = NULL,
            .exotic = NULL
        };
        JS_NewClass(rt, cos_dom_fragment_class_id, &fragment_def);
    }
}

/* ---- Live text content (element AND text-node capable: both wrapper
 * classes carry a real dom_node* underneath, and dom_node_get/set_text_content
 * is defined generically on Node, not just Element) ---- */

static JSValue cos_dom_node_get_text_content(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_NewString(ctx, "");
    dom_string *text = NULL;
    if (dom_node_get_text_content(node, &text) != DOM_NO_ERR || text == NULL) {
        return JS_NewString(ctx, "");
    }
    JSValue result = JS_NewStringLen(ctx, dom_string_data(text), dom_string_byte_length(text));
    dom_string_unref(text);
    return result;
}

static JSValue cos_dom_node_set_text_content(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_UNDEFINED;
    const char *text_c = "";
    bool owns_cstring = false;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        text_c = JS_ToCString(ctx, argv[0]);
        if (text_c == NULL) return JS_EXCEPTION;
        owns_cstring = true;
    }
    /* The node may be a CharacterData node; snapshot only its current
     * value, before libdom replaces its contents. */
    if (node != NULL) {
        dom_node_type node_type = DOM_ELEMENT_NODE;
        (void)dom_node_get_node_type(node, &node_type);
        if (node_type == DOM_TEXT_NODE || node_type == DOM_COMMENT_NODE)
            cos_dom_snapshot_character_data(node);
        else
            cos_dom_remember_old_value(NULL);
    }
    dom_string *text = NULL;
    if (dom_string_create((const uint8_t *)text_c, strlen(text_c), &text) == DOM_NO_ERR) {
        /* Per DOM spec this replaces all children with a single Text node
         * (or removes them all for ""); libdom's set_text_content already
         * implements exactly that, so there is no manual child-walking here. */
        (void)dom_node_set_text_content(node, text);
        dom_string_unref(text);
    }
    if (owns_cstring) JS_FreeCString(ctx, text_c);
        cos_dom_notify_mutation();
    return JS_UNDEFINED;
}
/* innerHTML setter: a bounded fragment parser that creates actual libdom
 * nodes. It intentionally accepts ordinary HTML element/text nesting and a
 * small safe attribute set; scripts/styles inserted this way remain inert,
 * matching browser fragment semantics and avoiding re-entrant parsing. */
static bool cos_dom_html_void_tag(const char *tag)
{
    return strcmp(tag, "br") == 0 || strcmp(tag, "img") == 0 ||
           strcmp(tag, "hr") == 0 || strcmp(tag, "meta") == 0 ||
           strcmp(tag, "link") == 0 || strcmp(tag, "input") == 0 ||
           strcmp(tag, "source") == 0;
}

static void cos_dom_html_append_text(dom_document *doc, dom_node *parent,
                                     const char *text, size_t length)
{
    if (doc == NULL || parent == NULL || text == NULL || length == 0) return;
    dom_string *data = NULL; dom_text *node = NULL; dom_node *result = NULL;
    if (dom_string_create((const uint8_t *)text, length, &data) == DOM_NO_ERR &&
        dom_document_create_text_node(doc, data, &node) == DOM_NO_ERR && node != NULL) {
        (void)dom_node_append_child(parent, (dom_node *)node, &result);
        if (result != NULL) dom_node_unref(result);
        dom_node_unref((dom_node *)node);
    }
    if (data != NULL) dom_string_unref(data);
}

static void cos_dom_html_set_attr(dom_element *element, const char *name,
                                  size_t name_len, const char *value, size_t value_len)
{
    if (element == NULL || name == NULL || value == NULL || name_len == 0) return;
    /* Keep the fragment bridge predictable: enough for normal content and
     * style/class/id-based JS, without parsing event-handler attributes. */
    char key[40]; char val[384];
    if (name_len >= sizeof(key)) return;
    memcpy(key, name, name_len); key[name_len] = '\0';
    for (size_t i = 0; key[i] != '\0'; ++i) {
        if (key[i] >= 'A' && key[i] <= 'Z') key[i] = (char)(key[i] - 'A' + 'a');
    }
    /* Preserve arbitrary valid HTML attributes, including data-* and aria-*;
     * event-handler attributes are stored as data only and are not executed by
     * this bridge. */
    if (value_len >= sizeof(val)) value_len = sizeof(val) - 1;
    memcpy(val, value, value_len); val[value_len] = '\0';
    dom_string *n = NULL; dom_string *v = NULL;
    if (dom_string_create((const uint8_t *)key, strlen(key), &n) == DOM_NO_ERR &&
        dom_string_create((const uint8_t *)val, strlen(val), &v) == DOM_NO_ERR) {
        (void)dom_element_set_attribute(element, n, v);
    }
    if (n != NULL) dom_string_unref(n);
    if (v != NULL) dom_string_unref(v);
}

static void cos_dom_html_apply_attrs(dom_element *element, const char *start, const char *end)
{
    const char *p = start;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/')) ++p;
        const char *name = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) ++p;
        size_t name_len = (size_t)(p - name);
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (name_len == 0 || p >= end || *p != '=') { while (p < end && *p != ' ') ++p; continue; }
        ++p; while (p < end && (*p == ' ' || *p == '\t')) ++p;
        char quote = 0;
        if (p < end && (*p == '\'' || *p == '\"')) quote = *p++;
        const char *value = p;
        if (quote) while (p < end && *p != quote) ++p;
        else while (p < end && *p != ' ' && *p != '\t' && *p != '/') ++p;
        cos_dom_html_set_attr(element, name, name_len, value, (size_t)(p - value));
        if (quote && p < end) ++p;
    }
}

static JSValue cos_dom_element_get_inner_html(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct cos_dom_serial_buffer buf = { ctx, NULL, 0, 0 };
    if (!cos_dom_serialize_node(ctx, this_val, &buf, false)) {
        if (buf.data != NULL) js_free(ctx, buf.data);
        return JS_EXCEPTION;
    }
    JSValue result = JS_NewStringLen(ctx, buf.data != NULL ? buf.data : "", buf.length);
    if (buf.data != NULL) js_free(ctx, buf.data);
    return result;
}

static JSValue cos_dom_element_set_inner_html(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    dom_element *root = cos_dom_unwrap_element(ctx, this_val);
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (root == NULL || doc == NULL) return JS_UNDEFINED;
    const char *html = "";
    bool owns = false;
    if (argc >= 1 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        html = JS_ToCString(ctx, argv[0]);
        if (html == NULL) return JS_EXCEPTION;
        owns = true;
    }
    /* The overwhelmingly common `element.innerHTML = "plain text"` case
     * must use libdom's native text-content replacement.  Manually creating
     * and appending a Text node while the HTML parser is still processing an
     * inline script leaves the compact HTML-to-box converter with a stale
     * child traversal and rendered a blank page in the real GUI test. */
    if (strchr(html, '<') == NULL) {
        dom_string *text = NULL;
        if (dom_string_create((const uint8_t *)html, strlen(html), &text) == DOM_NO_ERR) {
            (void)dom_node_set_text_content((dom_node *)root, text);
            dom_string_unref(text);
        }
        if (owns) JS_FreeCString(ctx, html);
        cos_dom_notify_mutation();
        return JS_UNDEFINED;
    }

    /* Remove all prior children before adding a structural fragment. */
    for (;;) {
        dom_node *child = NULL; dom_node *removed = NULL;
        if (dom_node_get_first_child((dom_node *)root, &child) != DOM_NO_ERR || child == NULL) break;
        (void)dom_node_remove_child((dom_node *)root, child, &removed);
        if (removed != NULL) dom_node_unref(removed);
        dom_node_unref(child);
    }
    dom_node *parents[32]; int depth = 1; parents[0] = (dom_node *)root;
    size_t pos = 0; size_t text_start = 0; size_t html_len = strlen(html);
    while (pos < html_len) {
        if (html[pos] != '<') { ++pos; continue; }
        if (pos > text_start) cos_dom_html_append_text(doc, parents[depth - 1], html + text_start, pos - text_start);
        size_t close = pos + 1; while (close < html_len && html[close] != '>') ++close;
        if (close >= html_len) { cos_dom_html_append_text(doc, parents[depth - 1], html + pos, html_len - pos); break; }
        size_t p = pos + 1; while (p < close && (html[p] == ' ' || html[p] == '\t')) ++p;
        bool closing = p < close && html[p] == '/'; if (closing) ++p;
        size_t tag_start = p; while (p < close && ((html[p] >= 'a' && html[p] <= 'z') || (html[p] >= 'A' && html[p] <= 'Z') || (html[p] >= '0' && html[p] <= '9'))) ++p;
        size_t tag_len = p - tag_start; char tag[48];
        if (tag_len > 0 && tag_len < sizeof(tag)) {
            memcpy(tag, html + tag_start, tag_len); tag[tag_len] = '\0';
            for (size_t i = 0; tag[i] != '\0'; ++i) if (tag[i] >= 'A' && tag[i] <= 'Z') tag[i] = (char)(tag[i] - 'A' + 'a');
            if (closing) { if (depth > 1) --depth; }
            else if (strcmp(tag, "script") != 0 && strcmp(tag, "style") != 0) {
                dom_string *name = NULL; dom_element *element = NULL; dom_node *result = NULL;
                if (dom_string_create((const uint8_t *)tag, strlen(tag), &name) == DOM_NO_ERR &&
                    dom_document_create_element(doc, name, &element) == DOM_NO_ERR && element != NULL) {
                    cos_dom_html_apply_attrs(element, html + p, html + close);
                    (void)dom_node_append_child(parents[depth - 1], (dom_node *)element, &result);
                    if (result != NULL) dom_node_unref(result);
                    bool self_close = close > pos && html[close - 1] == '/';
                    if (!self_close && !cos_dom_html_void_tag(tag) && depth < (int)(sizeof(parents) / sizeof(parents[0]))) {
                        parents[depth++] = (dom_node *)element;
                    } else {
                        dom_node_unref((dom_node *)element);
                    }
                }
                if (name != NULL) dom_string_unref(name);
            }
        }
        pos = close + 1; text_start = pos;
    }
    while (depth > 1) dom_node_unref(parents[--depth]);
    if (owns) JS_FreeCString(ctx, html);
    cos_dom_notify_mutation();
    return JS_UNDEFINED;
}

/* ---- id / className: thin live wrappers over the existing, already-real
 * getAttribute/setAttribute pair, matching how the DOM spec itself defines
 * these two IDL attributes as reflections of the "id"/"class" content
 * attributes rather than separate storage. ---- */

static JSValue cos_dom_element_get_named_attr(JSContext *ctx, JSValueConst this_val,
                                              const char *attr_name)
{
    JSValue name = JS_NewString(ctx, attr_name);
    JSValueConst argv1[1];
    argv1[0] = name;
    JSValue result = cos_dom_element_get_attribute(ctx, this_val, 1, argv1);
    JS_FreeValue(ctx, name);
    if (JS_IsNull(result)) {
        JS_FreeValue(ctx, result);
        return JS_NewString(ctx, "");
    }
    return result;
}

static JSValue cos_dom_element_set_named_attr(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              const char *attr_name)
{
    if (argc < 1) return JS_UNDEFINED;
    JSValue name = JS_NewString(ctx, attr_name);
    JSValueConst argv2[2];
    argv2[0] = name;
    argv2[1] = argv[0];
    /* cos_dom_element_set_attribute() already requests a redraw on success. */
    JSValue result = cos_dom_element_set_attribute(ctx, this_val, 2, argv2);
    JS_FreeValue(ctx, name);
    return result;
}

static JSValue cos_dom_element_get_id(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return cos_dom_element_get_named_attr(ctx, this_val, "id");
}
static JSValue cos_dom_element_set_id(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    return cos_dom_element_set_named_attr(ctx, this_val, argc, argv, "id");
}
static JSValue cos_dom_element_get_class_name(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return cos_dom_element_get_named_attr(ctx, this_val, "class");
}
static JSValue cos_dom_element_set_class_name(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    return cos_dom_element_set_named_attr(ctx, this_val, argc, argv, "class");
}
static JSValue cos_dom_element_get_slot(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return cos_dom_element_get_named_attr(ctx, this_val, "slot");
}
static JSValue cos_dom_element_set_slot(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return cos_dom_element_set_named_attr(ctx, this_val, argc, argv, "slot");
}
static JSValue cos_dom_element_get_name(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return cos_dom_element_get_named_attr(ctx, this_val, "name");
}
static JSValue cos_dom_element_set_name(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return cos_dom_element_set_named_attr(ctx, this_val, argc, argv, "name");
}

/* ---- Real tree mutation: appendChild / removeChild / insertBefore /
 * parentNode.  These are what let script actually build and rearrange page
 * content instead of only reading/annotating nodes that already exist. ---- */

static void cos_dom_fire_slotchange(JSContext *ctx, JSValueConst slot)
{
    if (!JS_IsObject(slot)) return;
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "slotchange"));
    JS_SetPropertyStr(ctx, event, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_FALSE);
    JSValue dispatch = JS_GetPropertyStr(ctx, slot, "dispatchEvent");
    if (JS_IsFunction(ctx, dispatch)) {
        JSValue args[1] = { event };
        JSValue result = JS_Call(ctx, dispatch, slot, 1, args);
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, dispatch);
    JS_FreeValue(ctx, event);
}
static void cos_dom_assign_light_node_to_slot(JSContext *ctx, JSValueConst host,
                                              JSValueConst child)
{
    JSValue root = JS_GetPropertyStr(ctx, host, "_cos_shadow_root");
    if (!JS_IsObject(root)) { JS_FreeValue(ctx, root); return; }
    JSValue slots = JS_GetPropertyStr(ctx, root, "childNodes");
    JSValue length = JS_GetPropertyStr(ctx, slots, "length"); uint32_t count = 0;
    JS_ToUint32(ctx, &count, length);
    JSValue child_slot = JS_GetPropertyStr(ctx, child, "slot");
    const char *child_slot_c = JS_ToCString(ctx, child_slot);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue candidate = JS_GetPropertyUint32(ctx, slots, i);
        JSValue tag = JS_GetPropertyStr(ctx, candidate, "tagName");
        const char *tag_c = JS_ToCString(ctx, tag);
        if (tag_c != NULL && strcmp(tag_c, "SLOT") == 0) {
            JSValue slot_name = cos_dom_element_get_named_attr(ctx, candidate, "name");
            const char *slot_name_c = JS_ToCString(ctx, slot_name);
            if (child_slot_c != NULL && slot_name_c != NULL && strcmp(child_slot_c, slot_name_c) == 0) {
                JSValue previous = JS_GetPropertyStr(ctx, child, "_cos_assigned_slot");
                bool changed = !JS_IsStrictEqual(ctx, previous, candidate);
                JS_FreeValue(ctx, previous);
                JS_SetPropertyStr(ctx, child, "_cos_assigned_slot", JS_DupValue(ctx, candidate));
                if (changed) cos_dom_fire_slotchange(ctx, candidate);
                JS_FreeValue(ctx, slot_name); JS_FreeValue(ctx, candidate); JS_FreeValue(ctx, tag);
                if (tag_c != NULL) JS_FreeCString(ctx, tag_c);
                if (slot_name_c != NULL) JS_FreeCString(ctx, slot_name_c);
                break;
            }
            JS_FreeValue(ctx, slot_name);
            if (slot_name_c != NULL) JS_FreeCString(ctx, slot_name_c);
        }
        if (tag_c != NULL) JS_FreeCString(ctx, tag_c);
        JS_FreeValue(ctx, tag); JS_FreeValue(ctx, candidate);
    }
    if (child_slot_c != NULL) JS_FreeCString(ctx, child_slot_c);
    JS_FreeValue(ctx, child_slot); JS_FreeValue(ctx, length); JS_FreeValue(ctx, slots); JS_FreeValue(ctx, root);
}

static JSValue cos_dom_element_append_child(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    dom_node *parent = cos_dom_unwrap_any_node(ctx, this_val);
    if (parent == NULL) return JS_ThrowTypeError(ctx, "appendChild called on a non-DOM node");
    if (argc < 1) return JS_ThrowTypeError(ctx, "appendChild requires a node argument");
    dom_node *child = cos_dom_unwrap_any_node(ctx, argv[0]);
    if (child == NULL) return JS_ThrowTypeError(ctx, "appendChild argument is not a DOM node");
    dom_node *result = NULL;
    dom_exception err = dom_node_append_child(parent, child, &result);
    if (result != NULL) dom_node_unref(result);
    if (err != DOM_NO_ERR) return JS_ThrowTypeError(ctx, "appendChild failed");
    cos_dom_assign_light_node_to_slot(ctx, this_val, argv[0]);
    cos_dom_notify_mutation();
    return JS_DupValue(ctx, argv[0]);
}

static JSValue cos_dom_element_remove_child(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    dom_node *parent = cos_dom_unwrap_any_node(ctx, this_val);
    if (parent == NULL) return JS_ThrowTypeError(ctx, "removeChild called on a non-DOM node");
    if (argc < 1) return JS_ThrowTypeError(ctx, "removeChild requires a node argument");
    dom_node *child = cos_dom_unwrap_any_node(ctx, argv[0]);
    if (child == NULL) return JS_ThrowTypeError(ctx, "removeChild argument is not a DOM node");
    dom_node *result = NULL;
    JSValue old_slot = JS_GetPropertyStr(ctx, argv[0], "_cos_assigned_slot");
    dom_exception err = dom_node_remove_child(parent, child, &result);
    if (result != NULL) dom_node_unref(result);
    if (err != DOM_NO_ERR) {
        JS_FreeValue(ctx, old_slot);
        return JS_ThrowTypeError(ctx, "removeChild failed (not a child of this node?)");
    }
    bool had_slot = JS_IsObject(old_slot);
    JS_SetPropertyStr(ctx, (JSValue)argv[0], "_cos_assigned_slot", JS_NULL);
    if (had_slot) cos_dom_fire_slotchange(ctx, old_slot);
    JS_FreeValue(ctx, old_slot);
    cos_dom_notify_mutation();
    return JS_DupValue(ctx, argv[0]);
}

static JSValue cos_dom_element_insert_before(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    dom_node *parent = cos_dom_unwrap_any_node(ctx, this_val);
    if (parent == NULL) return JS_ThrowTypeError(ctx, "insertBefore called on a non-DOM node");
    if (argc < 1) return JS_ThrowTypeError(ctx, "insertBefore requires a node argument");
    dom_node *new_node = cos_dom_unwrap_any_node(ctx, argv[0]);
    if (new_node == NULL) return JS_ThrowTypeError(ctx, "insertBefore argument is not a DOM node");
    /* A null/undefined/omitted reference means "insert at the end", matching
     * Node.insertBefore(node, null) in the DOM spec. */
    dom_node *ref_node = NULL;
    if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        ref_node = cos_dom_unwrap_any_node(ctx, argv[1]);
        if (ref_node == NULL) {
            return JS_ThrowTypeError(ctx, "insertBefore reference is not a DOM node");
        }
    }
    dom_node *result = NULL;
    dom_exception err = dom_node_insert_before(parent, new_node, ref_node, &result);
    if (result != NULL) dom_node_unref(result);
    if (err != DOM_NO_ERR) return JS_ThrowTypeError(ctx, "insertBefore failed");
    cos_dom_notify_mutation();
    return JS_DupValue(ctx, argv[0]);
}

static JSValue cos_dom_node_get_parent_node(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_NULL;
    dom_node *parent = NULL;
    if (dom_node_get_parent_node(node, &parent) != DOM_NO_ERR || parent == NULL) {
        return JS_NULL;
    }
    /* Only Element parents are wrapped: the one other real case (a root
     * node's parent being the Document itself) has no JS-visible Document
     * node in this compact bridge - the global `document` object is a plain
     * compatibility object, not a wrapped dom_document. Returning null there
     * instead of risking a wrong-vtable cast is the safe simplification. */
    dom_node_type type = (dom_node_type)0;
    JSValue result = JS_NULL;
    if (dom_node_get_node_type(parent, &type) == DOM_NO_ERR && type == DOM_ELEMENT_NODE) {
        result = cos_dom_wrap_element(ctx, (dom_element *)parent);
    }
    dom_node_unref(parent);
    return result;
}

static JSValue cos_dom_wrap_node(JSContext *ctx, dom_node *node)
{
    if (node == NULL) return JS_NULL;
    dom_node_type type = (dom_node_type)0;
    if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) return JS_NULL;
    if (type == DOM_ELEMENT_NODE) return cos_dom_wrap_element(ctx, (dom_element *)node);
    if (type == DOM_TEXT_NODE) return cos_dom_wrap_text_node(ctx, (dom_text *)node);
    if (type == DOM_COMMENT_NODE) return cos_dom_wrap_comment_node(ctx, (dom_comment *)node);
    if (type == DOM_DOCUMENT_FRAGMENT_NODE) return cos_dom_wrap_fragment(ctx, node);
    return JS_NULL;
}

/* magic: 0 parent, 1 first child, 2 last child, 3 previous, 4 next */
static JSValue cos_dom_node_get_relative(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_NULL;
    dom_node *relative = NULL;
    dom_exception err = DOM_NO_ERR;
    if (magic == 0) err = dom_node_get_parent_node(node, &relative);
    else if (magic == 1) err = dom_node_get_first_child(node, &relative);
    else if (magic == 2) err = dom_node_get_last_child(node, &relative);
    else if (magic == 3) err = dom_node_get_previous_sibling(node, &relative);
    else err = dom_node_get_next_sibling(node, &relative);
    if (err != DOM_NO_ERR || relative == NULL) return JS_NULL;
    JSValue result = cos_dom_wrap_node(ctx, relative);
    dom_node_unref(relative);
    return result;
}

static JSValue cos_dom_node_get_children(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    JSValue result = JS_NewArray(ctx);
    if (node == NULL) return result;
    uint32_t out = 0; dom_node *child = NULL;
    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) return result;
    while (child != NULL && out < 4096u) {
        dom_node *next = NULL; (void)dom_node_get_next_sibling(child, &next);
        dom_node_type type = (dom_node_type)0;
        if (!magic || (dom_node_get_node_type(child, &type) == DOM_NO_ERR && type == DOM_ELEMENT_NODE)) {
            JSValue wrapped = cos_dom_wrap_node(ctx, child);
            if (!JS_IsNull(wrapped) && !JS_IsException(wrapped)) JS_SetPropertyUint32(ctx, result, out++, wrapped);
            else JS_FreeValue(ctx, wrapped);
        }
        dom_node_unref(child); child = next;
    }
    return result;
}

static JSValue cos_dom_node_clone_node(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_ThrowTypeError(ctx, "cloneNode called on a non-DOM node");
    dom_node *clone = NULL;
    if (dom_node_clone_node(node, argc > 0 && JS_ToBool(ctx, argv[0]), &clone) != DOM_NO_ERR || clone == NULL) return JS_EXCEPTION;
    JSValue result = cos_dom_wrap_node(ctx, clone); dom_node_unref(clone); return result;
}

static JSValue cos_dom_element_replace_child(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    dom_node *parent = cos_dom_unwrap_any_node(ctx, this_val);
    if (parent == NULL || argc < 2) return JS_ThrowTypeError(ctx, "replaceChild requires two DOM nodes");
    dom_node *replacement = cos_dom_unwrap_any_node(ctx, argv[0]);
    dom_node *old_child = cos_dom_unwrap_any_node(ctx, argv[1]);
    if (replacement == NULL || old_child == NULL) return JS_ThrowTypeError(ctx, "replaceChild arguments must be DOM nodes");
    dom_node *removed = NULL;
    if (dom_node_replace_child(parent, replacement, old_child, &removed) != DOM_NO_ERR) return JS_ThrowTypeError(ctx, "replaceChild failed");
    if (removed != NULL) dom_node_unref(removed);
        cos_dom_notify_mutation();
    return JS_DupValue(ctx, argv[1]);
}

static void cos_dom_define_relative_accessor(JSContext *ctx, JSValueConst proto,
                                             const char *name, int magic)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue getter = JS_NewCFunctionMagic(ctx, cos_dom_node_get_relative, name, 0,
                                           JS_CFUNC_generic_magic, magic);
    if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter);
    }
    JS_FreeAtom(ctx, atom);
}

static void cos_dom_define_children_accessor(JSContext *ctx, JSValueConst proto,
                                             const char *name, int magic)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue getter = JS_NewCFunctionMagic(ctx, cos_dom_node_get_children, name, 0,
                                           JS_CFUNC_generic_magic, magic);
    if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter);
    }
    JS_FreeAtom(ctx, atom);
}

static void cos_dom_libdom_event_handler(dom_event *event, void *opaque)
{
    JSContext *ctx = (JSContext *)opaque;
    dom_string *type = NULL; dom_event_target *target = NULL;
    if (ctx == NULL || dom_event_get_type(event, &type) != DOM_NO_ERR || type == NULL ||
        dom_event_get_target(event, &target) != DOM_NO_ERR || target == NULL) {
        if (type != NULL) dom_string_unref(type);
        return;
    }
    char name[64]; size_t n = dom_string_byte_length(type);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, dom_string_data(type), n); name[n] = '\0';
    serial_puts("[QJS/DOM] libdom event "); serial_puts(name);
    serial_puts(" target="); serial_putdec((uint64_t)(uintptr_t)target); serial_puts("\n");
    (void)cos_js_dispatch_dom_event(ctx, target, name);
    dom_string_unref(type);
    dom_node_unref((dom_node *)target);
}

static bool cos_dom_listener_option(JSContext *ctx, int argc, JSValueConst *argv,
                                    const char *name)
{
    if (argc < 3 || JS_IsUndefined(argv[2]) || JS_IsNull(argv[2])) return false;
    if (JS_IsBool(argv[2])) return JS_ToBool(ctx, argv[2]) > 0;
    if (!JS_IsObject(argv[2])) return false;
    JSValue value = JS_GetPropertyStr(ctx, argv[2], name);
    int enabled = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    return enabled > 0;
}

static JSValue cos_dom_node_add_event_listener(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv)
{
    if (cos_dom_unwrap_any_node(ctx, this_val) == NULL || argc < 2 ||
        !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (type == NULL) return JS_EXCEPTION;
    JSValue table = JS_GetPropertyStr(ctx, this_val, "__cos_dom_events");
    if (!JS_IsObject(table)) { JS_FreeValue(ctx, table); table = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__cos_dom_events", JS_DupValue(ctx, table)); }
    JSValue list = JS_GetPropertyStr(ctx, table, type);
    if (!JS_IsArray(list)) {
        JS_FreeValue(ctx, list);
        list = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, table, type, JS_DupValue(ctx, list));
    }
    /* NetSurf's generic DOM dispatcher is the authoritative ingress. Keep a
     * bounded listener record so capture/once/passive semantics are resolved
     * synchronously against the real libdom ancestor path. */
    uint32_t length = 0;
    JSValue length_value = JS_GetPropertyStr(ctx, list, "length");
    (void)JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    if (length < 128u) {
        JSValue record = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, record, "callback", JS_DupValue(ctx, argv[1]));
        JS_SetPropertyStr(ctx, record, "capture", JS_NewBool(ctx, cos_dom_listener_option(ctx, argc, argv, "capture")));
        JS_SetPropertyStr(ctx, record, "once", JS_NewBool(ctx, cos_dom_listener_option(ctx, argc, argv, "once")));
        JS_SetPropertyStr(ctx, record, "passive", JS_NewBool(ctx, cos_dom_listener_option(ctx, argc, argv, "passive")));
        JS_SetPropertyUint32(ctx, list, length, record);
    }
    JS_FreeValue(ctx, list); JS_FreeValue(ctx, table); JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue cos_dom_node_dispatch_event(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL || argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "dispatchEvent requires an Event object");
    }
    JSValue type_value = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, type_value);
    if (type == NULL || type[0] == '\0') {
        if (type != NULL) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, type_value);
        return JS_ThrowTypeError(ctx, "Event.type must be a non-empty string");
    }
    bool delivered = cos_js_dispatch_bound_dom_event(node, type);
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, type_value);
    /* EventTarget.dispatchEvent() returns true when dispatch completes without
     * a canceled default action. The compact bridge reports successful delivery
     * for a valid event path; cancellation state remains on the event object. */
    return JS_NewBool(ctx, delivered || true);
}

static JSValue cos_dom_node_remove_event_listener(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv)
{
    if (cos_dom_unwrap_any_node(ctx, this_val) == NULL || argc < 2 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]); if (type == NULL) return JS_EXCEPTION;
    bool capture = cos_dom_listener_option(ctx, argc, argv, "capture");
    JSValue table = JS_GetPropertyStr(ctx, this_val, "__cos_dom_events");
    JSValue list = JS_IsObject(table) ? JS_GetPropertyStr(ctx, table, type) : JS_UNDEFINED;
    uint32_t length = 0;
    if (JS_IsArray(list)) {
        JSValue length_value = JS_GetPropertyStr(ctx, list, "length");
        (void)JS_ToUint32(ctx, &length, length_value); JS_FreeValue(ctx, length_value);
    }
    for (uint32_t i = 0; i < length; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, list, i);
        JSValue callback = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "callback") : JS_DupValue(ctx, entry);
        JSValue capture_value = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "capture") : JS_FALSE;
        bool same = JS_IsStrictEqual(ctx, callback, argv[1]) && (JS_ToBool(ctx, capture_value) > 0) == capture;
        JS_FreeValue(ctx, callback); JS_FreeValue(ctx, capture_value);
        if (same) { JS_SetPropertyUint32(ctx, list, i, JS_NULL); JS_FreeValue(ctx, entry); break; }
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, list); JS_FreeValue(ctx, table); JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

/* query selector implementations are defined below the prototype installers. */
static JSValue cos_dom_query_selector(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
static JSValue cos_dom_query_selector_all(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv);

/* ---- DOM Core additions ---- */
static JSValue cos_dom_node_has_child_nodes(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_NewBool(ctx, false);
    dom_node *child = NULL;
    dom_exception err = dom_node_get_first_child(node, &child);
    if (child != NULL) dom_node_unref(child);
    return JS_NewBool(ctx, err == DOM_NO_ERR && child != NULL);
}

static JSValue cos_dom_node_get_node_type_value(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    dom_node_type type = 0;
    if (node == NULL || dom_node_get_node_type(node, &type) != DOM_NO_ERR)
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int32_t)type);
}

static JSValue cos_dom_node_get_node_name_value(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    dom_string *name = NULL;
    if (node == NULL || dom_node_get_node_name(node, &name) != DOM_NO_ERR || name == NULL)
        return JS_NewString(ctx, "");
    JSValue result = JS_NewStringLen(ctx, dom_string_data(name), dom_string_byte_length(name));
    dom_string_unref(name);
    return result;
}

static JSValue cos_dom_element_remove(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue parent = cos_dom_node_get_parent_node(ctx, this_val, 0, NULL);
    if (JS_IsNull(parent) || JS_IsUndefined(parent)) {
        JS_FreeValue(ctx, parent);
        return JS_UNDEFINED;
    }
    JSValue args[1] = { JS_DupValue(ctx, this_val) };
    JSValue result = cos_dom_element_remove_child(ctx, parent, 1, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, parent);
    return JS_IsException(result) ? result : JS_UNDEFINED;
}

static JSValue cos_dom_element_get_elements_by_class_name(JSContext *ctx,
                                                          JSValueConst this_val,
                                                          int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NewArray(ctx);
    const char *class_name = JS_ToCString(ctx, argv[0]);
    if (class_name == NULL) return JS_EXCEPTION;
    size_t n = strlen(class_name);
    JSValue selector;
    char *buf = js_malloc(ctx, n + 2);
    if (buf == NULL) {
        JS_FreeCString(ctx, class_name);
        return JS_EXCEPTION;
    }
    buf[0] = '.';
    memcpy(buf + 1, class_name, n + 1);
    selector = JS_NewStringLen(ctx, buf, n + 1);
    js_free(ctx, buf);
    JS_FreeCString(ctx, class_name);
    if (JS_IsException(selector)) return selector;
    JSValue result = cos_dom_query_selector_all(ctx, this_val, 1, &selector);
    JS_FreeValue(ctx, selector);
    return result;
}

static JSValue cos_dom_element_toggle_attribute(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "toggleAttribute requires a name");
    JSValue name = JS_ToString(ctx, argv[0]);
    if (JS_IsException(name)) return name;
    JSValue present = cos_dom_element_has_attribute(ctx, this_val, 1, &name);
    bool force = argc >= 2 && !JS_IsUndefined(argv[1]);
    bool enabled = force ? JS_ToBool(ctx, argv[1]) : !JS_ToBool(ctx, present);
    JS_FreeValue(ctx, present);
    JSValue result;
    if (enabled) {
        JSValue empty = JS_NewString(ctx, "");
        JSValue args[2] = { JS_DupValue(ctx, name), empty };
        result = cos_dom_element_set_attribute(ctx, this_val, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    } else {
        result = cos_dom_element_remove_attribute(ctx, this_val, 1, &name);
    }
    JS_FreeValue(ctx, name);
    if (JS_IsException(result)) return result;
    return JS_NewBool(ctx, enabled);
}

static JSValue cos_dom_element_matches(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "matches requires a selector");
    JSValue matches = cos_dom_query_selector_all(ctx, this_val, 1, argv);
    if (JS_IsException(matches)) return matches;
    JSValue length = JS_GetPropertyStr(ctx, matches, "length");
    uint32_t count = 0;
    if (!JS_IsException(length)) (void)JS_ToUint32(ctx, &count, length);
    JS_FreeValue(ctx, length);
    bool found = false;
    if (count > 4096u) count = 4096u;
    for (uint32_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, matches, i);
        if (!JS_IsException(item)) {
            found = JS_IsStrictEqual(ctx, item, this_val);
            JS_FreeValue(ctx, item);
            if (found) break;
        }
    }
    JS_FreeValue(ctx, matches);
    return JS_NewBool(ctx, found);
}

static JSValue cos_dom_shadow_get_root_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}
static JSValue cos_dom_element_assigned_nodes(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue result = JS_NewArray(ctx);
    JSValue tag = JS_GetPropertyStr(ctx, this_val, "tagName");
    const char *tag_c = JS_ToCString(ctx, tag);
    if (tag_c == NULL || strcmp(tag_c, "SLOT") != 0) {
        if (tag_c != NULL) JS_FreeCString(ctx, tag_c);
        JS_FreeValue(ctx, tag);
        return result;
    }
    JS_FreeCString(ctx, tag_c); JS_FreeValue(ctx, tag);
    JSValue host = JS_GetPropertyStr(ctx, this_val, "_cos_shadow_host");
    JSValue name = cos_dom_element_get_named_attr(ctx, this_val, "name");
    const char *name_c = JS_ToCString(ctx, name);
    JSValue light = JS_IsObject(host) ? JS_GetPropertyStr(ctx, host, "childNodes") : JS_UNDEFINED;
    JSValue length = JS_GetPropertyStr(ctx, light, "length"); uint32_t count = 0, out = 0;
    JS_ToUint32(ctx, &count, length);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue child = JS_GetPropertyUint32(ctx, light, i);
        JSValue slot = JS_GetPropertyStr(ctx, child, "slot");
        const char *slot_c = JS_ToCString(ctx, slot);
        bool match = slot_c != NULL && name_c != NULL && strcmp(slot_c, name_c) == 0;
        if (slot_c != NULL) JS_FreeCString(ctx, slot_c); JS_FreeValue(ctx, slot);
        if (match) JS_SetPropertyUint32(ctx, result, out++, child);
        else JS_FreeValue(ctx, child);
    }
    if (out == 0) {
        JSValue fallback = JS_GetPropertyStr(ctx, this_val, "childNodes");
        JSValue flen = JS_GetPropertyStr(ctx, fallback, "length"); uint32_t fc = 0;
        JS_ToUint32(ctx, &fc, flen);
        for (uint32_t i = 0; i < fc; ++i) {
            JSValue child = JS_GetPropertyUint32(ctx, fallback, i);
            JS_SetPropertyUint32(ctx, result, out++, child);
        }
        JS_FreeValue(ctx, flen); JS_FreeValue(ctx, fallback);
    }
    JS_FreeValue(ctx, length); JS_FreeValue(ctx, light); JS_FreeValue(ctx, name); JS_FreeValue(ctx, host);
    return result;
}

static JSValue cos_dom_element_get_assigned_slot(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue slot = JS_GetPropertyStr(ctx, this_val, "_cos_assigned_slot");
    return JS_IsObject(slot) ? slot : (JS_FreeValue(ctx, slot), JS_NULL);
}

static JSValue cos_dom_shadow_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue children = JS_GetPropertyStr(ctx, this_val, "childNodes");
    JSValue length = JS_GetPropertyStr(ctx, children, "length"); uint32_t index = 0; JS_ToUint32(ctx, &index, length); JS_FreeValue(ctx, length);
    JSValue host = JS_GetPropertyStr(ctx, this_val, "host");
    for (int i = 0; i < argc; ++i) if (JS_IsObject(argv[i])) {
        JS_SetPropertyUint32(ctx, children, index++, JS_DupValue(ctx, argv[i]));
        JS_SetPropertyStr(ctx, argv[i], "_cos_shadow_host", JS_DupValue(ctx, host));
        JSValue slot_tag = JS_GetPropertyStr(ctx, argv[i], "tagName");
        const char *slot_tag_c = JS_ToCString(ctx, slot_tag);
        if (slot_tag_c != NULL && strcmp(slot_tag_c, "SLOT") == 0 && JS_IsObject(host)) {
            JSValue slot_name = cos_dom_element_get_named_attr(ctx, argv[i], "name");
            const char *slot_name_c = JS_ToCString(ctx, slot_name);
            JSValue light = JS_GetPropertyStr(ctx, host, "childNodes");
            JSValue light_len = JS_GetPropertyStr(ctx, light, "length"); uint32_t light_count = 0;
            JS_ToUint32(ctx, &light_count, light_len);
            for (uint32_t li = 0; li < light_count; ++li) {
                JSValue light_child = JS_GetPropertyUint32(ctx, light, li);
                JSValue light_slot = JS_GetPropertyStr(ctx, light_child, "slot");
                const char *light_slot_c = JS_ToCString(ctx, light_slot);
                if (slot_name_c != NULL && light_slot_c != NULL && strcmp(slot_name_c, light_slot_c) == 0)
                    JS_SetPropertyStr(ctx, light_child, "_cos_assigned_slot", JS_DupValue(ctx, argv[i]));
                if (light_slot_c != NULL) JS_FreeCString(ctx, light_slot_c);
                JS_FreeValue(ctx, light_slot); JS_FreeValue(ctx, light_child);
            }
            JS_FreeValue(ctx, light_len); JS_FreeValue(ctx, light); JS_FreeValue(ctx, slot_name);
            if (slot_name_c != NULL) JS_FreeCString(ctx, slot_name_c);
        }
        if (slot_tag_c != NULL) JS_FreeCString(ctx, slot_tag_c);
        JS_FreeValue(ctx, slot_tag);
    }
    JS_FreeValue(ctx, host);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "childNodes", children);
    return JS_UNDEFINED;
}
static JSValue cos_dom_element_attach_shadow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "attachShadow requires options");
    JSValue mode = JS_GetPropertyStr(ctx, argv[0], "mode"); const char *mode_c = JS_ToCString(ctx, mode);
    if (mode_c == NULL || (strcmp(mode_c, "open") != 0 && strcmp(mode_c, "closed") != 0)) { if (mode_c != NULL) JS_FreeCString(ctx, mode_c); JS_FreeValue(ctx, mode); return JS_ThrowTypeError(ctx, "ShadowRoot mode must be open or closed"); }
    JSValue existing = JS_GetPropertyStr(ctx, this_val, "_cos_shadow_root");
    if (JS_IsObject(existing)) { JS_FreeValue(ctx, existing); JS_FreeCString(ctx, mode_c); JS_FreeValue(ctx, mode); return JS_ThrowTypeError(ctx, "Element already has a shadow root"); }
    JS_FreeValue(ctx, existing);
    JSValue root = JS_NewObject(ctx); JS_SetPropertyStr(ctx, root, "host", JS_DupValue(ctx, this_val)); JS_SetPropertyStr(ctx, root, "mode", JS_NewString(ctx, mode_c));
    JS_SetPropertyStr(ctx, root, "nodeType", JS_NewInt32(ctx, DOM_DOCUMENT_FRAGMENT_NODE));
    JS_SetPropertyStr(ctx, root, "nodeName", JS_NewString(ctx, "#document-fragment"));
    JS_SetPropertyStr(ctx, root, "getRootNode", JS_NewCFunction(ctx, cos_dom_shadow_get_root_node, "getRootNode", 1));
    JSValue delegates_focus = JS_GetPropertyStr(ctx, argv[0], "delegatesFocus");
    JS_SetPropertyStr(ctx, root, "delegatesFocus", JS_NewBool(ctx, JS_ToBool(ctx, delegates_focus)));
    JS_FreeValue(ctx, delegates_focus);
    JS_SetPropertyStr(ctx, root, "childNodes", JS_NewArray(ctx)); JS_SetPropertyStr(ctx, root, "append", JS_NewCFunction(ctx, cos_dom_shadow_append, "append", 0)); JS_SetPropertyStr(ctx, root, "appendChild", JS_NewCFunction(ctx, cos_dom_shadow_append, "appendChild", 1));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_shadow_root", JS_DupValue(ctx, root));
    if (strcmp(mode_c, "closed") == 0) JS_SetPropertyStr(ctx, (JSValue)this_val, "_cos_shadow_closed", JS_TRUE);
    JS_FreeCString(ctx, mode_c); JS_FreeValue(ctx, mode); return root;
}
static JSValue cos_dom_element_get_shadow_root(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv; JSValue closed = JS_GetPropertyStr(ctx, this_val, "_cos_shadow_closed"); bool is_closed = JS_ToBool(ctx, closed); JS_FreeValue(ctx, closed);
    if (is_closed) return JS_NULL; JSValue root = JS_GetPropertyStr(ctx, this_val, "_cos_shadow_root"); return JS_IsObject(root) ? root : (JS_FreeValue(ctx, root), JS_NULL);
}
static JSValue cos_dom_node_get_root_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv; dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    if (node == NULL) return JS_DupValue(ctx, this_val);
    dom_node *current = node; dom_node_ref(current);
    for (unsigned depth = 0; depth < 4096; ++depth) {
        dom_node *parent = NULL; if (dom_node_get_parent_node(current, &parent) != DOM_NO_ERR || parent == NULL) break;
        dom_node_type type = (dom_node_type)0; dom_node_get_node_type(parent, &type);
        if (type == DOM_DOCUMENT_NODE) { dom_node_unref(parent); dom_node_unref(current); JSValue global = JS_GetGlobalObject(ctx); JSValue document = JS_GetPropertyStr(ctx, global, "document"); JS_FreeValue(ctx, global); return document; }
        dom_node_unref(current); current = parent;
    }
    JSValue result = cos_dom_wrap_node(ctx, current); dom_node_unref(current); if (JS_IsNull(result)) return JS_DupValue(ctx, this_val); return result;
}
static JSValue cos_dom_node_get_is_connected(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv; JSValue root = cos_dom_node_get_root_node(ctx, this_val, 0, NULL); JSValue global = JS_GetGlobalObject(ctx); JSValue document = JS_GetPropertyStr(ctx, global, "document"); bool connected = JS_IsStrictEqual(ctx, root, document); JS_FreeValue(ctx, global); JS_FreeValue(ctx, document); JS_FreeValue(ctx, root); return JS_NewBool(ctx, connected);
}
static void cos_dom_install_element_proto(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "getAttribute",
                      JS_NewCFunction(ctx, cos_dom_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "setAttribute",
                      JS_NewCFunction(ctx, cos_dom_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, proto, "hasAttribute",
                      JS_NewCFunction(ctx, cos_dom_element_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "removeAttribute",
                      JS_NewCFunction(ctx, cos_dom_element_remove_attribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "getValue",
                      JS_NewCFunction(ctx, cos_dom_element_get_value, "getValue", 0));
    JS_SetPropertyStr(ctx, proto, "setValue",
                      JS_NewCFunction(ctx, cos_dom_element_set_value, "setValue", 1));
    JS_SetPropertyStr(ctx, proto, "appendChild",
                      JS_NewCFunction(ctx, cos_dom_element_append_child, "appendChild", 1));
    JS_SetPropertyStr(ctx, proto, "removeChild",
                      JS_NewCFunction(ctx, cos_dom_element_remove_child, "removeChild", 1));
    JS_SetPropertyStr(ctx, proto, "insertBefore",
                      JS_NewCFunction(ctx, cos_dom_element_insert_before, "insertBefore", 2));
    JS_SetPropertyStr(ctx, proto, "replaceChild",
                      JS_NewCFunction(ctx, cos_dom_element_replace_child, "replaceChild", 2));
    JS_SetPropertyStr(ctx, proto, "cloneNode",
                      JS_NewCFunction(ctx, cos_dom_node_clone_node, "cloneNode", 1));
    JS_SetPropertyStr(ctx, proto, "attachShadow",
                      JS_NewCFunction(ctx, cos_dom_element_attach_shadow, "attachShadow", 1));
    cos_js_define_readonly_accessor(ctx, proto, "shadowRoot", cos_dom_element_get_shadow_root);
    JS_SetPropertyStr(ctx, proto, "addEventListener",
                      JS_NewCFunction(ctx, cos_dom_node_add_event_listener, "addEventListener", 3));
    JS_SetPropertyStr(ctx, proto, "removeEventListener",
                      JS_NewCFunction(ctx, cos_dom_node_remove_event_listener, "removeEventListener", 3));
    JS_SetPropertyStr(ctx, proto, "dispatchEvent",
                      JS_NewCFunction(ctx, cos_dom_node_dispatch_event, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, proto, "click",
                      JS_NewCFunction(ctx, cos_dom_element_click, "click", 0));
    JS_SetPropertyStr(ctx, proto, "hasChildNodes",
                      JS_NewCFunction(ctx, cos_dom_node_has_child_nodes, "hasChildNodes", 0));
    JS_SetPropertyStr(ctx, proto, "remove",
                      JS_NewCFunction(ctx, cos_dom_element_remove, "remove", 0));
    JS_SetPropertyStr(ctx, proto, "getElementsByClassName",
                      JS_NewCFunction(ctx, cos_dom_element_get_elements_by_class_name,
                                      "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, proto, "toggleAttribute",
                      JS_NewCFunction(ctx, cos_dom_element_toggle_attribute,
                                      "toggleAttribute", 2));
    JS_SetPropertyStr(ctx, proto, "getAttributeNames",
                      JS_NewCFunction(ctx, cos_dom_element_get_attribute_names,
                                      "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, proto, "append",
                      JS_NewCFunction(ctx, cos_dom_element_append_variadic, "append", 0));
    JS_SetPropertyStr(ctx, proto, "prepend",
                      JS_NewCFunction(ctx, cos_dom_element_prepend_variadic, "prepend", 0));
    JS_SetPropertyStr(ctx, proto, "before",
                      JS_NewCFunction(ctx, cos_dom_element_before, "before", 0));
    JS_SetPropertyStr(ctx, proto, "after",
                      JS_NewCFunction(ctx, cos_dom_element_after, "after", 0));
    JS_SetPropertyStr(ctx, proto, "replaceWith",
                      JS_NewCFunction(ctx, cos_dom_element_replace_with, "replaceWith", 0));
    JS_SetPropertyStr(ctx, proto, "matches",
                      JS_NewCFunction(ctx, cos_dom_element_matches, "matches", 1));
    JS_SetPropertyStr(ctx, proto, "closest",
                      JS_NewCFunction(ctx, cos_dom_element_closest, "closest", 1));
    JS_SetPropertyStr(ctx, proto, "contains",
                      JS_NewCFunction(ctx, cos_dom_node_contains, "contains", 1));
    JS_SetPropertyStr(ctx, proto, "hasChildNodes",
                      JS_NewCFunction(ctx, cos_dom_node_has_child_nodes, "hasChildNodes", 0));
    JS_SetPropertyStr(ctx, proto, "assignedNodes",
                      JS_NewCFunction(ctx, cos_dom_element_assigned_nodes, "assignedNodes", 1));
    cos_js_define_readonly_accessor(ctx, proto, "assignedSlot", cos_dom_element_get_assigned_slot);
    JS_SetPropertyStr(ctx, proto, "isSameNode",
                      JS_NewCFunction(ctx, cos_dom_node_is_same_node, "isSameNode", 1));
    JS_SetPropertyStr(ctx, proto, "isEqualNode",
                      JS_NewCFunction(ctx, cos_dom_node_is_equal_node, "isEqualNode", 1));
    JS_SetPropertyStr(ctx, proto, "compareDocumentPosition",
                      JS_NewCFunction(ctx, cos_dom_node_compare_position, "compareDocumentPosition", 1));
    JS_SetPropertyStr(ctx, proto, "normalize",
                      JS_NewCFunction(ctx, cos_dom_node_normalize, "normalize", 0));
    JS_SetPropertyStr(ctx, proto, "getRootNode", JS_NewCFunction(ctx, cos_dom_node_get_root_node, "getRootNode", 1));
    cos_js_define_readonly_accessor(ctx, proto, "isConnected", cos_dom_node_get_is_connected);
    cos_js_define_readonly_accessor(ctx, proto, "ownerDocument", cos_dom_owner_document);
    struct { const char *name; JSCFunction *get; } node_meta[] = {
        { "nodeType", cos_dom_node_get_node_type_value },
        { "nodeName", cos_dom_node_get_node_name_value },
    };
    for (size_t i = 0; i < sizeof(node_meta) / sizeof(node_meta[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, node_meta[i].name);
        JSValue getter = JS_NewCFunction(ctx, node_meta[i].get, node_meta[i].name, 0);
        if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, JS_UNDEFINED,
                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0)
            JS_FreeValue(ctx, getter);
        JS_FreeAtom(ctx, atom);
    }

    /* Web scripts conventionally access form controls through `element.value`,
     * not the bridge's historical getValue()/setValue() helpers.  Bind this as
     * an accessor to the real libdom input/textarea value, retaining the
     * wrapper's node reference and avoiding a copied, stale JS string. The
     * same pattern gives textContent/id/className/parentNode live read/write
     * access to the real tree instead of a one-time snapshot. */
    /* `innerHTML = ...` now creates a bounded real libdom fragment rather
     * than replacing the tree with literal text. The getter remains a textual
     * readback until an HTML serializer is added, but writes are visible DOM
     * mutations and can be selected by later script code. */
    struct { const char *name; JSCFunction *get; JSCFunction *set; } accessors[] = {
        { "value",       cos_dom_element_get_value,      cos_dom_element_set_value },
        { "disabled",    cos_dom_element_get_disabled,   cos_dom_element_set_disabled },
        { "textContent", cos_dom_node_get_text_content,  cos_dom_node_set_text_content },
        { "innerHTML",   cos_dom_element_get_inner_html, cos_dom_element_set_inner_html },
        { "outerHTML",   cos_dom_element_get_outer_html, NULL },
        { "id",          cos_dom_element_get_id,         cos_dom_element_set_id },
        { "className",   cos_dom_element_get_class_name, cos_dom_element_set_class_name },
        { "slot",        cos_dom_element_get_slot,        cos_dom_element_set_slot },
        { "name",        cos_dom_element_get_name,        cos_dom_element_set_name },
        { "parentNode",  cos_dom_node_get_parent_node,   NULL },
    };
    for (size_t i = 0; i < sizeof(accessors) / sizeof(accessors[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, accessors[i].name);
        JSValue getter = JS_NewCFunction(ctx, accessors[i].get, accessors[i].name, 0);
        JSValue setter = accessors[i].set != NULL
            ? JS_NewCFunction(ctx, accessors[i].set, accessors[i].name, 1)
            : JS_UNDEFINED;
        if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter,
                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
            /* JS_DefinePropertyGetSet only takes ownership on success. */
            JS_FreeValue(ctx, getter);
            JS_FreeValue(ctx, setter);
        }
        JS_FreeAtom(ctx, atom);
    }
    cos_dom_define_relative_accessor(ctx, proto, "parentNode", 0);
    cos_dom_define_relative_accessor(ctx, proto, "parentElement", 0);
    cos_dom_define_relative_accessor(ctx, proto, "firstChild", 1);
    cos_dom_define_relative_accessor(ctx, proto, "firstElementChild", 1);
    cos_dom_define_relative_accessor(ctx, proto, "lastElementChild", 2);
    cos_dom_define_relative_accessor(ctx, proto, "previousElementSibling", 3);
    cos_dom_define_relative_accessor(ctx, proto, "nextElementSibling", 4);
    cos_dom_define_relative_accessor(ctx, proto, "lastChild", 2);
    cos_dom_define_relative_accessor(ctx, proto, "previousSibling", 3);
    cos_dom_define_relative_accessor(ctx, proto, "nextSibling", 4);
    cos_dom_define_children_accessor(ctx, proto, "childNodes", 0);
    cos_dom_define_children_accessor(ctx, proto, "children", 1);
    JS_SetClassProto(ctx, cos_dom_element_class_id, proto);
}

/* The Text node proto is deliberately tiny: textContent (shared with
 * elements above) and parentNode cover every realistic use of a
 * createTextNode() result - building it up, appending it, and later finding
 * its way back to the element that holds it. */
static void cos_dom_install_characterdata_proto(JSContext *ctx, JSClassID class_id, bool allow_split)
{
    JSValue proto = JS_NewObject(ctx);
    JSAtom node_type_atom = JS_NewAtom(ctx, "nodeType");
    JSValue node_type_getter = JS_NewCFunction(ctx, cos_dom_node_get_node_type_value, "nodeType", 0);
    JS_DefinePropertyGetSet(ctx, proto, node_type_atom, node_type_getter, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_type_atom);
    JSAtom node_name_atom = JS_NewAtom(ctx, "nodeName");
    JSValue node_name_getter = JS_NewCFunction(ctx, cos_dom_node_get_node_name_value, "nodeName", 0);
    JS_DefinePropertyGetSet(ctx, proto, node_name_atom, node_name_getter, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_name_atom);
    struct { const char *name; JSCFunction *get; JSCFunction *set; } accessors[] = {
        { "textContent", cos_dom_node_get_text_content, cos_dom_node_set_text_content },
        { "nodeValue",   cos_dom_node_get_text_content, cos_dom_node_set_text_content },
        { "data",        cos_dom_node_get_text_content, cos_dom_node_set_text_content },
        { "length",      cos_dom_characterdata_get_length, NULL },
        { "parentNode",  cos_dom_node_get_parent_node,  NULL },
    };
    for (size_t i = 0; i < sizeof(accessors) / sizeof(accessors[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, accessors[i].name);
        JSValue getter = JS_NewCFunction(ctx, accessors[i].get, accessors[i].name, 0);
        JSValue setter = accessors[i].set != NULL
            ? JS_NewCFunction(ctx, accessors[i].set, accessors[i].name, 1)
            : JS_UNDEFINED;
        if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter,
                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, getter);
            JS_FreeValue(ctx, setter);
        }
        JS_FreeAtom(ctx, atom);
    }
    cos_dom_define_relative_accessor(ctx, proto, "parentNode", 0);
    cos_dom_define_relative_accessor(ctx, proto, "previousSibling", 3);
    cos_dom_define_relative_accessor(ctx, proto, "nextSibling", 4);
    JS_SetPropertyStr(ctx, proto, "substringData",
                      JS_NewCFunction(ctx, cos_dom_characterdata_substring_data, "substringData", 2));
    JS_SetPropertyStr(ctx, proto, "appendData",
                      JS_NewCFunction(ctx, cos_dom_characterdata_append_data, "appendData", 1));
    JS_SetPropertyStr(ctx, proto, "insertData",
                      JS_NewCFunction(ctx, cos_dom_characterdata_insert_data, "insertData", 2));
    JS_SetPropertyStr(ctx, proto, "deleteData",
                      JS_NewCFunction(ctx, cos_dom_characterdata_delete_data, "deleteData", 2));
    JS_SetPropertyStr(ctx, proto, "replaceData",
                      JS_NewCFunction(ctx, cos_dom_characterdata_replace_data, "replaceData", 3));
    if (allow_split) {
        JS_SetPropertyStr(ctx, proto, "splitText",
                          JS_NewCFunction(ctx, cos_dom_text_split_text, "splitText", 1));
    }
    JS_SetClassProto(ctx, class_id, proto);
}

static void cos_dom_install_fragment_proto(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "appendChild",
                      JS_NewCFunction(ctx, cos_dom_element_append_child, "appendChild", 1));
    JS_SetPropertyStr(ctx, proto, "removeChild",
                      JS_NewCFunction(ctx, cos_dom_element_remove_child, "removeChild", 1));
    JS_SetPropertyStr(ctx, proto, "insertBefore",
                      JS_NewCFunction(ctx, cos_dom_element_insert_before, "insertBefore", 2));
    JS_SetPropertyStr(ctx, proto, "replaceChild",
                      JS_NewCFunction(ctx, cos_dom_element_replace_child, "replaceChild", 2));
    JS_SetPropertyStr(ctx, proto, "cloneNode",
                      JS_NewCFunction(ctx, cos_dom_node_clone_node, "cloneNode", 1));
    JS_SetPropertyStr(ctx, proto, "addEventListener",
                      JS_NewCFunction(ctx, cos_dom_node_add_event_listener, "addEventListener", 3));
    JS_SetPropertyStr(ctx, proto, "removeEventListener",
                      JS_NewCFunction(ctx, cos_dom_node_remove_event_listener, "removeEventListener", 3));
    JS_SetPropertyStr(ctx, proto, "dispatchEvent",
                      JS_NewCFunction(ctx, cos_dom_node_dispatch_event, "dispatchEvent", 1));
    struct { const char *name; JSCFunction *get; JSCFunction *set; } accessors[] = {
        { "textContent", cos_dom_node_get_text_content, cos_dom_node_set_text_content },
        { "parentNode",  cos_dom_node_get_parent_node,  NULL },
    };
    for (size_t i = 0; i < sizeof(accessors) / sizeof(accessors[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, accessors[i].name);
        JSValue getter = JS_NewCFunction(ctx, accessors[i].get, accessors[i].name, 0);
        JSValue setter = accessors[i].set != NULL
            ? JS_NewCFunction(ctx, accessors[i].set, accessors[i].name, 1)
            : JS_UNDEFINED;
        if (JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter,
                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, getter); JS_FreeValue(ctx, setter);
        }
        JS_FreeAtom(ctx, atom);
    }
    cos_dom_define_relative_accessor(ctx, proto, "firstChild", 1);
    cos_dom_define_relative_accessor(ctx, proto, "lastChild", 2);
    cos_dom_define_children_accessor(ctx, proto, "childNodes", 0);
    cos_dom_define_children_accessor(ctx, proto, "children", 1);
    JS_SetClassProto(ctx, cos_dom_fragment_class_id, proto);
}

/* ---- Allocator ---- */

static void* cos_qjs_calloc(void* opaque, size_t count, size_t size) {
    (void)opaque;
    if (count != 0 && size > (size_t)-1 / count) return NULL;
    size_t total = count * size;
    void* p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}
static void* cos_qjs_malloc(void* opaque, size_t size) {
    (void)opaque;
    return kmalloc(size);
}
static void cos_qjs_free(void* opaque, void* ptr) {
    (void)opaque;
    kfree(ptr);
}
static void* cos_qjs_realloc(void* opaque, void* ptr, size_t size) {
    (void)opaque;
    return krealloc(ptr, size);
}
static size_t cos_qjs_malloc_usable_size(const void* ptr) {
    /* QuickJS only uses this for its own memory-usage accounting
     * (JS_ComputeMemoryUsage) - kmalloc doesn't expose a "how big was
     * this block really" query, and 0 is the documented "don't know"
     * value the engine handles correctly. */
    (void)ptr;
    return 0;
}

static const JSMallocFunctions cos_qjs_malloc_funcs = {
    cos_qjs_calloc,
    cos_qjs_malloc,
    cos_qjs_free,
    cos_qjs_realloc,
    cos_qjs_malloc_usable_size,
};

/* ---- console object ---- */

/* Helper: stringify up to `argc` JS values separated by spaces. */
static void cos_js_console_log_impl(JSContext *ctx, int argc, JSValueConst *argv,
                                     const char *prefix)
{
    serial_puts(prefix);
    for (int i = 0; i < argc; i++) {
        if (i > 0) serial_puts(" ");
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            serial_puts(s);
            JS_FreeCString(ctx, s);
        } else {
            serial_puts("[?]");
        }
    }
    serial_puts("\n");
}

static JSValue cos_console_log(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    cos_js_console_log_impl(ctx, argc, argv, "[JS/console.log] ");
    return JS_UNDEFINED;
}
static JSValue cos_console_warn(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    cos_js_console_log_impl(ctx, argc, argv, "[JS/console.warn] ");
    return JS_UNDEFINED;
}
static JSValue cos_console_error(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    cos_js_console_log_impl(ctx, argc, argv, "[JS/console.error] ");
    return JS_UNDEFINED;
}
static JSValue cos_console_info(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    cos_js_console_log_impl(ctx, argc, argv, "[JS/console.info] ");
    return JS_UNDEFINED;
}

static void cos_js_install_console(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, cos_console_log,   "log",   1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, cos_console_warn,  "warn",  1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, cos_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, cos_console_info,  "info",  1));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ---- Compact browser compatibility globals ---- */
/* NetSurf owns the authoritative libdom tree and event dispatch.  These
 * objects deliberately provide a benign browser-shaped surface to external
 * scripts that only perform feature detection or schedule background work.
 * They keep a missing DOM API from aborting parsing/layout, while forms,
 * links and rendering remain on NetSurf's standard native path. */
static JSValue cos_web_noop(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue cos_web_return_null(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NULL;
}

static JSValue cos_web_return_empty_array(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

/* Read-only bridge for the most common Wikipedia bootstrap query.  The
 * document pointer is page-local and supplied through JS context opaque. */
static JSValue cos_dom_get_element_by_id(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc == NULL) return JS_NULL;

    const char *id_c = JS_ToCString(ctx, argv[0]);
    if (id_c == NULL) return JS_NULL;
    dom_string *id = NULL;
    dom_element *element = NULL;
    JSValue result = JS_NULL;
    if (dom_string_create((const uint8_t *)id_c, strlen(id_c), &id) == DOM_NO_ERR &&
        dom_document_get_element_by_id(doc, id, &element) == DOM_NO_ERR &&
        element != NULL) {
        JSValue obj = cos_dom_wrap_element(ctx, element);
        if (JS_IsException(obj)) {
            dom_node_unref((dom_node *)element);
            if (id != NULL) dom_string_unref(id);
            JS_FreeCString(ctx, id_c);
            return obj;
        }
        result = obj;
        dom_node_unref((dom_node *)element);
    }
    if (id != NULL) dom_string_unref(id);
    JS_FreeCString(ctx, id_c);
    return result;
}

static JSValue cos_dom_get_elements_by_tag_name(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv);

static bool cos_dom_selector_object_matches(JSContext *ctx, JSValueConst object,
                                             const char *class_name,
                                             const char *attr_name,
                                             const char *attr_value)
{
    if (JS_IsNull(object) || JS_IsUndefined(object)) return false;
    if (class_name && class_name[0]) {
        JSValue classes = JS_GetPropertyStr(ctx, object, "className");
        const char *text = JS_ToCString(ctx, classes);
        bool ok = cos_dom_class_contains(text, class_name);
        if (text) JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, classes);
        if (!ok) return false;
    }
    if (attr_name && attr_name[0]) {
        JSValue getter = JS_GetPropertyStr(ctx, object, "getAttribute");
        JSValue name = JS_NewString(ctx, attr_name);
        JSValue value = JS_Call(ctx, getter, object, 1, &name);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, getter);
        if (JS_IsException(value) || JS_IsNull(value) || JS_IsUndefined(value)) {
            JS_FreeValue(ctx, value);
            return false;
        }
        const char *text = JS_ToCString(ctx, value);
        bool ok = text != NULL && (!attr_value || strcmp(text, attr_value) == 0);
        if (text) JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, value);
        if (!ok) return false;
    }
    return true;
}

static void cos_dom_selector_parts(const char *selector, char *tag, size_t tag_size,
                                   char *class_name, size_t class_size,
                                   char *attr_name, size_t attr_size,
                                   char *attr_value, size_t value_size)
{
    if (!tag || !class_name || !attr_name || !attr_value) return;
    tag[0] = class_name[0] = attr_name[0] = attr_value[0] = '\0';
    if (!selector) return;
    const char *p = selector;
    while (*p == ' ' || *p == '\t') ++p;
    size_t n = 0;
    while (*p && *p != '.' && *p != '[' && *p != ' ' && *p != '\t' && n + 1 < tag_size)
        tag[n++] = *p++;
    tag[n] = '\0';
    if (!tag[0]) { tag[0] = '*'; tag[1] = '\0'; }
    if (*p == '.') {
        ++p; n = 0;
        while (*p && *p != '[' && *p != ' ' && *p != '\t' && n + 1 < class_size)
            class_name[n++] = *p++;
        class_name[n] = '\0';
    }
    if (*p == '[') {
        ++p; n = 0;
        while (*p && *p != ']' && *p != '=' && n + 1 < attr_size)
            attr_name[n++] = *p++;
        while (n && (attr_name[n-1] == ' ' || attr_name[n-1] == '\t')) --n;
        attr_name[n] = '\0';
        if (*p == '=') {
            ++p; while (*p == ' ' || *p == '\t' || *p == '\"' || *p == '\'') ++p;
            n = 0;
            while (*p && *p != ']' && *p != '\"' && *p != '\'' && n + 1 < value_size)
                attr_value[n++] = *p++;
            while (n && (attr_value[n-1] == ' ' || attr_value[n-1] == '\t')) --n;
            attr_value[n] = '\0';
        }
    }
}

static JSValue cos_dom_query_selector(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    if (selector[0] == '#' && selector[1]) {
        JSValue id = JS_NewString(ctx, selector + 1);
        JSValue result = cos_dom_get_element_by_id(ctx, JS_UNDEFINED, 1, &id);
        JS_FreeValue(ctx, id); JS_FreeCString(ctx, selector); return result;
    }
    char tag[96], class_name[128], attr_name[96], attr_value[192];
    cos_dom_selector_parts(selector, tag, sizeof(tag), class_name, sizeof(class_name),
                           attr_name, sizeof(attr_name), attr_value, sizeof(attr_value));
    JSValue tag_arg = JS_NewString(ctx, tag);
    JSValue matches = cos_dom_get_elements_by_tag_name(ctx, JS_UNDEFINED, 1, &tag_arg);
    JS_FreeValue(ctx, tag_arg);
    JSValue result = JS_NULL;
    int64_t length64 = 0;
    (void)JS_GetLength(ctx, matches, &length64);
    uint32_t length = length64 > 0 ? (uint32_t)length64 : 0;
    for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, matches, i);
        if (cos_dom_selector_object_matches(ctx, item, class_name, attr_name, attr_value[0] ? attr_value : NULL)) {
            result = item; break;
        }
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, matches);
    JS_FreeCString(ctx, selector);
    return result;
}

/* Return a snapshot array of real libdom elements. This intentionally accepts
 * tag-name queries and `#id` only: they cover common bootstrap code while
 * avoiding a partial CSS selector parser that could silently mis-select nodes. */
static JSValue cos_dom_get_elements_by_tag_name(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue result = JS_NewArray(ctx);
    if (argc < 1 || !JS_IsString(argv[0])) return result;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc == NULL) return result;

    const char *tag_c = JS_ToCString(ctx, argv[0]);
    if (tag_c == NULL) return result;
    dom_string *tag = NULL;
    dom_nodelist *list = NULL;
    uint32_t length = 0;
    if (dom_string_create((const uint8_t *)tag_c, strlen(tag_c), &tag) == DOM_NO_ERR &&
        dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR &&
        list != NULL) {
        (void)dom_nodelist_get_length(list, &length);
        /* Bound one hostile document's collection expansion without changing
         * normal page behavior. */
        if (length > 4096) length = 4096;
        for (uint32_t i = 0; i < length; ++i) {
            dom_node *node = NULL;
            if (dom_nodelist_item(list, i, &node) == DOM_NO_ERR && node != NULL) {
                JSValue element = cos_dom_wrap_element(ctx, (dom_element *)node);
                if (JS_IsException(element)) {
                    dom_node_unref(node);
                    break;
                }
                JS_SetPropertyUint32(ctx, result, i, element);
                dom_node_unref(node);
            }
        }
        dom_nodelist_unref(list);
    }
    if (tag != NULL) dom_string_unref(tag);
    JS_FreeCString(ctx, tag_c);
    return result;
}

static JSValue cos_dom_query_selector_all(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue result = JS_NewArray(ctx);
    if (argc < 1 || !JS_IsString(argv[0])) return result;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    /* Selector groups are routine on normal pages (`.num, .op`).  Split only
     * on top-level commas so attribute values remain intact, and merge the
     * ordinary snapshot results without duplicate wrappers. */
    if (strchr(selector, ',') != NULL) {
        uint32_t out = 0;
        const char *part_start = selector;
        int bracket_depth = 0;
        char quote = '\0';
        for (const char *p = selector; ; ++p) {
            char c = *p;
            if (quote) { if (c == quote) quote = '\0'; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '[') ++bracket_depth;
            else if (c == ']' && bracket_depth > 0) --bracket_depth;
            if ((c == ',' && quote == '\0' && bracket_depth == 0) || c == '\0') {
                size_t n = (size_t)(p - part_start);
                if (n > 0 && n < 256) {
                    char part[256];
                    memcpy(part, part_start, n); part[n] = '\0';
                    JSValue part_arg = JS_NewString(ctx, part);
                    JSValue group = cos_dom_query_selector_all(ctx, this_val, 1, &part_arg);
                    JS_FreeValue(ctx, part_arg);
                    int64_t group_len64 = 0;
                    (void)JS_GetLength(ctx, group, &group_len64);
                    uint32_t group_len = group_len64 > 0 ? (uint32_t)group_len64 : 0;
                    for (uint32_t i = 0; i < group_len && out < 4096u; ++i) {
                        JSValue item = JS_GetPropertyUint32(ctx, group, i);
                        bool duplicate = false;
                        for (uint32_t j = 0; j < out; ++j) {
                            JSValue prior = JS_GetPropertyUint32(ctx, result, j);
                            duplicate = JS_IsStrictEqual(ctx, item, prior);
                            JS_FreeValue(ctx, prior);
                            if (duplicate) break;
                        }
                        if (!duplicate) JS_SetPropertyUint32(ctx, result, out++, item);
                        else JS_FreeValue(ctx, item);
                    }
                    JS_FreeValue(ctx, group);
                }
                if (c == '\0') break;
                part_start = p + 1;
            }
        }
        JS_FreeCString(ctx, selector);
        return result;
    }
    if (selector[0] == '#' && selector[1]) {
        JSValue id = JS_NewString(ctx, selector + 1);
        JSValue node = cos_dom_get_element_by_id(ctx, JS_UNDEFINED, 1, &id);
        JS_FreeValue(ctx, id);
        if (!JS_IsNull(node) && !JS_IsException(node)) JS_SetPropertyUint32(ctx, result, 0, node);
        else JS_FreeValue(ctx, node);
        JS_FreeCString(ctx, selector); return result;
    }
    char tag[96], class_name[128], attr_name[96], attr_value[192];
    cos_dom_selector_parts(selector, tag, sizeof(tag), class_name, sizeof(class_name),
                           attr_name, sizeof(attr_name), attr_value, sizeof(attr_value));
    JSValue tag_arg = JS_NewString(ctx, tag);
    JSValue matches = cos_dom_get_elements_by_tag_name(ctx, JS_UNDEFINED, 1, &tag_arg);
    JS_FreeValue(ctx, tag_arg);
    int64_t length64 = 0;
    (void)JS_GetLength(ctx, matches, &length64);
    uint32_t length = length64 > 0 ? (uint32_t)length64 : 0;
    uint32_t out = 0;
    for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, matches, i);
        if (cos_dom_selector_object_matches(ctx, item, class_name, attr_name, attr_value[0] ? attr_value : NULL))
            JS_SetPropertyUint32(ctx, result, out++, item);
        else JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, matches);
    JS_FreeCString(ctx, selector);
    return result;
}

/* ---- Bounded owner-thread web timers ---- */

#define COS_JS_TIMER_MAX 64
#define COS_JS_TIMER_MAX_DELAY_MS 60000ULL
#define COS_JS_TIMER_FRAME_MS 16ULL
#define COS_JS_TIMER_PUMP_BUDGET 8

typedef struct {
    JSContext *ctx;
    JSValue callback;
    uint64_t due_tick;
    uint64_t interval_tick;
    uint32_t id;
    bool active;
    bool repeating;
    bool animation_frame;
} cos_js_timer_slot_t;

static cos_js_timer_slot_t cos_js_timers[COS_JS_TIMER_MAX];
static uint32_t cos_js_next_timer_id = 1;

static void cos_js_report_pending_exception(JSContext *ctx);

static uint32_t cos_js_timer_allocate_id(void)
{
    uint32_t id = cos_js_next_timer_id++;
    if (id == 0) id = cos_js_next_timer_id++;
    return id;
}

static JSValue cos_web_schedule_timer(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv,
                                      bool repeating, bool animation_frame)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "timer callback must be a function");
    }

    uint64_t delay = animation_frame ? COS_JS_TIMER_FRAME_MS : 1;
    if (!animation_frame && argc >= 2) {
        int64_t requested = 0;
        if (JS_ToInt64(ctx, &requested, argv[1]) < 0) return JS_EXCEPTION;
        if (requested > 0) delay = (uint64_t)requested;
    }
    if (delay > COS_JS_TIMER_MAX_DELAY_MS) delay = COS_JS_TIMER_MAX_DELAY_MS;

    for (unsigned int i = 0; i < COS_JS_TIMER_MAX; ++i) {
        cos_js_timer_slot_t *slot = &cos_js_timers[i];
        if (slot->active) continue;
        slot->ctx = ctx;
        slot->callback = JS_DupValue(ctx, argv[0]);
        slot->due_tick = get_timer_ticks() + delay;
        slot->interval_tick = repeating ? delay : 0;
        slot->id = cos_js_timer_allocate_id();
        slot->active = true;
        slot->repeating = repeating;
        slot->animation_frame = animation_frame;
        return JS_NewUint32(ctx, slot->id);
    }

    /* A saturated queue is a normal resource limit, not a kernel allocation
     * failure. Returning zero mirrors browser APIs that cannot allocate an id
     * while preserving the GUI thread's bounded frame time. */
    return JS_NewInt32(ctx, 0);
}

static JSValue cos_web_queue_microtask(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    /* The browser bridge has no separate microtask queue yet. Reuse the
     * owner-thread timer queue with the minimum bounded delay; this preserves
     * callback ordering and avoids executing page code on a network thread. */
    return cos_web_schedule_timer(ctx, this_val, argc, argv, false, false);
}

static JSValue cos_web_set_timeout(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return cos_web_schedule_timer(ctx, this_val, argc, argv, false, false);
}

static JSValue cos_web_set_interval(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return cos_web_schedule_timer(ctx, this_val, argc, argv, true, false);
}

static JSValue cos_web_request_animation_frame(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    return cos_web_schedule_timer(ctx, this_val, argc, argv, false, true);
}

static JSValue cos_web_clear_timer(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
    for (unsigned int i = 0; i < COS_JS_TIMER_MAX; ++i) {
        cos_js_timer_slot_t *slot = &cos_js_timers[i];
        if (slot->active && slot->ctx == ctx && slot->id == id) {
            JS_FreeValue(ctx, slot->callback);
            slot->callback = JS_UNDEFINED;
            slot->active = false;
            break;
        }
    }
    return JS_UNDEFINED;
}

void cos_js_cancel_context_timers(JSContext *ctx)
{
    if (ctx == NULL) return;
    for (unsigned int i = 0; i < COS_JS_TIMER_MAX; ++i) {
        cos_js_timer_slot_t *slot = &cos_js_timers[i];
        if (slot->active && slot->ctx == ctx) {
            JS_FreeValue(ctx, slot->callback);
            slot->callback = JS_UNDEFINED;
            slot->active = false;
        }
    }
}

void cos_js_pump_timers(void)
{
    const uint64_t now = get_timer_ticks();
    unsigned int dispatched = 0;

    for (unsigned int i = 0;
         i < COS_JS_TIMER_MAX && dispatched < COS_JS_TIMER_PUMP_BUDGET;
         ++i) {
        cos_js_timer_slot_t *slot = &cos_js_timers[i];
        if (!slot->active || now < slot->due_tick) continue;

        JSContext *ctx = slot->ctx;
        JSValue callback = JS_DupValue(ctx, slot->callback);
        const bool animation_frame = slot->animation_frame;
        if (slot->repeating) {
            /* Do not catch up an unbounded number of missed intervals. One
             * callback per GUI pass protects browser responsiveness. */
            slot->due_tick = now + slot->interval_tick;
        } else {
            JS_FreeValue(ctx, slot->callback);
            slot->callback = JS_UNDEFINED;
            slot->active = false;
        }

        JSValue result;
        JS_UpdateStackTop(JS_GetRuntime(ctx));
        if (animation_frame) {
            JSValue arg = JS_NewFloat64(ctx, (double)now);
            result = JS_Call(ctx, callback, JS_UNDEFINED, 1, (JSValueConst *)&arg);
            JS_FreeValue(ctx, arg);
        } else {
            result = JS_Call(ctx, callback, JS_UNDEFINED, 0, NULL);
        }
        if (JS_IsException(result)) cos_js_report_pending_exception(ctx);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, callback);
        ++dispatched;
    }

    if (dispatched == COS_JS_TIMER_PUMP_BUDGET) {
        serial_puts("[QJS] timer callback batch capped\n");
    }
}

void cos_js_pump_pending_jobs(void)
{
    JSContext *active = cos_js_active_page_context;
    if (active == NULL) return;
    JSRuntime *rt = JS_GetRuntime(active);
    if (rt == NULL) return;

    /* Execute all pending microtasks without artificial batch limits.
     * Promise chains and async/await continuations drain completely. */
    for (;;) {
        JSContext *job_ctx = NULL;
        int rc = JS_ExecutePendingJob(rt, &job_ctx);
        if (rc <= 0) {
            if (rc < 0 && job_ctx != NULL) cos_js_report_pending_exception(job_ctx);
            break;
        }
    }
}

static JSValue cos_web_event_composed_path(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);

static JSValue cos_web_make_event(JSContext *ctx, int argc, JSValueConst *argv,
                                  bool custom)
{
    JSValue event = JS_NewObject(ctx);
    if (argc > 0 && !JS_IsUndefined(argv[0]))
        JS_SetPropertyStr(ctx, event, "type", JS_ToString(ctx, argv[0]));
    else
        JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, event, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "target", JS_NULL);
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "composedPath",
                      JS_NewCFunction(ctx, cos_web_event_composed_path, "composedPath", 0));
    if (custom) JS_SetPropertyStr(ctx, event, "detail", JS_NULL);
    JS_SetPropertyStr(ctx, event, "screenX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "screenY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "clientX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "clientY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "button", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "buttons", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "key", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, event, "code", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, event, "which", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "data", JS_NULL);
    JS_SetPropertyStr(ctx, event, "inputType", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, event, "relatedTarget", JS_NULL);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue bubbles = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        JSValue cancelable = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        JSValue detail = JS_GetPropertyStr(ctx, argv[1], "detail");
        if (!JS_IsUndefined(bubbles)) JS_SetPropertyStr(ctx, event, "bubbles", bubbles); else JS_FreeValue(ctx, bubbles);
        if (!JS_IsUndefined(cancelable)) JS_SetPropertyStr(ctx, event, "cancelable", cancelable); else JS_FreeValue(ctx, cancelable);
        if (custom && !JS_IsUndefined(detail)) JS_SetPropertyStr(ctx, event, "detail", detail); else JS_FreeValue(ctx, detail);
        const char *event_fields[] = { "screenX", "screenY", "clientX", "clientY", "button", "buttons", "key", "code", "which", "data", "inputType", "relatedTarget" };
        for (unsigned int i = 0; i < sizeof(event_fields) / sizeof(event_fields[0]); ++i) {
            JSValue field = JS_GetPropertyStr(ctx, argv[1], event_fields[i]);
            if (!JS_IsUndefined(field)) JS_SetPropertyStr(ctx, event, event_fields[i], field); else JS_FreeValue(ctx, field);
        }
    }
    return event;
}
static JSValue cos_web_event_composed_path(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue path = JS_NewArray(ctx);
    JSValue target = JS_GetPropertyStr(ctx, this_val, "target");
    if (!JS_IsException(target) && !JS_IsNull(target) && !JS_IsUndefined(target))
        JS_SetPropertyUint32(ctx, path, 0, target);
    else
        JS_FreeValue(ctx, target);
    return path;
}

static JSValue cos_web_event_constructor(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return cos_web_make_event(ctx, argc, argv, false);
}
static JSValue cos_web_custom_event_constructor(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    return cos_web_make_event(ctx, argc, argv, true);
}

static JSValue cos_web_performance_now(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* PIT ticks are configured at 1kHz, so this is a monotonic millisecond
     * clock.  Returning a real changing value avoids feature-detection and
     * animation bootstrap failures caused by the previous constant zero. */
    return JS_NewFloat64(ctx, (double)get_timer_ticks());
}

static JSValue cos_web_match_media(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue media = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, media, "matches", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, media, "addListener",
                      JS_NewCFunction(ctx, cos_web_noop, "addListener", 1));
    JS_SetPropertyStr(ctx, media, "removeListener",
                      JS_NewCFunction(ctx, cos_web_noop, "removeListener", 1));
    JS_SetPropertyStr(ctx, media, "addEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, media, "removeEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "removeEventListener", 2));
    return media;
}

static JSValue cos_web_new_element(JSContext *ctx)
{
    JSValue element = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, element, "style", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, element, "className", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, element, "id", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, element, "value", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, element, "textContent", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, element, "innerHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, element, "appendChild",
                      JS_NewCFunction(ctx, cos_web_noop, "appendChild", 1));
    JS_SetPropertyStr(ctx, element, "removeChild",
                      JS_NewCFunction(ctx, cos_web_noop, "removeChild", 1));
    JS_SetPropertyStr(ctx, element, "setAttribute",
                      JS_NewCFunction(ctx, cos_web_noop, "setAttribute", 2));
    JS_SetPropertyStr(ctx, element, "getAttribute",
                      JS_NewCFunction(ctx, cos_web_return_null, "getAttribute", 1));
    JS_SetPropertyStr(ctx, element, "addEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, element, "removeEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, element, "querySelector",
                      JS_NewCFunction(ctx, cos_web_return_null, "querySelector", 1));
    JS_SetPropertyStr(ctx, element, "querySelectorAll",
                      JS_NewCFunction(ctx, cos_web_return_empty_array, "querySelectorAll", 1));
    return element;
}

/* Before this fix, createElement() always returned an inert placeholder
 * (cos_web_new_element(): plain object, noop appendChild/setAttribute, ...)
 * with no libdom node behind it at all - so a script-built element could
 * never actually be inserted into the page, no matter how it was used.
 * When a document is bound, this now creates a genuine libdom element via
 * the same path NetSurf's own parser uses, wrapped through the real element
 * class so setAttribute/appendChild/textContent etc. all work on it. */
static JSValue cos_web_create_element(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc != NULL && argc >= 1 && JS_IsString(argv[0])) {
        const char *tag_c = JS_ToCString(ctx, argv[0]);
        if (tag_c == NULL) return JS_EXCEPTION;
        dom_string *tag = NULL;
        dom_element *element = NULL;
        JSValue result = JS_NULL;
        if (dom_string_create((const uint8_t *)tag_c, strlen(tag_c), &tag) == DOM_NO_ERR &&
            dom_document_create_element(doc, tag, &element) == DOM_NO_ERR &&
            element != NULL) {
            result = cos_dom_wrap_element(ctx, element);
            dom_node_unref((dom_node *)element);
        }
        if (tag != NULL) dom_string_unref(tag);
        JS_FreeCString(ctx, tag_c);
        /* Real element (or a genuine JS exception to propagate) - return it.
         * JS_EXCEPTION is never JS_NULL, so an exception from wrapping still
         * takes this path correctly. */
        if (!JS_IsNull(result)) return result;
        /* Real creation failed (invalid tag name, OOM, ...) - fall through to
         * the inert placeholder rather than surface a hard failure for what
         * scripts generally treat as an always-succeeds call. */
    }
    /* No document bound yet (script running before the first element has
     * been parsed) - an inert placeholder keeps such scripts from throwing,
     * matching this bridge's existing fail-soft philosophy elsewhere. */
    return cos_web_new_element(ctx);
}

static JSValue cos_web_create_element_ns(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createElementNS requires namespace and qualifiedName");
    return cos_web_create_element(ctx, this_val, 1, &argv[1]);
}

static JSValue cos_web_normalize_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc != NULL && dom_node_normalize((dom_node *)doc) != DOM_NO_ERR) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* Document is exposed as a JS host object rather than through the element
 * wrapper class, so the generic libdom-backed Node method cannot be installed
 * on it.  The document nevertheless participates in the Node API; report
 * whether its live documentElement exists, which is equivalent to the
 * document having a child in the parsed HTML document used by this bridge. */
static JSValue cos_dom_document_has_child_nodes(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue element = JS_GetPropertyStr(ctx, this_val, "documentElement");
    bool has_child = JS_IsObject(element);
    JS_FreeValue(ctx, element);
    return JS_NewBool(ctx, has_child);
}

static JSValue cos_web_import_node(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    dom_node *source = argc > 0 ? cos_dom_unwrap_any_node(ctx, argv[0]) : NULL;
    dom_node *copy = NULL;
    bool deep = argc > 1 ? JS_ToBool(ctx, argv[1]) : false;
    if (doc == NULL || source == NULL || dom_document_import_node(doc, source,
            deep, &copy) != DOM_NO_ERR || copy == NULL)
        return JS_EXCEPTION;
    JSValue result = cos_dom_wrap_node(ctx, copy);
    dom_node_unref(copy);
    return result;
}

static JSValue cos_web_adopt_node(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    dom_node *source = argc > 0 ? cos_dom_unwrap_any_node(ctx, argv[0]) : NULL;
    dom_node *adopted = NULL;
    if (doc == NULL || source == NULL || dom_document_adopt_node(doc, source, &adopted) != DOM_NO_ERR || adopted == NULL)
        return JS_EXCEPTION;
    JSValue result = cos_dom_wrap_node(ctx, adopted);
    dom_node_unref(adopted);
    return result;
}

static JSValue cos_dom_characterdata_get_length(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); dom_ulong length = 0;
    if (node == NULL || dom_characterdata_get_length((dom_characterdata *)node, &length) != DOM_NO_ERR)
        return JS_NewInt32(ctx, 0);
    return JS_NewUint32(ctx, length);
}
static JSValue cos_dom_characterdata_substring_data(JSContext *ctx, JSValueConst this_val,
                                                     int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); uint32_t offset = 0, count = 0;
    if (node == NULL || argc < 2 || JS_ToUint32(ctx, &offset, argv[0]) < 0 || JS_ToUint32(ctx, &count, argv[1]) < 0)
        return JS_EXCEPTION;
    dom_string *data = NULL;
    if (dom_characterdata_substring_data((dom_characterdata *)node, offset, count, &data) != DOM_NO_ERR || data == NULL)
        return JS_ThrowDOMException(ctx, "IndexSizeError", "Invalid CharacterData range");
    JSValue result = JS_NewStringLen(ctx, dom_string_data(data), dom_string_byte_length(data));
    dom_string_unref(data); return result;
}
static JSValue cos_dom_characterdata_make_string(JSContext *ctx, JSValueConst value, dom_string **out)
{
    const char *text = JS_ToCString(ctx, value);
    if (text == NULL) return JS_EXCEPTION;
    dom_exception err = dom_string_create((const uint8_t *)text, strlen(text), out);
    JS_FreeCString(ctx, text);
    return err == DOM_NO_ERR ? JS_UNDEFINED : JS_EXCEPTION;
}
static JSValue cos_dom_characterdata_append_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); dom_string *data = NULL;
    if (node == NULL || argc < 1 || JS_IsException(cos_dom_characterdata_make_string(ctx, argv[0], &data))) return JS_EXCEPTION;
    cos_dom_snapshot_character_data(node);
    dom_exception err = dom_characterdata_append_data((dom_characterdata *)node, data); if (data != NULL) dom_string_unref(data);
    if (err != DOM_NO_ERR) return JS_ThrowDOMException(ctx, "IndexSizeError", "CharacterData mutation failed");
    cos_dom_notify_character_data_mutation(); return JS_UNDEFINED;
}
static JSValue cos_dom_characterdata_insert_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); uint32_t offset = 0; dom_string *data = NULL;
    if (node == NULL || argc < 2 || JS_ToUint32(ctx, &offset, argv[0]) < 0 || JS_IsException(cos_dom_characterdata_make_string(ctx, argv[1], &data))) return JS_EXCEPTION;
    cos_dom_snapshot_character_data(node);
    dom_exception err = dom_characterdata_insert_data((dom_characterdata *)node, offset, data); if (data != NULL) dom_string_unref(data);
    if (err != DOM_NO_ERR) return JS_ThrowDOMException(ctx, "IndexSizeError", "CharacterData mutation failed");
    cos_dom_notify_character_data_mutation(); return JS_UNDEFINED;
}
static JSValue cos_dom_characterdata_delete_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); uint32_t offset = 0, count = 0;
    if (node == NULL || argc < 2 || JS_ToUint32(ctx, &offset, argv[0]) < 0 || JS_ToUint32(ctx, &count, argv[1]) < 0) return JS_EXCEPTION;
    cos_dom_snapshot_character_data(node);
    dom_exception err = dom_characterdata_delete_data((dom_characterdata *)node, offset, count);
    if (err != DOM_NO_ERR) return JS_ThrowDOMException(ctx, "IndexSizeError", "CharacterData mutation failed");
    cos_dom_notify_character_data_mutation(); return JS_UNDEFINED;
}
static JSValue cos_dom_characterdata_replace_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val); uint32_t offset = 0, count = 0; dom_string *data = NULL;
    if (node == NULL || argc < 3 || JS_ToUint32(ctx, &offset, argv[0]) < 0 || JS_ToUint32(ctx, &count, argv[1]) < 0 || JS_IsException(cos_dom_characterdata_make_string(ctx, argv[2], &data))) return JS_EXCEPTION;
    cos_dom_snapshot_character_data(node);
    dom_exception err = dom_characterdata_replace_data((dom_characterdata *)node, offset, count, data); if (data != NULL) dom_string_unref(data);
    if (err != DOM_NO_ERR) return JS_ThrowDOMException(ctx, "IndexSizeError", "CharacterData mutation failed");
    cos_dom_notify_character_data_mutation(); return JS_UNDEFINED;
}
static JSValue cos_dom_text_split_text(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    dom_node *node = cos_dom_unwrap_any_node(ctx, this_val);
    uint32_t offset = 0;
    if (node == NULL || argc < 1 || JS_ToUint32(ctx, &offset, argv[0]) < 0)
        return JS_EXCEPTION;
    dom_text *result_text = NULL;
    if (dom_text_split_text((dom_text *)node, offset, &result_text) != DOM_NO_ERR || result_text == NULL)
        return JS_EXCEPTION;
    JSValue result = cos_dom_wrap_text_node(ctx, result_text);
    dom_node_unref((dom_node *)result_text);
    return result;
}

/* document.createComment(): a real libdom Comment node with the
 * CharacterData interface but without Text.splitText(). */
static JSValue cos_web_create_comment(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    const char *data_c = ""; bool owns_cstring = false;
    if (argc >= 1) {
        data_c = JS_ToCString(ctx, argv[0]);
        if (data_c == NULL) return JS_EXCEPTION;
        owns_cstring = true;
    }
    JSValue result = JS_NULL;
    if (doc != NULL) {
        dom_string *data = NULL; dom_comment *comment = NULL;
        if (dom_string_create((const uint8_t *)data_c, strlen(data_c), &data) == DOM_NO_ERR &&
            dom_document_create_comment(doc, data, &comment) == DOM_NO_ERR && comment != NULL) {
            result = cos_dom_wrap_comment_node(ctx, comment);
            dom_node_unref((dom_node *)comment);
        }
        if (data != NULL) dom_string_unref(data);
    }
    if (owns_cstring) JS_FreeCString(ctx, data_c);
    return result;
}

/* document.createTextNode(): real libdom Text node when a document is
 * bound, pairing with the real appendChild/insertBefore above so the common
 * `parent.appendChild(document.createTextNode(str))` idiom actually builds
 * visible page content instead of silently doing nothing. */
static JSValue cos_web_create_text_node(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    const char *text_c = "";
    bool owns_cstring = false;
    if (argc >= 1) {
        text_c = JS_ToCString(ctx, argv[0]);
        if (text_c == NULL) return JS_EXCEPTION;
        owns_cstring = true;
    }
    JSValue result = JS_NULL;
    if (doc != NULL) {
        dom_string *data = NULL;
        dom_text *text_node = NULL;
        if (dom_string_create((const uint8_t *)text_c, strlen(text_c), &data) == DOM_NO_ERR &&
            dom_document_create_text_node(doc, data, &text_node) == DOM_NO_ERR &&
            text_node != NULL) {
            result = cos_dom_wrap_text_node(ctx, text_node);
            dom_node_unref((dom_node *)text_node);
        }
        if (data != NULL) dom_string_unref(data);
    }
        if (owns_cstring) JS_FreeCString(ctx, text_c);
    return result;
}

/* document.createDocumentFragment(): return a real, detached libdom
 * container. It can be populated with ordinary DOM nodes and appended into
 * the page with the same mutation and redraw path as an Element. */
static JSValue cos_web_create_document_fragment(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc == NULL) return JS_NULL;
    struct dom_document_fragment *fragment = NULL;
    if (dom_document_create_document_fragment(doc, &fragment) != DOM_NO_ERR || fragment == NULL) {
        return JS_EXCEPTION;
    }
    JSValue result = cos_dom_wrap_fragment(ctx, (dom_node *)fragment);
    dom_node_unref((dom_node *)fragment);
    return result;
}
/* document.documentElement / document.head / document.body: live accessors
 * that resolve from the real, currently-bound libdom document on every
 * access, instead of a one-time snapshot taken before any page existed.
 *
 * Before this fix these three were permanently-inert placeholder objects
 * created once at context-construction time - before any page had loaded -
 * and were never reconnected once a real page bound in. getElementById()
 * already worked correctly (it looks the live document up fresh on every
 * call), but `document.body.appendChild(x)`,
 * `document.documentElement.className = ...`, and similar extremely common
 * patterns silently did nothing: the noop stub methods swallowed them.
 *
 * These are read-only (getter, no setter): real browsers do allow
 * `document.body = someElement`, but implementing whole-body replacement is
 * a separate, larger feature than fixing the read/mutate path these
 * accessors are here for. Assigning to them is a silent no-op rather than a
 * thrown error, consistent with this bridge's fail-soft treatment of
 * unsupported browser features elsewhere. */
static JSValue cos_dom_get_document_element_accessor(JSContext *ctx, JSValueConst this_val,
                                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc != NULL) {
        dom_element *root = NULL;
        if (dom_document_get_document_element(doc, &root) == DOM_NO_ERR && root != NULL) {
            JSValue result = cos_dom_wrap_element(ctx, root);
            dom_node_unref((dom_node *)root);
            return result;
        }
    }
    return cos_web_new_element(ctx);
}

static JSValue cos_dom_get_document_body_accessor(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc != NULL) {
        /* dom_html_document_get_body() is declared in terms of
         * struct dom_html_element, which this translation unit has no
         * typedef for; dom_element is the same underlying libdom node and
         * the macro wrapper casts internally regardless (matching the
         * dom_html_input_element/dom_html_text_area_element casts already
         * used elsewhere in this file for the same reason). */
        dom_element *body = NULL;
        if (dom_html_document_get_body((dom_html_document *)doc, &body) == DOM_NO_ERR &&
            body != NULL) {
            JSValue result = cos_dom_wrap_element(ctx, body);
            dom_node_unref((dom_node *)body);
            return result;
        }
    }
    return cos_web_new_element(ctx);
}

/* libdom's dom_html_document vtable has get_body() but no matching
 * get_head(); walking the parsed tree for the first <head> is what
 * getElementsByTagName("head")[0] would do too, just without allocating the
 * intermediate collection object. */
static JSValue cos_dom_get_document_head_accessor(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc != NULL) {
        dom_string *tag = NULL;
        dom_nodelist *list = NULL;
        JSValue result = JS_NULL;
        if (dom_string_create((const uint8_t *)"head", 4, &tag) == DOM_NO_ERR &&
            dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR &&
            list != NULL) {
            dom_node *node = NULL;
            if (dom_nodelist_item(list, 0, &node) == DOM_NO_ERR && node != NULL) {
                result = cos_dom_wrap_element(ctx, (dom_element *)node);
                dom_node_unref(node);
            }
            dom_nodelist_unref(list);
        }
        if (tag != NULL) dom_string_unref(tag);
        if (!JS_IsNull(result)) return result;
    }
    return cos_web_new_element(ctx);
}

/* document.title is a live libdom property, rather than a detached string
 * stored on the JS compatibility object.  This makes title reads/writes
 * survive normal page parsing and gives the Browser chrome a standards-based
 * signal that a script has completed meaningful page state work. */
static JSValue cos_dom_get_document_title_accessor(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    dom_string *title = NULL;
    if (doc != NULL && dom_html_document_get_title((dom_html_document *)doc, &title) == DOM_NO_ERR &&
        title != NULL) {
        JSValue result = JS_NewStringLen(ctx, dom_string_data(title),
                                         dom_string_byte_length(title));
        dom_string_unref(title);
        return result;
    }
    return JS_NewString(ctx, "");
}

static JSValue cos_dom_set_document_title_accessor(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    dom_document *doc = (dom_document *)JS_GetContextOpaque(ctx);
    if (doc == NULL) return JS_UNDEFINED;
    const char *title_c = JS_ToCString(ctx, argv[0]);
    if (title_c == NULL) return JS_EXCEPTION;
    dom_string *title = NULL;
    dom_exception err = dom_string_create((const uint8_t *)title_c, strlen(title_c), &title);
    if (err == DOM_NO_ERR && title != NULL) {
        err = dom_html_document_set_title((dom_html_document *)doc, title);
        dom_string_unref(title);
    }
    if (err == DOM_NO_ERR) {
        cos_netsurf_browser_set_document_title(title_c);
    }
    JS_FreeCString(ctx, title_c);
    return err == DOM_NO_ERR ? JS_UNDEFINED : JS_EXCEPTION;
}

/* document.cookie is a live view into the same bounded HTTP cookie jar used
 * for request headers. It is deliberately URL-scoped and never exposes the
 * jar for an opaque/file: origin. HTTP response Set-Cookie already enters this
 * jar, so a script sees new same-path cookies after the owning fetch completes. */
static JSValue cos_dom_get_document_cookie_accessor(JSContext *ctx, JSValueConst this_val,
                                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char url[HTTP_MAX_URL];
    char cookies[1024];
    if (!cos_js_web_get_page_url(ctx, url, sizeof(url)) ||
        http_get_document_cookie_for_url(url, cookies, sizeof(cookies)) == 0) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, cookies);
}

static JSValue cos_dom_set_document_cookie_accessor(JSContext *ctx, JSValueConst this_val,
                                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    char url[HTTP_MAX_URL];
    if (!cos_js_web_get_page_url(ctx, url, sizeof(url))) {
        return JS_ThrowTypeError(ctx, "SecurityError: document has no cookie origin");
    }
    const char *assignment = JS_ToCString(ctx, argv[0]);
    if (assignment == NULL) return JS_EXCEPTION;
    size_t len = strlen(assignment);
    if (len == 0 || len >= 1024) {
        JS_FreeCString(ctx, assignment);
        return JS_ThrowRangeError(ctx, "document.cookie assignment is invalid or too large");
    }
    int status = http_set_document_cookie_for_url(url, assignment);
    JS_FreeCString(ctx, assignment);
    if (status != 0) {
        return JS_ThrowTypeError(ctx, "SecurityError: cookies require an HTTP(S) origin");
    }
    return JS_UNDEFINED;
}

/* All Location writes are first resolved against the committed page URL, then
 * queued. Browser-window navigation therefore happens only after QuickJS and
 * libdom have returned to the GUI owner loop; this prevents a script from
 * destroying its own active document context. */
static JSValue cos_web_get_location_href_accessor(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char url[HTTP_MAX_URL];
    return cos_js_web_get_page_url(ctx, url, sizeof(url)) ?
        JS_NewString(ctx, url) : JS_NewString(ctx, "");
}

static JSValue cos_web_queue_location_navigation(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Location navigation requires a URL");
    const char *input = JS_ToCString(ctx, argv[0]);
    if (input == NULL) return JS_EXCEPTION;
    char resolved[HTTP_MAX_URL];
    bool valid = cos_js_web_resolve_page_url(ctx, input, resolved, sizeof(resolved));
    JS_FreeCString(ctx, input);
    if (!valid) return JS_ThrowTypeError(ctx, "TypeError: invalid or unsupported navigation URL");
    if (!cos_netsurf_browser_queue_navigation(resolved)) {
        return JS_ThrowRangeError(ctx, "Navigation queue rejected URL");
    }
    return JS_UNDEFINED;
}

static JSValue cos_web_location_reload(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char url[HTTP_MAX_URL];
    if (!cos_js_web_get_page_url(ctx, url, sizeof(url)) ||
        !cos_netsurf_browser_queue_navigation(url)) {
        return JS_ThrowTypeError(ctx, "Location.reload has no active document URL");
    }
    return JS_UNDEFINED;
}

/* Installs a getter-only accessor property (see the three functions above)
 * directly on a plain instance object such as `document`, using the same
 * JS_DefinePropertyGetSet pattern as the shared element/text prototypes. */
static void cos_js_define_readonly_accessor(JSContext *ctx, JSValueConst obj,
                                            const char *name, JSCFunction *getter_fn)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue getter = JS_NewCFunction(ctx, getter_fn, name, 0);
    if (JS_DefinePropertyGetSet(ctx, obj, atom, getter, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter);
    }
    JS_FreeAtom(ctx, atom);
}

static void cos_js_define_readwrite_accessor(JSContext *ctx, JSValueConst obj,
                                              const char *name, JSCFunction *getter_fn,
                                              JSCFunction *setter_fn)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue getter = JS_NewCFunction(ctx, getter_fn, name, 0);
    JSValue setter = JS_NewCFunction(ctx, setter_fn, name, 1);
    if (JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter);
        JS_FreeValue(ctx, setter);
    }
    JS_FreeAtom(ctx, atom);
}

static void cos_js_install_browser_compat(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_NewObject(ctx);
    JSValue navigator = JS_NewObject(ctx);
    JSValue location = JS_NewObject(ctx);
    JSValue performance = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, document, "readyState", JS_NewString(ctx, "loading"));
    JS_SetPropertyStr(ctx, document, "URL", JS_NewString(ctx, ""));
    /* A real live Cookie accessor shares the bounded HTTP jar used by both
     * HTTP/1.1 and HTTP/2 requests, instead of the former static empty string. */
    cos_js_define_readwrite_accessor(ctx, document, "cookie",
                                     cos_dom_get_document_cookie_accessor,
                                     cos_dom_set_document_cookie_accessor);
    JS_SetPropertyStr(ctx, document, "visibilityState", JS_NewString(ctx, "visible"));
    cos_js_define_readonly_accessor(ctx, document, "documentElement",
                                    cos_dom_get_document_element_accessor);
    cos_js_define_readonly_accessor(ctx, document, "head", cos_dom_get_document_head_accessor);
    cos_js_define_readonly_accessor(ctx, document, "body", cos_dom_get_document_body_accessor);
    cos_js_define_readwrite_accessor(ctx, document, "title",
                                     cos_dom_get_document_title_accessor,
                                     cos_dom_set_document_title_accessor);
    JS_SetPropertyStr(ctx, document, "createElement",
                      JS_NewCFunction(ctx, cos_web_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, global, "MutationObserver",
                      JS_NewCFunction2(ctx, cos_dom_mutation_observer_constructor,
                                       "MutationObserver", 1,
                                       JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "Range",
                      JS_NewCFunction2(ctx, cos_dom_range_constructor,
                                       "Range", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "XMLSerializer",
                      JS_NewCFunction2(ctx, cos_dom_xml_serializer_constructor, "XMLSerializer", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "DOMParser",
                      JS_NewCFunction2(ctx, cos_dom_parser_constructor, "DOMParser", 0, JS_CFUNC_constructor, 0));
    JSValue dom_exception_ctor = JS_NewCFunction2(ctx, cos_dom_exception_constructor, "DOMException", 1, JS_CFUNC_constructor_or_func, 0);
    JSValue dom_exception_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dom_exception_proto, "constructor", JS_DupValue(ctx, dom_exception_ctor));
    JS_SetPropertyStr(ctx, dom_exception_proto, "toString", JS_NewCFunction(ctx, cos_dom_exception_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "prototype", dom_exception_proto);
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INDEX_SIZE_ERR", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "DOMSTRING_SIZE_ERR", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "HIERARCHY_REQUEST_ERR", JS_NewInt32(ctx, 3));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "WRONG_DOCUMENT_ERR", JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INVALID_CHARACTER_ERR", JS_NewInt32(ctx, 5));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "NO_MODIFICATION_ALLOWED_ERR", JS_NewInt32(ctx, 7));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "NOT_FOUND_ERR", JS_NewInt32(ctx, 8));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "NOT_SUPPORTED_ERR", JS_NewInt32(ctx, 9));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INUSE_ATTRIBUTE_ERR", JS_NewInt32(ctx, 10));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INVALID_STATE_ERR", JS_NewInt32(ctx, 11));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "SYNTAX_ERR", JS_NewInt32(ctx, 12));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INVALID_MODIFICATION_ERR", JS_NewInt32(ctx, 13));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "NAMESPACE_ERR", JS_NewInt32(ctx, 14));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "INVALID_ACCESS_ERR", JS_NewInt32(ctx, 15));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "TYPE_MISMATCH_ERR", JS_NewInt32(ctx, 17));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "SECURITY_ERR", JS_NewInt32(ctx, 18));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "NETWORK_ERR", JS_NewInt32(ctx, 19));
    JS_SetPropertyStr(ctx, dom_exception_ctor, "ABORT_ERR", JS_NewInt32(ctx, 20));
    JS_SetPropertyStr(ctx, global, "DOMException", dom_exception_ctor);
    JS_SetPropertyStr(ctx, document, "nodeType", JS_NewInt32(ctx, DOM_DOCUMENT_NODE));
    JS_SetPropertyStr(ctx, document, "nodeName", JS_NewString(ctx, "#document"));
    JS_SetPropertyStr(ctx, document, "hasChildNodes",
                      JS_NewCFunction(ctx, cos_dom_document_has_child_nodes, "hasChildNodes", 0));
    JS_SetPropertyStr(ctx, document, "getRootNode",
                      JS_NewCFunction(ctx, cos_dom_node_get_root_node, "getRootNode", 1));
    cos_js_define_readonly_accessor(ctx, document, "isConnected", cos_dom_node_get_is_connected);
    JS_SetPropertyStr(ctx, document, "createRange",
                      JS_NewCFunction(ctx, cos_dom_range_constructor, "createRange", 0));
    JS_SetPropertyStr(ctx, document, "getSelection",
                      JS_NewCFunction(ctx, cos_dom_get_selection, "getSelection", 0));
    JS_SetPropertyStr(ctx, global, "getSelection",
                      JS_NewCFunction(ctx, cos_dom_get_selection, "getSelection", 0));
    /* Namespace is accepted for web compatibility; libdom's HTML document
     * creates the corresponding element using the same tree-backed path. */
    JS_SetPropertyStr(ctx, document, "createElementNS",
                      JS_NewCFunction(ctx, cos_web_create_element_ns, "createElementNS", 2));
    JS_SetPropertyStr(ctx, document, "createTextNode",
                      JS_NewCFunction(ctx, cos_web_create_text_node, "createTextNode", 1));
    JS_SetPropertyStr(ctx, document, "createComment",
                      JS_NewCFunction(ctx, cos_web_create_comment, "createComment", 1));
    JS_SetPropertyStr(ctx, document, "createDocumentFragment",
                      JS_NewCFunction(ctx, cos_web_create_document_fragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, document, "createEvent",
                      JS_NewCFunction(ctx, cos_web_event_constructor, "createEvent", 1));
    JS_SetPropertyStr(ctx, document, "importNode",
                      JS_NewCFunction(ctx, cos_web_import_node, "importNode", 2));
    JS_SetPropertyStr(ctx, document, "normalizeDocument",
                      JS_NewCFunction(ctx, cos_web_normalize_document, "normalizeDocument", 0));
    JS_SetPropertyStr(ctx, document, "adoptNode",
                      JS_NewCFunction(ctx, cos_web_adopt_node, "adoptNode", 1));
    JS_SetPropertyStr(ctx, document, "getElementById",
                      JS_NewCFunction(ctx, cos_dom_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "querySelector",
                      JS_NewCFunction(ctx, cos_dom_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, document, "querySelectorAll",
                      JS_NewCFunction(ctx, cos_dom_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, document, "getElementsByTagName",
                      JS_NewCFunction(ctx, cos_dom_get_elements_by_tag_name, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, document, "getElementsByClassName",
                      JS_NewCFunction(ctx, cos_dom_document_get_elements_by_class_name, "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, document, "addEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, document, "removeEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "removeEventListener", 2));

    JS_SetPropertyStr(ctx, navigator, "userAgent",
                      JS_NewString(ctx, "C-OS NetSurf 3.11 QuickJS"));
    JS_SetPropertyStr(ctx, navigator, "language", JS_NewString(ctx, "ja-JP"));
    JS_SetPropertyStr(ctx, navigator, "onLine", JS_NewBool(ctx, true));
    /* Initialise every common Location string property.  External scripts
     * legitimately call location.search.indexOf(...) even on a URL without a
     * query string, where the standard value is the empty string, not
     * undefined. cos_js_set_page_location later replaces these with the real
     * NetSurf document URL components. */
    cos_js_define_readwrite_accessor(ctx, location, "href",
                                     cos_web_get_location_href_accessor,
                                     cos_web_queue_location_navigation);
    JS_SetPropertyStr(ctx, location, "protocol", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "host", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "hostname", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "pathname", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "search", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "hash", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "origin", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, performance, "now",
                      JS_NewCFunction(ctx, cos_web_performance_now, "now", 0));

    JS_SetPropertyStr(ctx, global, "document", document);
    JS_SetPropertyStr(ctx, global, "navigator", navigator);
    JS_SetPropertyStr(ctx, global, "location", location);
    JS_SetPropertyStr(ctx, global, "performance", performance);
    JSValue event_ctor = JS_NewCFunction2(ctx, cos_web_event_constructor, "Event", 1,
                                          JS_CFUNC_constructor_or_func, 0);
    JS_SetPropertyStr(ctx, global, "Event", event_ctor);
    JSValue custom_event_ctor = JS_NewCFunction2(ctx, cos_web_custom_event_constructor, "CustomEvent", 1,
                                                 JS_CFUNC_constructor_or_func, 0);
    JS_SetPropertyStr(ctx, global, "CustomEvent", custom_event_ctor);
    /* These constructors share the standards-compatible Event initialization
     * path.  Specialized fields are added by their option dictionaries in the
     * next browser-compatibility layer, while propagation and cancellation
     * semantics remain identical to Event. */
    JS_SetPropertyStr(ctx, global, "MouseEvent", JS_NewCFunction2(ctx, cos_web_event_constructor,
                                                                  "MouseEvent", 1,
                                                                  JS_CFUNC_constructor_or_func, 0));
    JS_SetPropertyStr(ctx, global, "KeyboardEvent", JS_NewCFunction2(ctx, cos_web_event_constructor,
                                                                      "KeyboardEvent", 1,
                                                                      JS_CFUNC_constructor_or_func, 0));
    JS_SetPropertyStr(ctx, global, "FocusEvent", JS_NewCFunction2(ctx, cos_web_event_constructor,
                                                                   "FocusEvent", 1,
                                                                   JS_CFUNC_constructor_or_func, 0));
    JS_SetPropertyStr(ctx, global, "InputEvent", JS_NewCFunction2(ctx, cos_web_event_constructor,
                                                                   "InputEvent", 1,
                                                                   JS_CFUNC_constructor_or_func, 0));
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "top", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "parent", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunction(ctx, cos_web_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunction(ctx, cos_web_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunction(ctx, cos_web_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunction(ctx, cos_web_clear_timer, "clearInterval", 1));
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
                      JS_NewCFunction(ctx, cos_web_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
                      JS_NewCFunction(ctx, cos_web_clear_timer, "cancelAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "queueMicrotask",
                      JS_NewCFunction(ctx, cos_web_queue_microtask, "queueMicrotask", 1));
    JS_SetPropertyStr(ctx, global, "addEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, global, "removeEventListener",
                      JS_NewCFunction(ctx, cos_web_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, global, "matchMedia",
                      JS_NewCFunction(ctx, cos_web_match_media, "matchMedia", 1));
    JS_SetPropertyStr(ctx, location, "assign",
                      JS_NewCFunction(ctx, cos_web_queue_location_navigation, "assign", 1));
    JS_SetPropertyStr(ctx, location, "replace",
                      JS_NewCFunction(ctx, cos_web_queue_location_navigation, "replace", 1));
    JS_SetPropertyStr(ctx, location, "reload",
                      JS_NewCFunction(ctx, cos_web_location_reload, "reload", 0));

    /* Keep page-level listeners even though the compact bridge does not yet
     * expose individual libdom nodes.  js_fire_event() dispatches these lists
     * when NetSurf reports lifecycle events. */
    static const char event_bridge[] =
        "(function(){var L=Object.create(null);"
        "function add(t,f){if(typeof f!=='function')return;"
        "(L[t]||(L[t]=[])).push(f);}"
        "function rem(t,f){var a=L[t]||[],i=a.indexOf(f);"
        "if(i>=0)a.splice(i,1);}"
        "globalThis._cos_event_listeners=L;"
        "document.addEventListener=add;document.removeEventListener=rem;"
        "window.addEventListener=add;window.removeEventListener=rem;"
        "globalThis.__cos_make_dataset=function(el){"
        "function an(p){return 'data-'+String(p).replace(/[A-Z]/g,function(c){return '-'+c.toLowerCase();});}"
        "function pn(n){var s=n.slice(5),o='';for(var i=0;i<s.length;i++){if(s[i]==='-'&&i+1<s.length)o+=s[++i].toUpperCase();else o+=s[i];}return o;}"
        "var h={get:function(t,p){if(typeof p==='symbol')return t[p];if(p==='toJSON')return function(){var o={};h.ownKeys(t).forEach(function(k){o[k]=h.get(t,k);});return o;};var n=an(p);return el.hasAttribute(n)?el.getAttribute(n):undefined;},"
        "set:function(t,p,v){if(typeof p!=='string')return false;el.setAttribute(an(p),String(v));return true;},"
        "deleteProperty:function(t,p){if(typeof p!=='string')return false;el.removeAttribute(an(p));return true;},"
        "ownKeys:function(){var a=el.getAttributeNames(),o=[];for(var i=0;i<a.length;i++)if(a[i].indexOf('data-')===0)o.push(pn(a[i]));return o;},"
        "getOwnPropertyDescriptor:function(t,p){if(typeof p==='string'&&el.hasAttribute(an(p)))return {enumerable:true,configurable:true};return undefined;}};"
        "return new Proxy(Object.create(null),h);};})();";
    (void)cos_js_eval_quiet(ctx, event_bridge, sizeof(event_bridge) - 1,
                            "<browser-compat>");
    JS_FreeValue(ctx, global);
}

/* Synchronise the most commonly consumed read-only Location URL components
 * from NetSurf's canonical page URL.  This is deliberately data-only: script
 * assignments to location.href do not navigate until browser-window navigation
 * is wired as a separate owner-thread operation. */
void cos_js_set_page_location(JSContext *ctx, const char *url)
{
    if (ctx == NULL) return;
    if (url == NULL) url = "";

    const char *end = url + strlen(url);
    const char *scheme = strstr(url, "://");
    const char *authority = scheme != NULL ? scheme + 3 : url;
    const char *path = authority;
    while (path < end && *path != '/' && *path != '?' && *path != '#') ++path;
    const char *query = path;
    while (query < end && *query != '?' && *query != '#') ++query;
    const char *fragment = query;
    while (fragment < end && *fragment != '#') ++fragment;
    const char *path_end = query;
    const char *query_end = fragment;

    const char *host_begin = authority;
    const char *host_end = path;
    const char *hostname_end = host_end;
    /* A simple authority parser covers ordinary DNS host[:port] URLs.  Keep
     * bracketed IPv6 literal handling intact by not splitting inside []. */
    if (host_begin < host_end && *host_begin == '[') {
        const char *close = host_begin;
        while (close < host_end && *close != ']') ++close;
        if (close < host_end) hostname_end = close + 1;
    } else {
        for (const char *p = host_begin; p < host_end; ++p) {
            if (*p == ':') hostname_end = p;
        }
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue location = JS_GetPropertyStr(ctx, global, "location");
    JSValue document = JS_GetPropertyStr(ctx, global, "document");
    if (JS_IsObject(location)) {
#define COS_SET_URL_PART(name, ptr, len) \
        JS_SetPropertyStr(ctx, location, name, JS_NewStringLen(ctx, ptr, len))
        /* `href` is an accessor: assigning through JS_SetPropertyStr here
         * would invoke its script-navigation setter while merely synchronising
         * the committed NetSurf URL, causing an infinite same-page reload.
         * The accessor reads the page URL directly from cos_js_web state. */
        COS_SET_URL_PART("protocol", url, scheme != NULL ? (size_t)(scheme - url + 1) : 0);
        COS_SET_URL_PART("host", host_begin, (size_t)(host_end - host_begin));
        COS_SET_URL_PART("hostname", host_begin, (size_t)(hostname_end - host_begin));
        COS_SET_URL_PART("pathname", path < path_end ? path : "/",
                         path < path_end ? (size_t)(path_end - path) : 1);
        COS_SET_URL_PART("search", query < query_end ? query : "",
                         query < query_end ? (size_t)(query_end - query) : 0);
        COS_SET_URL_PART("hash", fragment < end ? fragment : "",
                         fragment < end ? (size_t)(end - fragment) : 0);
        COS_SET_URL_PART("origin", scheme != NULL ? url : "",
                         scheme != NULL ? (size_t)(path - url) : 0);
#undef COS_SET_URL_PART
    }
    if (JS_IsObject(document)) {
        JS_SetPropertyStr(ctx, document, "URL",
                          JS_NewStringLen(ctx, url, (size_t)(end - url)));
    }
    JS_FreeValue(ctx, document);
    JS_FreeValue(ctx, location);
    JS_FreeValue(ctx, global);
}

/* ---- COS global API object ---- */

static JSValue cos_api_print(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) {
        serial_puts(s);
        serial_puts("\n");
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue cos_api_get_mem_info(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* Pull memory stats from the kernel allocator.
     * memory_get_used() / memory_get_total() are declared in memory.h
     * and implemented in kernel/memory.c. */
    uint64_t used  = memory_get_used();
    uint64_t total = memory_get_total();

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "used",  JS_NewInt64(ctx, (int64_t)used));
    JS_SetPropertyStr(ctx, obj, "total", JS_NewInt64(ctx, (int64_t)total));
    return obj;
}

static void cos_js_install_cos_api(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cos_obj = JS_NewObject(ctx);

    /* COS.version */
    JS_SetPropertyStr(ctx, cos_obj, "version",
        JS_NewString(ctx, COS_VERSION_STRING));

    /* COS.osName */
    JS_SetPropertyStr(ctx, cos_obj, "osName",
        JS_NewString(ctx, COS_OS_NAME));

    /* COS.print(s) */
    JS_SetPropertyStr(ctx, cos_obj, "print",
        JS_NewCFunction(ctx, cos_api_print, "print", 1));

    /* COS.getMemInfo() */
    JS_SetPropertyStr(ctx, cos_obj, "getMemInfo",
        JS_NewCFunction(ctx, cos_api_get_mem_info, "getMemInfo", 0));

    JS_SetPropertyStr(ctx, global, "COS", cos_obj);
    JS_FreeValue(ctx, global);
}

/* ---- Privileged OS drawing API ---- */

/* This command queue is intentionally fixed-size and has no JS-visible
 * framebuffer pointer.  Only the direct C-OS runtime gets the `OS` object;
 * page contexts created by NetSurf never call cos_js_enable_privileged_os_api.
 * The GUI owner composites it after desktop rendering, keeping a single owner
 * for QuickJS, the queue, and the backbuffer even on SMP systems. */
#define COS_JS_OS_DRAW_MAX_COMMANDS 64

typedef struct {
    int x;
    int y;
    int w;
    int h;
    uint32_t color;
} cos_js_os_draw_command_t;

static cos_js_os_draw_command_t cos_js_os_draw_commands[COS_JS_OS_DRAW_MAX_COMMANDS];
static unsigned int cos_js_os_draw_count;

static JSValue cos_js_os_draw_rect(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    int64_t x, y, w, h, color;
    if (argc < 5 ||
        JS_ToInt64(ctx, &x, argv[0]) < 0 ||
        JS_ToInt64(ctx, &y, argv[1]) < 0 ||
        JS_ToInt64(ctx, &w, argv[2]) < 0 ||
        JS_ToInt64(ctx, &h, argv[3]) < 0 ||
        JS_ToInt64(ctx, &color, argv[4]) < 0) {
        return JS_ThrowTypeError(ctx, "OS.drawRect expects five integer arguments");
    }

    /* Reject rather than wrap hostile numeric values.  Width/height are
     * bounded by the physical desktop, and the accepted origin range keeps
     * all following signed arithmetic within int64_t. */
    if (w <= 0 || h <= 0 || w > (int64_t)SCREEN_W || h > (int64_t)SCREEN_H ||
        x < -(int64_t)SCREEN_W || x > (int64_t)SCREEN_W ||
        y < -(int64_t)SCREEN_H || y > (int64_t)SCREEN_H ||
        color < 0 || color > 0x00FFFFFF) {
        return JS_FALSE;
    }

    /* Clip using signed 64-bit intermediates before narrowing to C drawing
     * coordinates.  Off-screen rectangles are valid no-ops, not errors. */
    if (x >= (int64_t)SCREEN_W || y >= (int64_t)SCREEN_H ||
        x + w <= 0 || y + h <= 0) {
        return JS_FALSE;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int64_t)SCREEN_W) w = (int64_t)SCREEN_W - x;
    if (y + h > (int64_t)SCREEN_H) h = (int64_t)SCREEN_H - y;
    if (w <= 0 || h <= 0) return JS_FALSE;
    if (cos_js_os_draw_count >= COS_JS_OS_DRAW_MAX_COMMANDS) return JS_FALSE;

    cos_js_os_draw_command_t *cmd =
        &cos_js_os_draw_commands[cos_js_os_draw_count++];
    cmd->x = (int)x;
    cmd->y = (int)y;
    cmd->w = (int)w;
    cmd->h = (int)h;
    cmd->color = (uint32_t)color;
    gui_request_redraw();
    return JS_TRUE;
}

static JSValue cos_js_os_clear_drawings(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (cos_js_os_draw_count != 0) {
        cos_js_os_draw_count = 0;
        gui_request_redraw();
    }
    return JS_UNDEFINED;
}

void cos_js_enable_privileged_os_api(JSContext *ctx)
{
    if (ctx == NULL) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue os = JS_GetPropertyStr(ctx, global, "OS");
    if (!JS_IsObject(os)) {
        JS_FreeValue(ctx, os);
        os = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "OS", JS_DupValue(ctx, os));
    }
    JS_SetPropertyStr(ctx, os, "drawRect",
                      JS_NewCFunction(ctx, cos_js_os_draw_rect, "drawRect", 5));
    JS_SetPropertyStr(ctx, os, "clearDrawings",
                      JS_NewCFunction(ctx, cos_js_os_clear_drawings, "clearDrawings", 0));
    JS_FreeValue(ctx, os);
    JS_FreeValue(ctx, global);
}

bool cos_js_os_draw_overlay(void)
{
    if (cos_js_os_draw_count == 0) return false;
    for (unsigned int i = 0; i < cos_js_os_draw_count; ++i) {
        const cos_js_os_draw_command_t *cmd = &cos_js_os_draw_commands[i];
        vga_fill_rect(cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
    }
    return true;
}

/* ---- Public API ---- */

/* Every allocation QuickJS makes (runtime, contexts, every JS object,
 * string, and bytecode buffer it ever creates) goes through
 * kmalloc/krealloc/kfree via the functions above - there is no
 * separate "JS heap" carved out of memory, it all comes from the same
 * kernel heap everything else in C-OS uses. */
JSRuntime* cos_js_new_runtime(void) {
    JSRuntime* rt = JS_NewRuntime2(&cos_qjs_malloc_funcs, NULL);
    if (rt) {
        JS_SetMemoryLimit(rt, COS_QUICKJS_RUNTIME_MEMORY_LIMIT_BYTES);
        /* Page JavaScript has a deterministic 24MiB ceiling, independent of
         * NetSurf's larger shared HTML/CSS/image resource budget. */
        /* QuickJS's own recursion-depth guard defaults to 1 MiB
         * (JS_DEFAULT_STACK_SIZE) if nothing here configures it - a budget
         * that assumes a normal desktop-OS thread stack. The kernel thread
         * every browser tab's JS actually runs on (gui_main, see kernel.c)
         * has a real stack of GUI_MAIN_STACK_SIZE (256 KiB). Left
         * unconfigured, QuickJS would let a deeply-recursive real page (ad
         * tech and consent-management scripts are frequent offenders) walk
         * straight through the bottom of that real, 128x-smaller stack
         * before its own guard ever thought to object - corrupting whatever
         * memory sits below it instead of cleanly throwing a catchable
         * RangeError. 128 KiB leaves half of the real stack as headroom for
         * the native C call chain around the interpreter (NetSurf's
         * HTML/CSS/layout pipeline, this bridge's own eval wrappers) and as
         * a safety margin, while still being generous enough for real-world
         * script depth. Keep in sync with GUI_MAIN_STACK_SIZE in kernel.c. */
        JS_SetMaxStackSize(rt, 128u * 1024u);
        cos_dom_register_class(rt);
    }
    return rt;
}

JSContext* cos_js_new_context(JSRuntime* rt) {
    JSContext* ctx = JS_NewContext(rt);
    if (ctx) {
        /* Install console and COS globals automatically in every new
         * context so scripts can use console.log() and COS.version
         * without any extra setup. */
        cos_js_install_console(ctx);
        cos_js_install_browser_compat(ctx);
        cos_js_install_cos_api(ctx);
        cos_dom_install_element_proto(ctx);
        cos_dom_install_characterdata_proto(ctx, cos_dom_text_class_id, true);
        cos_dom_install_characterdata_proto(ctx, cos_dom_comment_class_id, false);
        cos_dom_install_fragment_proto(ctx);
        /* Browser-shaped APIs are owner-thread mediated.  They only queue
         * bounded work here; GUI lifecycle polling resolves their Promise/XHR
         * completions after network I/O has finished. */
        cos_js_web_install(ctx);
    }
    return ctx;
}

void cos_js_bind_document(JSContext *ctx, void *document)
{
    if (ctx != NULL) {
        JS_SetContextOpaque(ctx, document);
        cos_js_active_page_context = document != NULL ? ctx : NULL;
        /* The document URL is authoritative for relative fetch resolution and
         * origin-partitioned Web Storage.  A document lacking a normal HTTP(S)
         * URL intentionally receives no persistent storage area. */
        if (document != NULL) {
            dom_string *url = NULL;
            if (dom_html_document_get_url((dom_html_document *)document, &url) == DOM_NO_ERR &&
                url != NULL) {
                char page_url[HTTP_MAX_URL];
                size_t n = dom_string_byte_length(url);
                if (n >= sizeof(page_url)) n = sizeof(page_url) - 1;
                memcpy(page_url, dom_string_data(url), n);
                page_url[n] = '\0';
                cos_js_web_set_origin(ctx, page_url);
                cos_js_set_page_location(ctx, page_url);
                dom_string_unref(url);
            }
        }
    }
}

/* Evaluates `script` and reports the result (or a thrown exception,
 * with its message and stack trace if present) to the serial console.
 * This is intentionally the simplest possible "does the engine
 * actually run code" check, independent of any browser/DOM
 * integration - useful on its own as a smoke test once this is
 * linked into a real build, and as the shape a future shell "js"
 * command or NetSurf script-tag handler would follow. */
/* Shared by both eval variants below: pulls the pending exception off
 * `ctx`, logs its message and (when present) stack trace to the
 * serial console, and frees it. Does not touch `result`/rethrow -
 * callers still own freeing the JSValue they got from JS_Eval. */
static void cos_js_report_pending_exception(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    serial_puts("[QJS] Exception: ");
    serial_puts(msg ? msg : "(unable to stringify exception)");
    serial_puts("\n");
    if (msg) JS_FreeCString(ctx, msg);

    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* stack_str = JS_ToCString(ctx, stack);
        if (stack_str) {
            serial_puts("[QJS] Stack: ");
            serial_puts(stack_str);
            serial_puts("\n");
            JS_FreeCString(ctx, stack_str);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

void cos_js_eval_and_report(JSContext* ctx, const char* script, const char* filename) {
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    JSValue result = JS_Eval(ctx, script, strlen(script), filename ? filename : "<eval>",
                              JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        cos_js_report_pending_exception(ctx);
    } else {
        const char* str = JS_ToCString(ctx, result);
        serial_puts("[QJS] Result: ");
        serial_puts(str ? str : "(unable to stringify result)");
        serial_puts("\n");
        if (str) JS_FreeCString(ctx, str);
    }

    JS_FreeValue(ctx, result);
}

bool cos_js_eval_quiet(JSContext* ctx, const char* script, size_t script_len, const char* filename) {
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    JSValue result = JS_Eval(ctx, script, script_len, filename ? filename : "<script>",
                              JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(result);
    if (!ok) {
        cos_js_report_pending_exception(ctx);
    }
    JS_FreeValue(ctx, result);
    return ok;
}

typedef struct {
    bool stop_propagation;
    bool stop_immediate;
    bool default_prevented;
    bool passive_listener;
} cos_dom_event_state;

static cos_dom_event_state *cos_dom_event_state_from_data(JSContext *ctx,
                                                            JSValueConst value)
{
    int64_t raw = 0;
    if (JS_ToInt64(ctx, &raw, value) != 0) return NULL;
    return (cos_dom_event_state *)(uintptr_t)raw;
}

static JSValue cos_dom_event_stop_propagation(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv,
                                               int magic, JSValue *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    cos_dom_event_state *state = cos_dom_event_state_from_data(ctx, func_data[0]);
    if (state != NULL) state->stop_propagation = true;
    return JS_UNDEFINED;
}

static JSValue cos_dom_event_stop_immediate_propagation(JSContext *ctx, JSValueConst this_val,
                                                         int argc, JSValueConst *argv,
                                                         int magic, JSValue *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    cos_dom_event_state *state = cos_dom_event_state_from_data(ctx, func_data[0]);
    if (state != NULL) { state->stop_propagation = true; state->stop_immediate = true; }
    return JS_UNDEFINED;
}

static JSValue cos_dom_event_prevent_default(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv,
                                             int magic, JSValue *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    cos_dom_event_state *state = cos_dom_event_state_from_data(ctx, func_data[0]);
    if (state != NULL && !state->passive_listener) {
        state->default_prevented = true;
        /* Scripts observe this synchronously inside the same listener. */
        JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
    }
    return JS_UNDEFINED;
}

static JSValue cos_dom_new_event(JSContext *ctx, const char *event_type,
                                 JSValueConst target, cos_dom_event_state *state)
{
    JSValue event = JS_NewObject(ctx);
    JSValue data[1] = { JS_NewInt64(ctx, (int64_t)(uintptr_t)state) };
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, event_type));
    JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "bubbles", JS_TRUE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_TRUE);
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "stopPropagation", JS_NewCFunctionData(ctx, cos_dom_event_stop_propagation, 0, 0, 1, data));
    JS_SetPropertyStr(ctx, event, "stopImmediatePropagation", JS_NewCFunctionData(ctx, cos_dom_event_stop_immediate_propagation, 0, 0, 1, data));
    JS_SetPropertyStr(ctx, event, "preventDefault", JS_NewCFunctionData(ctx, cos_dom_event_prevent_default, 0, 0, 1, data));
    JS_FreeValue(ctx, data[0]);
    return event;
}

static bool cos_dom_dispatch_node_listeners(JSContext *ctx, dom_node *node,
                                            const char *event_type, JSValue event,
                                            cos_dom_event_state *state,
                                            bool capture, int phase)
{
    JSValue current = cos_dom_wrap_node(ctx, node);
    if (JS_IsNull(current) || JS_IsException(current)) { JS_FreeValue(ctx, current); return false; }
    JSValue table = JS_GetPropertyStr(ctx, current, "__cos_dom_events");
    JSValue listeners = JS_IsObject(table) ? JS_GetPropertyStr(ctx, table, event_type) : JS_UNDEFINED;
    uint32_t count = 0;
    if (JS_IsArray(listeners)) {
        JSValue length = JS_GetPropertyStr(ctx, listeners, "length");
        (void)JS_ToUint32(ctx, &count, length); JS_FreeValue(ctx, length);
    }
    if (count > 128u) count = 128u;
    bool delivered = false;
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_DupValue(ctx, current));
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, phase));
    for (uint32_t i = 0; i < count && !state->stop_immediate; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, listeners, i);
        JSValue callback = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "callback") : JS_DupValue(ctx, entry);
        JSValue capture_value = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "capture") : JS_FALSE;
        JSValue once_value = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "once") : JS_FALSE;
        JSValue passive_value = JS_IsObject(entry) ? JS_GetPropertyStr(ctx, entry, "passive") : JS_FALSE;
        bool matches = (JS_ToBool(ctx, capture_value) > 0) == capture;
        if (matches && JS_IsFunction(ctx, callback)) {
            delivered = true;
            state->passive_listener = JS_ToBool(ctx, passive_value) > 0;
            JSValue result = JS_Call(ctx, callback, current, 1, (JSValueConst *)&event);
            if (JS_IsException(result)) cos_js_report_pending_exception(ctx);
            JS_FreeValue(ctx, result);
            state->passive_listener = false;
            if (JS_ToBool(ctx, once_value) > 0) JS_SetPropertyUint32(ctx, listeners, i, JS_NULL);
        }
        JS_FreeValue(ctx, callback); JS_FreeValue(ctx, capture_value);
        JS_FreeValue(ctx, once_value); JS_FreeValue(ctx, passive_value); JS_FreeValue(ctx, entry);
    }
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_NewBool(ctx, state->default_prevented));
    JS_FreeValue(ctx, listeners); JS_FreeValue(ctx, table); JS_FreeValue(ctx, current);
    return delivered;
}

bool cos_js_dispatch_dom_event(JSContext *ctx, void *target_node, const char *event_type)
{
    if (ctx == NULL || target_node == NULL || event_type == NULL || event_type[0] == '\0') return false;
    JSValue target = cos_dom_wrap_node(ctx, (dom_node *)target_node);
    if (JS_IsNull(target) || JS_IsException(target)) { JS_FreeValue(ctx, target); return false; }
    cos_dom_event_state state = {0};
    JSValue event = cos_dom_new_event(ctx, event_type, target, &state);
    bool delivered = cos_dom_dispatch_node_listeners(ctx, (dom_node *)target_node, event_type, event, &state, true, 2);
    if (!state.stop_immediate) delivered |= cos_dom_dispatch_node_listeners(ctx, (dom_node *)target_node, event_type, event, &state, false, 2);
    JS_FreeValue(ctx, event); JS_FreeValue(ctx, target);
    return delivered;
}

bool cos_js_dispatch_bound_dom_event(void *target_node, const char *event_type)
{
    if (cos_js_active_page_context == NULL || target_node == NULL ||
        event_type == NULL || event_type[0] == '\0') {
        serial_puts("[QJS/DOM] bound dispatch unavailable\n");
        return false;
    }
    dom_node *path[64]; unsigned count = 1; path[0] = (dom_node *)target_node;
    while (count < 64u) {
        dom_node *parent = NULL;
        if (dom_node_get_parent_node(path[count - 1], &parent) != DOM_NO_ERR || parent == NULL) break;
        path[count++] = parent;
    }
    JSValue target = cos_dom_wrap_node(cos_js_active_page_context, path[0]);
    if (JS_IsNull(target) || JS_IsException(target)) { JS_FreeValue(cos_js_active_page_context, target); goto done; }
    cos_dom_event_state state = {0};
    JSValue event = cos_dom_new_event(cos_js_active_page_context, event_type, target, &state);
    bool delivered = false;
    /* Capture walks document-to-parent; target executes capture then bubble
     * listeners; bubble walks parent-to-document. */
    for (unsigned i = count; i-- > 1u && !state.stop_propagation; )
        delivered |= cos_dom_dispatch_node_listeners(cos_js_active_page_context, path[i], event_type, event, &state, true, 1);
    if (!state.stop_propagation) {
        delivered |= cos_dom_dispatch_node_listeners(cos_js_active_page_context, path[0], event_type, event, &state, true, 2);
        if (!state.stop_immediate)
            delivered |= cos_dom_dispatch_node_listeners(cos_js_active_page_context, path[0], event_type, event, &state, false, 2);
    }
    for (unsigned i = 1; i < count && !state.stop_propagation; ++i)
        delivered |= cos_dom_dispatch_node_listeners(cos_js_active_page_context, path[i], event_type, event, &state, false, 3);
    JS_FreeValue(cos_js_active_page_context, event); JS_FreeValue(cos_js_active_page_context, target);
    (void)cos_js_dispatch_event(cos_js_active_page_context, event_type);
    for (unsigned i = 1; i < count; ++i) dom_node_unref(path[i]);
    return delivered;
done:
    for (unsigned i = 1; i < count; ++i) dom_node_unref(path[i]);
    return false;
}

bool cos_js_dispatch_event(JSContext *ctx, const char *event_type)
{
    if (ctx == NULL || event_type == NULL || event_type[0] == '\0') return false;

    /* Event names originate in NetSurf, but keep the generated expression
     * closed to identifiers before embedding it in a tiny compatibility call.
     * This avoids treating an unexpected event label as JavaScript source. */
    char type[48];
    size_t n = 0;
    while (event_type[n] != '\0' && n + 1 < sizeof(type)) {
        char c = event_type[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) break;
        type[n] = c;
        ++n;
    }
    type[n] = '\0';
    if (n == 0) return false;

    /* Lifecycle dispatch must remain syntactically complete even after the
     * fixed compatibility wrapper and an event label are substituted. 512
     * bytes was marginal and produced a truncated `<browser-event>` script,
     * preventing Google and other pages from observing DOMContentLoaded/load.
     *
     * readyState is updated FIRST, before any listener runs. Per the HTML
     * spec, the document's readiness state changes to "interactive"/
     * "complete" and only then does the corresponding event fire - so a
     * DOMContentLoaded or load handler that checks `document.readyState`
     * (a common defensive pattern, e.g. libraries doing
     * `if (document.readyState !== 'loading') init(); else
     * document.addEventListener('DOMContentLoaded', init)`) must see the new
     * value, not the stale one. The previous ordering updated readyState
     * only after every listener had already run. */
    char script[2048];
    int written = snprintf(script, sizeof(script),
        "(function(){"
        "if(typeof document!=='undefined'&&document.readyState!==undefined&&'%s'==='DOMContentLoaded')document.readyState='interactive';"
        "if(typeof document!=='undefined'&&document.readyState!==undefined&&'%s'==='load')document.readyState='complete';"
        "var e={type:'%s'};var a=(globalThis._cos_event_listeners||{})['%s']||[];"
        "for(var i=0;i<a.length;i++){try{a[i](e);}catch(x){}}"
        "if(typeof document!=='undefined'&&typeof document['on%s']==='function')document['on%s'](e);"
        "if(typeof window!=='undefined'&&typeof window['on%s']==='function')window['on%s'](e);})();",
        type, type, type, type, type, type, type, type);
    if (written < 0 || (size_t)written >= sizeof(script)) return false;
    return cos_js_eval_quiet(ctx, script, (size_t)written, "<browser-event>");
}

/* Feed desktop keyboard input to ordinary window.addEventListener('keydown',
 * ...) listeners.  The browser port remains the owner of form editing; this
 * bridge adds the standard page-level event so shortcuts and calculators work
 * without page-specific hooks. */
bool cos_js_dispatch_window_keydown(uint32_t key)
{
    JSContext *ctx = cos_js_active_page_context;
    if (ctx == NULL) return false;
    const char *key_expr = NULL;
    switch (key) {
        case 8u:  key_expr = "'Backspace'"; break;
        case 9u:  key_expr = "'Tab'"; break;
        case 13u: key_expr = "'Enter'"; break;
        case 27u: key_expr = "'Escape'"; break;
        case 32u: key_expr = "' '"; break;
        default: break;
    }
    char script[1536];
    int written;
    if (key_expr != NULL) {
        written = snprintf(script, sizeof(script),
            "(function(){var e={type:'keydown',key:%s,keyCode:%u,which:%u,bubbles:true,cancelable:true,defaultPrevented:false,preventDefault:function(){this.defaultPrevented=true;}};"
            "e.target=(typeof document!=='undefined'&&(document.activeElement||document))||null;"
            "var a=(globalThis._cos_event_listeners||{}).keydown||[];for(var i=0;i<a.length;i++){try{a[i](e);}catch(x){}}"
            "if(typeof window!=='undefined'&&typeof window.onkeydown==='function')window.onkeydown(e);})();",
            key_expr, key, key);
    } else {
        written = snprintf(script, sizeof(script),
            "(function(){var e={type:'keydown',key:String.fromCharCode(%u),keyCode:%u,which:%u,bubbles:true,cancelable:true,defaultPrevented:false,preventDefault:function(){this.defaultPrevented=true;}};"
            "e.target=(typeof document!=='undefined'&&(document.activeElement||document))||null;"
            "var a=(globalThis._cos_event_listeners||{}).keydown||[];for(var i=0;i<a.length;i++){try{a[i](e);}catch(x){}}"
            "if(typeof window!=='undefined'&&typeof window.onkeydown==='function')window.onkeydown(e);})();",
            key, key, key);
    }
    if (written < 0 || (size_t)written >= sizeof(script)) return false;
    return cos_js_eval_quiet(ctx, script, (size_t)written, "<browser-keydown>");
}
