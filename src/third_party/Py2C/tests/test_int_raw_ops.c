/* 生の int64 演算ヘルパ（AOT アンボクシング用。runtime.h の p2c_int_*_raw）が、
 * P2C_Object* 版と「同じ結果・同じ例外」になることを固定するテスト。
 *
 * これらのヘルパは codegen が型付きローカル（int64_t）から直接呼ぶため、
 * 意味論がずれると生成コードだけが静かに違う結果を出す。ここでは
 * 値のマトリクス（0・±1・±7・境界値・シフト量の境界）について全演算子を
 * 突き合わせ、例外が出る場合も「同じ名前の例外が出るか」まで確認する。
 *
 * 比較ヘルパ（p2c_int_*_obj）は相手が int なら生比較、それ以外は
 * 従来の比較経路へ委譲するので、int / float / bool / None / str を相手に
 * 突き合わせる。 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "runtime/python_code_to_c_runtime.h"

typedef int64_t (*P2C_RawBin)(int64_t, int64_t);
typedef P2C_Object* (*P2C_ObjBin)(P2C_Object*, P2C_Object*);
typedef bool (*P2C_RawCmp)(int64_t, P2C_Object*);
typedef P2C_Object* (*P2C_ObjCmp)(P2C_Object*, P2C_Object*);

static int failures = 0;

static const char *exc_name(P2C_Object *exc) {
    if (p2c_exc_name_match(exc, "OverflowError")) return "OverflowError";
    if (p2c_exc_name_match(exc, "ZeroDivisionError")) return "ZeroDivisionError";
    if (p2c_exc_name_match(exc, "ValueError")) return "ValueError";
    if (p2c_exc_name_match(exc, "TypeError")) return "TypeError";
    return "other";
}

/* raw 版を呼び、例外が出たらその名前を返す（出なければ NULL で *out に結果）。 */
static const char *call_raw(P2C_RawBin op, int64_t a, int64_t b, int64_t *out) {
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        *out = op(a, b);
        p2c_exc_stack = frame.prev;
        return NULL;
    }
    p2c_exc_stack = frame.prev;
    return exc_name(frame.exc);
}

/* P2C_Object* 版を呼び、同じ形式で返す。 */
static const char *call_obj(P2C_ObjBin op, int64_t a, int64_t b, int64_t *out) {
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        P2C_Object *r = op(p2c_obj_from_int(a), p2c_obj_from_int(b));
        *out = p2c_obj_as_int(r);
        p2c_exc_stack = frame.prev;
        return NULL;
    }
    p2c_exc_stack = frame.prev;
    return exc_name(frame.exc);
}

static void compare_binop(const char *label, P2C_RawBin raw, P2C_ObjBin obj,
                          int64_t a, int64_t b) {
    int64_t rv = 0, ov = 0;
    const char *re = call_raw(raw, a, b, &rv);
    const char *oe = call_obj(obj, a, b, &ov);
    bool same = (re || oe) ? (re && oe && strcmp(re, oe) == 0) : (rv == ov);
    if (!same) {
        printf("FAIL: %s(%lld, %lld) raw=%s/%lld obj=%s/%lld\n", label,
               (long long)a, (long long)b, re ? re : "ok", (long long)rv,
               oe ? oe : "ok", (long long)ov);
        failures++;
    }
}

static void compare_cmp(const char *label, P2C_RawCmp raw, P2C_ObjCmp obj,
                        int64_t a, P2C_Object *b) {
    /* longjmp をまたいで読む値は volatile（生成Cと同じ規約）。 */
    volatile bool rv = false, ov = false;
    const char *volatile re = NULL;
    const char *volatile oe = NULL;
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack; p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) { rv = raw(a, b); p2c_exc_stack = frame.prev; }
    else { p2c_exc_stack = frame.prev; re = exc_name(frame.exc); }
    frame.prev = p2c_exc_stack; p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        ov = p2c_obj_is_truthy(obj(p2c_obj_from_int(a), b));
        p2c_exc_stack = frame.prev;
    } else { p2c_exc_stack = frame.prev; oe = exc_name(frame.exc); }
    bool same = (re || oe) ? (re && oe && strcmp(re, oe) == 0) : (rv == ov);
    if (!same) {
        printf("FAIL: %s(%lld, <obj>) raw=%s/%d obj=%s/%d\n", label,
               (long long)a, re ? re : "ok", (int)rv, oe ? oe : "ok", (int)ov);
        failures++;
    }
}

