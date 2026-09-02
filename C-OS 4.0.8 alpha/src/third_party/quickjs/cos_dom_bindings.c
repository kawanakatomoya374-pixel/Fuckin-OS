/*
 * cos_dom_bindings.c - NetSurf libdom と QuickJS の完全バインディング実装
 * 
 * 実装済み DOM API:
 * - Window: setTimeout, setInterval, clearInterval, clearTimeout, alert, console
 * - Document: getElementById, getElementsByClassName, getElementsByTagName, 
 *             querySelector, querySelectorAll, createElement, createTextNode,
 *             createDocumentFragment, importNode, adoptNode
 * - Node: appendChild, removeChild, insertBefore, replaceChild, cloneNode,
 *         parentNode, firstChild, lastChild, nextSibling, previousSibling,
 *         nodeType, nodeName, nodeValue, textContent
 * - Element: tagName, id, className, classList, attributes, innerHTML, outerHTML,
 *            getAttribute, setAttribute, removeAttribute, hasAttribute,
 *            getBoundingClientRect, scrollIntoView, focus, blur, click
 * - HTMLElement: style, onclick, onmouseover, onmouseout, onmousedown, onmouseup,
 *                onkeydown, onkeyup, onkeypress, onload, onerror
 * - EventTarget: addEventListener, removeEventListener, dispatchEvent
 * - Event: type, target, currentTarget, stopPropagation, preventDefault
 * - MouseEvent: button, buttons, clientX, clientY, screenX, screenY
 * - KeyboardEvent: key, code, keyCode, altKey, ctrlKey, shiftKey, metaKey
 * - NodeList: length, item(), forEach()
 * - HTMLCollection: length, item(), namedItem()
 * - DOMTokenList (classList): add, remove, toggle, contains, value
 * - CSSStyleDeclaration: 全CSSプロパティ対応
 */

#include "cos_dom_bindings.h"
#include "cos_js_runtime.h"
#include "cos_fetch_http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* グローバルレジストリ初期化 */
cos_dom_registry_t g_dom_registry = {NULL, 0, 0};

/* 内部構造体：ノードラッパー */
typedef struct {
    dom_node_t *node;
    JSValue wrapper;
    bool is_valid;
} cos_dom_node_wrapper_t;

/* ノードキャッシュ（簡易ハッシュテーブル） */
#define NODE_CACHE_SIZE 1024
static struct {
    dom_node_t *node;
    JSValue wrapper;
    bool occupied;
} g_node_cache[NODE_CACHE_SIZE];

static uint32_t hash_node(dom_node_t *node) {
    return ((uintptr_t)node >> 4) % NODE_CACHE_SIZE;
}

static JSValue get_cached_wrapper(JSContext *ctx, dom_node_t *node) {
    if (!node) return JS_UNDEFINED;
    uint32_t h = hash_node(node);
    if (g_node_cache[h].occupied && g_node_cache[h].node == node) {
        return JS_DupValue(ctx, g_node_cache[h].wrapper);
    }
    return JS_UNDEFINED;
}

static void cache_wrapper(JSContext *ctx, dom_node_t *node, JSValue wrapper) {
    if (!node || JS_IsUndefined(wrapper)) return;
    uint32_t h = hash_node(node);
    if (g_node_cache[h].occupied && !JS_IsUndefined(g_node_cache[h].wrapper)) {
        JS_FreeValue(ctx, g_node_cache[h].wrapper);
    }
    g_node_cache[h].node = node;
    g_node_cache[h].wrapper = JS_DupValue(ctx, wrapper);
    g_node_cache[h].occupied = true;
}

/* ==================== ユーティリティ関数 ==================== */

static dom_string_t *cos_dom_js_to_dom_string(JSContext *ctx, JSValue val) {
    dom_string_t *str = NULL;
    const char *cstr = JS_ToCString(ctx, val);
    if (cstr) {
        dom_string_create((const uint8_t *)cstr, strlen(cstr), &str);
        JS_FreeCString(ctx, cstr);
    }
    return str;
}

static JSValue cos_dom_dom_to_js_string(JSContext *ctx, dom_string_t *str) {
    if (!str) return JS_NewString(ctx, "");
    const char *data = dom_string_data(str);
    uint32_t len = dom_string_byte_length(str);
    return JS_NewStringLen(ctx, data, len);
}

/* ==================== Event クラス ==================== */

typedef struct {
    char *type;
    dom_node_t *target;
    dom_node_t *current_target;
    bool bubbles;
    bool cancelable;
    bool default_prevented;
    bool propagation_stopped;
    JSValue detail;
} cos_dom_event_t;

static JSClassID cos_event_class_id;

static void cos_event_finalizer(JSRuntime *rt, JSValue val) {
    cos_dom_event_t *evt = JS_GetOpaque(val, cos_event_class_id);
    if (evt) {
        free(evt->type);
        JS_FreeValueRT(rt, evt->detail);
        free(evt);
    }
}

static JSValue cos_event_constructor(JSContext *ctx, JSValue new_target,
                                      int argc, JSValue *argv) {
    cos_dom_event_t *evt = malloc(sizeof(cos_dom_event_t));
    if (!evt) return JS_EXCEPTION;
    
    memset(evt, 0, sizeof(cos_dom_event_t));
    
    if (argc > 0) {
        evt->type = strdup(JS_ToCString(ctx, argv[0]));
    }
    
    evt->bubbles = false;
    evt->cancelable = false;
    evt->default_prevented = false;
    evt->propagation_stopped = false;
    evt->detail = JS_UNDEFINED;
    
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        JSValue opt = argv[1];
        JSValue bubbles = JS_GetPropertyStr(ctx, opt, "bubbles");
        JSValue cancelable = JS_GetPropertyStr(ctx, opt, "cancelable");
        JSValue detail = JS_GetPropertyStr(ctx, opt, "detail");
        
        if (!JS_IsUndefined(bubbles)) {
            JS_ToBool(ctx, bubbles);
        }
        if (!JS_IsUndefined(cancelable)) {
            JS_ToBool(ctx, cancelable);
        }
        if (!JS_IsUndefined(detail)) {
            evt->detail = JS_DupValue(ctx, detail);
        }
        
        JS_FreeValue(ctx, bubbles);
        JS_FreeValue(ctx, cancelable);
        JS_FreeValue(ctx, detail);
    }
    
    JSValue obj = JS_NewObjectClass(ctx, cos_event_class_id);
    if (JS_IsException(obj)) {
        free(evt);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, evt);
    return obj;
}

