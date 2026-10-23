/*
 * 共有ヒープの一本化（NO_STDLIB 構成）の回帰。
 *
 * NO_STDLIB（自作OS/カーネル埋め込み）では確保の入口が2系統ある。
 *   1. p2c_heap_alloc() 系 … ランタイムと変換器コアの内部確保
 *   2. malloc/calloc/realloc/free … ランタイム同梱 libc スタブ
 *      （PYTHON_CODE_TO_C_NO_LIBC_STUBS ではカーネル提供のもの）
 * この2系統が別々のヒープを指すと「変換器とランタイムのヒープは1つ」という
 * 前提が崩れ、
 *   - カーネルが設定したアロケータの外にランタイムが別の領域を確保してしまう
 *   - 変換結果のC文字列を、確保したアロケータの free で解放できない
 * といった破綻が起きる。ここでは
 *   A. p2c_platform_set_allocator() を設定したら、スタブの malloc 系も
 *      共有ヒープ（＝カーネルのアロケータ）へ委譲されること
 *      （静的線形フォールバックの領域 heap_used が 0 のまま = 未使用）
 *   B. p2c_heap_alloc() のブロックと raw malloc のブロックが相互に free でき、
 *      解放がカーネルのアロケータへ届くこと
 *   C. プラットフォーム未設定なら、スタブの線形ヒープが唯一のヒープになり、
 *      realloc がヘッダに記録した旧サイズぶんだけコピーすること
 *   D. ヒープが1つも無い構成ではランタイムの確保は NULL を返し、暗黙の
 *      静的ヒープへは落ちないこと
 *      （変換器コアの静的アリーナは P2C_COMPILER_FALLBACK_HEAP_SIZE という
 *        明示的な契約なので、ここでは対象外）
 * を確認する。
 */
#include "core/python_code_to_c.h"
#include "platform/python_code_to_c_platform.h"
#include "runtime/python_code_to_c_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#error "このテストは -DPYTHON_CODE_TO_C_NO_STDLIB でビルドする（ランタイム同梱スタブの検証）"
#endif
#ifdef PYTHON_CODE_TO_C_NO_LIBC_STUBS
#error "スタブ本体の検証なので PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義しないこと"
#endif

#define CHECK(cond, code) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return (code); } } while (0)

/* ============================================================
 * カーネルのアロケータ相当（呼び出し回数と生存ブロック数を数えるアリーナ）
 * ============================================================ */

#define PLAT_ARENA_SIZE (2u * 1024u * 1024u)
#define PLAT_ALIGNMENT  16u

/* p2c_runtime_shutdown() はモジュール/クラスレジストリを reset して再利用するため、
 * 停止後も少数のブロックがカーネルのヒープに残る（リークではない）。 */
#define P2C_TEST_RETAINED_MAX 4u

static unsigned char g_plat_arena[PLAT_ARENA_SIZE];
/* カウンタは volatile: glibc の malloc は __attribute__((__malloc__)) を持つため、
 * コンパイラは「malloc 呼び出しはグローバルを変更しない」と仮定して呼び出しをまたぎ
 * 値を使い回す。差し替えた malloc の副作用をここで正しく観測するために volatile を
 * 付ける（カーネル側で同様のカウンタを取る場合も同じ注意が必要）。 */
static volatile size_t g_plat_cursor        = 0;
static volatile size_t g_plat_alloc_calls   = 0;
static volatile size_t g_plat_realloc_calls = 0;
static volatile size_t g_plat_free_calls    = 0;
static volatile size_t g_plat_live_blocks   = 0;
static volatile size_t g_plat_bad_free      = 0;

static size_t plat_round(size_t size) {
    if (size == 0) return PLAT_ALIGNMENT;
    return (size + (PLAT_ALIGNMENT - 1u)) & ~(size_t)(PLAT_ALIGNMENT - 1u);
}

