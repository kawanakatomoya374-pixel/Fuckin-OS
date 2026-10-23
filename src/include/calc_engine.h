#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

#include <stdbool.h>
#include "calc_school_math.h"

typedef calc_school_result_t calc_engine_result_t;

int calc_engine_init(void);
bool calc_engine_evaluate(const char* expr, bool deg_mode, calc_engine_result_t* out);
const char* calc_engine_name(void);

#endif
