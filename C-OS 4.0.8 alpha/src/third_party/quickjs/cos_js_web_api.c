/*
 * cos_js_web_api.c - bounded browser-shaped Web APIs for C-OS QuickJS.
 *
 * Remote scripts remain sandboxed: they can only queue HTTP(S) work and use
 * origin-keyed key/value storage.  No kernel pointer, filesystem path, raw
 * socket or privileged OS drawing interface is exposed to web content.
 */
#include "cos_js_web_api.h"

#include "memory.h"
#include "serial.h"
#include "http.h"
#include "api/fs_api.h"
#include "task.h"

extern void gui_request_redraw(void);

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define COS_WEB_CONTEXT_MAX       64
#define COS_WEB_REQUEST_MAX       128
#define COS_WEB_LOCAL_AREAS       64
#define COS_WEB_SESSION_AREAS     64
#define COS_WEB_STORAGE_ITEMS     512
#define COS_WEB_ORIGIN_MAX        512
#define COS_WEB_URL_MAX           HTTP_MAX_URL
#define COS_WEB_METHOD_MAX        16
#define COS_WEB_BODY_MAX          (64u * 1024u * 1024u)
#define COS_WEB_STORAGE_KEY_MAX   256
#define COS_WEB_STORAGE_VALUE_MAX (1024u * 1024u)
#define COS_WEB_STORAGE_QUOTA     (16u * 1024u * 1024u)
#define COS_WEB_STORAGE_FILE      "/.cos_web_storage_v1"
#define COS_WEB_STORE_MAGIC       0x43575331u /* CWS1 */

typedef struct {
    bool used;
    char key[COS_WEB_STORAGE_KEY_MAX];
    char value[COS_WEB_STORAGE_VALUE_MAX];
} cos_web_storage_item_t;

typedef struct {
    bool used;
    char origin[COS_WEB_ORIGIN_MAX];
    uintptr_t session_owner;
    cos_web_storage_item_t items[COS_WEB_STORAGE_ITEMS];
} cos_web_storage_area_t;

typedef struct {
    bool used;
    JSContext *ctx;
    uintptr_t session_owner;
    char origin[COS_WEB_ORIGIN_MAX];
    char page_url[COS_WEB_URL_MAX];
} cos_web_context_t;

typedef enum {
    COS_WEB_REQUEST_FETCH = 1,
    COS_WEB_REQUEST_XHR = 2,
} cos_web_request_kind_t;

typedef struct cos_web_request {
    bool used;
    bool cancelled;
    bool worker_queued;
    bool worker_running;
    bool transport_complete;
    bool transport_success;
    bool js_values_owned;
    cos_web_request_kind_t kind;
    JSContext *ctx;
    char url[COS_WEB_URL_MAX];
    char method[COS_WEB_METHOD_MAX];
    char body[COS_WEB_BODY_MAX];
    JSValue resolve;
    JSValue reject;
    JSValue xhr;
    http_client_t *http;
    int status;
    char content_type[128];
    struct cos_web_request *worker_next;
} cos_web_request_t;

typedef struct {
    uint32_t magic;
    cos_web_storage_area_t areas[COS_WEB_LOCAL_AREAS];
} cos_web_storage_disk_t;

static cos_web_context_t g_web_contexts[COS_WEB_CONTEXT_MAX];
static cos_web_request_t g_web_requests[COS_WEB_REQUEST_MAX];
static cos_web_storage_area_t g_local_areas[COS_WEB_LOCAL_AREAS];
static cos_web_storage_area_t g_session_areas[COS_WEB_SESSION_AREAS];
static bool g_local_loaded;
static cos_web_request_t *g_web_worker_head;
static cos_web_request_t *g_web_worker_tail;
static bool g_web_worker_started;
static volatile uint32_t g_web_worker_lock;