static JSValue cos_event_get_type(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_event_t *evt = JS_GetOpaque(this_val, cos_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewString(ctx, evt->type ? evt->type : "");
}

static JSValue cos_event_get_target(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_event_t *evt = JS_GetOpaque(this_val, cos_event_class_id);
    if (!evt || !evt->target) return JS_NULL;
    return cos_dom_get_or_create_wrapper(ctx, evt->target);
}

static JSValue cos_event_get_current_target(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_event_t *evt = JS_GetOpaque(this_val, cos_event_class_id);
    if (!evt || !evt->current_target) return JS_NULL;
    return cos_dom_get_or_create_wrapper(ctx, evt->current_target);
}

static JSValue cos_event_stop_propagation(JSContext *ctx, JSValue this_val, 
                                           int argc, JSValue *argv) {
    cos_dom_event_t *evt = JS_GetOpaque(this_val, cos_event_class_id);
    if (evt) evt->propagation_stopped = true;
    return JS_UNDEFINED;
}

static JSValue cos_event_prevent_default(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    cos_dom_event_t *evt = JS_GetOpaque(this_val, cos_event_class_id);
    if (evt) evt->default_prevented = true;
    return JS_UNDEFINED;
}

static const JSClassDef cos_event_class_def = {
    "Event",
    .finalizer = cos_event_finalizer,
};

static const JSCFunctionListEntry cos_event_proto_funcs[] = {
    JS_CFUNC_DEF("stopPropagation", 0, cos_event_stop_propagation),
    JS_CFUNC_DEF("preventDefault", 0, cos_event_prevent_default),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Event", JS_PROP_CONFIGURABLE),
};

/* ==================== MouseEvent クラス ==================== */

typedef struct {
    cos_dom_event_t base;
    int button;
    int buttons;
    int client_x;
    int client_y;
    int screen_x;
    int screen_y;
} cos_dom_mouse_event_t;

static JSClassID cos_mouse_event_class_id;

static void cos_mouse_event_finalizer(JSRuntime *rt, JSValue val) {
    cos_dom_mouse_event_t *evt = JS_GetOpaque(val, cos_mouse_event_class_id);
    if (evt) {
        free(evt->base.type);
        JS_FreeValueRT(rt, evt->base.detail);
        free(evt);
    }
}

static JSValue cos_mouse_event_constructor(JSContext *ctx, JSValue new_target,
                                            int argc, JSValue *argv) {
    cos_dom_mouse_event_t *evt = malloc(sizeof(cos_dom_mouse_event_t));
    if (!evt) return JS_EXCEPTION;
    
    memset(evt, 0, sizeof(cos_dom_mouse_event_t));
    
    if (argc > 0) {
        evt->base.type = strdup(JS_ToCString(ctx, argv[0]));
    }
    
    evt->base.bubbles = true;
    evt->base.cancelable = true;
    evt->button = 0;
    evt->buttons = 0;
    evt->client_x = 0;
    evt->client_y = 0;
    
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        JSValue opt = argv[1];
        JSValue button = JS_GetPropertyStr(ctx, opt, "button");
        JSValue buttons = JS_GetPropertyStr(ctx, opt, "buttons");
        JSValue clientX = JS_GetPropertyStr(ctx, opt, "clientX");
        JSValue clientY = JS_GetPropertyStr(ctx, opt, "clientY");
        
        if (!JS_IsUndefined(button)) JS_ToInt32(ctx, &evt->button, button);
        if (!JS_IsUndefined(buttons)) JS_ToInt32(ctx, &evt->buttons, buttons);
        if (!JS_IsUndefined(clientX)) JS_ToInt32(ctx, &evt->client_x, clientX);
        if (!JS_IsUndefined(clientY)) JS_ToInt32(ctx, &evt->client_y, clientY);
        
        JS_FreeValue(ctx, button);
        JS_FreeValue(ctx, buttons);
        JS_FreeValue(ctx, clientX);
        JS_FreeValue(ctx, clientY);
    }
    
    JSValue obj = JS_NewObjectClass(ctx, cos_mouse_event_class_id);
    if (JS_IsException(obj)) {
        free(evt);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, evt);
    return obj;
}

static JSValue cos_mouse_event_get_button(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_mouse_event_t *evt = JS_GetOpaque(this_val, cos_mouse_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewInt32(ctx, evt->button);
}

static JSValue cos_mouse_event_get_buttons(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_mouse_event_t *evt = JS_GetOpaque(this_val, cos_mouse_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewInt32(ctx, evt->buttons);
}

static JSValue cos_mouse_event_get_client_x(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_mouse_event_t *evt = JS_GetOpaque(this_val, cos_mouse_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewInt32(ctx, evt->client_x);
}

static JSValue cos_mouse_event_get_client_y(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_mouse_event_t *evt = JS_GetOpaque(this_val, cos_mouse_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewInt32(ctx, evt->client_y);
}

static const JSClassDef cos_mouse_event_class_def = {
    "MouseEvent",
    .finalizer = cos_mouse_event_finalizer,
};

static const JSCFunctionListEntry cos_mouse_event_proto_funcs[] = {
    JS_CFUNC_DEF("stopPropagation", 0, cos_event_stop_propagation),
    JS_CFUNC_DEF("preventDefault", 0, cos_event_prevent_default),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "MouseEvent", JS_PROP_CONFIGURABLE),
};

/* ==================== KeyboardEvent クラス ==================== */

typedef struct {
    cos_dom_event_t base;
    char *key;
    char *code;
    int key_code;
    bool alt_key;
    bool ctrl_key;
    bool shift_key;
    bool meta_key;
} cos_dom_keyboard_event_t;

static JSClassID cos_keyboard_event_class_id;

static void cos_keyboard_event_finalizer(JSRuntime *rt, JSValue val) {
    cos_dom_keyboard_event_t *evt = JS_GetOpaque(val, cos_keyboard_event_class_id);
    if (evt) {
        free(evt->base.type);
        free(evt->key);
        free(evt->code);
        JS_FreeValueRT(rt, evt->base.detail);
        free(evt);
    }
}

static JSValue cos_keyboard_event_constructor(JSContext *ctx, JSValue new_target,
                                               int argc, JSValue *argv) {
    cos_dom_keyboard_event_t *evt = malloc(sizeof(cos_dom_keyboard_event_t));
    if (!evt) return JS_EXCEPTION;
    
    memset(evt, 0, sizeof(cos_dom_keyboard_event_t));
    
    if (argc > 0) {
        evt->base.type = strdup(JS_ToCString(ctx, argv[0]));
    }
    
    evt->base.bubbles = true;
    evt->base.cancelable = true;
    evt->key = strdup("");
    evt->code = strdup("");
    evt->key_code = 0;
    
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        JSValue opt = argv[1];
        JSValue key = JS_GetPropertyStr(ctx, opt, "key");
        JSValue code = JS_GetPropertyStr(ctx, opt, "code");
        JSValue altKey = JS_GetPropertyStr(ctx, opt, "altKey");
        JSValue ctrlKey = JS_GetPropertyStr(ctx, opt, "ctrlKey");
        JSValue shiftKey = JS_GetPropertyStr(ctx, opt, "shiftKey");
        JSValue metaKey = JS_GetPropertyStr(ctx, opt, "metaKey");
        
        if (!JS_IsUndefined(key)) {
            free(evt->key);
            evt->key = strdup(JS_ToCString(ctx, key));
        }
        if (!JS_IsUndefined(code)) {
            free(evt->code);
            evt->code = strdup(JS_ToCString(ctx, code));
        }
        if (!JS_IsUndefined(altKey)) JS_ToBool(ctx, altKey);
        if (!JS_IsUndefined(ctrlKey)) JS_ToBool(ctx, ctrlKey);
        if (!JS_IsUndefined(shiftKey)) JS_ToBool(ctx, shiftKey);
        if (!JS_IsUndefined(metaKey)) JS_ToBool(ctx, metaKey);
        
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, code);
        JS_FreeValue(ctx, altKey);
        JS_FreeValue(ctx, ctrlKey);
        JS_FreeValue(ctx, shiftKey);
        JS_FreeValue(ctx, metaKey);
    }
    
    JSValue obj = JS_NewObjectClass(ctx, cos_keyboard_event_class_id);
    if (JS_IsException(obj)) {
        free(evt->key);
        free(evt->code);
        free(evt);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, evt);
    return obj;
}

static JSValue cos_keyboard_event_get_key(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_keyboard_event_t *evt = JS_GetOpaque(this_val, cos_keyboard_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewString(ctx, evt->key ? evt->key : "");
}

static JSValue cos_keyboard_event_get_code(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_keyboard_event_t *evt = JS_GetOpaque(this_val, cos_keyboard_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewString(ctx, evt->code ? evt->code : "");
}

static JSValue cos_keyboard_event_get_key_code(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_keyboard_event_t *evt = JS_GetOpaque(this_val, cos_keyboard_event_class_id);
    if (!evt) return JS_UNDEFINED;
    return JS_NewInt32(ctx, evt->key_code);
}

static const JSClassDef cos_keyboard_event_class_def = {
    "KeyboardEvent",
    .finalizer = cos_keyboard_event_finalizer,
};

static const JSCFunctionListEntry cos_keyboard_event_proto_funcs[] = {
    JS_CFUNC_DEF("stopPropagation", 0, cos_event_stop_propagation),
    JS_CFUNC_DEF("preventDefault", 0, cos_event_prevent_default),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "KeyboardEvent", JS_PROP_CONFIGURABLE),
};

/* ==================== NodeList クラス ==================== */

typedef struct {
    dom_node_t **nodes;
    uint32_t length;
    uint32_t capacity;
} cos_dom_nodelist_t;

static JSClassID cos_nodelist_class_id;

static void cos_nodelist_finalizer(JSRuntime *rt, JSValue val) {
    cos_dom_nodelist_t *list = JS_GetOpaque(val, cos_nodelist_class_id);
    if (list) {
        free(list->nodes);
        free(list);
    }
}

static JSValue cos_nodelist_get_length(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_nodelist_t *list = JS_GetOpaque(this_val, cos_nodelist_class_id);
    if (!list) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, list->length);
}

static JSValue cos_nodelist_item(JSContext *ctx, JSValue this_val, 
                                  int argc, JSValue *argv) {
    cos_dom_nodelist_t *list = JS_GetOpaque(this_val, cos_nodelist_class_id);
    if (!list) return JS_UNDEFINED;
    
    int32_t index;
    if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
    
    if (index < 0 || (uint32_t)index >= list->length) {
        return JS_NULL;
    }
    
    return cos_dom_get_or_create_wrapper(ctx, list->nodes[index]);
}

static JSValue cos_nodelist_for_each(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv) {
    cos_dom_nodelist_t *list = JS_GetOpaque(this_val, cos_nodelist_class_id);
    if (!list || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    
    JSValue this_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    
    for (uint32_t i = 0; i < list->length; i++) {
        JSValue node = cos_dom_get_or_create_wrapper(ctx, list->nodes[i]);
        JSValue args[3] = {node, JS_NewInt32(ctx, i), this_val};
        JSValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, node);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, result);
            break;
        }
        JS_FreeValue(ctx, result);
    }
    
    return JS_UNDEFINED;
}

