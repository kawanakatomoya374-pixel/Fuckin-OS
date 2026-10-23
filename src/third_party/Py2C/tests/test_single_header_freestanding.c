#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#define PYTHON_CODE_TO_C_NO_STDLIB
#endif
#define P2C_SINGLE_HEADER_IMPLEMENTATION
#include "python_code_to_c_single.h"

void p2c_single_header_freestanding_compile_contract(void);

void p2c_single_header_freestanding_compile_contract(void) {
    P2C_TranspileOptions options = P2C_DEFAULT_TRANSPILER_OPTIONS;
    options.baremetal = true;
    options.strict_c11 = true;
    (void)options;
}