static void web_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) return;
    size_t i = 0;
    if (src != NULL) {
        while (src[i] != '\0' && i + 1 < dst_size) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static void web_worker_lock_acquire(void)
{
    while (__atomic_exchange_n(&g_web_worker_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        thread_yield();
    }
}

static void web_worker_lock_release(void)
{
    __atomic_store_n(&g_web_worker_lock, 0u, __ATOMIC_RELEASE);
}

static void web_worker_enqueue(cos_web_request_t *request)
{
    if (request == NULL) return;
    web_worker_lock_acquire();
    request->worker_next = NULL;
    if (g_web_worker_tail != NULL) g_web_worker_tail->worker_next = request;
    else g_web_worker_head = request;
    g_web_worker_tail = request;
    request->worker_queued = true;
    web_worker_lock_release();
}

static cos_web_request_t *web_worker_dequeue(void)
{
    web_worker_lock_acquire();
    cos_web_request_t *request = g_web_worker_head;
    if (request != NULL) {
        g_web_worker_head = request->worker_next;
        if (g_web_worker_head == NULL) g_web_worker_tail = NULL;
        request->worker_next = NULL;
        request->worker_queued = false;
        request->worker_running = true;
    }
    web_worker_lock_release();
    return request;
}

static bool web_starts_with(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL) return false;
    while (*prefix != '\0') {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static void web_upper_method(char *method)
{
    if (method == NULL) return;
    for (size_t i = 0; method[i] != '\0'; ++i) {
        if (method[i] >= 'a' && method[i] <= 'z') {
            method[i] = (char)(method[i] - 'a' + 'A');
        }
    }
}

static cos_web_context_t *web_context_for(JSContext *ctx, bool create)
{
    for (size_t i = 0; i < COS_WEB_CONTEXT_MAX; ++i) {
        if (g_web_contexts[i].used && g_web_contexts[i].ctx == ctx) {
            return &g_web_contexts[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < COS_WEB_CONTEXT_MAX; ++i) {
        if (!g_web_contexts[i].used) {
            memset(&g_web_contexts[i], 0, sizeof(g_web_contexts[i]));
            g_web_contexts[i].used = true;
            g_web_contexts[i].ctx = ctx;
            g_web_contexts[i].session_owner = (uintptr_t)ctx;
            return &g_web_contexts[i];
        }
    }
    return NULL;
}

static bool web_origin_from_url(const char *url, char *origin, size_t origin_size)
{
    if (url == NULL || origin == NULL || origin_size == 0) return false;
    /* C-OS file: documents live in the OS-owned storage sandbox and are opened
     * only through the bounded NetSurf file fetcher.  Give them one explicit
     * local origin so user-created/downloaded HTML can use localStorage and
     * sessionStorage without receiving a misleading SecurityError.  This is
     * deliberately not a host filesystem origin and never exposes a raw path
     * to script; the normal HTTP(S) origin split remains unchanged. */
    if (web_starts_with(url, "file:")) {
        const char *local_origin = "file://c-os-storage";
        size_t n = strlen(local_origin);
        if (n >= origin_size) return false;
        memcpy(origin, local_origin, n + 1);
        return true;
    }
    if (!web_starts_with(url, "http://") && !web_starts_with(url, "https://")) return false;
    const char *p = url;
    while (*p != '\0' && *p != ':') ++p;
    if (*p != ':') return false;
    p += 3; /* :// */
    if (*p == '\0') return false;
    const char *end = p;
    while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') ++end;
    size_t n = (size_t)(end - url);
    if (n == 0 || n >= origin_size) return false;
    memcpy(origin, url, n);
    origin[n] = '\0';
    return true;
}

static bool web_resolve_url(JSContext *ctx, const char *input, char *out, size_t out_size)
{
    if (input == NULL || input[0] == '\0' || out == NULL || out_size == 0) return false;
    if (web_starts_with(input, "http://") || web_starts_with(input, "https://") ||
        web_starts_with(input, "file:")) {
        web_copy(out, out_size, input);
        return strlen(input) < out_size;
    }
    cos_web_context_t *state = web_context_for(ctx, false);
    if (state == NULL || state->page_url[0] == '\0') return false;
    const char *base = state->page_url;
    const char *scheme = strstr(base, "://");
    if (scheme == NULL) return false;
    const char *authority = scheme + 3;
    const char *path = authority;
    while (*path != '\0' && *path != '/' && *path != '?' && *path != '#') ++path;

    /* Network-path, root-path, query-only and fragment-only references are
     * common in modern pages. Keep fragment/query removal separate so the
     * relative path case never uses stale page state. */
    if (input[0] == '/' && input[1] == '/') {
        size_t scheme_len = (size_t)(scheme - base + 1);
        if (scheme_len + strlen(input) + 1 > out_size) return false;
        memcpy(out, base, scheme_len);
        web_copy(out + scheme_len, out_size - scheme_len, input);
        return true;
    }
    if (input[0] == '/') {
        size_t root_len = (size_t)(path - base);
        if (root_len + strlen(input) + 1 > out_size) return false;
        memcpy(out, base, root_len);
        web_copy(out + root_len, out_size - root_len, input);
        return true;
    }
    const char *base_end = base + strlen(base);
    const char *fragment = strchr(base, '#');
    if (fragment != NULL) base_end = fragment;
    if (input[0] == '#') {
        size_t prefix_len = (size_t)(base_end - base);
        if (prefix_len + strlen(input) + 1 > out_size) return false;
        memcpy(out, base, prefix_len);
        web_copy(out + prefix_len, out_size - prefix_len, input);
        return true;
    }
    if (input[0] == '?') {
        const char *query = base;
        while (query < base_end && *query != '?') ++query;
        size_t prefix_len = (size_t)(query - base);
        if (prefix_len + strlen(input) + 1 > out_size) return false;
        memcpy(out, base, prefix_len);
        web_copy(out + prefix_len, out_size - prefix_len, input);
        return true;
    }
    const char *last = base_end;
    while (last > path && last[-1] != '/') --last;
    size_t dir_len = (size_t)(last - base);
    if (dir_len + strlen(input) + 1 > out_size) return false;
    memcpy(out, base, dir_len);
    web_copy(out + dir_len, out_size - dir_len, input);
    return true;
}

bool cos_js_web_get_page_url(JSContext *ctx, char *out, size_t out_size)
{
    cos_web_context_t *state = web_context_for(ctx, false);
    if (out == NULL || out_size == 0 || state == NULL || state->page_url[0] == '\0') return false;
    web_copy(out, out_size, state->page_url);
    return strlen(state->page_url) < out_size;
}

bool cos_js_web_resolve_page_url(JSContext *ctx, const char *input,
                                 char *out, size_t out_size)
{
    return web_resolve_url(ctx, input, out, out_size);
}

static cos_web_storage_area_t *web_storage_area(cos_web_context_t *state, bool local, bool create)
{
    if (state == NULL || state->origin[0] == '\0') return NULL;
    cos_web_storage_area_t *areas = local ? g_local_areas : g_session_areas;
    size_t count = local ? COS_WEB_LOCAL_AREAS : COS_WEB_SESSION_AREAS;
    for (size_t i = 0; i < count; ++i) {
        if (areas[i].used && strcmp(areas[i].origin, state->origin) == 0 &&
            (local || areas[i].session_owner == state->session_owner)) {
            return &areas[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (!areas[i].used) {
            memset(&areas[i], 0, sizeof(areas[i]));
            areas[i].used = true;
            areas[i].session_owner = local ? 0 : state->session_owner;
            web_copy(areas[i].origin, sizeof(areas[i].origin), state->origin);
            return &areas[i];
        }
    }
    return NULL;
}

static size_t web_storage_bytes(const cos_web_storage_area_t *area)
{
    size_t total = 0;
    if (area == NULL) return 0;
    for (size_t i = 0; i < COS_WEB_STORAGE_ITEMS; ++i) {
        if (area->items[i].used) {
            total += strlen(area->items[i].key) + strlen(area->items[i].value);
        }
    }
    return total;
}

static int web_storage_index(cos_web_storage_area_t *area, const char *key)
{
    if (area == NULL || key == NULL) return -1;
    for (size_t i = 0; i < COS_WEB_STORAGE_ITEMS; ++i) {
        if (area->items[i].used && strcmp(area->items[i].key, key) == 0) return (int)i;
    }
    return -1;
}

static void web_storage_load_local(void)
{
    if (g_local_loaded) return;
    cos_web_storage_disk_t disk;
    uint64_t read_size = 0;
    if (fs_api_read_file(COS_WEB_STORAGE_FILE, &disk, sizeof(disk), &read_size) &&
        read_size == sizeof(disk) && disk.magic == COS_WEB_STORE_MAGIC) {
        memcpy(g_local_areas, disk.areas, sizeof(g_local_areas));
    }
    /* A missing or invalid file means a clean persistent store. Do not retry
     * filesystem I/O on every Storage API call before the first mutation. */
    g_local_loaded = true;
}

static void web_storage_save_local(void)
{
    cos_web_storage_disk_t disk;
    disk.magic = COS_WEB_STORE_MAGIC;
    memcpy(disk.areas, g_local_areas, sizeof(g_local_areas));
    if (fs_api_write_file(COS_WEB_STORAGE_FILE, &disk, sizeof(disk))) {
        g_local_loaded = true;
    }
}

static JSValue web_storage_error(JSContext *ctx, const char *message)
{
    return JS_ThrowTypeError(ctx, "SecurityError: %s", message);
}

static bool web_storage_kind(JSContext *ctx, JSValueConst this_val, bool *local)
{
    JSValue kind = JS_GetPropertyStr(ctx, this_val, "_cos_storage_kind");
    const char *str = JS_ToCString(ctx, kind);
    bool ok = false;
    if (str != NULL) {
        if (strcmp(str, "local") == 0) { *local = true; ok = true; }
        else if (strcmp(str, "session") == 0) { *local = false; ok = true; }
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, kind);
    return ok;
}

static JSValue web_storage_get_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_NULL;
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    if (area == NULL) return web_storage_error(ctx, "storage is unavailable for this origin");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int index = web_storage_index(area, key);
    JSValue result = index >= 0 ? JS_NewString(ctx, area->items[index].value) : JS_NULL;
    JS_FreeCString(ctx, key);
    return result;
}

static JSValue web_storage_set_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_UNDEFINED;
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    if (area == NULL) return web_storage_error(ctx, "storage is unavailable for this origin");
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (key == NULL || value == NULL) {
        if (key) JS_FreeCString(ctx, key);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }
    if (strlen(key) >= COS_WEB_STORAGE_KEY_MAX || strlen(value) >= COS_WEB_STORAGE_VALUE_MAX) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        return JS_ThrowRangeError(ctx, "QuotaExceededError: storage item is too large");
    }
    int index = web_storage_index(area, key);
    if (index < 0) {
        for (size_t i = 0; i < COS_WEB_STORAGE_ITEMS; ++i) {
            if (!area->items[i].used) { index = (int)i; break; }
        }
    }
    size_t current = web_storage_bytes(area);
    size_t old = (index >= 0 && area->items[index].used) ?
        strlen(area->items[index].key) + strlen(area->items[index].value) : 0;
    size_t incoming = strlen(key) + strlen(value);
    if (index < 0 || current - old + incoming > COS_WEB_STORAGE_QUOTA) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        return JS_ThrowRangeError(ctx, "QuotaExceededError: origin storage quota exceeded");
    }
    area->items[index].used = true;
    web_copy(area->items[index].key, sizeof(area->items[index].key), key);
    web_copy(area->items[index].value, sizeof(area->items[index].value), value);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    if (local) web_storage_save_local();
    return JS_UNDEFINED;
}

static JSValue web_storage_remove_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_UNDEFINED;
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    if (area == NULL) return web_storage_error(ctx, "storage is unavailable for this origin");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int index = web_storage_index(area, key);
    if (index >= 0) memset(&area->items[index], 0, sizeof(area->items[index]));
    JS_FreeCString(ctx, key);
    if (local) web_storage_save_local();
    return JS_UNDEFINED;
}

static JSValue web_storage_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_UNDEFINED;
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    if (area == NULL) return web_storage_error(ctx, "storage is unavailable for this origin");
    memset(area->items, 0, sizeof(area->items));
    if (local) web_storage_save_local();
    return JS_UNDEFINED;
}

static JSValue web_storage_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_NULL;
    int32_t wanted = -1;
    if (JS_ToInt32(ctx, &wanted, argv[0]) < 0 || wanted < 0) return JS_NULL;
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    if (area == NULL) return JS_NULL;
    int found = 0;
    for (size_t i = 0; i < COS_WEB_STORAGE_ITEMS; ++i) {
        if (!area->items[i].used) continue;
        if (found++ == wanted) return JS_NewString(ctx, area->items[i].key);
    }
    return JS_NULL;
}

