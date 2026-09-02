/*
 * quickjs_jit.h - QuickJS マルチティアJITエンジン
 * 
 * Tier 1: インタプリタ（QuickJS標準）
 * Tier 2: TinyCCによるベースラインJIT（関数レベル）
 * Tier 3: 最適化JIT（ホットスポット検出後）
 */

#ifndef QUICKJS_JIT_H
#define QUICKJS_JIT_H

#include "quickjs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* JIT設定 */
#define COS_JIT_TIER1_THRESHOLD      0        /* インタプリタ（常時有効） */
#define COS_JIT_TIER2_THRESHOLD      5        /* 5回実行でTier2コンパイル */
#define COS_JIT_TIER3_THRESHOLD      100      /* 100回実行でTier3最適化 */
#define COS_JIT_CODE_CACHE_SIZE      4096     /* ネイティブコードキャッシュエントリ数 */
#define COS_JIT_MAX_NATIVE_SIZE      (2 * 1024 * 1024) /* 単一関数の最大ネイティブコードサイズ 2MiB */

/* JITコンパイルステータス */
typedef enum {
    COS_JIT_STATUS_NOT_COMPILED = 0,
    COS_JIT_STATUS_TIER2_COMPILING,
    COS_JIT_STATUS_TIER2_READY,
    COS_JIT_STATUS_TIER3_COMPILING,
    COS_JIT_STATUS_TIER3_READY,
    COS_JIT_STATUS_COMPILE_FAILED
} cos_jit_status_t;

/* 関数プロファイル情報 */
typedef struct {
    JSValue func_obj;           /* 関数オブジェクト */
    uint32_t invocation_count;  /* 呼び出し回数 */
    uint32_t total_time_us;     /* 総実行時間（マイクロ秒） */
    cos_jit_status_t status;    /* コンパイルステータス */
    void *native_code;          /* ネイティブコードポインタ */
    size_t native_code_size;    /* ネイティブコードサイズ */
    bool is_hotspot;            /* ホットスポットフラグ */
} cos_jit_function_profile_t;

/* JITコードキャッシュエントリ */
typedef struct {
    JSAtom func_atom;           /* 関数名アトム */
    cos_jit_function_profile_t profile;
    uint64_t last_used_tick;    /* 最終使用ティック */
    bool is_valid;              /* エントリ有効フラグ */
} cos_jit_cache_entry_t;

/* JITエンジンコンテキスト */
typedef struct {
    JSRuntime *rt;                          /* QuickJSランタイム */
    cos_jit_cache_entry_t *cache;           /* コードキャッシュ */
    size_t cache_count;                     /* キャッシュエントリ数 */
    size_t cache_capacity;                  /* キャッシュ容量 */
    uint64_t current_tick;                  /* 現在ティック */
    bool jit_enabled;                       /* JIT有効フラグ */
    bool tier3_enabled;                     /* Tier3最適化有効フラグ */
    size_t total_compiled_functions;        /* コンパイル済み関数総数 */
    size_t total_native_code_bytes;         /* 生成されたネイティブコード総バイト数 */
} cos_jit_context_t;

/* JITエンジン初期化 */
cos_jit_context_t* cos_jit_init(JSRuntime *rt);

/* JITエンジン終了 */
void cos_jit_destroy(cos_jit_context_t *jit_ctx);

/* 関数呼び出しフック（プロファイリング用） */
void cos_jit_on_function_call(cos_jit_context_t *jit_ctx, JSValueConst func_obj);

/* 関数のJITコンパイル要求 */
int cos_jit_compile_function(cos_jit_context_t *jit_ctx, JSContext *ctx, JSValueConst func_obj);

/* ネイティブコード実行（JITコールバック） */
JSValue cos_jit_execute_native(cos_jit_context_t *jit_ctx, JSContext *ctx, 
                                JSValueConst func_obj, int argc, JSValueConst *argv);

/* JIT統計情報の取得 */
void cos_jit_get_stats(cos_jit_context_t *jit_ctx, 
                       size_t *compiled_count, 
                       size_t *native_bytes,
                       size_t *cache_entries);

/* ホットスポット関数の検出 */
bool cos_jit_is_hotspot(cos_jit_context_t *jit_ctx, JSValueConst func_obj);

/* キャッシュのガベージコレクション */
void cos_jit_gc_cache(cos_jit_context_t *jit_ctx);

#endif /* QUICKJS_JIT_H */
