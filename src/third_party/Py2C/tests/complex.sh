#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
set -eu
CC=${CC:-cc}
mkdir -p $BD/tests
python3 tests/complex_alpha10.py > $BD/tests/complex_alpha10.python.out
./$BD/python-code-to-c tests/complex_alpha10.py -o $BD/tests/complex_alpha10.c
$CC -I./include -Wall -Wextra -Werror -std=gnu11 $BD/tests/complex_alpha10.c \
  src/runtime/python_code_to_c_runtime.c \
  src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
  src/platform/python_code_to_c_platform.c \
  src/platform/python_code_to_c_platform_hosted.c \
  src/platform/python_code_to_c_gui.c \
  src/modules/python_code_to_c_pygame.c -lm -o $BD/tests/complex_alpha10
$BD/tests/complex_alpha10 > $BD/tests/complex_alpha10.c.out
diff -u $BD/tests/complex_alpha10.python.out $BD/tests/complex_alpha10.c.out
cases=$(wc -l < $BD/tests/complex_alpha10.c.out | tr -d ' ')
test "$cases" = "60"
printf '%s\n' 'complex_alpha10: 60 CPython-differential cases passed'