static void *plat_alloc_fn(size_t size, void *user) {
    (void)user;
    size_t total = plat_round(size);
    if (g_plat_cursor + total > (size_t)PLAT_ARENA_SIZE) return NULL;
    unsigned char *block = g_plat_arena + g_plat_cursor;
    g_plat_cursor += total;
    g_plat_alloc_calls++;
    g_plat_live_blocks++;
    return (void*)block;
}

static void plat_free_fn(void *ptr, size_t size, void *user) {
    (void)size;
    (void)user;
    if (!ptr) return;
    g_plat_free_calls++;
    if (g_plat_live_blocks == 0) { g_plat_bad_free++; return; }
    g_plat_live_blocks--;
}

static void *plat_realloc_fn(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)user;
    g_plat_realloc_calls++;
    /* 線形アリーナなので旧ブロックは残し、新しいブロックへ移し替える。 */
    void *moved = plat_alloc_fn(new_size, NULL);
    if (!moved) return NULL;
    if (ptr) {
        size_t copy = old_size < new_size ? old_size : new_size;
        if (copy) memcpy(moved, ptr, copy);
        plat_free_fn(ptr, old_size, NULL);
    }
    return moved;
}

/* 変換器コアとランタイムの両方が共有ヒープ（＝上のアリーナ）を使うこと。 */
static int transpile_uses_shared_heap(void) {
    char *code = NULL;
    if (python_to_c("print(1 + 2)\n", NULL, &code) != P2C_OK) return 1;
    if (!code || !strstr(code, "p2c_")) { free(code); return 2; }
    if (p2c_core_static_allocator_active()) { free(code); return 3; } /* 静的フォールバック依存 */
    free(code);                                                      /* raw free = カーネルのヒープ */
    return 0;
}

/* ============================================================
 * カーネル側プラットフォーム補助（NO_STDLIB では未定義。自作OS が提供する部分）
 * ============================================================ */

void p2c_platform_init(void) {}
void p2c_platform_shutdown(void) {}

void p2c_platform_write_n(const char *s, size_t len) {
    const P2C_Platform *platform = p2c_platform_current();
    /* カーネルなら UART 等へ。テストでは診断用に stdout へ流す。 */
    if (platform && platform->write && s && len) { platform->write(1, s, len, platform->user); return; }
    if (s && len) (void)fwrite(s, 1, len, stdout);
}

void p2c_platform_write(const char *s) {
    size_t len = 0;
    if (s) while (s[len] != '\0') len++;
    p2c_platform_write_n(s, len);
}

size_t p2c_platform_read_line(char *buf, size_t cap) {
    if (buf && cap > 0) buf[0] = '\0';
    return 0; /* 入力なし（EOF） */
}

void p2c_platform_abort(const char *reason) {
    p2c_platform_write(reason);
    (void)fflush(stdout);
    abort();
}