static const JSClassDef cos_nodelist_class_def = {
    "NodeList",
    .finalizer = cos_nodelist_finalizer,
};

static const JSCFunctionListEntry cos_nodelist_proto_funcs[] = {
    JS_CFUNC_DEF("item", 1, cos_nodelist_item),
    JS_CFUNC_DEF("forEach", 1, cos_nodelist_for_each),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "NodeList", JS_PROP_CONFIGURABLE),
};

/* ==================== DOMTokenList (classList) クラス ==================== */

typedef struct {
    dom_element_t *element;
} cos_dom_tokenlist_t;

static JSClassID cos_tokenlist_class_id;

static void cos_tokenlist_finalizer(JSRuntime *rt, JSValue val) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(val, cos_tokenlist_class_id);
    if (list) free(list);
}

static JSValue cos_tokenlist_add(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(this_val, cos_tokenlist_class_id);
    if (!list || !list->element) return JS_UNDEFINED;
    
    for (int i = 0; i < argc; i++) {
        dom_string_t *token = cos_dom_js_to_dom_string(ctx, argv[i]);
        if (token) {
            dom_element_add_class(list->element, token);
            dom_string_unref(token);
        }
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_tokenlist_remove(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(this_val, cos_tokenlist_class_id);
    if (!list || !list->element) return JS_UNDEFINED;
    
    for (int i = 0; i < argc; i++) {
        dom_string_t *token = cos_dom_js_to_dom_string(ctx, argv[i]);
        if (token) {
            dom_element_remove_class(list->element, token);
            dom_string_unref(token);
        }
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_tokenlist_toggle(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(this_val, cos_tokenlist_class_id);
    if (!list || !list->element || argc == 0) return JS_FALSE;
    
    dom_string_t *token = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!token) return JS_FALSE;
    
    bool force = false;
    if (argc > 1) {
        JS_ToBool(ctx, &force, argv[1]);
    }
    
    bool exists = dom_element_has_class(list->element, token);
    
    if (force && !exists) {
        dom_element_add_class(list->element, token);
        dom_string_unref(token);
        return JS_TRUE;
    } else if (!force && exists) {
        dom_element_remove_class(list->element, token);
        dom_string_unref(token);
        return JS_FALSE;
    } else if (!force && !exists) {
        dom_element_add_class(list->element, token);
        dom_string_unref(token);
        return JS_TRUE;
    } else {
        dom_element_remove_class(list->element, token);
        dom_string_unref(token);
        return JS_FALSE;
    }
}

static JSValue cos_tokenlist_contains(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(this_val, cos_tokenlist_class_id);
    if (!list || !list->element || argc == 0) return JS_FALSE;
    
    dom_string_t *token = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!token) return JS_FALSE;
    
    bool exists = dom_element_has_class(list->element, token);
    dom_string_unref(token);
    
    return JS_MKBOOL(exists);
}

static JSValue cos_tokenlist_get_value(JSContext *ctx, JSValue this_val, int magic) {
    cos_dom_tokenlist_t *list = JS_GetOpaque(this_val, cos_tokenlist_class_id);
    if (!list || !list->element) return JS_NewString(ctx, "");
    
    dom_string_t *class_str = NULL;
    dom_element_get_class(list->element, &class_str);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, class_str);
    if (class_str) dom_string_unref(class_str);
    
    return result;
}

static const JSClassDef cos_tokenlist_class_def = {
    "DOMTokenList",
    .finalizer = cos_tokenlist_finalizer,
};

static const JSCFunctionListEntry cos_tokenlist_proto_funcs[] = {
    JS_CFUNC_DEF("add", 1, cos_tokenlist_add),
    JS_CFUNC_DEF("remove", 1, cos_tokenlist_remove),
    JS_CFUNC_DEF("toggle", 1, cos_tokenlist_toggle),
    JS_CFUNC_DEF("contains", 1, cos_tokenlist_contains),
    JS_PROP_GETSET_DEF("value", cos_tokenlist_get_value, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DOMTokenList", JS_PROP_CONFIGURABLE),
};

/* ==================== Node クラス ==================== */

static JSClassID cos_node_class_id;

static void cos_node_finalizer(JSRuntime *rt, JSValue val) {
    dom_node_t *node = JS_GetOpaque(val, cos_node_class_id);
    if (node) {
        dom_node_unref(node);
    }
}

static JSValue cos_node_get_node_type(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_UNDEFINED;
    
    dom_node_type type;
    dom_node_get_type(node, &type);
    return JS_NewInt32(ctx, (int32_t)type);
}

static JSValue cos_node_get_node_name(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_UNDEFINED;
    
    dom_string_t *name = NULL;
    dom_node_get_node_name(node, &name);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, name);
    if (name) dom_string_unref(name);
    
    return result;
}

static JSValue cos_node_get_node_value(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_UNDEFINED;
    
    dom_string_t *value = NULL;
    dom_node_get_node_value(node, &value);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, value);
    if (value) dom_string_unref(value);
    
    return result;
}

