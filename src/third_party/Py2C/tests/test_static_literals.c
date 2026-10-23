/* 不変の静的リテラル（P2C_STATIC_STR）の性質を確認するテスト。
 *
 * 確認する性質（Round-8 で導入。docs/review/PERF_ALPHA1.0.md §7 参照）:
 *   1. 内容が正しく、str として扱える（空文字列・エスケープ含む）
 *   2. GC の追跡対象にならない（オブジェクト数が増えず、収集後も生きている）
 *   3. 辞書のキーとして使える（ポインタ比較の高速路と内容比較の両方が成立）
 *   4. ランタイムの終了→再初期化をまたいでも有効
 *      （ヒープにもコンテキストにも属さないため。embed 構成で重要）
 *
 * 生成コードはこれらを「使うたびに確保する」代わりに参照するので、
 * ここの前提が崩れると use-after-free やキー不一致として現れる。 */
#include "runtime/python_code_to_c_runtime.h"
#include <stdio.h>
#include <string.h>

P2C_STATIC_STR(lit_hello, "hello", 5);
P2C_STATIC_STR(lit_empty, "", 0);
P2C_STATIC_STR(lit_esc, "tab\there", 8);

static int failures = 0;
static void check(bool cond, const char *msg) {
    if (cond) {
        printf("ok: %s\n", msg);
    } else {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

int main(void) {
    p2c_runtime_init(NULL, 0);

    /* 1. 内容 */
    check(p2c_obj_is_str(&lit_hello), "静的リテラルは str と判定される");
    check(strcmp(p2c_obj_as_str(&lit_hello), "hello") == 0, "内容が正しい");
    check(strlen(p2c_obj_as_str(&lit_hello)) == 5, "長さが正しい");
    check(p2c_obj_is_truthy(&lit_hello), "空でない文字列は真");
    check(!p2c_obj_is_truthy(&lit_empty), "空文字列は偽");
    check(strcmp(p2c_obj_as_str(&lit_esc), "tab\there") == 0, "エスケープを含む内容も正しい");

    /* 2. 辞書のキーとして使える（内容が同じ別オブジェクトとも一致すること） */
    P2C_Object *d = p2c_dict_new();
    p2c_dict_set(d, &lit_hello, p2c_obj_from_int(42));
    P2C_Object *same = p2c_obj_from_str("hello");
    P2C_Object *got = same ? p2c_dict_get(d, same) : NULL;
    check(got != NULL && p2c_obj_as_int(got) == 42,
          "動的に作った同じ内容の文字列でもキーとして引ける");

    /* 3. GC の追跡対象にならない */
    size_t before = p2c_gc_object_count();
    (void)p2c_obj_as_str(&lit_hello);
    (void)p2c_obj_is_truthy(&lit_esc);
    check(p2c_gc_object_count() == before, "リテラルの参照ではオブジェクト数が増えない");
    p2c_gc_collect();
    check(strcmp(p2c_obj_as_str(&lit_hello), "hello") == 0, "GC 後も内容が保たれる");
    check(!p2c_obj_is_none(&lit_hello), "GC 後もオブジェクトは有効");

    p2c_runtime_shutdown();

    /* 4. 再初期化をまたいでも有効（ヒープ・コンテキストに属さない） */
    p2c_runtime_init(NULL, 0);
    check(strcmp(p2c_obj_as_str(&lit_hello), "hello") == 0, "再初期化後も内容が正しい");
    check(strcmp(p2c_obj_as_str(&lit_esc), "tab\there") == 0, "再初期化後もエスケープが正しい");
    p2c_runtime_shutdown();

    if (failures == 0) {
        printf("test-static-literals: ok\n");
        return 0;
    }
    printf("test-static-literals: FAILED (%d)\n", failures);
    return 1;
}
