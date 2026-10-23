#include "calc_engine.h"

int calc_engine_init(void) {
    return 0;
}

bool calc_engine_evaluate(const char* expr, bool deg_mode, calc_engine_result_t* out) {
    return calc_school_math_evaluate(expr, deg_mode, out);
}

const char* calc_engine_name(void) {
    return "School Math Engine";
}
