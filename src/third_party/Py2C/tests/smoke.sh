#!/bin/sh
set -eu

compile_run() {
    pyfile="$1"
    cfile="$2"
    binfile="$3"
    ./bin/python_code_to_c "$pyfile" -o "$cfile"
    cc -I./include "$cfile" src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c -lm -o "$binfile"
    "$binfile"
}

compile_run examples/basic.py /tmp/python_code_to_c_basic.c /tmp/python_code_to_c_basic
compile_run examples/fib.py /tmp/python_code_to_c_fib.c /tmp/python_code_to_c_fib
compile_run examples/containers.py /tmp/python_code_to_c_containers.c /tmp/python_code_to_c_containers
compile_run examples/class_counter.py /tmp/python_code_to_c_class_counter.c /tmp/python_code_to_c_class_counter
compile_run examples/import_try.py /tmp/python_code_to_c_import_try.c /tmp/python_code_to_c_import_try
compile_run examples/varargs.py /tmp/python_code_to_c_varargs.c /tmp/python_code_to_c_varargs
compile_run examples/precision.py /tmp/python_code_to_c_precision.c /tmp/python_code_to_c_precision
