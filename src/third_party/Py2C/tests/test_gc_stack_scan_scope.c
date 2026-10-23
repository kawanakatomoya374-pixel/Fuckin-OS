/*
 * 保守的スタックスキャンの「走査範囲」と「生存ローカル保護」の回帰。
 *
 * 以前は宣言されたスタック区間（Linuxのmainスレッドでは既定8MiB）全体を
 * 毎回走査していたため、しきい値を小さくすると
 * 「収集回数 × スタック長」のコストが支配的になっていた。
 * 現在は「現在のフレーム（実際に使われている範囲）から区間の上端まで」だけを
 * 走査する。このテストは
 *   1. その走査範囲がスレッドのスタックマッピング長に対して十分小さいこと
 *   2. スタック上のローカル変数からしか到達できないオブジェクトが
 *      自動GCで回収されないこと
 * を確認する。
 *
 * サニタイザについて: ASan (detect_stack_use_after_return=1, GCC libasanの既定)
 * はローカル変数をヒープ上の「fake stack」へ置くため、保守的スタックスキャン
 * からは見えない（実スタック区間外になる）。これはASanの構造上の性質であり、
 * ランタイム側では対処できない。したがってASanビルドでは
 * 2（ローカル保護）の厳密検証をスキップし、走査が実行されたことだけを確認する。
 * 実スタックでの厳密検証は非サニタイザビルドで行う。
 */
#if !defined(_GNU_SOURCE)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_getattr_np / pthread_attr_getstack に必要 */
#endif
#endif
#include "runtime/python_code_to_c_runtime.h"

#if defined(__SANITIZE_ADDRESS__)
#  define P2C_TEST_HAVE_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define P2C_TEST_HAVE_ASAN 1
#  endif
#endif
#ifndef P2C_TEST_HAVE_ASAN
#  define P2C_TEST_HAVE_ASAN 0
#endif

#if defined(__linux__)
#  include <pthread.h>
#endif

#if defined(__linux__) && !P2C_TEST_HAVE_ASAN
/* このスレッドにOSが割り当てたスタック領域のサイズ。 */
static size_t host_stack_mapping_size(void) {
    pthread_attr_t attr;
    void *addr = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    if (pthread_attr_getstack(&attr, &addr, &size) != 0) size = 0;
    pthread_attr_destroy(&attr);
    return addr ? size : 0;
}
#endif

/* スタック上のローカル変数からのみ到達できるリストを保持したまま
 * 自動GCを多発させる。以前の実装（スタックスキャン無効）では
 * このオブジェクトが回収され、内容が壊れていた。 */
static int local_only_object_survives(void) {
    p2c_gc_set_threshold(1);
    size_t collections_before = p2c_gc_collections_run();

    P2C_Object * volatile kept = p2c_list_new();
    if (!kept) return 10;
#if P2C_TEST_HAVE_ASAN
    /* ASanはローカル変数をfake stackへ置くため保守的スキャンから見えない。
     * 回収させないようピン留めし、走査範囲の確認だけを行う。 */
    p2c_obj_incref(kept);
#endif
    for (int i = 0; i < 64; i++) {
        P2C_Object *tmp = p2c_list_new();          /* ここで自動収集が走る */
        if (!tmp) return 11;
        p2c_list_append(tmp, p2c_obj_from_int(i));
        p2c_list_append(kept, tmp);
    }
    if (p2c_gc_collections_run() == collections_before) return 12; /* 収集が動いていない */
    if (p2c_list_len(kept) != 64) return 13;                       /* 内容が壊れた */
    for (size_t i = 0; i < 64; i++) {
        P2C_Object *item = p2c_list_get(kept, i);
        if (!item || p2c_list_len(item) != 1) return 14;
    }
    return 0;
}

int main(void) {
    int stack_mark = 0;
    p2c_runtime_init(NULL, 0);
    p2c_gc_init(&stack_mark);

    /* Linux では pthread から実境界を取得できるのでスキャン可能。 */
    if (!p2c_gc_stack_scan_available()) return 20;

    P2C_Object *scratch = p2c_list_new();
    if (!scratch) return 21;
    p2c_gc_collect();

    int status = local_only_object_survives();
    if (status != 0) return status;

    /* 直近の走査語数はスタックマッピング長に比例してはならない。 */
    size_t words = p2c_gc_last_stack_words();
    if (words == 0) return 22; /* スタックスキャンが1語も走っていない */
#if !P2C_TEST_HAVE_ASAN
    /* 走査範囲の上限検証は、ローカルが実スタック上にある非サニタイザビルドで行う。 */
#if defined(__linux__)
    {
        size_t mapping = host_stack_mapping_size();
        if (mapping != 0 && words > (mapping / sizeof(void*)) / 4u) return 23;
    }
#endif
    /* 使用中スタックは通常 256KiB に収まる（8MiB 全体を走査していたら超過する）。 */
    if (words > (256u * 1024u) / sizeof(void*)) return 24;
#endif

    p2c_runtime_shutdown();
    return 0;
}
