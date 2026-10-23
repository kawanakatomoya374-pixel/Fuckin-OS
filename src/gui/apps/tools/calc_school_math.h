#ifndef CALC_SCHOOL_MATH_H
#define CALC_SCHOOL_MATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool handled;
    char display[160];
    char status[96];
    char steps[4096];
} calc_school_result_t;

bool calc_school_math_evaluate(const char* expr, bool deg_mode, calc_school_result_t* out);

#endif