static JSValue web_storage_get_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    bool local = false;
    if (!web_storage_kind(ctx, this_val, &local)) return JS_NewInt32(ctx, 0);
    web_storage_load_local();
    cos_web_context_t *state = web_context_for(ctx, false);
    cos_web_storage_area_t *area = web_storage_area(state, local, true);
    int count = 0;
    if (area != NULL) {
        for (size_t i = 0; i < COS_WEB_STORAGE_ITEMS; ++i) if (area->items[i].used) ++count;
    }
    return JS_NewInt32(ctx, count);
}

static JSValue web_new_storage(JSContext *ctx, const char *kind)
{
    JSValue storage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, storage, "_cos_storage_kind", JS_NewString(ctx, kind));
    JS_SetPropertyStr(ctx, storage, "getItem", JS_NewCFunction(ctx, web_storage_get_item, "getItem", 1));
    JS_SetPropertyStr(ctx, storage, "setItem", JS_NewCFunction(ctx, web_storage_set_item, "setItem", 2));
    JS_SetPropertyStr(ctx, storage, "removeItem", JS_NewCFunction(ctx, web_storage_remove_item, "removeItem", 1));
    JS_SetPropertyStr(ctx, storage, "clear", JS_NewCFunction(ctx, web_storage_clear, "clear", 0));
    JS_SetPropertyStr(ctx, storage, "key", JS_NewCFunction(ctx, web_storage_key, "key", 1));
    JSAtom atom = JS_NewAtom(ctx, "length");
    JSValue getter = JS_NewCFunction(ctx, web_storage_get_length, "length", 0);
    if (JS_DefinePropertyGetSet(ctx, storage, atom, getter, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, getter);
    }
    JS_FreeAtom(ctx, atom);
    return storage;
}

