/*
 * quickjs_jit.c - QuickJS マルチティアJITエンジン実装
 * 
 * TinyCCを使用したユーザー空間Ring3 JITコンパイル
 * Tier 1: インタプリタ（QuickJS標準）
 * Tier 2: TinyCCによるベースラインJIT（関数レベル）
 * Tier 3: 最適化JIT（ホットスポット検出後）
 */

#include "quickjs_jit.h"
#include "quickjs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <libtcc.h>

/* デバッグ出力マクロ */
#ifdef COS_JIT_DEBUG
#define JIT_DEBUG(fmt, ...) fprintf(stderr, "[JIT] " fmt "\n", ##__VA_ARGS__)
#else
#define JIT_DEBUG(fmt, ...) ((void)0)
#endif

#define JIT_ERROR(fmt, ...) fprintf(stderr, "[JIT ERROR] " fmt "\n", ##__VA_ARGS__)

/* ==================== 内部構造体 ==================== */

/* コンパイル対象の関数情報 */
typedef struct {
    const char *source_code;    /* 関数のソースコード */
    size_t source_len;          /* ソースコード長 */
    const char *func_name;      /* 関数名 */
    JSValue func_obj;           /* 関数オブジェクト */
} cos_jit_compile_target_t;

/* ネイティブコードラッパー */
typedef JSValue (*cos_jit_native_func_t)(JSContext *ctx, JSValueConst this_val, 
                                          int argc, JSValueConst *argv);

/* ==================== ユーティリティ関数 ==================== */

static uint64_t get_current_tick(void) {
    static uint64_t tick = 0;
    return ++tick;
}

static void* jit_malloc(size_t size) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        JIT_ERROR("mmap failed: %d", errno);
        return NULL;
    }
    return ptr;
}

static void jit_free(void *ptr, size_t size) {
    if (ptr) {
        munmap(ptr, size);
    }
}

/* ==================== TinyCCコンパイラー統合 ==================== */

/* TinyCCを使用してCコードをネイティブコードにコンパイル */
static void* compile_with_tcc(const char *c_source, size_t source_len, 
                               size_t *out_size, char *error_buf, size_t error_buf_size) {
    TCCState *s = tcc_new();
    if (!s) {
        snprintf(error_buf, error_buf_size, "Failed to create TCC state");
        return NULL;
    }

    /* コンパイルオプション設定 */
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_options(s, "-O2 -shared -fPIC");

    /* エラーハンドラ設定 */
    tcc_set_error_func(s, error_buf, NULL);

    /* ソースコードをメモリ上でコンパイル */
    if (tcc_compile_string(s, c_source) == -1) {
        snprintf(error_buf, error_buf_size, "Compilation failed");
        tcc_delete(s);
        return NULL;
    }

    /* シンボル解決 */
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0) {
        snprintf(error_buf, error_buf_size, "Relocation failed");
        tcc_delete(s);
        return NULL;
    }

    /* コードサイズ取得 */
    int code_size = tcc_get_symbol(s, NULL);
    if (code_size <= 0) {
        /* シンボルを取得してサイズを計算 */
        void *sym_addr = NULL;
        /* ダミーシンボルでリロケーション完了を確認 */
        tcc_get_symbol(s, &sym_addr);
        code_size = 4096; /* デフォルトサイズ */
    }

    /* メモリにコードをコピー */
    void *code_mem = jit_malloc(code_size + 4096);
    if (!code_mem) {
        tcc_delete(s);
        return NULL;
    }

    /* 実際のコード生成（TCC_OUTPUT_MEMORY使用時） */
    int reloc_result = tcc_relocate(s, code_mem);
    if (reloc_result < 0) {
        jit_free(code_mem, code_size + 4096);
        tcc_delete(s);
        return NULL;
    }

    *out_size = code_size;
    tcc_delete(s);

    JIT_DEBUG("Compiled %zu bytes of C code to %d bytes of native code", 
              source_len, code_size);
    return code_mem;
}

