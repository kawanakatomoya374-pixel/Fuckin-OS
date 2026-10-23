/* ランタイムインスタンス（コンテキスト）の分離を確認するテスト。
 *
 * 自作OS のカーネルでは「タスクごとに 1 つの Python 実行環境」を持ちたい。
 * そのためにランタイムは、GC（追跡リスト・ルート・しきい値）、クラス／
 * モジュールレジストリ、オブジェクトのフリーリスト、式評価スタックを
 * インスタンスごとに分けて持つ（docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md）。
 *
 * ここでは次を固定する:
 *   1. インスタンスごとに GC の追跡対象が独立している
 *   2. 片方で収集しても、もう片方のオブジェクトは壊れない
 *   3. 片方を destroy しても、もう片方は使い続けられる
 *   4. フリーリストもインスタンスごとに独立している
 *   5. select(NULL) で既定インスタンスへ戻れる
 */
#include "runtime/python_code_to_c_runtime.h"
#include <stdio.h>
#include <string.h>

/* ルート登録するスロットはファイルスコープ（静的記憶域）に置く。
 * ローカルのアドレスを登録すると、関数から戻った後に GC が消えた
 * スタックフレームを読んでしまう。 */
static P2C_Object *g_list_a = NULL;
static P2C_Object *g_list_b = NULL;

static int failures = 0;
static void check(bool cond, const char *msg) {
    if (cond) {
        printf("ok: %s\n", msg);
    } else {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

/* インスタンスを選択して初期化し、base..base+63 の整数を持つリストを作る
 * （カーネルのタスクエントリに相当する処理）。 */
static bool task_build(struct P2C_RuntimeContext *ctx, P2C_Object **slot, int base) {
    p2c_runtime_context_select(ctx);
    P2C_GC_ENTER_MAIN();
    p2c_runtime_init(NULL, 0);
    *slot = p2c_list_new();
    if (!*slot) return false;
    p2c_gc_register_root(slot);
    for (int i = 0; i < 64; i++) {
        P2C_Object *v = p2c_obj_from_int((int64_t)base + i);
        if (!v) return false;
        p2c_list_append(*slot, v);
    }
    return true;
}

static bool list_matches(P2C_Object *lst, int base) {
    if (!lst || p2c_len(lst) != 64u) return false;
    for (int i = 0; i < 64; i++) {
        P2C_Object *v = p2c_list_get(lst, (size_t)i);
        if (!v || p2c_obj_as_int(v) != (int64_t)base + i) return false;
    }
    return true;
}

int main(void) {
    struct P2C_RuntimeContext *a = p2c_runtime_context_create();
    struct P2C_RuntimeContext *b = p2c_runtime_context_create();
    check(a != NULL && b != NULL, "インスタンスを 2 つ作成できる");
    check(a != b, "2 つのインスタンスは別物");
    check(p2c_runtime_context_current() != a && p2c_runtime_context_current() != b,
          "作成しただけでは選択されない");

    check(task_build(a, &g_list_a, 1000), "インスタンスAでリストを作れる");
    p2c_runtime_context_select(a);
    size_t count_a = p2c_gc_object_count();
    check(count_a > 0, "A は自分のオブジェクトを数えている");

    check(task_build(b, &g_list_b, 2000), "インスタンスBでリストを作れる");
    p2c_runtime_context_select(b);
    size_t count_b = p2c_gc_object_count();
    check(count_b > 0, "B は自分のオブジェクトを数えている");

    /* 1. B で大量に確保しても A の追跡対象は増えない */
    for (int i = 0; i < 200; i++) {
        P2C_Object *garbage = p2c_list_new();
        if (garbage) p2c_list_append(garbage, p2c_obj_from_int((int64_t)i));
    }
    p2c_runtime_context_select(a);
    check(p2c_gc_object_count() == count_a, "B の確保は A の追跡対象に影響しない");

    /* 4. フリーリストも独立（B の確保・回収で A の統計が動かない） */
    p2c_runtime_context_select(a);
    size_t hits_a = p2c_obj_pool_hits();
    size_t misses_a = p2c_obj_pool_misses();
    p2c_runtime_context_select(b);
    for (int i = 0; i < 50; i++) {
        P2C_Object *tmp = p2c_list_new();
        (void)tmp;
    }
    p2c_gc_collect();
    p2c_runtime_context_select(a);
    check(p2c_obj_pool_hits() == hits_a && p2c_obj_pool_misses() == misses_a,
          "B の確保と回収は A のフリーリスト統計を動かさない");

    /* 2. 片方の収集がもう片方に影響しない */
    p2c_runtime_context_select(a);
    p2c_gc_collect();
    check(list_matches(g_list_a, 1000), "A で収集しても A のリストは無事");
    p2c_runtime_context_select(b);
    check(list_matches(g_list_b, 2000), "A で収集しても B のリストは無事");
    p2c_gc_collect();
    check(list_matches(g_list_b, 2000), "B で収集しても B のリストは無事");
    p2c_runtime_context_select(a);
    check(list_matches(g_list_a, 1000), "B で収集しても A のリストは無事");

    /* 3. B を破棄しても A は使い続けられる */
    p2c_runtime_context_destroy(b);
    check(p2c_runtime_context_current() == a, "B の destroy 後も選択中は A のまま");
    p2c_gc_collect();
    check(list_matches(g_list_a, 1000), "B の destroy 後も A のリストは無事");
    P2C_Object *extra = p2c_list_new();
    check(extra != NULL, "B の destroy 後も A で確保できる");
    if (extra) p2c_list_append(extra, p2c_obj_from_int(7));

    /* 5. 既定インスタンスへ戻れる（destroy 後に選択が消えない） */
    p2c_runtime_context_destroy(a);
    check(p2c_runtime_context_current() != NULL, "A の destroy 後も選択中のインスタンスは有効");
    p2c_runtime_context_select(NULL);
    P2C_GC_ENTER_MAIN();               /* スタック境界を宣言（収集を有効にする） */
    p2c_runtime_init(NULL, 0);
    P2C_Object *d = p2c_list_new();
    check(d != NULL, "既定インスタンスも使える");
    if (d) p2c_list_append(d, p2c_obj_from_int(1));
    p2c_gc_collect();
    check(p2c_gc_collections_run() > 0, "既定インスタンスでは収集が実際に走る");
    p2c_runtime_shutdown();

    if (failures == 0) {
        printf("test-runtime-instances: ok\n");
        return 0;
    }
    printf("test-runtime-instances: FAILED (%d)\n", failures);
    return 1;
}