static void web_dispatch_xhr(JSContext *ctx, JSValueConst xhr, const char *property)
{
    JSValue callback = JS_GetPropertyStr(ctx, xhr, property);
    if (JS_IsFunction(ctx, callback)) {
        JSValue result = JS_Call(ctx, callback, xhr, 0, NULL);
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, callback);
}

static void web_xhr_set_ready_state(JSContext *ctx, JSValueConst xhr, int state)
{
    JS_SetPropertyStr(ctx, (JSValue)xhr, "readyState", JS_NewInt32(ctx, state));
    web_dispatch_xhr(ctx, xhr, "onreadystatechange");
}

static JSValue web_response_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body");
    JSValue call = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, (JSValueConst *)&body);
    JS_FreeValue(ctx, call);
    JS_FreeValue(ctx, body);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue web_response_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue parse = JS_GetPropertyStr(ctx, json, "parse");
    JSValue parsed = JS_IsFunction(ctx, parse) ? JS_Call(ctx, parse, json, 1, (JSValueConst *)&body) : JS_EXCEPTION;
    if (JS_IsException(parsed)) {
        JSValue exc = JS_GetException(ctx);
        JSValue call = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, (JSValueConst *)&exc);
        JS_FreeValue(ctx, call);
        JS_FreeValue(ctx, exc);
    } else {
        JSValue call = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, (JSValueConst *)&parsed);
        JS_FreeValue(ctx, call);
    }
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, parse);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, body);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue web_header_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) return JS_EXCEPTION;
    JSValue result = JS_NULL;
    if ((name[0] == 'c' || name[0] == 'C') &&
        (name[1] == 'o' || name[1] == 'O') &&
        (name[2] == 'n' || name[2] == 'N') &&
        (name[3] == 't' || name[3] == 'T') &&
        (name[4] == 'e' || name[4] == 'E') &&
        (name[5] == 'n' || name[5] == 'N') &&
        (name[6] == 't' || name[6] == 'T') && name[7] == '-' &&
        (name[8] == 't' || name[8] == 'T') &&
        (name[9] == 'y' || name[9] == 'Y') &&
        (name[10] == 'p' || name[10] == 'P') &&
        (name[11] == 'e' || name[11] == 'E') && name[12] == '\0') {
        JSValue value = JS_GetPropertyStr(ctx, this_val, "_content_type");
        result = JS_IsUndefined(value) ? JS_NULL : value;
        if (JS_IsUndefined(value)) JS_FreeValue(ctx, value);
    }
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue web_make_response(JSContext *ctx, const char *url, int status,
                                 const char *body, size_t body_len, const char *content_type)
{
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, response, "_body", JS_NewStringLen(ctx, body ? body : "", body ? body_len : 0));
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, web_response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, web_response_json, "json", 0));
    JSValue headers = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, headers, "_content_type", JS_NewString(ctx, content_type ? content_type : ""));
    JS_SetPropertyStr(ctx, headers, "get", JS_NewCFunction(ctx, web_header_get, "get", 1));
    JS_SetPropertyStr(ctx, response, "headers", headers);
    return response;
}

