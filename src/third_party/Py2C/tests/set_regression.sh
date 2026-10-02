#!/bin/sh
set -eu

run_case() {
    pyfile="$1"
    stem=$(basename "$pyfile" .py)
    cfile="build/tests/${stem}.c"
    binfile="build/tests/${stem}"
    pyout="build/tests/${stem}.python.out"
    cout="build/tests/${stem}.c.out"

    python3 "$pyfile" > "$pyout"
    ./build/python-code-to-c "$pyfile" -o "$cfile"
    cc -I./include -Wall -Wextra -Werror -std=gnu11 "$cfile" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c -lm -o "$binfile"
    "$binfile" > "$cout"
    diff -u "$pyout" "$cout"
}

mkdir -p build/tests
run_case tests/set_alpha06.py
run_case tests/set_comprehension_alpha06.py
run_case tests/frozenset_fallback_alpha06.py
printf '%s\n' 'set_regression_ok'
