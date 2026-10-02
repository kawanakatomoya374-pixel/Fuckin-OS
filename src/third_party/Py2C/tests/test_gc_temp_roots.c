/*
 * GCルート被覆と「式の途中で例外が脱出したとき」の後始末の回帰。
 *
 * 生成Cは二項演算を
 *     (p2c_binop_begin(left), p2c_binop_finish(p2c_obj_add, right))
 * の形で出すため、leftはTLS（g_binop_stack）にしか存在しない期間ができる。
 * 同じく p2c_active_exception もTLSにあり、f-stringのビルダ（P2C_String）は
 * ネイティブ確保でTLSの g_fstr_stack に置かれる。これらは保守的スタックスキャン
 * からは見えないため、以前は次の2つが壊れていた。
 *
 *  (1) right の評価中に自動GCが走ると、leftが回収されて解放済みポインタが
 *      p2c_binop_finish() から演算関数へ渡る（静かなUAF）。処理中の例外
 *      オブジェクトも同様に回収され得た。
 *  (2) 式の途中で例外が脱出する（longjmpする）と g_binop_depth / g_fstr_depth が
 *      戻らず、例外が脱出するたびに一時値が残る。反復すると64回目で
 *      「binary expression nesting limit exceeded」を誤発火し、f-stringの
 *      ビルダはリークしていた（生成Cのtryに保存/復元が無かった）。
 *
 * このテストは上記(1)(2)の修正を固定する。
 *   A. 式の途中の自動GCで左オペランドがルート化される
 *   B. p2c_active_exception がルート化される
 *   C. p2c_binop_rewind()/p2c_fstr_rewind() が深さを戻す（冪等・容量超えも安全）
 *   D. 200回例外が脱出しても深さが漏れず、その後の式が正しく動く
 *   E. p2c_runtime_shutdown() が式評価中の一時値を空にする
 *
 * サニタイザについて: このテストが検証するのはTLS上のルート被覆なので、ASanが
 * ローカル変数をfake stackへ置く影響を受けない。保守的スタックスキャンの走査範囲は
 * tests/test_gc_stack_scan_scope.c が担当する。
 */
#include "runtime/python_code_to_c_runtime.h"
#include <string.h>

/* 左オペランドが回収されていないことを確認してから加算する演算関数。
 * p2c_binop_finish() が渡す左オペランドは、まさにTLSから取り出した値。 */
static int g_probe_calls = 0;
static int g_probe_left_ok = 0;

static P2C_Object* probe_add(P2C_Object *left, P2C_Object *right) {
    g_probe_calls++;
    if (left && left->cls && left->cls->type_tag == OBJ_LIST && p2c_list_len(left) == 2) {
        P2C_Object *a = p2c_list_get(left, 0);
        P2C_Object *b = p2c_list_get(left, 1);
        if (a && b && p2c_obj_as_int(a) == 1 && p2c_obj_as_int(b) == 2) g_probe_left_ok++;
    }
    return p2c_obj_add(left, right);
}

/* 右オペランドの評価中に自動GC（しきい値1バイト）を多数誘発する。
 * 小さな整数はランタイムがキャッシュされるためGCオブジェクトを確保しない。
 * ここでは新しいリストを確保して、収集が確実に式の途中で走るようにする。
 * 直近の収集時のルート数をこの場で記録する（収集カウンタは「最後の収集」の
 * 値なので、その後 p2c_binop_finish() が深さを戻した後に読むと0になる）。 */
static size_t g_pressure_temp_roots = 0;
static size_t g_pressure_collections = 0;

static P2C_Object* make_pressure_list(int n) {
    P2C_Object *lst = p2c_list_new();
    if (!lst) return NULL;
    for (int i = 0; i < n; i++) {
        P2C_Object *item = p2c_list_new(); /* ここで自動GCが走る */
        if (!item) return lst;
        p2c_list_append(item, p2c_obj_from_int(i));
        p2c_list_append(lst, item);
    }
    g_pressure_temp_roots = p2c_gc_last_temp_roots();
    g_pressure_collections = p2c_gc_collections_run();
    return lst;
}

/* 右オペランドの評価中に例外を送出する（式の途中で脱出させる）。 */
static P2C_Object* raising_operand(void) {
    p2c_raise(p2c_make_exception("ValueError", "escaped from expression"));
    return &P2C_None;
}