static cos_web_request_t *web_alloc_request(void)
{
    for (size_t i = 0; i < COS_WEB_REQUEST_MAX; ++i) {
        if (!g_web_requests[i].used) {
            memset(&g_web_requests[i], 0, sizeof(g_web_requests[i]));
            g_web_requests[i].used = true;
            g_web_requests[i].resolve = JS_UNDEFINED;
            g_web_requests[i].reject = JS_UNDEFINED;
            g_web_requests[i].xhr = JS_UNDEFINED;
            return &g_web_requests[i];
        }
    }
    return NULL;
}

static void web_free_request(cos_web_request_t *request)
{
    if (request == NULL || !request->used) return;
    if (request->http != NULL) {
        http_destroy(request->http);
        request->http = NULL;
    }
    if (request->ctx != NULL && request->js_values_owned) {
        JS_FreeValue(request->ctx, request->resolve);
        JS_FreeValue(request->ctx, request->reject);
        JS_FreeValue(request->ctx, request->xhr);
    }
    memset(request, 0, sizeof(*request));
}

static JSValue web_xhr_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue xhr = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, xhr, "readyState", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, xhr, "status", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, xhr, "responseText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, xhr, "responseURL", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, xhr, "_method", JS_NewString(ctx, "GET"));
    JS_SetPropertyStr(ctx, xhr, "_url", JS_NewString(ctx, ""));
    return xhr;
}