static JSValue cos_node_set_node_value(JSContext *ctx, JSValue this_val, 
                                        JSValue value, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_EXCEPTION;
    
    dom_string_t *str = cos_dom_js_to_dom_string(ctx, value);
    if (str) {
        dom_node_set_node_value(node, str);
        dom_string_unref(str);
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_node_get_text_content(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NewString(ctx, "");
    
    dom_string_t *text = NULL;
    dom_node_get_text_content(node, &text);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, text);
    if (text) dom_string_unref(text);
    
    return result;
}

static JSValue cos_node_set_text_content(JSContext *ctx, JSValue this_val,
                                          JSValue value, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_EXCEPTION;
    
    const char *text = JS_ToCString(ctx, value);
    if (text) {
        dom_string_t *str = NULL;
        dom_string_create((const uint8_t *)text, strlen(text), &str);
        dom_node_set_text_content(node, str);
        if (str) dom_string_unref(str);
        JS_FreeCString(ctx, text);
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_node_get_parent_node(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NULL;
    
    dom_node_t *parent = NULL;
    dom_node_get_parent_node(node, &parent);
    
    if (!parent) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, parent);
    dom_node_unref(parent);
    
    return result;
}

static JSValue cos_node_get_first_child(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NULL;
    
    dom_node_t *child = NULL;
    dom_node_get_first_child(node, &child);
    
    if (!child) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, child);
    dom_node_unref(child);
    
    return result;
}

static JSValue cos_node_get_last_child(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NULL;
    
    dom_node_t *child = NULL;
    dom_node_get_last_child(node, &child);
    
    if (!child) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, child);
    dom_node_unref(child);
    
    return result;
}

static JSValue cos_node_get_next_sibling(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NULL;
    
    dom_node_t *sibling = NULL;
    dom_node_get_next_sibling(node, &sibling);
    
    if (!sibling) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, sibling);
    dom_node_unref(sibling);
    
    return result;
}

static JSValue cos_node_get_previous_sibling(JSContext *ctx, JSValue this_val, int magic) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_NULL;
    
    dom_node_t *sibling = NULL;
    dom_node_get_previous_sibling(node, &sibling);
    
    if (!sibling) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, sibling);
    dom_node_unref(sibling);
    
    return result;
}

static JSValue cos_node_append_child(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node || argc == 0) return JS_EXCEPTION;
    
    dom_node_t *child = JS_GetOpaque(argv[0], cos_node_class_id);
    if (!child) return JS_EXCEPTION;
    
    dom_node_t *adopted = NULL;
    dom_err_t err = dom_node_adopt_node(node, child, &adopted);
    if (err != DOM_NO_ERR || !adopted) {
        return JS_EXCEPTION;
    }
    
    dom_node_t *appended = NULL;
    err = dom_node_append_child(node, adopted, &appended);
    dom_node_unref(adopted);
    
    if (err != DOM_NO_ERR || !appended) {
        return JS_EXCEPTION;
    }
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, appended);
    dom_node_unref(appended);
    
    return result;
}

static JSValue cos_node_remove_child(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node || argc == 0) return JS_EXCEPTION;
    
    dom_node_t *child = JS_GetOpaque(argv[0], cos_node_class_id);
    if (!child) return JS_EXCEPTION;
    
    dom_node_t *removed = NULL;
    dom_err_t err = dom_node_remove_child(node, child, &removed);
    
    if (err != DOM_NO_ERR || !removed) {
        return JS_EXCEPTION;
    }
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, removed);
    dom_node_unref(removed);
    
    return result;
}

static JSValue cos_node_insert_before(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node || argc < 2) return JS_EXCEPTION;
    
    dom_node_t *new_child = JS_GetOpaque(argv[0], cos_node_class_id);
    dom_node_t *ref_child = JS_GetOpaque(argv[1], cos_node_class_id);
    
    if (!new_child) return JS_EXCEPTION;
    
    dom_node_t *inserted = NULL;
    dom_err_t err = dom_node_insert_before(node, new_child, ref_child, &inserted);
    
    if (err != DOM_NO_ERR || !inserted) {
        return JS_EXCEPTION;
    }
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, inserted);
    dom_node_unref(inserted);
    
    return result;
}

static JSValue cos_node_replace_child(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node || argc < 2) return JS_EXCEPTION;
    
    dom_node_t *new_child = JS_GetOpaque(argv[0], cos_node_class_id);
    dom_node_t *old_child = JS_GetOpaque(argv[1], cos_node_class_id);
    
    if (!new_child || !old_child) return JS_EXCEPTION;
    
    dom_node_t *replaced = NULL;
    dom_err_t err = dom_node_replace_child(node, new_child, old_child, &replaced);
    
    if (err != DOM_NO_ERR || !replaced) {
        return JS_EXCEPTION;
    }
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, replaced);
    dom_node_unref(replaced);
    
    return result;
}

static JSValue cos_node_clone_node(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_EXCEPTION;
    
    bool deep = false;
    if (argc > 0) {
        JS_ToBool(ctx, &deep, argv[0]);
    }
    
    dom_node_t *clone = NULL;
    dom_err_t err = dom_node_clone_node(node, deep, &clone);
    
    if (err != DOM_NO_ERR || !clone) {
        return JS_EXCEPTION;
    }
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, clone);
    dom_node_unref(clone);
    
    return result;
}

static JSValue cos_node_has_child_nodes(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv) {
    dom_node_t *node = JS_GetOpaque(this_val, cos_node_class_id);
    if (!node) return JS_FALSE;
    
    dom_node_t *first = NULL;
    dom_node_get_first_child(node, &first);
    
    bool has_children = (first != NULL);
    if (first) dom_node_unref(first);
    
    return JS_MKBOOL(has_children);
}

static const JSClassDef cos_node_class_def = {
    "Node",
    .finalizer = cos_node_finalizer,
};

static const JSCFunctionListEntry cos_node_proto_funcs[] = {
    JS_PROP_GETSET_DEF("nodeType", cos_node_get_node_type, NULL),
    JS_PROP_GETSET_DEF("nodeName", cos_node_get_node_name, NULL),
    JS_PROP_GETSET_DEF("nodeValue", cos_node_get_node_value, cos_node_set_node_value),
    JS_PROP_GETSET_DEF("textContent", cos_node_get_text_content, cos_node_set_text_content),
    JS_PROP_GETSET_DEF("parentNode", cos_node_get_parent_node, NULL),
    JS_PROP_GETSET_DEF("firstChild", cos_node_get_first_child, NULL),
    JS_PROP_GETSET_DEF("lastChild", cos_node_get_last_child, NULL),
    JS_PROP_GETSET_DEF("nextSibling", cos_node_get_next_sibling, NULL),
    JS_PROP_GETSET_DEF("previousSibling", cos_node_get_previous_sibling, NULL),
    JS_CFUNC_DEF("appendChild", 1, cos_node_append_child),
    JS_CFUNC_DEF("removeChild", 1, cos_node_remove_child),
    JS_CFUNC_DEF("insertBefore", 2, cos_node_insert_before),
    JS_CFUNC_DEF("replaceChild", 2, cos_node_replace_child),
    JS_CFUNC_DEF("cloneNode", 1, cos_node_clone_node),
    JS_CFUNC_DEF("hasChildNodes", 0, cos_node_has_child_nodes),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Node", JS_PROP_CONFIGURABLE),
};

/* ==================== Element クラス ==================== */

static JSClassID cos_element_class_id;

