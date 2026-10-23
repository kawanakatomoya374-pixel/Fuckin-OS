#include "baremetal/python_code_to_c_baremetal.h"

static P2C_Object* baremetal_child_step(P2C_Object *generator) {
    return p2c_generator_finish(generator, p2c_obj_from_int(17));
}

static P2C_Object* baremetal_parent_step(P2C_Object *generator) {
    switch (p2c_generator_state(generator)) {
        case 0:
            return p2c_generator_await(generator, p2c_generator_local_get(generator, "child"), 1);
        default:
            return p2c_generator_finish(generator,
                                        p2c_obj_add(p2c_generator_await_result(generator), p2c_obj_from_int(5)));
    }
}

static P2C_Object* baremetal_program(void) {
    P2C_Object *child = p2c_generator_new(baremetal_child_step, true);
    P2C_Object *parent = p2c_generator_new(baremetal_parent_step, true);
    p2c_gc_register_root(&child);
    p2c_gc_register_root(&parent);
    p2c_generator_local_set(parent, "child", child);
    P2C_Object *result = p2c_async_run(parent);
    p2c_print_multi((P2C_Object*[]){result}, 1);
    p2c_gc_unregister_root(&parent);
    p2c_gc_unregister_root(&child);
    return result;
}

int main(void) {
    static unsigned char heap[65536];
    static char console[64];
    P2C_BaremetalBoard board;
    p2c_baremetal_board_init(&board, heap, sizeof(heap), console, sizeof(console));
    p2c_baremetal_board_tick(&board, 10);
    if (p2c_baremetal_run(&board, baremetal_program) != 0) return 1;
    if (board.ticks != 10) return 2;
    if (console[0] != '2' || console[1] != '2' || console[2] != '\n' || console[3] != '\0') return 3;
    return 0;
}
