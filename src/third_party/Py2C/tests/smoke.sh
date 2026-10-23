#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
CC=${CC:-cc}
set -eu
mkdir -p "$BD/smoke"

compile_run() {
    pyfile="$1"
    cfile="$2"
    binfile="$3"
    "./$BD/python-code-to-c" "$pyfile" -o "$cfile"
    "$CC" -I./include "$cfile" src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c -lm -o "$binfile"
    "$binfile"
}

compile_run examples/basic.py $BD/smoke/python_code_to_c_basic.c $BD/smoke/python_code_to_c_basic
compile_run examples/fib.py $BD/smoke/python_code_to_c_fib.c $BD/smoke/python_code_to_c_fib
compile_run examples/containers.py $BD/smoke/python_code_to_c_containers.c $BD/smoke/python_code_to_c_containers
compile_run examples/class_counter.py $BD/smoke/python_code_to_c_class_counter.c $BD/smoke/python_code_to_c_class_counter
compile_run examples/import_try.py $BD/smoke/python_code_to_c_import_try.c $BD/smoke/python_code_to_c_import_try
compile_run examples/varargs.py $BD/smoke/python_code_to_c_varargs.c $BD/smoke/python_code_to_c_varargs
compile_run examples/precision.py $BD/smoke/python_code_to_c_precision.c $BD/smoke/python_code_to_c_precision