/* QuickJS関数をCソースコードに変換（簡易版） */
static char* generate_c_wrapper(JSContext *ctx, JSValueConst func_obj, 
                                 const char *func_name, size_t *out_len) {
    /* 実際にはQuickJSのバイトコードを解析してCコードを生成する必要がある */
    /* ここでは簡易的なラッパー関数を生成 */
    
    char *c_code = malloc(2048);
    if (!c_code) return NULL;

    /* TODO: 実際の関数本体を逆コンパイルしてCコード生成 */
    /* 現時点ではスタブ関数を生成 */
    snprintf(c_code, 2048,
        "#include <quickjs.h>\n"
        "\n"
        "JSValue %s_jit_wrapper(JSContext *ctx, JSValueConst this_val,\n"
        "                        int argc, JSValueConst *argv) {\n"
        "    /* JIT-compiled function: %s */\n"
        "    /* TODO: Replace with actual compiled code */\n"
        "    return JS_Call(ctx, JS_UNDEFINED, this_val, argc, argv);\n"
        "}\n",
        func_name, func_name);

    *out_len = strlen(c_code);
    return c_code;
}

/* ==================== JITキャッシュ管理 ==================== */

static cos_jit_cache_entry_t* find_cache_entry(cos_jit_context_t *jit_ctx, 
                                                JSValueConst func_obj) {
    for (size_t i = 0; i < jit_ctx->cache_count; i++) {
        if (jit_ctx->cache[i].is_valid && 
            jit_ctx->cache[i].profile.func_obj == func_obj) {
            return &jit_ctx->cache[i];
        }
    }
    return NULL;
}

static cos_jit_cache_entry_t* allocate_cache_entry(cos_jit_context_t *jit_ctx) {
    if (jit_ctx->cache_count < jit_ctx->cache_capacity) {
        return &jit_ctx->cache[jit_ctx->cache_count++];
    }
    
    /* キャッシュがいっぱいの場合はGCを実行 */
    cos_jit_gc_cache(jit_ctx);
    
    if (jit_ctx->cache_count < jit_ctx->cache_capacity) {
        return &jit_ctx->cache[jit_ctx->cache_count++];
    }
    
    return NULL; /* まだいっぱい */
}

/* ==================== 公開API実装 ==================== */

cos_jit_context_t* cos_jit_init(JSRuntime *rt) {
    cos_jit_context_t *jit_ctx = calloc(1, sizeof(cos_jit_context_t));
    if (!jit_ctx) {
        JIT_ERROR("Failed to allocate JIT context");
        return NULL;
    }

    jit_ctx->rt = rt;
    jit_ctx->cache_capacity = COS_JIT_CODE_CACHE_SIZE;
    jit_ctx->cache_count = 0;
    jit_ctx->cache = calloc(jit_ctx->cache_capacity, sizeof(cos_jit_cache_entry_t));
    if (!jit_ctx->cache) {
        JIT_ERROR("Failed to allocate JIT cache");
        free(jit_ctx);
        return NULL;
    }

    jit_ctx->current_tick = get_current_tick();
    jit_ctx->jit_enabled = true;
    jit_ctx->tier3_enabled = true;
    jit_ctx->total_compiled_functions = 0;
    jit_ctx->total_native_code_bytes = 0;

    JIT_DEBUG("JIT engine initialized with cache size %zu", jit_ctx->cache_capacity);
    return jit_ctx;
}

void cos_jit_destroy(cos_jit_context_t *jit_ctx) {
    if (!jit_ctx) return;

    /* キャッシュエントリの解放 */
    for (size_t i = 0; i < jit_ctx->cache_count; i++) {
        cos_jit_cache_entry_t *entry = &jit_ctx->cache[i];
        if (entry->is_valid && entry->profile.native_code) {
            jit_free(entry->profile.native_code, entry->profile.native_code_size);
        }
        if (entry->func_atom) {
            /* JSAtomの解放はJSContextが必要なのでスキップ */
        }
    }

    free(jit_ctx->cache);
    free(jit_ctx);
    JIT_DEBUG("JIT engine destroyed");
}

