#include <stdio.h>
#include "runtime/python_code_to_c_runtime.h"

typedef P2C_Object* (*P2C_BinaryOp)(P2C_Object *, P2C_Object *);

static bool raises_overflow(P2C_BinaryOp op, int64_t left, int64_t right) {
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        (void)op(p2c_obj_from_int(left), p2c_obj_from_int(right));
        p2c_exc_stack = frame.prev;
        return false;
    }
    p2c_exc_stack = frame.prev;
    return p2c_exc_name_match(frame.exc, "OverflowError");
}

int main(void) {
    P2C_GC_ENTER_MAIN();
    p2c_runtime_init(NULL, 0);
    if (!raises_overflow(p2c_obj_add, INT64_MAX, 1)) return 1;
    if (!raises_overflow(p2c_obj_sub, INT64_MIN, 1)) return 2;
    if (!raises_overflow(p2c_obj_mul, INT64_MAX, 2)) return 3;
    if (!raises_overflow(p2c_obj_mul, INT64_MIN, -1)) return 4;
    if (!raises_overflow(p2c_obj_floordiv, INT64_MIN, -1)) return 5;
    if (!raises_overflow(p2c_obj_pow, 2, 63)) return 6;
    if (!raises_overflow(p2c_obj_lshift, 1, 63)) return 7;
    if (p2c_obj_as_int(p2c_obj_mod(p2c_obj_from_int(INT64_MIN), p2c_obj_from_int(-1))) != 0) return 8;
    if (p2c_obj_as_int(p2c_obj_add(p2c_obj_from_int(INT64_MAX), p2c_obj_from_int(-1))) != INT64_MAX - 1) return 9;
    if (p2c_obj_as_int(p2c_obj_sub(p2c_obj_from_int(INT64_MIN), p2c_obj_from_int(-1))) != INT64_MIN + 1) return 10;
    if (p2c_obj_as_int(p2c_obj_mul(p2c_obj_from_int(3037000499LL), p2c_obj_from_int(3037000499LL))) != 9223372030926249001LL) return 11;
    p2c_runtime_shutdown();
    puts("int64_overflow_runtime_ok");
    return 0;
}