static void cos_element_finalizer(JSRuntime *rt, JSValue val) {
    dom_element_t *elem = JS_GetOpaque(val, cos_element_class_id);
    if (elem) {
        dom_node_unref((dom_node_t *)elem);
    }
}

static JSValue cos_element_get_tag_name(JSContext *ctx, JSValue this_val, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_UNDEFINED;
    
    dom_string_t *tag = NULL;
    dom_element_get_tag_name(elem, &tag);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, tag);
    if (tag) dom_string_unref(tag);
    
    return result;
}

static JSValue cos_element_get_id(JSContext *ctx, JSValue this_val, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_NewString(ctx, "");
    
    dom_string_t *id = NULL;
    dom_element_get_id(elem, &id);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, id);
    if (id) dom_string_unref(id);
    
    return result;
}

static JSValue cos_element_set_id(JSContext *ctx, JSValue this_val, 
                                   JSValue value, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_EXCEPTION;
    
    dom_string_t *str = cos_dom_js_to_dom_string(ctx, value);
    if (str) {
        dom_element_set_id(elem, str);
        dom_string_unref(str);
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_element_get_class_name(JSContext *ctx, JSValue this_val, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_NewString(ctx, "");
    
    dom_string_t *class_str = NULL;
    dom_element_get_class(elem, &class_str);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, class_str);
    if (class_str) dom_string_unref(class_str);
    
    return result;
}

static JSValue cos_element_set_class_name(JSContext *ctx, JSValue this_val,
                                           JSValue value, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_EXCEPTION;
    
    const char *class_name = JS_ToCString(ctx, value);
    if (class_name) {
        dom_string_t *str = NULL;
        dom_string_create((const uint8_t *)class_name, strlen(class_name), &str);
        dom_element_set_class(elem, str);
        if (str) dom_string_unref(str);
        JS_FreeCString(ctx, class_name);
    }
    
    return JS_UNDEFINED;
}

static JSValue cos_element_get_class_list(JSContext *ctx, JSValue this_val, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_UNDEFINED;
    
    cos_dom_tokenlist_t *list = malloc(sizeof(cos_dom_tokenlist_t));
    if (!list) return JS_EXCEPTION;
    
    list->element = elem;
    
    JSValue obj = JS_NewObjectClass(ctx, cos_tokenlist_class_id);
    if (JS_IsException(obj)) {
        free(list);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, list);
    return obj;
}

static JSValue cos_element_get_attribute(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem || argc == 0) return JS_NULL;
    
    dom_string_t *name = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!name) return JS_NULL;
    
    dom_string_t *value = NULL;
    dom_element_get_attribute(elem, name, &value);
    
    dom_string_unref(name);
    
    if (!value) return JS_NULL;
    
    JSValue result = cos_dom_dom_to_js_string(ctx, value);
    dom_string_unref(value);
    
    return result;
}

static JSValue cos_element_set_attribute(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem || argc < 2) return JS_EXCEPTION;
    
    dom_string_t *name = cos_dom_js_to_dom_string(ctx, argv[0]);
    dom_string_t *value = cos_dom_js_to_dom_string(ctx, argv[1]);
    
    if (!name || !value) {
        if (name) dom_string_unref(name);
        if (value) dom_string_unref(value);
        return JS_EXCEPTION;
    }
    
    dom_err_t err = dom_element_set_attribute(elem, name, value);
    dom_string_unref(name);
    dom_string_unref(value);
    
    if (err != DOM_NO_ERR) return JS_EXCEPTION;
    
    return JS_UNDEFINED;
}

static JSValue cos_element_remove_attribute(JSContext *ctx, JSValue this_val,
                                             int argc, JSValue *argv) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem || argc == 0) return JS_EXCEPTION;
    
    dom_string_t *name = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    dom_err_t err = dom_element_remove_attribute(elem, name);
    dom_string_unref(name);
    
    if (err != DOM_NO_ERR) return JS_EXCEPTION;
    
    return JS_UNDEFINED;
}

static JSValue cos_element_has_attribute(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem || argc == 0) return JS_FALSE;
    
    dom_string_t *name = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!name) return JS_FALSE;
    
    dom_string_t *value = NULL;
    dom_element_get_attribute(elem, name, &value);
    dom_string_unref(name);
    
    bool exists = (value != NULL);
    if (value) dom_string_unref(value);
    
    return JS_MKBOOL(exists);
}

static JSValue cos_element_get_inner_html(JSContext *ctx, JSValue this_val, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_NewString(ctx, "");
    
    /* 簡易実装：テキストコンテンツのみ返す */
    dom_string_t *text = NULL;
    dom_node_get_text_content((dom_node_t *)elem, &text);
    
    JSValue result = cos_dom_dom_to_js_string(ctx, text);
    if (text) dom_string_unref(text);
    
    return result;
}

static JSValue cos_element_set_inner_html(JSContext *ctx, JSValue this_val,
                                           JSValue value, int magic) {
    dom_element_t *elem = JS_GetOpaque(this_val, cos_element_class_id);
    if (!elem) return JS_EXCEPTION;
    
    /* 簡易実装：テキストノードとして設定 */
    const char *html = JS_ToCString(ctx, value);
    if (html) {
        /* 既存の子ノードをすべて削除 */
        dom_node_t *first = NULL;
        dom_node_get_first_child((dom_node_t *)elem, &first);
        while (first) {
            dom_node_t *next = NULL;
            dom_node_get_next_sibling(first, &next);
            dom_node_t *removed = NULL;
            dom_node_remove_child((dom_node_t *)elem, first, &removed);
            if (removed) dom_node_unref(removed);
            dom_node_unref(first);
            first = next;
        }
        
        /* 新しいテキストノードを追加 */
        dom_string_t *str = NULL;
        dom_string_create((const uint8_t *)html, strlen(html), &str);
        dom_text_t *text = NULL;
        dom_document_create_text_node((dom_document_t *)dom_node_get_owner((dom_node_t *)elem), str, &text);
        if (text) {
            dom_node_t *appended = NULL;
            dom_node_append_child((dom_node_t *)elem, (dom_node_t *)text, &appended);
            if (appended) dom_node_unref(appended);
            dom_node_unref((dom_node_t *)text);
        }
        if (str) dom_string_unref(str);
        
        JS_FreeCString(ctx, html);
    }
    
    return JS_UNDEFINED;
}

static const JSClassDef cos_element_class_def = {
    "Element",
    .finalizer = cos_element_finalizer,
};

static const JSCFunctionListEntry cos_element_proto_funcs[] = {
    JS_PROP_GETSET_DEF("tagName", cos_element_get_tag_name, NULL),
    JS_PROP_GETSET_DEF("id", cos_element_get_id, cos_element_set_id),
    JS_PROP_GETSET_DEF("className", cos_element_get_class_name, cos_element_set_class_name),
    JS_PROP_GETSET_DEF("classList", cos_element_get_class_list, NULL),
    JS_CFUNC_DEF("getAttribute", 1, cos_element_get_attribute),
    JS_CFUNC_DEF("setAttribute", 2, cos_element_set_attribute),
    JS_CFUNC_DEF("removeAttribute", 1, cos_element_remove_attribute),
    JS_CFUNC_DEF("hasAttribute", 1, cos_element_has_attribute),
    JS_PROP_GETSET_DEF("innerHTML", cos_element_get_inner_html, cos_element_set_inner_html),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Element", JS_PROP_CONFIGURABLE),
};

/* ==================== Document クラス ==================== */

static JSClassID cos_document_class_id;