/* A. 式の途中で自動GCが走っても左オペランドが生きている。 */
static int temp_operand_survives_collection(void) {
    p2c_gc_set_threshold(1);
    size_t collections_before = p2c_gc_collections_run();
    g_probe_calls = 0;
    g_probe_left_ok = 0;
    const int pressure = 32;

    /* 生成Cと同じ形。leftはこの式の間、TLSのbinopスタックにしか無い。 */
    P2C_Object *sum = (p2c_binop_begin(p2c_list_from_array((P2C_Object*[]){p2c_obj_from_int(1), p2c_obj_from_int(2)}, 2)),
                       p2c_binop_finish(probe_add, make_pressure_list(pressure)));
    if (!sum) return 10;
    if (p2c_gc_collections_run() == collections_before) return 11; /* 収集が動いていない */
    if (g_pressure_collections == collections_before) return 17;   /* 式の途中で収集が走っていない */
    if (g_pressure_temp_roots < 1) return 12;                      /* TLS一時値がルート化されていない */
    if (g_probe_calls != 1) return 13;
    if (g_probe_left_ok != 1) return 14;                           /* 左オペランドが壊れていた */
    if (p2c_list_len(sum) != 2u + (size_t)pressure) return 15;
    if (p2c_binop_depth() != 0) return 16;
    return 0;
}

/* B. 処理中の例外（p2c_active_exception）がルート化される。 */
static int active_exception_survives_collection(void) {
    p2c_gc_set_threshold(1);
    P2C_Object *exc = p2c_make_exception("ValueError", "gc temp root probe");
    if (!exc) return 20;
    p2c_active_exception = exc;
    p2c_gc_collect();
    if (p2c_gc_last_temp_roots() < 1) { p2c_active_exception = NULL; return 21; }
    if (!exc->u.v_exception.msg || strcmp(exc->u.v_exception.msg, "gc temp root probe") != 0) {
        p2c_active_exception = NULL;
        return 22;
    }
    p2c_active_exception = NULL;
    p2c_gc_collect();
    if (p2c_gc_last_temp_roots() != 0) return 23; /* 解除後はルート化しない */
    return 0;
}


/* C. 深さの保存/巻き戻しそのものの契約。 */
static int rewind_restores_depths(void) {
    if (p2c_binop_depth() != 0 || p2c_fstr_depth() != 0) return 30;

    p2c_binop_begin(p2c_obj_from_int(1));
    p2c_fstr_begin();
    if (p2c_binop_depth() != 1 || p2c_fstr_depth() != 1) return 31;

    /* 積んだ状態でも収集が通ること（TLSの左オペランドがルート化される）。 */
    p2c_gc_collect();
    if (p2c_gc_last_temp_roots() < 1) return 32;

    p2c_binop_rewind(0);
    p2c_fstr_rewind(0);
    if (p2c_binop_depth() != 0 || p2c_fstr_depth() != 0) return 33;

    /* 冪等: 保存値以下の深さでは何もしない。 */
    p2c_binop_rewind(0);
    p2c_fstr_rewind(0);
    if (p2c_binop_depth() != 0 || p2c_fstr_depth() != 0) return 34;

    /* 容量を超える保存値は「スタック全体を空にする」扱い。 */
    p2c_binop_begin(p2c_obj_from_int(1));
    p2c_fstr_begin();
    p2c_binop_rewind(9999);
    p2c_fstr_rewind(9999);
    if (p2c_binop_depth() != 0 || p2c_fstr_depth() != 0) return 35;

    /* 一部だけ戻す（内側のtryが外側の一時値を壊さない）。 */
    p2c_binop_begin(p2c_obj_from_int(1));
    p2c_binop_begin(p2c_obj_from_int(2));
    p2c_fstr_begin();
    p2c_fstr_begin();
    p2c_binop_rewind(1);
    p2c_fstr_rewind(1);
    if (p2c_binop_depth() != 1 || p2c_fstr_depth() != 1) return 36;
    p2c_binop_rewind(0);
    p2c_fstr_rewind(0);
    if (p2c_binop_depth() != 0 || p2c_fstr_depth() != 0) return 37;
    return 0;
}

/* D. 式の途中で例外が200回脱出しても一時値が残らない。
 *    生成Cのtryと同じく、例外フレームへ到達した時点で保存値へ戻す。 */
