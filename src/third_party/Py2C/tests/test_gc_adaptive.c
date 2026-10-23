/* 適応GCの回帰 (Alpha1.0)
 *
 * 同じチャーン負荷を「固定しきい値」と「適応しきい値」で実行し、
 *   - 適応時は収集回数が減る（＝スタックスキャンの回数が減る）
 *   - 追跡オブジェクト数のピークは上限内に収まる（メモリを抱え込まない）
 * ことを確認する。あわせて p2c_gc_stats() の値が内部状態と矛盾しないことも見る。
 *
 * 生存集合はローカル配列に置く（ホストの実装と同じく、保守的スタックスキャンが
 * 到達可能と判断する形にする。GCのルート枠は固定容量なので大量登録はしない）。 */
#define _GNU_SOURCE
#include "runtime/python_code_to_c_runtime.h"

#include <stdio.h>
#include <string.h>

#define LIVE_TARGET 20000
#define CHURN_PER_ROUND 2000
#define ROUNDS 8
#define BASE_THRESHOLD (64u * 1024u)
#define PEAK_OBJECT_LIMIT (140000u)

static size_t phase_churn(P2C_Object **live) {
    P2C_GcStats before;
    P2C_GcStats after;
    p2c_gc_stats(&before);
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < CHURN_PER_ROUND; i++) {
            /* すぐゴミになる一時オブジェクト（文字列） */
            (void)p2c_obj_from_str("0123456789abcdef0123456789abcdef");
        }
        /* 生存集合を少しずつ入れ替える（大半は到達可能なまま残る） */
        for (int i = 0; i < LIVE_TARGET; i += 4) {
            live[i] = p2c_obj_from_str("live-object-payload");
        }
    }
    p2c_gc_stats(&after);
    return after.collections - before.collections;
}

int main(void) {
    char stack_hint = 0;
    P2C_GcStats stats;
    /* この配列は「スタック上に置いた生きた集合を保守的スキャンが保持できるか」を
     * 検証するためのものなので、static/heap へ逃がしてはいけない（逃がすと
     * ルートから見えなくなり、テストの前提が崩れる）。スタック使用量だけが
     * 大きくなるため、この1ターゲットに限り -Wno-stack-usage を付けている
     * （Makefile の test-gc-adaptive を参照）。 */
    P2C_Object *live[LIVE_TARGET];
    P2C_Object *pinned = NULL;
    size_t fixed_collections, adaptive_collections;
    size_t peak;

    p2c_gc_init(&stack_hint);
    if (!p2c_gc_stack_scan_available()) {
        printf("gc_adaptive_skipped: stack bounds unavailable on this platform\n");
        return 0;
    }
    for (int i = 0; i < LIVE_TARGET; i++) live[i] = p2c_obj_from_str("live-object-initial");
    /* ルート登録API自体も1つだけ使っておく（機能の回帰）。 */
    pinned = p2c_obj_from_str("pinned");
    p2c_gc_register_root(&pinned);

    /* 1) 固定しきい値（適応オフ） */
    p2c_gc_set_adaptive(false);
    p2c_gc_set_threshold(BASE_THRESHOLD);
    fixed_collections = phase_churn(live);

    /* 2) 適応しきい値（同じ生存集合・同じ負荷） */
    p2c_gc_set_adaptive(true);
    p2c_gc_set_threshold(BASE_THRESHOLD);
    adaptive_collections = phase_churn(live);

    p2c_gc_stats(&stats);
    peak = stats.peak_objects;

    printf("gc_adaptive_ok: fixed=%zu adaptive=%zu growths=%zu peak=%zu threshold=%zu\n",
           fixed_collections, adaptive_collections, stats.threshold_growths, peak, stats.threshold);

    if (adaptive_collections == 0 || fixed_collections == 0) {
        printf("gc_adaptive_failed: no collections happened (workload too small)\n");
        return 1;
    }
    if (adaptive_collections >= fixed_collections) {
        printf("gc_adaptive_failed: adaptive did not reduce collections\n");
        return 2;
    }
    if (peak > PEAK_OBJECT_LIMIT) {
        printf("gc_adaptive_failed: too many tracked objects at peak\n");
        return 3;
    }
    if (stats.threshold_growths == 0) {
        printf("gc_adaptive_failed: threshold never grew\n");
        return 4;
    }
    if (stats.threshold > 4u * 1024u * 1024u) {
        printf("gc_adaptive_failed: threshold exceeded the adaptive cap\n");
        return 5;
    }
    if (stats.objects != (size_t)p2c_gc_object_count()) {
        printf("gc_adaptive_failed: stats.objects mismatch\n");
        return 6;
    }
    p2c_gc_unregister_root(&pinned);
    printf("gc_adaptive_regression_ok\n");
    return 0;
}