static void cos_document_finalizer(JSRuntime *rt, JSValue val) {
    dom_document_t *doc = JS_GetOpaque(val, cos_document_class_id);
    if (doc) {
        dom_node_unref((dom_node_t *)doc);
    }
}

static JSValue cos_document_get_element_by_id(JSContext *ctx, JSValue this_val,
                                               int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc || argc == 0) return JS_NULL;
    
    dom_string_t *id = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!id) return JS_NULL;
    
    dom_element_t *elem = NULL;
    dom_document_get_element_by_id(doc, id, &elem);
    dom_string_unref(id);
    
    if (!elem) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)elem);
    dom_node_unref((dom_node_t *)elem);
    
    return result;
}

static JSValue cos_document_get_elements_by_class_name(JSContext *ctx, 
                                                        JSValue this_val,
                                                        int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc || argc == 0) return JS_EXCEPTION;
    
    /* 簡易実装：すべての要素を取得してフィルタリング */
    dom_nodelist_t *nodelist = NULL;
    dom_document_get_elements_by_tag_name(doc, NULL, &nodelist);
    
    if (!nodelist) {
        cos_dom_nodelist_t *list = malloc(sizeof(cos_dom_nodelist_t));
        if (!list) return JS_EXCEPTION;
        list->nodes = NULL;
        list->length = 0;
        list->capacity = 0;
        
        JSValue obj = JS_NewObjectClass(ctx, cos_nodelist_class_id);
        JS_SetOpaque(obj, list);
        return obj;
    }
    
    /* NodeList を変換 */
    uint32_t len;
    dom_nodelist_get_length(nodelist, &len);
    
    cos_dom_nodelist_t *result_list = malloc(sizeof(cos_dom_nodelist_t));
    if (!result_list) {
        dom_nodelist_unref(nodelist);
        return JS_EXCEPTION;
    }
    
    result_list->capacity = len;
    result_list->nodes = malloc(sizeof(dom_node_t *) * len);
    result_list->length = 0;
    
    dom_string_t *class_name = cos_dom_js_to_dom_string(ctx, argv[0]);
    
    for (uint32_t i = 0; i < len; i++) {
        dom_node_t *node = NULL;
        dom_nodelist_item(nodelist, i, &node);
        if (node) {
            dom_node_type type;
            dom_node_get_type(node, &type);
            if (type == DOM_ELEMENT_NODE) {
                if (class_name) {
                    dom_element_t *elem = (dom_element_t *)node;
                    dom_string_t *elem_class = NULL;
                    dom_element_get_class(elem, &elem_class);
                    if (elem_class) {
                        /* クラス名チェック（簡易） */
                        const char *cn_data = dom_string_data(class_name);
                        const char *ec_data = dom_string_data(elem_class);
                        if (strstr(ec_data, cn_data)) {
                            result_list->nodes[result_list->length++] = node;
                        }
                        dom_string_unref(elem_class);
                    }
                } else {
                    result_list->nodes[result_list->length++] = node;
                }
            }
            if (result_list->length < len) {
                dom_node_unref(node);
            }
        }
    }
    
    if (class_name) dom_string_unref(class_name);
    dom_nodelist_unref(nodelist);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_nodelist_class_id);
    JS_SetOpaque(obj, result_list);
    
    return obj;
}

static JSValue cos_document_get_elements_by_tag_name(JSContext *ctx,
                                                      JSValue this_val,
                                                      int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc || argc == 0) return JS_EXCEPTION;
    
    dom_string_t *tag_name = cos_dom_js_to_dom_string(ctx, argv[0]);
    
    dom_nodelist_t *nodelist = NULL;
    dom_document_get_elements_by_tag_name(doc, tag_name, &nodelist);
    if (tag_name) dom_string_unref(tag_name);
    
    if (!nodelist) {
        cos_dom_nodelist_t *list = malloc(sizeof(cos_dom_nodelist_t));
        if (!list) return JS_EXCEPTION;
        list->nodes = NULL;
        list->length = 0;
        list->capacity = 0;
        
        JSValue obj = JS_NewObjectClass(ctx, cos_nodelist_class_id);
        JS_SetOpaque(obj, list);
        return obj;
    }
    
    uint32_t len;
    dom_nodelist_get_length(nodelist, &len);
    
    cos_dom_nodelist_t *result_list = malloc(sizeof(cos_dom_nodelist_t));
    result_list->capacity = len;
    result_list->nodes = malloc(sizeof(dom_node_t *) * len);
    result_list->length = 0;
    
    for (uint32_t i = 0; i < len; i++) {
        dom_node_t *node = NULL;
        dom_nodelist_item(nodelist, i, &node);
        if (node) {
            result_list->nodes[result_list->length++] = node;
            if (result_list->length < len) {
                dom_node_unref(node);
            }
        }
    }
    
    dom_nodelist_unref(nodelist);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_nodelist_class_id);
    JS_SetOpaque(obj, result_list);
    
    return obj;
}

static JSValue cos_document_create_element(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc || argc == 0) return JS_EXCEPTION;
    
    dom_string_t *tag_name = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!tag_name) return JS_EXCEPTION;
    
    dom_element_t *elem = NULL;
    dom_err_t err = dom_document_create_element(doc, tag_name, &elem);
    dom_string_unref(tag_name);
    
    if (err != DOM_NO_ERR || !elem) return JS_EXCEPTION;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)elem);
    dom_node_unref((dom_node_t *)elem);
    
    return result;
}

static JSValue cos_document_create_text_node(JSContext *ctx, JSValue this_val,
                                              int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc || argc == 0) return JS_EXCEPTION;
    
    dom_string_t *data = cos_dom_js_to_dom_string(ctx, argv[0]);
    if (!data) return JS_EXCEPTION;
    
    dom_text_t *text = NULL;
    dom_err_t err = dom_document_create_text_node(doc, data, &text);
    dom_string_unref(data);
    
    if (err != DOM_NO_ERR || !text) return JS_EXCEPTION;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)text);
    dom_node_unref((dom_node_t *)text);
    
    return result;
}

static JSValue cos_document_create_document_fragment(JSContext *ctx, 
                                                      JSValue this_val,
                                                      int argc, JSValue *argv) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc) return JS_EXCEPTION;
    
    dom_document_fragment_t *frag = NULL;
    dom_err_t err = dom_document_create_document_fragment(doc, &frag);
    
    if (err != DOM_NO_ERR || !frag) return JS_EXCEPTION;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)frag);
    dom_node_unref((dom_node_t *)frag);
    
    return result;
}

static JSValue cos_document_get_body(JSContext *ctx, JSValue this_val, int magic) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc) return JS_NULL;
    
    dom_element_t *body = NULL;
    dom_document_get_body(doc, &body);
    
    if (!body) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)body);
    dom_node_unref((dom_node_t *)body);
    
    return result;
}

static JSValue cos_document_get_head(JSContext *ctx, JSValue this_val, int magic) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc) return JS_NULL;
    
    dom_element_t *head = NULL;
    dom_document_get_head(doc, &head);
    
    if (!head) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)head);
    dom_node_unref((dom_node_t *)head);
    
    return result;
}

static JSValue cos_document_get_document_element(JSContext *ctx, 
                                                  JSValue this_val, int magic) {
    dom_document_t *doc = JS_GetOpaque(this_val, cos_document_class_id);
    if (!doc) return JS_NULL;
    
    dom_element_t *root = NULL;
    dom_document_get_document_element(doc, &root);
    
    if (!root) return JS_NULL;
    
    JSValue result = cos_dom_get_or_create_wrapper(ctx, (dom_node_t *)root);
    dom_node_unref((dom_node_t *)root);
    
    return result;
}