void cos_jit_on_function_call(cos_jit_context_t *jit_ctx, JSValueConst func_obj) {
    if (!jit_ctx || !jit_ctx->jit_enabled) return;

    cos_jit_cache_entry_t *entry = find_cache_entry(jit_ctx, func_obj);
    
    if (!entry) {
        /* 新しい関数の場合、キャッシュエントリを確保 */
        entry = allocate_cache_entry(jit_ctx);
        if (!entry) return; /* キャッシュ満杯 */

        entry->is_valid = true;
        entry->profile.func_obj = func_obj;
        entry->profile.invocation_count = 0;
        entry->profile.total_time_us = 0;
        entry->profile.status = COS_JIT_STATUS_NOT_COMPILED;
        entry->profile.native_code = NULL;
        entry->profile.native_code_size = 0;
        entry->profile.is_hotspot = false;
        entry->last_used_tick = jit_ctx->current_tick;
    }

    entry->profile.invocation_count++;
    entry->last_used_tick = jit_ctx->current_tick++;

    /* Tier2コンパイル閾値到達 */
    if (entry->profile.status == COS_JIT_STATUS_NOT_COMPILED &&
        entry->profile.invocation_count >= COS_JIT_TIER2_THRESHOLD) {
        entry->profile.status = COS_JIT_STATUS_TIER2_COMPILING;
        JIT_DEBUG("Function call count %u reached Tier2 threshold", 
                  entry->profile.invocation_count);
    }

    /* Tier3コンパイル閾値到達 */
    if (jit_ctx->tier3_enabled &&
        entry->profile.status == COS_JIT_STATUS_TIER2_READY &&
        entry->profile.invocation_count >= COS_JIT_TIER3_THRESHOLD) {
        entry->profile.status = COS_JIT_STATUS_TIER3_COMPILING;
        entry->profile.is_hotspot = true;
        JIT_DEBUG("Function call count %u reached Tier3 threshold (hotspot)", 
                  entry->profile.invocation_count);
    }
}

int cos_jit_compile_function(cos_jit_context_t *jit_ctx, JSContext *ctx, 
                              JSValueConst func_obj) {
    if (!jit_ctx || !ctx) return -1;

    cos_jit_cache_entry_t *entry = find_cache_entry(jit_ctx, func_obj);
    if (!entry) return -1;

    if (entry->profile.status != COS_JIT_STATUS_TIER2_COMPILING &&
        entry->profile.status != COS_JIT_STATUS_TIER3_COMPILING) {
        return 0; /* コンパイル不要 */
    }

    /* 関数名の取得 */
    JSValue name_val = JS_GetPropertyStr(ctx, func_obj, "name");
    const char *func_name = "anonymous";
    if (!JS_IsUndefined(name_val)) {
        func_name = JS_ToCString(ctx, name_val);
    }

    JIT_DEBUG("Compiling function '%s' (Tier: %s)", func_name,
              entry->profile.status == COS_JIT_STATUS_TIER2_COMPILING ? "2" : "3");

    /* Cソースコード生成 */
    size_t c_source_len = 0;
    char *c_source = generate_c_wrapper(ctx, func_obj, func_name, &c_source_len);
    if (!c_source) {
        entry->profile.status = COS_JIT_STATUS_COMPILE_FAILED;
        if (name_val != JS_UNDEFINED) JS_FreeCString(ctx, func_name);
        JS_FreeValue(ctx, name_val);
        return -1;
    }

    /* TinyCCでコンパイル */
    char error_buf[512] = {0};
    size_t native_size = 0;
    void *native_code = compile_with_tcc(c_source, c_source_len, &native_size, 
                                          error_buf, sizeof(error_buf));
    
    free(c_source);

    if (!native_code) {
        JIT_ERROR("Compilation failed: %s", error_buf);
        entry->profile.status = COS_JIT_STATUS_COMPILE_FAILED;
        if (name_val != JS_UNDEFINED) JS_FreeCString(ctx, func_name);
        JS_FreeValue(ctx, name_val);
        return -1;
    }

    /* ネイティブコードをキャッシュに登録 */
    if (entry->profile.native_code) {
        jit_free(entry->profile.native_code, entry->profile.native_code_size);
    }
    entry->profile.native_code = native_code;
    entry->profile.native_code_size = native_size;
    entry->profile.status = (entry->profile.status == COS_JIT_STATUS_TIER2_COMPILING) ?
                             COS_JIT_STATUS_TIER2_READY : COS_JIT_STATUS_TIER3_READY;
    
    jit_ctx->total_compiled_functions++;
    jit_ctx->total_native_code_bytes += native_size;

    JIT_DEBUG("Successfully compiled '%s' to %zu bytes of native code", 
              func_name, native_size);

    if (name_val != JS_UNDEFINED) JS_FreeCString(ctx, func_name);
    JS_FreeValue(ctx, name_val);
    return 0;
}

