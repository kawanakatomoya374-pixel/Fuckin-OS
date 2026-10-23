#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
CC=${CC:-cc}
set -eu

run_case() {
    pyfile="$1"
    stem=$(basename "$pyfile" .py)
    cfile="$BD/tests/${stem}.c"
    binfile="$BD/tests/${stem}"
    pyout="$BD/tests/${stem}.python.out"
    cout="$BD/tests/${stem}.c.out"

    python3 "$pyfile" > "$pyout"
    ./$BD/python-code-to-c "$pyfile" -o "$cfile"
    "$CC" -I./include -Wall -Wextra -Werror -std=gnu11 "$cfile" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c -lm -o "$binfile"
    "$binfile" > "$cout"
    diff -u "$pyout" "$cout"
}

mkdir -p $BD/tests
run_case tests/set_alpha10.py
run_case tests/set_comprehension_alpha10.py
run_case tests/frozenset_fallback_alpha10.py
printf '%s\n' 'set_regression_ok'
