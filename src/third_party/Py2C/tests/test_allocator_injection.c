/*
 * 変換器コア / ランタイムへのアロケータ注入の回帰。
 *
 * 自作OSで変換器をサービスとして常駐させる場合、
 *   1. カーネルのヒープ（kmalloc/kfree、arena、slab）を唯一のヒープにする
 *   2. 変換器自身の静的バッファ（P2C_COMPILER_FALLBACK_HEAP_SIZE, 既定64KiB）に
 *      依存しない
 * ことが要件になる。ここでは
 *   - p2c_platform_set_allocator() … カーネルのアロケータ関数だけを1回で注入
 *     （共有ヒープ = ランタイム + 変換器コアの内部確保がすべてここを通る）
 *   - p2c_set_default_allocator()  … P2C_Allocator を明示注入
 * の両方を検証し、注入が有効な限り静的フォールバックが使われないこと、
 * 注入アロケータが確保に失敗する場合だけ静的フォールバックへ落ちることを
 * 確認する。
 *
 * 変換結果のC文字列だけは raw malloc/free（ホストでは libc、カーネル構成では
 * カーネルの malloc）で管理される契約なので、注入アロケータとは独立に
 * free() で解放する。
 */
#include "core/python_code_to_c.h"
#include "platform/python_code_to_c_platform.h"
#include "runtime/python_code_to_c_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, code) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return (code); } } while (0)

/* --- 注入するアロケータ（ヘッダ付きカウンタ） --- */

typedef struct {
    size_t             size;
    unsigned long long guard;
} TestHeader;

#define TEST_GUARD 0x5032434845415041ull /* "P2CHEAPA" */

static size_t g_alloc_calls   = 0;
static size_t g_realloc_calls = 0;
static size_t g_free_calls    = 0;
static size_t g_live_blocks   = 0;
static size_t g_bad_free      = 0;
static int    g_fail_alloc    = 0;

static void *raw_alloc(size_t size) {
    g_alloc_calls++;
    if (g_fail_alloc) return NULL;
    TestHeader *hdr = (TestHeader*)malloc(sizeof(TestHeader) + (size ? size : 1u));
    if (!hdr) return NULL;
    hdr->size  = size;
    hdr->guard = TEST_GUARD;
    g_live_blocks++;
    return (void*)(hdr + 1);
}

static void *raw_realloc(void *ptr, size_t new_size) {
    g_realloc_calls++;
    if (g_fail_alloc) return NULL;
    if (!ptr) return raw_alloc(new_size);
    TestHeader *hdr = ((TestHeader*)ptr) - 1;
    if (hdr->guard != TEST_GUARD) { g_bad_free++; return NULL; }
    TestHeader *grown = (TestHeader*)realloc(hdr, sizeof(TestHeader) + (new_size ? new_size : 1u));
    if (!grown) return NULL;
    grown->size = new_size;
    return (void*)(grown + 1);
}

static void raw_free(void *ptr) {
    if (!ptr) return;
    TestHeader *hdr = ((TestHeader*)ptr) - 1;
    g_free_calls++;
    if (hdr->guard != TEST_GUARD) { g_bad_free++; return; }
    hdr->guard = 0;
    g_live_blocks--;
    free(hdr);
}

/* P2C_Allocator 版（変換器コアの既定アロケータ） */
static void *injected_alloc(void *ctx, size_t size) { (void)ctx; return raw_alloc(size); }
static void *injected_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx; (void)old_size;
    return raw_realloc(ptr, new_size);
}
static void injected_free(void *ctx, void *ptr) { (void)ctx; raw_free(ptr); }
static P2C_Allocator injected_allocator = { NULL, injected_alloc, injected_free, injected_realloc };

/* P2C_Platform 版（カーネルの kmalloc/krealloc/kfree 相当） */
static void *plat_alloc(size_t size, void *user) { (void)user; return raw_alloc(size); }
static void *plat_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)old_size; (void)user;
    return raw_realloc(ptr, new_size);
}
static void plat_free(void *ptr, size_t size, void *user) { (void)size; (void)user; raw_free(ptr); }