JSValue cos_jit_execute_native(cos_jit_context_t *jit_ctx, JSContext *ctx,
                                JSValueConst func_obj, int argc, JSValueConst *argv) {
    if (!jit_ctx || !ctx) {
        return JS_Call(ctx, func_obj, JS_UNDEFINED, argc, argv);
    }

    cos_jit_cache_entry_t *entry = find_cache_entry(jit_ctx, func_obj);
    
    /* JITコンパイル済みコードがある場合 */
    if (entry && entry->profile.native_code && 
        (entry->profile.status == COS_JIT_STATUS_TIER2_READY ||
         entry->profile.status == COS_JIT_STATUS_TIER3_READY)) {
        
        /* ネイティブコードを実行 */
        cos_jit_native_func_t native_func = 
            (cos_jit_native_func_t)((char*)entry->profile.native_code);
        
        JIT_DEBUG("Executing native code for function (status=%d)", 
                  entry->profile.status);
        
        /* TODO: 実際のネイティブコード呼び出し */
        /* 現時点ではインタプリタにフォールバック */
    }

    /* プロファイリング更新 */
    cos_jit_on_function_call(jit_ctx, func_obj);

    /* JITコンパイルが要求されていれば非同期で実行 */
    if (entry && (entry->profile.status == COS_JIT_STATUS_TIER2_COMPILING ||
                  entry->profile.status == COS_JIT_STATUS_TIER3_COMPILING)) {
        cos_jit_compile_function(jit_ctx, ctx, func_obj);
    }

    /* インタプリタで実行 */
    return JS_Call(ctx, func_obj, JS_UNDEFINED, argc, argv);
}

void cos_jit_get_stats(cos_jit_context_t *jit_ctx, 
                       size_t *compiled_count, 
                       size_t *native_bytes,
                       size_t *cache_entries) {
    if (!jit_ctx) return;

    if (compiled_count) *compiled_count = jit_ctx->total_compiled_functions;
    if (native_bytes) *native_bytes = jit_ctx->total_native_code_bytes;
    if (cache_entries) *cache_entries = jit_ctx->cache_count;
}

bool cos_jit_is_hotspot(cos_jit_context_t *jit_ctx, JSValueConst func_obj) {
    if (!jit_ctx) return false;

    cos_jit_cache_entry_t *entry = find_cache_entry(jit_ctx, func_obj);
    return entry && entry->profile.is_hotspot;
}

void cos_jit_gc_cache(cos_jit_context_t *jit_ctx) {
    if (!jit_ctx || jit_ctx->cache_count == 0) return;

    uint64_t current_tick = jit_ctx->current_tick;
    size_t freed = 0;

    /* LRUポリシーで古いエントリを削除 */
    for (size_t i = 0; i < jit_ctx->cache_count; i++) {
        cos_jit_cache_entry_t *entry = &jit_ctx->cache[i];
        
        if (!entry->is_valid) continue;

        /* 長時間使用されていないエントリを削除 */
        if (current_tick - entry->last_used_tick > 10000) {
            if (entry->profile.native_code) {
                jit_free(entry->profile.native_code, entry->profile.native_code_size);
                jit_ctx->total_native_code_bytes -= entry->profile.native_code_size;
            }
            entry->is_valid = false;
            entry->profile.native_code = NULL;
            entry->profile.status = COS_JIT_STATUS_NOT_COMPILED;
            freed++;
        }
    }

    if (freed > 0) {
        JIT_DEBUG("GC freed %zu cache entries", freed);
    }
}