static int escaped_exception_does_not_leak_depth(void) {
    const int iterations = 200;
    volatile int caught_binop = 0;
    volatile int caught_fstr = 0;

    for (volatile int i = 0; i < iterations; i++) {
        P2C_ExceptFrame ef;
        ef.prev = p2c_exc_stack;
        ef.exc = NULL;
        p2c_exc_stack = &ef;
        volatile size_t saved_binop = p2c_binop_depth();
        volatile size_t saved_fstr = p2c_fstr_depth();
        if (P2C_SETJMP(ef.env) == 0) {
            /* 右オペランドの評価中に例外が脱出する。 */
            (void)(p2c_binop_begin(p2c_list_from_array((P2C_Object*[]){p2c_obj_from_int(1)}, 1)),
                   p2c_binop_finish(p2c_obj_add, raising_operand()));
        } else {
            if (!p2c_exc_name_match(ef.exc, "ValueError")) return 40; /* nesting limit等の誤発火 */
            p2c_binop_rewind(saved_binop);
            p2c_fstr_rewind(saved_fstr);
            caught_binop++;
        }
        p2c_exc_stack = ef.prev;
    }

    for (volatile int i = 0; i < iterations; i++) {
        P2C_ExceptFrame ef;
        ef.prev = p2c_exc_stack;
        ef.exc = NULL;
        p2c_exc_stack = &ef;
        volatile size_t saved_binop = p2c_binop_depth();
        volatile size_t saved_fstr = p2c_fstr_depth();
        if (P2C_SETJMP(ef.env) == 0) {
            /* f-stringの評価中に例外が脱出する（ビルダはネイティブ確保）。 */
            p2c_fstr_begin();
            p2c_fstr_append(raising_operand());
            (void)p2c_fstr_finish();
        } else {
            if (!p2c_exc_name_match(ef.exc, "ValueError")) return 45;
            p2c_binop_rewind(saved_binop);
            p2c_fstr_rewind(saved_fstr);
            caught_fstr++;
        }
        p2c_exc_stack = ef.prev;
    }

    if (caught_binop != iterations) return 41;
    if (caught_fstr != iterations) return 46;
    if (p2c_binop_depth() != 0) return 42;
    if (p2c_fstr_depth() != 0) return 47;

    /* 後始末の後も式とf-stringが正しく動く。 */
    P2C_Object *sum = (p2c_binop_begin(p2c_obj_from_int(20)),
                       p2c_binop_finish(p2c_obj_add, p2c_obj_from_int(22)));
    if (!sum || p2c_obj_as_int(sum) != 42) return 43;

    p2c_fstr_begin();
    p2c_fstr_append(p2c_obj_from_int(7));
    P2C_Object *text = p2c_fstr_finish();
    if (!text || p2c_len(text) != 1) return 48;
    return 0;
}

/* E. 停止時の後始末（式評価中の一時値を空にする）。 */
static int shutdown_clears_expression_state(void) {
    p2c_binop_begin(p2c_obj_from_int(1));
    p2c_fstr_begin();
    if (p2c_binop_depth() != 1 || p2c_fstr_depth() != 1) return 50;
    p2c_runtime_shutdown();
    if (p2c_binop_depth() != 0) return 51;
    if (p2c_fstr_depth() != 0) return 52;
    if (p2c_gc_last_temp_roots() != 0) return 53;
    return 0;
}

int main(void) {
    int stack_mark = 0;
    p2c_runtime_init(NULL, 0);
    p2c_gc_init(&stack_mark);

    /* スタック境界が無い環境では p2c_gc_collect() が安全側で停止するため、
     * このテストの前提（収集が実際に走る）を満たさない。 */
    if (!p2c_gc_stack_scan_available()) return 60;

    int status = temp_operand_survives_collection();
    if (status != 0) return status;
    status = active_exception_survives_collection();
    if (status != 0) return status;
    status = rewind_restores_depths();
    if (status != 0) return status;
    status = escaped_exception_does_not_leak_depth();
    if (status != 0) return status;
    /* 収集が実際に走っていることは各フェーズで確認済み（ここまでで不足なら
     * 上のどのチェックでも失敗する）。最後に停止時の後始末を確認する。 */
    status = shutdown_clears_expression_state();
    if (status != 0) return status;
    return 0;
}
