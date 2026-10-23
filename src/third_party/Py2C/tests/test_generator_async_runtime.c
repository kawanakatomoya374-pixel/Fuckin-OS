#include "runtime/python_code_to_c_runtime.h"

static P2C_Object* count_step(P2C_Object *generator) {
    switch (p2c_generator_state(generator)) {
        case 0:
            p2c_generator_local_set(generator, "base", p2c_obj_from_int(7));
            return p2c_generator_yield(generator, p2c_generator_local_get(generator, "base"), 1);
        case 1:
            return p2c_generator_yield(generator,
                                       p2c_obj_add(p2c_generator_local_get(generator, "base"), p2c_obj_from_int(2)),
                                       2);
        default:
            return p2c_generator_finish(generator, p2c_obj_from_int(99));
    }
}

static P2C_Object* child_step(P2C_Object *generator) {
    (void)generator;
    return p2c_generator_finish(generator, p2c_obj_from_int(17));
}

static P2C_Object* parent_step(P2C_Object *generator) {
    switch (p2c_generator_state(generator)) {
        case 0:
            return p2c_generator_await(generator, p2c_generator_local_get(generator, "child"), 1);
        default:
            return p2c_generator_finish(generator,
                                        p2c_obj_add(p2c_generator_await_result(generator), p2c_obj_from_int(5)));
    }
}

static int expects_stop_iteration(P2C_Object *generator) {
    P2C_ExceptFrame frame;
    frame.exc = NULL;
    frame.prev = p2c_exc_stack;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        (void)p2c_builtin_next(generator);
        p2c_exc_stack = frame.prev;
        return 0;
    }
    p2c_exc_stack = frame.prev;
    return p2c_exc_name_match(frame.exc, "StopIteration") ? 1 : 0;
}

int main(void) {
    p2c_runtime_init(NULL, 0);
    P2C_GC_ENTER_MAIN();

    P2C_Object *generator = p2c_generator_new(count_step, false);
    p2c_gc_register_root(&generator);
    if (p2c_obj_as_int(p2c_builtin_next(generator)) != 7) return 1;
    p2c_gc_collect();
    if (p2c_obj_as_int(p2c_builtin_next(generator)) != 9) return 2;
    if (!expects_stop_iteration(generator)) return 3;
    if (!p2c_generator_is_done(generator)) return 4;
    if (p2c_obj_as_int(p2c_generator_result(generator)) != 99) return 5;

    P2C_Object *child = p2c_generator_new(child_step, true);
    P2C_Object *parent = p2c_generator_new(parent_step, true);
    p2c_gc_register_root(&child);
    p2c_gc_register_root(&parent);
    p2c_generator_local_set(parent, "child", child);
    if (p2c_obj_as_int(p2c_async_run(parent)) != 22) return 6;
    if (!p2c_generator_is_done(child) || !p2c_generator_is_done(parent)) return 7;
    if (p2c_async_pending_count() != 0) return 8;

    p2c_gc_unregister_root(&parent);
    p2c_gc_unregister_root(&child);
    p2c_gc_unregister_root(&generator);
    p2c_runtime_shutdown();
    return 0;
}