static JSValue web_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "XMLHttpRequest.open requires method and URL");
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *input = JS_ToCString(ctx, argv[1]);
    if (method == NULL || input == NULL) {
        if (method) JS_FreeCString(ctx, method);
        if (input) JS_FreeCString(ctx, input);
        return JS_EXCEPTION;
    }
    if (argc >= 3 && !JS_ToBool(ctx, argv[2])) {
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, input);
        return JS_ThrowTypeError(ctx, "synchronous XMLHttpRequest is not supported");
    }
    char resolved[COS_WEB_URL_MAX];
    bool valid = web_resolve_url(ctx, input, resolved, sizeof(resolved));
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, input);
    if (!valid) return JS_ThrowTypeError(ctx, "XMLHttpRequest supports only HTTP(S) URLs");
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_method", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, (JSValue)this_val, "_url", JS_NewString(ctx, resolved));
    web_xhr_set_ready_state(ctx, this_val, 1);
    return JS_UNDEFINED;
}

static JSValue web_xhr_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    for (size_t i = 0; i < COS_WEB_REQUEST_MAX; ++i) {
        if (g_web_requests[i].used && g_web_requests[i].kind == COS_WEB_REQUEST_XHR &&
            g_web_requests[i].ctx == ctx && JS_IsStrictEqual(ctx, g_web_requests[i].xhr, this_val)) {
            g_web_requests[i].cancelled = true;
        }
    }
    JS_SetPropertyStr(ctx, (JSValue)this_val, "status", JS_NewInt32(ctx, 0));
    web_xhr_set_ready_state(ctx, this_val, 0);
    web_dispatch_xhr(ctx, this_val, "onabort");
    return JS_UNDEFINED;
}

static JSValue web_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue state = JS_GetPropertyStr(ctx, this_val, "readyState");
    int32_t ready = 0;
    (void)JS_ToInt32(ctx, &ready, state);
    JS_FreeValue(ctx, state);
    if (ready != 1) return JS_ThrowTypeError(ctx, "XMLHttpRequest must be opened before send");
    cos_web_request_t *request = web_alloc_request();
    if (request == NULL) return JS_ThrowRangeError(ctx, "network request queue is full");
    JSValue method = JS_GetPropertyStr(ctx, this_val, "_method");
    JSValue url = JS_GetPropertyStr(ctx, this_val, "_url");
    const char *method_c = JS_ToCString(ctx, method);
    const char *url_c = JS_ToCString(ctx, url);
    if (method_c == NULL || url_c == NULL || url_c[0] == '\0') {
        if (method_c) JS_FreeCString(ctx, method_c);
        if (url_c) JS_FreeCString(ctx, url_c);
        JS_FreeValue(ctx, method);
        JS_FreeValue(ctx, url);
        web_free_request(request);
        return JS_ThrowTypeError(ctx, "XMLHttpRequest has no valid URL");
    }
    request->kind = COS_WEB_REQUEST_XHR;
    request->ctx = ctx;
    web_copy(request->method, sizeof(request->method), method_c);
    web_upper_method(request->method);
    web_copy(request->url, sizeof(request->url), url_c);
    JS_FreeCString(ctx, method_c);
    JS_FreeCString(ctx, url_c);
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, url);
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        const char *body = JS_ToCString(ctx, argv[0]);
        if (body == NULL || strlen(body) >= sizeof(request->body)) {
            if (body) JS_FreeCString(ctx, body);
            web_free_request(request);
            return JS_ThrowRangeError(ctx, "XMLHttpRequest body is too large");
        }
        web_copy(request->body, sizeof(request->body), body);
        JS_FreeCString(ctx, body);
    }
    request->xhr = JS_DupValue(ctx, this_val);
    request->js_values_owned = true;
    web_xhr_set_ready_state(ctx, this_val, 2);
    return JS_UNDEFINED;
}

static JSValue web_xhr_set_request_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* Transport currently uses a fixed safe request-header profile. Exposing
     * the method as a no-op avoids breaking feature detection while preventing
     * hostile scripts from forging kernel-layer headers. */
    return JS_UNDEFINED;
}

static void web_install_xhr_methods(JSContext *ctx, JSValueConst xhr)
{
    JS_SetPropertyStr(ctx, (JSValue)xhr, "open", JS_NewCFunction(ctx, web_xhr_open, "open", 3));
    JS_SetPropertyStr(ctx, (JSValue)xhr, "send", JS_NewCFunction(ctx, web_xhr_send, "send", 1));
    JS_SetPropertyStr(ctx, (JSValue)xhr, "abort", JS_NewCFunction(ctx, web_xhr_abort, "abort", 0));
    JS_SetPropertyStr(ctx, (JSValue)xhr, "setRequestHeader", JS_NewCFunction(ctx, web_xhr_set_request_header, "setRequestHeader", 2));
}

static JSValue web_xhr_factory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue xhr = web_xhr_constructor(ctx, this_val, argc, argv);
    web_install_xhr_methods(ctx, xhr);
    return xhr;
}

