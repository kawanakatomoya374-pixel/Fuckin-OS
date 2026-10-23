#include "baremetal/python_code_to_c_baremetal.h"

static P2C_Object* pulse_step(P2C_Object *generator) {
    switch (p2c_generator_state(generator)) {
        case 0:
            return p2c_generator_yield(generator, p2c_obj_from_int(7), 1);
        case 1:
            return p2c_generator_yield(generator, p2c_obj_from_int(9), 2);
        default:
            return p2c_generator_finish(generator, &P2C_None);
    }
}

static P2C_Object* sensor_step(P2C_Object *generator) {
    return p2c_generator_finish(generator, p2c_obj_from_int(17));
}

static P2C_Object* boot_step(P2C_Object *generator) {
    switch (p2c_generator_state(generator)) {
        case 0:
            return p2c_generator_await(generator, p2c_generator_local_get(generator, "sensor"), 1);
        default:
            return p2c_generator_finish(generator,
                                        p2c_obj_add(p2c_generator_await_result(generator), p2c_obj_from_int(5)));
    }
}

P2C_Object* p2c_baremetal_hello_program(void) {
    P2C_Object *pulses = p2c_generator_new(pulse_step, false);
    P2C_Object *sensor = p2c_generator_new(sensor_step, true);
    P2C_Object *boot = p2c_generator_new(boot_step, true);
    p2c_gc_register_root(&pulses);
    p2c_gc_register_root(&sensor);
    p2c_gc_register_root(&boot);
    p2c_generator_local_set(boot, "sensor", sensor);
    p2c_print_multi((P2C_Object*[]){p2c_builtin_next(pulses)}, 1);
    p2c_print_multi((P2C_Object*[]){p2c_builtin_next(pulses)}, 1);
    p2c_print_multi((P2C_Object*[]){p2c_async_run(boot)}, 1);
    p2c_gc_unregister_root(&boot);
    p2c_gc_unregister_root(&sensor);
    p2c_gc_unregister_root(&pulses);
    return &P2C_None;
}
