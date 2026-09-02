#ifndef COS_DOM_BINDINGS_H
#define COS_DOM_BINDINGS_H

#include <quickjs.h>
#include <dom/dom.h>
#include "cos_js_runtime.h"

/* DOMバインディングコンテキスト */
typedef struct {
    JSContext *ctx;
    dom_document_t *document;
    dom_window_t *window;
    dom_node_t *root_node;
    uint32_t event_listener_count;
} cos_dom_binding_ctx_t;

/* グローバルレジストリ（複数ドキュメント対応） */
typedef struct {
    cos_dom_binding_ctx_t **contexts;
    size_t count;
    size_t capacity;
} cos_dom_registry_t;

extern cos_dom_registry_t g_dom_registry;

/* 初期化・終了処理 */
int cos_dom_bindings_init(JSContext *ctx);
void cos_dom_bindings_cleanup(JSContext *ctx);

/* Windowオブジェクト生成 */
JSValue cos_dom_create_window(JSContext *ctx, dom_document_t *doc);

/* Documentオブジェクト生成 */
JSValue cos_dom_create_document(JSContext *ctx, dom_document_t *doc);

/* Nodeオブジェクト生成 */
JSValue cos_dom_create_node(JSContext *ctx, dom_node_t *node);

/* Elementオブジェクト生成 */
JSValue cos_dom_create_element(JSContext *ctx, dom_element_t *elem);

/* Textノード生成 */
JSValue cos_dom_create_text(JSContext *ctx, dom_text_t *text);

/* イベントリスナー登録用ヘルパー */
int cos_dom_add_event_listener(dom_node_t *node, const char *event_type, 
                                JSValue func, bool capture);
int cos_dom_remove_event_listener(dom_node_t *node, const char *event_type, 
                                   JSValue func, bool capture);

/* DOM→JS変換キャッシュ */
JSValue cos_dom_get_or_create_wrapper(JSContext *ctx, dom_node_t *node);

/* イベントディスパッチ */
int cos_dom_dispatch_event(dom_node_t *node, const char *event_type, 
                           JSValue detail);

/* libdomエラーハンドリング */
#define COS_DOM_CHECK_ERR(err) \
    do { \
        if ((err) != DOM_NO_ERR) { \
            fprintf(stderr, "DOM error: %d at %s:%d\n", (err), __FILE__, __LINE__); \
        } \
    } while(0)

#endif /* COS_DOM_BINDINGS_H */