static JSValue web_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    serial_puts("[QJS/Web] fetch() called\n");
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch requires a URL");
    const char *input = JS_ToCString(ctx, argv[0]);
    if (input == NULL) return JS_EXCEPTION;
    char resolved[COS_WEB_URL_MAX];
    bool valid = web_resolve_url(ctx, input, resolved, sizeof(resolved));
    JS_FreeCString(ctx, input);
    if (!valid) return JS_ThrowTypeError(ctx, "fetch supports only HTTP(S) URLs");
    cos_web_request_t *request = web_alloc_request();
    if (request == NULL) return JS_ThrowRangeError(ctx, "network request queue is full");
    request->kind = COS_WEB_REQUEST_FETCH;
    request->ctx = ctx;
    web_copy(request->url, sizeof(request->url), resolved);
    web_copy(request->method, sizeof(request->method), "GET");
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue method = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method)) {
            const char *method_c = JS_ToCString(ctx, method);
            if (method_c != NULL) {
                web_copy(request->method, sizeof(request->method), method_c);
                JS_FreeCString(ctx, method_c);
            }
        }
        JS_FreeValue(ctx, method);
        JSValue body = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            const char *body_c = JS_ToCString(ctx, body);
            if (body_c == NULL || strlen(body_c) >= sizeof(request->body)) {
                if (body_c) JS_FreeCString(ctx, body_c);
                JS_FreeValue(ctx, body);
                web_free_request(request);
                return JS_ThrowRangeError(ctx, "fetch body is too large");
            }
            web_copy(request->body, sizeof(request->body), body_c);
            JS_FreeCString(ctx, body_c);
        }
        JS_FreeValue(ctx, body);
    }
    web_upper_method(request->method);
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        web_free_request(request);
        return promise;
    }
    request->resolve = resolving[0];
    request->reject = resolving[1];
    request->js_values_owned = true;
    serial_puts("[QJS/Web] fetch() queued\n");
    return promise;
}

static void web_finish_fetch(cos_web_request_t *request, bool success, int status,
                             const char *body, size_t body_len, const char *content_type)
{
    JSContext *ctx = request->ctx;
    JSValue argument;
    if (success) {
        serial_puts("[QJS/Web] fetch() fulfilled\n");
        argument = web_make_response(ctx, request->url, status, body, body_len, content_type);
        JSValue call = JS_Call(ctx, request->resolve, JS_UNDEFINED, 1, (JSValueConst *)&argument);
        JS_FreeValue(ctx, call);
    } else {
        argument = JS_NewString(ctx, "NetworkError: request failed");
        JSValue call = JS_Call(ctx, request->reject, JS_UNDEFINED, 1, (JSValueConst *)&argument);
        JS_FreeValue(ctx, call);
    }
    JS_FreeValue(ctx, argument);
}

static void web_finish_xhr(cos_web_request_t *request, bool success, int status,
                           const char *body, size_t body_len)
{
    JSContext *ctx = request->ctx;
    if (request->cancelled) return;
    JS_SetPropertyStr(ctx, request->xhr, "status", JS_NewInt32(ctx, success ? status : 0));
    JS_SetPropertyStr(ctx, request->xhr, "responseURL", JS_NewString(ctx, success ? request->url : ""));
    JS_SetPropertyStr(ctx, request->xhr, "responseText", JS_NewStringLen(ctx, success && body ? body : "", success && body ? body_len : 0));
    web_xhr_set_ready_state(ctx, request->xhr, 4);
    web_dispatch_xhr(ctx, request->xhr, success ? "onload" : "onerror");
}

static void web_run_request_transport(cos_web_request_t *request)
{
    if (request == NULL || !request->used) return;
    if (request->cancelled) {
        __atomic_store_n(&request->worker_running, false, __ATOMIC_RELEASE);
        __atomic_store_n(&request->transport_complete, true, __ATOMIC_RELEASE);
        gui_request_redraw();
        return;
    }
    http_client_t *http = http_create();
    int rc = -1;
    if (http != NULL) {
        if (strcmp(request->method, "POST") == 0) rc = http_post(http, request->url, request->body);
        else if (strcmp(request->method, "GET") == 0) rc = http_get(http, request->url);
    }
    request->http = http;
    request->transport_success = (http != NULL && rc == 0);
    request->status = request->transport_success ? http_status_code(http) : 0;
    if (request->transport_success) {
        const char *type = http_get_header(http, "Content-Type");
        web_copy(request->content_type, sizeof(request->content_type), type);
    }
    __atomic_store_n(&request->worker_running, false, __ATOMIC_RELEASE);
    __atomic_store_n(&request->transport_complete, true, __ATOMIC_RELEASE);
    gui_request_redraw();
}

