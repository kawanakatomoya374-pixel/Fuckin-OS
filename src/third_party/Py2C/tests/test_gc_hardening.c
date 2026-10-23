/* GC ハードニングの回帰 (Alpha1.0)
 *
 * 検証する設計上の改善:
 *   1. 明示ルート表が動的に拡張される（旧: 固定 P2C_GC_ROOT_CAPACITY で abort）
 *   2. マークフェーズが反復（ワークリスト）で、深い入れ子でもCスタックを
 *      使い尽くさない（旧: 再帰。深さ数十万でスタックオーバーフロー）
 *   3. 非オブジェクト領域（文字列データ・配列）も GC の確保量に計上され、
 *      大きなチャーンでも収集が行われる
 *
 * 旧実装では (1) は 1025 個目で abort、(2) は深い入れ子の収集で落ちていた。
 */
#define _GNU_SOURCE
#include "runtime/python_code_to_c_runtime.h"

#include <stdio.h>

#define ROOT_SLOTS 5000
#define DEEP_DEPTH 200000
#define WIDE_COUNT 200000

static P2C_Object *g_roots[ROOT_SLOTS];
static P2C_Object *g_deep = NULL;
static P2C_Object *g_wide = NULL;

/* 実体化アンカーの回帰: ジェネレータの要素を join で実体化している最中に
 * 収集が走っても要素が解放されないこと（以前は生の配列しか参照が無く、
 * join/sorted の結果が壊れた / 落ちた）。 */
static int g_gen_counter;
static P2C_Object *g_gen_step(P2C_Object *generator) {
    if (g_gen_counter >= 16) return p2c_generator_finish(generator, &P2C_None);
    {
        char buf[16];
        int v = g_gen_counter++;
        int i = 0;
        if (v == 0) buf[i++] = '0';
        while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        buf[i] = '\0';
        /* 逆順に作った数字を反転する */
        for (int a = 0, b = i - 1; a < b; a++, b--) { char t = buf[a]; buf[a] = buf[b]; buf[b] = t; }
        return p2c_generator_yield(generator, p2c_obj_from_str(buf), 0);
    }
}

static int check_materialization_anchor(void) {
    /* しきい値を小さくして、実体化中に必ず収集が走るようにする。 */
    size_t saved = 0;
    P2C_GcStats before;
    p2c_gc_stats(&before);
    saved = before.collections;
    (void)saved;
    p2c_gc_set_threshold(2048);

    g_gen_counter = 0;
    P2C_Object *gen = p2c_generator_new(g_gen_step, false);
    if (!gen) return 1;
    P2C_Object *sep = p2c_obj_from_str(",");
    P2C_Object *joined = p2c_call_attr(sep, "join", (P2C_Object*[]){gen}, 1);
    const char *text = p2c_obj_as_str(joined);
    if (strcmp(text, "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15") != 0) {
        printf("gc_hardening_fail: materialization anchor corrupted: [%s]\n", text);
        return 6;
    }
    /* sorted() も要素を実体化する経路。 */
    g_gen_counter = 0;
    P2C_Object *gen2 = p2c_generator_new(g_gen_step, false);
    P2C_Object *sorted = p2c_builtin_sorted(gen2);
    if (p2c_len(sorted) != 16) {
        printf("gc_hardening_fail: sorted() materialization lost elements (%lld)\n", (long long)p2c_len(sorted));
        return 7;
    }
    p2c_gc_set_threshold(256u * 1024u);
    return 0;
}

int main(void) {
    char stack_hint = 0;
    p2c_gc_init(&stack_hint);
    if (!p2c_gc_stack_scan_available()) {
        printf("gc_hardening_skipped: stack bounds unavailable on this platform\n");
        return 0;
    }

    /* --- 1. ルート表の動的拡張 --- */
    for (int i = 0; i < ROOT_SLOTS; i++) {
        g_roots[i] = p2c_obj_from_int(i);
        p2c_gc_register_root(&g_roots[i]);
    }
    if (p2c_gc_root_capacity() < ROOT_SLOTS) {
        printf("gc_hardening_fail: root capacity did not grow (%zu)\n", p2c_gc_root_capacity());
        return 1;
    }
    p2c_gc_collect();
    for (int i = 0; i < ROOT_SLOTS; i += 617) {
        if (!g_roots[i] || g_roots[i]->cls->type_tag != OBJ_INT || g_roots[i]->u.v_int != (int64_t)i) {
            printf("gc_hardening_fail: registered root %d was collected\n", i);
            return 2;
        }
    }

    /* --- 2. 深い入れ子（反復マーク） --- */
    g_deep = p2c_list_new();
    p2c_gc_register_root(&g_deep);
    for (int i = 0; i < DEEP_DEPTH; i++) {
        P2C_Object *inner = p2c_list_new();
        if (!inner) { printf("gc_hardening_fail: OOM building deep list\n"); return 3; }
        p2c_list_append(inner, g_deep);
        g_deep = inner;
    }
    p2c_gc_collect();   /* 旧実装はここで深さ分の再帰により落ちる */
    {
        P2C_Object *cur = g_deep;
        long depth = 0;
        while (cur && cur->cls->type_tag == OBJ_LIST && cur->u.v_list.len == 1) {
            cur = cur->u.v_list.items[0];
            depth++;
        }
        if (depth != DEEP_DEPTH) {
            printf("gc_hardening_fail: deep list depth %ld != %d\n", depth, DEEP_DEPTH);
            return 4;
        }
    }

    /* --- 3. 幅の広いリスト（ワークリスト拡張） --- */
    g_wide = p2c_list_new();
    p2c_gc_register_root(&g_wide);
    for (int i = 0; i < WIDE_COUNT; i++) p2c_list_append(g_wide, p2c_obj_from_int(i));
    p2c_gc_collect();
    if (p2c_len(g_wide) != WIDE_COUNT) {
        printf("gc_hardening_fail: wide list length %lld != %d\n", (long long)p2c_len(g_wide), WIDE_COUNT);
        return 5;
    }

    /* --- 4. 実体化アンカー（収集が走る中での join/sorted） --- */
    {
        int rc = check_materialization_anchor();
        if (rc != 0) return rc;
    }

    printf("gc_hardening_ok: roots=%zu deep=%d wide=%d\n",
           p2c_gc_root_capacity(), DEEP_DEPTH, WIDE_COUNT);
    p2c_runtime_shutdown();
    return 0;
}