int main(void) {
    P2C_GC_ENTER_MAIN();
    p2c_runtime_init(NULL, 0);

    static const int64_t vals[] = {
        0, 1, -1, 2, -2, 3, -3, 7, -7, 62, 63, 64, -64,
        3037000499LL, -3037000499LL,
        INT64_MAX, INT64_MAX - 1, INT64_MIN, INT64_MIN + 1
    };
    const size_t nv = sizeof(vals) / sizeof(vals[0]);

    const struct { const char *label; P2C_RawBin raw; P2C_ObjBin obj; } bins[] = {
        { "add",      p2c_int_add_raw,      p2c_obj_add },
        { "sub",      p2c_int_sub_raw,      p2c_obj_sub },
        { "mul",      p2c_int_mul_raw,      p2c_obj_mul },
        { "floordiv", p2c_int_floordiv_raw, p2c_obj_floordiv },
        { "mod",      p2c_int_mod_raw,      p2c_obj_mod },
        { "bitand",   p2c_int_bitand_raw,   p2c_obj_bitand },
        { "bitor",    p2c_int_bitor_raw,    p2c_obj_bitor },
        { "bitxor",   p2c_int_bitxor_raw,   p2c_obj_bitxor },
        { "lshift",   p2c_int_lshift_raw,   p2c_obj_lshift },
        { "rshift",   p2c_int_rshift_raw,   p2c_obj_rshift }
    };
    for (size_t i = 0; i < sizeof(bins) / sizeof(bins[0]); i++) {
        for (size_t x = 0; x < nv; x++) {
            for (size_t y = 0; y < nv; y++) {
                compare_binop(bins[i].label, bins[i].raw, bins[i].obj, vals[x], vals[y]);
            }
        }
    }

    /* 単項演算は P2C_Object* 版と直接比較する。 */
    for (size_t x = 0; x < nv; x++) {
        int64_t raw_neg = p2c_int_neg_raw(vals[x]);
        int64_t obj_neg = p2c_obj_as_int(p2c_obj_neg(p2c_obj_from_int(vals[x])));
        if (raw_neg != obj_neg) {
            printf("FAIL: neg(%lld) %lld != %lld\n", (long long)vals[x],
                   (long long)raw_neg, (long long)obj_neg);
            failures++;
        }
        int64_t raw_inv = p2c_int_invert_raw(vals[x]);
        int64_t obj_inv = p2c_obj_as_int(p2c_obj_invert(p2c_obj_from_int(vals[x])));
        if (raw_inv != obj_inv) {
            printf("FAIL: invert(%lld)\n", (long long)vals[x]);
            failures++;
        }
    }

    /* 混在比較: 相手が int / float / bool / None / str のそれぞれで一致すること。 */
    const struct { const char *label; P2C_RawCmp raw; P2C_ObjCmp obj; } cmps[] = {
        { "lt", p2c_int_lt_obj, p2c_obj_lt },
        { "le", p2c_int_le_obj, p2c_obj_le },
        { "gt", p2c_int_gt_obj, p2c_obj_gt },
        { "ge", p2c_int_ge_obj, p2c_obj_ge },
        { "eq", p2c_int_eq_obj, p2c_obj_eq },
        { "ne", p2c_int_ne_obj, p2c_obj_ne }
    };
    P2C_Object *others[] = {
        p2c_obj_from_int(3), p2c_obj_from_int(-3), p2c_obj_from_int(INT64_MAX),
        p2c_obj_from_float(3.5), p2c_obj_from_float(-0.5),
        &P2C_True, &P2C_False, &P2C_None, p2c_obj_from_str("3")
    };
    for (size_t c = 0; c < sizeof(cmps) / sizeof(cmps[0]); c++) {
        for (size_t x = 0; x < nv; x++) {
            for (size_t o = 0; o < sizeof(others) / sizeof(others[0]); o++) {
                compare_cmp(cmps[c].label, cmps[c].raw, cmps[c].obj, vals[x], others[o]);
            }
        }
    }

    p2c_runtime_shutdown();
    if (failures == 0) {
        printf("test-int-raw-ops: ok\n");
        return 0;
    }
    printf("test-int-raw-ops: FAILED (%d)\n", failures);
    return 1;
}