int main(void) {
    /* ---------- フェーズ0: ヒープ未設定（暗黙の静的ヒープを作らない） ---------- */
    CHECK(!p2c_heap_usable(), 1);
    CHECK(malloc(8) == NULL, 2);
    CHECK(p2c_heap_alloc(8) == NULL, 3);
    CHECK(p2c_heap_calloc(4, 4) == NULL, 4);
    CHECK(p2c_runtime_heap_used() == 0, 5);

    /* ---------- フェーズ1: カーネルのアロケータだけを注入 ---------- */
    p2c_platform_set_allocator(plat_alloc_fn, plat_realloc_fn, plat_free_fn, NULL);
    CHECK(p2c_heap_uses_platform(), 10);

    /* スタブの malloc がカーネルのアロケータへ委譲される */
    size_t before = g_plat_alloc_calls;
    char *raw = (char*)malloc(64);
    CHECK(raw != NULL, 11);
    CHECK(g_plat_alloc_calls > before, 12); /* 静的アリーナではなくカーネルから */
    memset(raw, 'A', 63);
    raw[63] = '\0';

    /* スタブの realloc もカーネルへ委譲し、旧内容を保持する */
    before = g_plat_alloc_calls;
    raw = (char*)realloc(raw, 512);
    CHECK(raw != NULL, 13);
    CHECK(g_plat_alloc_calls > before, 14);
    CHECK(raw[0] == 'A' && raw[62] == 'A', 15);

    /* スタブの free がカーネルのアロケータへ届く */
    size_t free_before = g_plat_free_calls;
    free(raw);
    CHECK(g_plat_free_calls > free_before, 16);

    /* calloc もゼロ初期化されたカーネルのブロックを返す */
    unsigned char *zeroed = (unsigned char*)calloc(16, 4);
    CHECK(zeroed != NULL, 17);
    for (size_t i = 0; i < 64; i++) CHECK(zeroed[i] == 0, 18);
    free(zeroed);

    /* 共有ヒープのブロックを raw free、raw malloc のブロックを p2c_heap_free */
    free_before = g_plat_free_calls;
    void *shared = p2c_heap_alloc(32);
    CHECK(shared != NULL, 19);
    free(shared);
    CHECK(g_plat_free_calls > free_before, 20);

    char *mixed = (char*)malloc(32);
    CHECK(mixed != NULL, 21);
    free_before = g_plat_free_calls;
    p2c_heap_free(mixed);
    CHECK(g_plat_free_calls > free_before, 22);

    /* フェーズ1のブロックはすべてカーネルへ返った（別ヒープが無い証拠） */
    CHECK(g_plat_live_blocks == 0, 23);
    CHECK(g_plat_bad_free == 0, 24);
    CHECK(p2c_runtime_heap_used() == 0, 25); /* 静的線形ヒープは1バイトも使っていない */

    /* ---------- フェーズ2: ランタイムも変換器もカーネルのヒープだけを使う ---------- */
    /* ベースライン: ここまでに glibc（printf のバッファ等）が確保した分を含む。
     * フェーズ2の後にこの数へ戻れば、ランタイム/変換器が確保した分は返却されている。 */
    (void)fflush(stdout);
    size_t live_baseline = g_plat_live_blocks;
    size_t frees_baseline = g_plat_free_calls;
    p2c_runtime_init(NULL, 0); /* アリーナ引数なし = カーネルのヒープが唯一のヒープ */
    CHECK(p2c_heap_usable(), 30);

    before = g_plat_alloc_calls;
    P2C_Object *joined = p2c_obj_add(p2c_obj_from_str("hook"), p2c_obj_from_str("ed"));
    CHECK(joined != NULL, 31);
    CHECK(g_plat_alloc_calls > before, 32);
    CHECK(p2c_runtime_heap_used() == 0, 33); /* ランタイム内部確保もスタブの静的領域へ行かない */

    CHECK(transpile_uses_shared_heap() == 0, 34);
    CHECK(p2c_runtime_heap_used() == 0, 35);
    CHECK(g_plat_bad_free == 0, 36);

    p2c_runtime_shutdown();
    /* shutdown はモジュール/クラスレジストリを「reset」して再利用するため、
     * 少数のブロックはカーネルのヒープに残る（リークではない）。 */
    CHECK(g_plat_live_blocks <= live_baseline + P2C_TEST_RETAINED_MAX, 37);
    CHECK(g_plat_free_calls > frees_baseline, 38); /* ランタイムのブロックは実際に返却された */

    /* ---------- フェーズ3: プラットフォーム解除 → スタブの線形ヒープが唯一のヒープ ---------- */
    p2c_platform_set_allocator(NULL, NULL, NULL, NULL);
    CHECK(!p2c_heap_uses_platform(), 40);
    CHECK(g_plat_live_blocks <= live_baseline + P2C_TEST_RETAINED_MAX, 41); /* 解除後もカーネルのヒープは漏れていない */

    static unsigned char g_linear_arena[512u * 1024u];
    p2c_runtime_init(g_linear_arena, sizeof(g_linear_arena));
    CHECK(p2c_heap_usable(), 42);
    CHECK(p2c_runtime_heap_used() > 0, 43); /* 起動時の内部確保が線形ヒープから出る */
    size_t used_before = p2c_runtime_heap_used();

    char *lin = (char*)malloc(32);
    CHECK(lin != NULL, 44);
    /* ブロックヘッダ（サイズ + magic）付きで確保される */
    CHECK(p2c_runtime_heap_used() >= used_before + 32 + 16, 45);
    CHECK(p2c_runtime_heap_peak() >= p2c_runtime_heap_used(), 46);
    memset(lin, 'Z', 32);

    free_before = g_plat_free_calls;
    lin = (char*)realloc(lin, 4096);
    CHECK(lin != NULL, 47);
    CHECK(lin[0] == 'Z' && lin[31] == 'Z', 48); /* 旧ブロック長（ヘッダ記録値）ぶんだけコピー */
    free(lin);
    CHECK(g_plat_free_calls == free_before, 49); /* 線形ヒープの free はカーネルへ行かない */
    CHECK(g_plat_bad_free == 0, 50);

    /* 線形ヒープのブロックを p2c_heap_free で解放しても壊れない */
    char *both = (char*)malloc(24);
    CHECK(both != NULL, 51);
    p2c_heap_free(both);
    CHECK(g_plat_bad_free == 0, 52);
    CHECK(g_plat_free_calls == free_before, 53);
    CHECK(p2c_runtime_heap_used() > 0, 54);

    /* 線形ヒープ上でコンテナ/ランタイムの確保経路を通す */
    P2C_Object *items = p2c_list_new();
    CHECK(items != NULL, 55);
    for (int i = 0; i < 16; i++) p2c_list_append(items, p2c_obj_from_int(i));
    CHECK(p2c_list_len(items) == 16, 56);
    CHECK(p2c_runtime_heap_used() > used_before, 57);

    /* ---------- フェーズ4: 起動順序で「線形ヒープ→カーネルアロケータ」へ移行 ---------- */
    /* 線形ヒープのブロックが残った状態でカーネルアロケータを設定する。既存ブロックを
     * カーネルの free へ渡したり、逆に新しいブロックを線形ヒープへ戻したりしないこと。 */
    char *linear_block = (char*)malloc(48);
    CHECK(linear_block != NULL, 60);
    memset(linear_block, 'M', 48);
    char *linear_stale = (char*)malloc(16);
    CHECK(linear_stale != NULL, 61);

    p2c_platform_set_allocator(plat_alloc_fn, plat_realloc_fn, plat_free_fn, NULL);
    CHECK(p2c_heap_uses_platform(), 62);

    /* 線形ブロックの realloc: カーネルのヒープへ移し替え、旧サイズぶんだけコピー */
    linear_block = (char*)realloc(linear_block, 256);
    CHECK(linear_block != NULL, 63);
    CHECK(linear_block[0] == 'M' && linear_block[47] == 'M', 64);
    free(linear_block);
    CHECK(g_plat_bad_free == 0, 65);

    /* 線形ブロックの free: カーネルの free へは渡さず、領域にも触れない */
    free_before = g_plat_free_calls;
    free(linear_stale);
    CHECK(g_plat_free_calls == free_before, 66);
    CHECK(g_plat_bad_free == 0, 67);

    /* 移行後は新しい確保がカーネルのヒープから出る */
    char *migrated = (char*)malloc(64);
    CHECK(migrated != NULL, 68);
    free_before = g_plat_free_calls;
    free(migrated);
    CHECK(g_plat_free_calls > free_before, 69);

    p2c_platform_set_allocator(NULL, NULL, NULL, NULL);
    p2c_runtime_shutdown();

    printf("heap unification: ok (plat alloc=%zu realloc=%zu free=%zu bad_free=%zu arena=%zu)\n",
           g_plat_alloc_calls, g_plat_realloc_calls, g_plat_free_calls, g_plat_bad_free,
           (size_t)PLAT_ARENA_SIZE);
    return 0;
}