static const char *SOURCE =
    "def add(a, b):\n"
    "    return a + b\n"
    "print(add(20, 22))\n";

/* 変換が成功し、生成コードが得られること。結果のC文字列は raw malloc/free。 */
static int transpile_ok(void) {
    char *code = NULL;
    P2C_Result result = python_to_c(SOURCE, NULL, &code);
    if (result != P2C_OK) { printf("transpile failed: %d\n", (int)result); return 0; }
    int ok = (code != NULL && strstr(code, "p2c_") != NULL);
    free(code);
    return ok;
}

int main(void) {
    p2c_runtime_init(NULL, 0);
    CHECK(p2c_heap_usable(), 10); /* ホストは libc のヒープが使える */

    /* ---- 1. P2C_Allocator の明示注入：変換器コアがそれを使う ---- */
    p2c_set_default_allocator(&injected_allocator);
    const size_t before = g_alloc_calls;
    CHECK(transpile_ok(), 11);
    CHECK(g_alloc_calls > before, 12);                        /* 注入アロケータが使われた */
    CHECK(!p2c_core_static_allocator_active(), 13);           /* 静的フォールバックは未使用 */
    CHECK(g_bad_free == 0, 14);                               /* 他アロケータのポインタを解放していない */
    CHECK(g_free_calls > 0, 15);
    p2c_set_default_allocator(NULL);
    CHECK(g_bad_free == 0, 16);

    /* ---- 2. 確保に失敗するアロケータ：静的フォールバックで変換は成立する ---- */
    g_fail_alloc = 1;
    p2c_set_default_allocator(&injected_allocator);
    const size_t fail_before = g_alloc_calls;
    CHECK(transpile_ok(), 17);
    CHECK(g_alloc_calls > fail_before, 18);                   /* プローブで1回は試す */
    CHECK(p2c_core_static_allocator_active(), 19);            /* フォールバックへ落ちた */
    CHECK(g_bad_free == 0, 20);
    g_fail_alloc = 0;
    p2c_set_default_allocator(NULL);

    /* ---- 3. カーネルのアロケータを1回で注入（共有ヒープ経由） ---- */
    p2c_platform_set_allocator(plat_alloc, plat_realloc, plat_free, NULL);
    CHECK(p2c_heap_uses_platform(), 21);
    const size_t heap_before = g_alloc_calls;
    void *probe = p2c_heap_alloc(128);
    CHECK(probe != NULL, 22);
    CHECK(g_alloc_calls > heap_before, 23);                   /* p2c_heap_alloc が注入ヒープを使う */
    p2c_heap_free(probe);
    CHECK(g_bad_free == 0, 24);

    /* 変換器コアとランタイムの両方が同じヒープを使う */
    const size_t shared_before = g_alloc_calls;
    CHECK(transpile_ok(), 25);
    CHECK(!p2c_core_static_allocator_active(), 26);
    CHECK(g_alloc_calls > shared_before, 27);
    P2C_Object *obj = p2c_obj_from_int(7);
    CHECK(obj != NULL, 28);
    CHECK(g_alloc_calls > shared_before, 29);                 /* ランタイムも同じヒープ */
    CHECK(g_bad_free == 0, 30);

    /* 解放順序: 先にランタイムを停止してからプラットフォームを解除する */
    p2c_runtime_shutdown();
    p2c_platform_set_allocator(NULL, NULL, NULL, NULL);
    CHECK(!p2c_heap_uses_platform(), 31);
    CHECK(g_bad_free == 0, 32);

    printf("allocator injection: ok (alloc=%lu realloc=%lu free=%lu live=%lu)\n",
           (unsigned long)g_alloc_calls, (unsigned long)g_realloc_calls,
           (unsigned long)g_free_calls, (unsigned long)g_live_blocks);
    return 0;
}