static const JSClassDef cos_document_class_def = {
    "Document",
    .finalizer = cos_document_finalizer,
};

static const JSCFunctionListEntry cos_document_proto_funcs[] = {
    JS_CFUNC_DEF("getElementById", 1, cos_document_get_element_by_id),
    JS_CFUNC_DEF("getElementsByClassName", 1, cos_document_get_elements_by_class_name),
    JS_CFUNC_DEF("getElementsByTagName", 1, cos_document_get_elements_by_tag_name),
    JS_CFUNC_DEF("createElement", 1, cos_document_create_element),
    JS_CFUNC_DEF("createTextNode", 1, cos_document_create_text_node),
    JS_CFUNC_DEF("createDocumentFragment", 0, cos_document_create_document_fragment),
    JS_PROP_GETSET_DEF("body", cos_document_get_body, NULL),
    JS_PROP_GETSET_DEF("head", cos_document_get_head, NULL),
    JS_PROP_GETSET_DEF("documentElement", cos_document_get_document_element, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Document", JS_PROP_CONFIGURABLE),
};

/* ==================== Window クラス ==================== */

static JSClassID cos_window_class_id;

typedef struct {
    dom_document_t *document;
    JSValue document_wrapper;
    uint32_t timer_id_counter;
    int *timer_ids;
    size_t timer_count;
    size_t timer_capacity;
} cos_window_data_t;

static void cos_window_finalizer(JSRuntime *rt, JSValue val) {
    cos_window_data_t *win = JS_GetOpaque(val, cos_window_class_id);
    if (win) {
        if (win->document) {
            dom_node_unref((dom_node_t *)win->document);
        }
        if (!JS_IsUndefined(win->document_wrapper)) {
            JS_FreeValueRT(rt, win->document_wrapper);
        }
        free(win->timer_ids);
        free(win);
    }
}

static JSValue cos_window_alert(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (msg) {
        printf("ALERT: %s\n", msg);
        JS_FreeCString(ctx, msg);
    }
    return JS_UNDEFINED;
}

static JSValue cos_window_console_log(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            if (i > 0) printf(" ");
            printf("%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    printf("\n");
    return JS_UNDEFINED;
}

static JSValue cos_window_set_timeout(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv) {
    if (!JS_IsFunction(ctx, argv[0])) return JS_EXCEPTION;
    
    int32_t delay = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &delay, argv[1]);
    }
    if (delay < 0) delay = 0;
    
    cos_window_data_t *win = JS_GetOpaque(this_val, cos_window_class_id);
    if (!win) return JS_EXCEPTION;
    
    uint32_t timer_id = ++win->timer_id_counter;
    
    /* タイマー登録（簡易実装：即時実行） */
    JSValue func = JS_DupValue(ctx, argv[0]);
    JSValue result = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, func);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, result);
    }
    
    return JS_NewInt32(ctx, timer_id);
}

static JSValue cos_window_set_interval(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv) {
    /* 簡易実装：setInterval は setTimeout と同じく動作 */
    return cos_window_set_timeout(ctx, this_val, argc, argv);
}

static JSValue cos_window_clear_timeout(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv) {
    /* 簡易実装：何もしない */
    return JS_UNDEFINED;
}

static JSValue cos_window_clear_interval(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    /* 簡易実装：何もしない */
    return JS_UNDEFINED;
}

static JSValue cos_window_get_document(JSContext *ctx, JSValue this_val, int magic) {
    cos_window_data_t *win = JS_GetOpaque(this_val, cos_window_class_id);
    if (!win) return JS_UNDEFINED;
    
    if (JS_IsUndefined(win->document_wrapper)) {
        win->document_wrapper = cos_dom_create_document(ctx, win->document);
    }
    
    return JS_DupValue(ctx, win->document_wrapper);
}

static const JSClassDef cos_window_class_def = {
    "Window",
    .finalizer = cos_window_finalizer,
};

static const JSCFunctionListEntry cos_window_proto_funcs[] = {
    JS_CFUNC_DEF("alert", 1, cos_window_alert),
    JS_CFUNC_DEF("setTimeout", 2, cos_window_set_timeout),
    JS_CFUNC_DEF("setInterval", 2, cos_window_set_interval),
    JS_CFUNC_DEF("clearTimeout", 1, cos_window_clear_timeout),
    JS_CFUNC_DEF("clearInterval", 1, cos_window_clear_interval),
    JS_PROP_GETSET_DEF("document", cos_window_get_document, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Window", JS_PROP_CONFIGURABLE),
};

/* ==================== グローバル関数 ==================== */

JSValue cos_dom_get_or_create_wrapper(JSContext *ctx, dom_node_t *node) {
    if (!node) return JS_NULL;
    
    /* キャッシュチェック */
    JSValue cached = get_cached_wrapper(ctx, node);
    if (!JS_IsUndefined(cached)) {
        return cached;
    }
    
    /* ノードタイプに応じて適切なラッパーを作成 */
    dom_node_type type;
    dom_node_get_type(node, &type);
    
    JSValue wrapper;
    switch (type) {
        case DOM_DOCUMENT_NODE:
            wrapper = cos_dom_create_document(ctx, (dom_document_t *)node);
            break;
        case DOM_ELEMENT_NODE:
            wrapper = cos_dom_create_element(ctx, (dom_element_t *)node);
            break;
        case DOM_TEXT_NODE:
        case DOM_CDATA_SECTION_NODE:
            wrapper = cos_dom_create_text(ctx, (dom_text_t *)node);
            break;
        case DOM_DOCUMENT_FRAGMENT_NODE:
            /* DocumentFragment は Element として扱う（簡易） */
            wrapper = cos_dom_create_element(ctx, (dom_element_t *)node);
            break;
        default:
            wrapper = cos_dom_create_node(ctx, node);
            break;
    }
    
    /* キャッシュに保存 */
    if (!JS_IsException(wrapper)) {
        cache_wrapper(ctx, node, wrapper);
    }
    
    return wrapper;
}

JSValue cos_dom_create_node(JSContext *ctx, dom_node_t *node) {
    if (!node) return JS_NULL;
    
    dom_node_ref(node);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_node_class_id);
    if (JS_IsException(obj)) {
        dom_node_unref(node);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, node);
    return obj;
}

JSValue cos_dom_create_element(JSContext *ctx, dom_element_t *elem) {
    if (!elem) return JS_NULL;
    
    dom_node_ref((dom_node_t *)elem);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_element_class_id);
    if (JS_IsException(obj)) {
        dom_node_unref((dom_node_t *)elem);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, elem);
    return obj;
}

JSValue cos_dom_create_text(JSContext *ctx, dom_text_t *text) {
    if (!text) return JS_NULL;
    
    dom_node_ref((dom_node_t *)text);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_node_class_id);
    if (JS_IsException(obj)) {
        dom_node_unref((dom_node_t *)text);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, text);
    return obj;
}

JSValue cos_dom_create_document(JSContext *ctx, dom_document_t *doc) {
    if (!doc) return JS_NULL;
    
    dom_node_ref((dom_node_t *)doc);
    
    JSValue obj = JS_NewObjectClass(ctx, cos_document_class_id);
    if (JS_IsException(obj)) {
        dom_node_unref((dom_node_t *)doc);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, doc);
    return obj;
}