static void web_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        cos_web_request_t *request = web_worker_dequeue();
        if (request == NULL) {
            thread_yield();
            continue;
        }
        web_run_request_transport(request);
    }
}

static bool web_start_worker(void)
{
    if (g_web_worker_started) return true;
    thread_t *worker = thread_create_kernel("qjs_http", (void *)web_worker_main, NULL);
    if (worker == NULL) {
        serial_puts("[QJS/Web] HTTP worker creation failed; using owner fallback\n");
        return false;
    }
    g_web_worker_started = true;
    serial_puts("[QJS/Web] asynchronous HTTP worker online\n");
    return true;
}

static void web_process_completed_request(cos_web_request_t *request)
{
    if (request == NULL || !request->used) return;
    if (!request->cancelled && request->transport_complete) {
        const char *body = request->transport_success && request->http != NULL ?
            http_response_body(request->http) : NULL;
        size_t body_len = request->transport_success && request->http != NULL ?
            (size_t)http_response_length(request->http) : 0;
        if (request->kind == COS_WEB_REQUEST_FETCH) {
            web_finish_fetch(request, request->transport_success, request->status,
                             body, body_len, request->content_type);
        } else {
            web_finish_xhr(request, request->transport_success, request->status,
                           body, body_len);
        }
    }
    web_free_request(request);
}

void cos_js_pump_web_requests(void)
{
    bool worker = web_start_worker();
    for (size_t i = 0; i < COS_WEB_REQUEST_MAX; ++i) {
        cos_web_request_t *request = &g_web_requests[i];
        if (!request->used) continue;
        if (request->cancelled) {
            if (!request->worker_running) web_free_request(request);
            continue;
        }
        if (!worker) {
            if (!request->transport_complete) web_run_request_transport(request);
        } else         if (!request->worker_queued && !request->worker_running &&
                   !request->transport_complete) {
            serial_puts("[QJS/Web] dispatching request to worker\n");
            web_worker_enqueue(request);
        }
        if (request->transport_complete && !request->worker_running) {
            web_process_completed_request(request);
        }
    }
}

void cos_js_cancel_context_web_state(JSContext *ctx)
{
    if (ctx == NULL) return;
    for (size_t i = 0; i < COS_WEB_REQUEST_MAX; ++i) {
        if (g_web_requests[i].used && g_web_requests[i].ctx == ctx) {
            cos_web_request_t *request = &g_web_requests[i];
            if (request->worker_queued || request->worker_running) {
                request->cancelled = true;
                /* The worker may finish after the JS context is gone. Detach
                 * all JS-owned values now; it only needs the transport fields. */
                request->ctx = NULL;
                request->js_values_owned = false;
                request->resolve = JS_UNDEFINED;
                request->reject = JS_UNDEFINED;
                request->xhr = JS_UNDEFINED;
            } else {
                web_free_request(request);
            }
        }
    }
    for (size_t i = 0; i < COS_WEB_CONTEXT_MAX; ++i) {
        if (g_web_contexts[i].used && g_web_contexts[i].ctx == ctx) {
            memset(&g_web_contexts[i], 0, sizeof(g_web_contexts[i]));
        }
    }
}

void cos_js_web_set_origin(JSContext *ctx, const char *url)
{
    cos_web_context_t *state = web_context_for(ctx, true);
    if (state == NULL) return;
    web_copy(state->page_url, sizeof(state->page_url), url);
    if (!web_origin_from_url(url, state->origin, sizeof(state->origin))) {
        state->origin[0] = '\0';
    }
    /* Validation-only diagnostic for page binding; retained until both local
     * and remote origins are proven to reach Storage consistently. */
    serial_puts("[QJS/Web] origin url=");
    serial_puts(state->page_url[0] ? state->page_url : "(empty)");
    serial_puts(" key=");
    serial_puts(state->origin[0] ? state->origin : "(opaque)");
    serial_puts("\n");
}

void cos_js_web_install(JSContext *ctx)
{
    if (ctx == NULL) return;
    if (web_context_for(ctx, true) == NULL) {
        serial_puts("[QJS/Web] context table exhausted\n");
        return;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, web_fetch, "fetch", 2));
    JSValue xhr_ctor = JS_NewCFunction2(ctx, web_xhr_factory, "XMLHttpRequest", 0,
                                        JS_CFUNC_constructor_or_func, 0);
    JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);
    JS_SetPropertyStr(ctx, global, "localStorage", web_new_storage(ctx, "local"));
    JS_SetPropertyStr(ctx, global, "sessionStorage", web_new_storage(ctx, "session"));
    JS_FreeValue(ctx, global);
}
