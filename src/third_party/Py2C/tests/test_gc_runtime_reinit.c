#include "runtime/python_code_to_c_runtime.h"

/* 最初の初期化で登録される数（組み込み/例外クラス）。再初期化ではこの値に戻る。 */
static int expected_base = -1;
static int base_probe = 0;

static void create_unrooted_cycle(void) {
    P2C_Object *left = p2c_list_new();
    P2C_Object *right = p2c_list_new();
    if (!left || !right) return;
    p2c_list_append(left, right);
    p2c_list_append(right, left);
}

static int exercise_epoch(void) {
    P2C_Object *root = p2c_list_new();
    if (!root) return 20;
    p2c_gc_register_root(&root);
    p2c_gc_register_root(&root);
    if (p2c_gc_root_count() != 1) return 21;

    for (int i = 0; i < 96; i++) {
        P2C_Object *child = p2c_list_new();
        if (!child) return 22;
        p2c_list_append(child, p2c_obj_from_int(i));
        p2c_list_append(root, child);
        if ((i % 8) == 0) p2c_gc_collect();
    }
    p2c_obj_incref(root);
    p2c_gc_collect();
    p2c_obj_decref(root);
    p2c_gc_unregister_root(&root);
    if (p2c_gc_root_count() != 0) return 23;
    root = NULL;
    create_unrooted_cycle();
    p2c_gc_collect();
    return p2c_gc_is_collecting() ? 24 : 0;
}

int main(void) {
    int stack_mark = 0;
    for (int epoch = 0; epoch < 3; epoch++) {
        p2c_runtime_init(NULL, 0);
        if (!p2c_runtime_is_active()) return 10;
        /* ランタイムコンテキストは初期化のたびに同じ状態から始まる
         * （前エポックのクラス登録が積み上がらない）。 */
        base_probe = (int)p2c_runtime_class_count();
        if (expected_base < 0) expected_base = base_probe;
        else if (base_probe != expected_base) return 14;
        p2c_gc_init(&stack_mark);
        p2c_gc_set_threshold(1);
        if (!p2c_class_new("ReinitProbe", NULL, NULL, NULL)) return 11;
        if (p2c_runtime_class_count() != (size_t)base_probe + 1) return 15;
        int status = exercise_epoch();
        if (status != 0) return status;
        p2c_runtime_shutdown();
        if (p2c_runtime_is_active()) return 12;
        if (p2c_runtime_class_count() != 0) return 16;
        if (p2c_gc_root_count() != 0 || p2c_gc_object_count() != 0) return 13;
    }
    return 0;
}