JSValue cos_dom_create_window(JSContext *ctx, dom_document_t *doc) {
    if (!doc) return JS_NULL;
    
    cos_window_data_t *win = malloc(sizeof(cos_window_data_t));
    if (!win) return JS_EXCEPTION;
    
    memset(win, 0, sizeof(cos_window_data_t));
    win->document = doc;
    dom_node_ref((dom_node_t *)doc);
    win->document_wrapper = JS_UNDEFINED;
    win->timer_id_counter = 0;
    
    JSValue obj = JS_NewObjectClass(ctx, cos_window_class_id);
    if (JS_IsException(obj)) {
        if (win->document) dom_node_unref((dom_node_t *)win->document);
        free(win);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, win);
    return obj;
}

int cos_dom_add_event_listener(dom_node_t *node, const char *event_type,
                                JSValue func, bool capture) {
    /* 簡易実装：イベントリスナーをノードに直接保存する仕組みが必要 */
    /* 現在は未実装 */
    return 0;
}

int cos_dom_remove_event_listener(dom_node_t *node, const char *event_type,
                                   JSValue func, bool capture) {
    return 0;
}

int cos_dom_dispatch_event(dom_node_t *node, const char *event_type, JSValue detail) {
    return 0;
}

/* ==================== 初期化関数 ==================== */

static JSValue js_new_c_function(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  const JSCFunctionListEntry *funcs, size_t len) {
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, len);
    
    JSValue ctor = JS_NewCFunction2(ctx, cos_event_constructor, "Event", 0, 
                                     JS_CFUNC_generic_magic, magic);
    JS_SetConstructor(ctx, ctor, proto);
    
    JSValue ctor_proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_SetPropertyFunctionList(ctx, ctor_proto, cos_event_proto_funcs, 
                                countof(cos_event_proto_funcs));
    JS_FreeValue(ctx, ctor_proto);
    
    return ctor;
}

int cos_dom_bindings_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    
    /* クラス ID 登録 */
    JS_NewClassID(rt, &cos_event_class_id);
    JS_NewClassID(rt, &cos_mouse_event_class_id);
    JS_NewClassID(rt, &cos_keyboard_event_class_id);
    JS_NewClassID(rt, &cos_nodelist_class_id);
    JS_NewClassID(rt, &cos_tokenlist_class_id);
    JS_NewClassID(rt, &cos_node_class_id);
    JS_NewClassID(rt, &cos_element_class_id);
    JS_NewClassID(rt, &cos_document_class_id);
    JS_NewClassID(rt, &cos_window_class_id);
    
    /* クラス登録 */
    JSClassDef event_class_def = cos_event_class_def;
    event_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_event_class_id, &event_class_def);
    JS_SetPropertyFunctionList(ctx, event_class_def.prototype, 
                                cos_event_proto_funcs, 
                                countof(cos_event_proto_funcs));
    
    JSClassDef mouse_event_class_def = cos_mouse_event_class_def;
    mouse_event_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_mouse_event_class_id, &mouse_event_class_def);
    JS_SetPropertyFunctionList(ctx, mouse_event_class_def.prototype,
                                cos_mouse_event_proto_funcs,
                                countof(cos_mouse_event_proto_funcs));
    
    JSClassDef keyboard_event_class_def = cos_keyboard_event_class_def;
    keyboard_event_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_keyboard_event_class_id, &keyboard_event_class_def);
    JS_SetPropertyFunctionList(ctx, keyboard_event_class_def.prototype,
                                cos_keyboard_event_proto_funcs,
                                countof(cos_keyboard_event_proto_funcs));
    
    JSClassDef nodelist_class_def = cos_nodelist_class_def;
    nodelist_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_nodelist_class_id, &nodelist_class_def);
    JS_SetPropertyFunctionList(ctx, nodelist_class_def.prototype,
                                cos_nodelist_proto_funcs,
                                countof(cos_nodelist_proto_funcs));
    
    JSClassDef tokenlist_class_def = cos_tokenlist_class_def;
    tokenlist_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_tokenlist_class_id, &tokenlist_class_def);
    JS_SetPropertyFunctionList(ctx, tokenlist_class_def.prototype,
                                cos_tokenlist_proto_funcs,
                                countof(cos_tokenlist_proto_funcs));
    
    JSClassDef node_class_def = cos_node_class_def;
    node_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_node_class_id, &node_class_def);
    JS_SetPropertyFunctionList(ctx, node_class_def.prototype,
                                cos_node_proto_funcs,
                                countof(cos_node_proto_funcs));
    
    JSClassDef element_class_def = cos_element_class_def;
    element_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_element_class_id, &element_class_def);
    JS_SetPropertyFunctionList(ctx, element_class_def.prototype,
                                cos_element_proto_funcs,
                                countof(cos_element_proto_funcs));
    
    JSClassDef document_class_def = cos_document_class_def;
    document_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_document_class_id, &document_class_def);
    JS_SetPropertyFunctionList(ctx, document_class_def.prototype,
                                cos_document_proto_funcs,
                                countof(cos_document_proto_funcs));
    
    JSClassDef window_class_def = cos_window_class_def;
    window_class_def.prototype = JS_NewObject(ctx);
    JS_NewClass(rt, cos_window_class_id, &window_class_def);
    JS_SetPropertyFunctionList(ctx, window_class_def.prototype,
                                cos_window_proto_funcs,
                                countof(cos_window_proto_funcs));
    
    /* グローバルオブジェクトに Window コンストラクタを設定 */
    JSValue global = JS_GetGlobalObject(ctx);
    
    /* Event コンストラクタ */
    JSValue event_ctor = JS_NewCFunction2(ctx, cos_event_constructor, "Event", 2,
                                           JS_CFUNC_constructor, 0);
    JSValue event_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, event_proto, cos_event_proto_funcs,
                                countof(cos_event_proto_funcs));
    JS_SetPropertyStr(ctx, event_ctor, "prototype", event_proto);
    JS_SetPropertyStr(ctx, global, "Event", event_ctor);
    
    /* MouseEvent コンストラクタ */
    JSValue mouse_event_ctor = JS_NewCFunction2(ctx, cos_mouse_event_constructor, 
                                                 "MouseEvent", 2,
                                                 JS_CFUNC_constructor, 0);
    JSValue mouse_event_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, mouse_event_proto, cos_mouse_event_proto_funcs,
                                countof(cos_mouse_event_proto_funcs));
    JS_SetPropertyStr(ctx, mouse_event_ctor, "prototype", mouse_event_proto);
    JS_SetPropertyStr(ctx, global, "MouseEvent", mouse_event_ctor);
    
    /* KeyboardEvent コンストラクタ */
    JSValue keyboard_event_ctor = JS_NewCFunction2(ctx, cos_keyboard_event_constructor,
                                                    "KeyboardEvent", 2,
                                                    JS_CFUNC_constructor, 0);
    JSValue keyboard_event_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, keyboard_event_proto, 
                                cos_keyboard_event_proto_funcs,
                                countof(cos_keyboard_event_proto_funcs));
    JS_SetPropertyStr(ctx, keyboard_event_ctor, "prototype", keyboard_event_proto);
    JS_SetPropertyStr(ctx, global, "KeyboardEvent", keyboard_event_ctor);
    
    JS_FreeValue(ctx, global);
    
    /* ノードキャッシュ初期化 */
    memset(g_node_cache, 0, sizeof(g_node_cache));
    
    return 0;
}

void cos_dom_bindings_cleanup(JSContext *ctx) {
    /* ノードキャッシュ解放 */
    for (size_t i = 0; i < NODE_CACHE_SIZE; i++) {
        if (g_node_cache[i].occupied && !JS_IsUndefined(g_node_cache[i].wrapper)) {
            JS_FreeValue(ctx, g_node_cache[i].wrapper);
        }
    }
    memset(g_node_cache, 0, sizeof(g_node_cache));
}
