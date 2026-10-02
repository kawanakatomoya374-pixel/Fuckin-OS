#!/bin/sh
# 指定した Python フィクスチャを変換・ビルドして CPython と出力を比較する簡易差分。
#   使い方: sh tests/quick_diff_alpha06.sh tests/foo_alpha06.py [more...]
set -eu
CC=${CC:-cc}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/quick
fail=0
for f in "$@"; do
    stem=$(basename "$f" .py)
    ./build/python-code-to-c "$f" -o "build/quick/$stem.c"
    $CC -I./include -std=gnu11 "build/quick/$stem.c" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c -lm -o "build/quick/$stem"
    python3 "$f" > "build/quick/$stem.pyout" 2>&1 || true
    "./build/quick/$stem" > "build/quick/$stem.cout" 2>&1 || true
    if diff -q "build/quick/$stem.pyout" "build/quick/$stem.cout" > /dev/null; then
        lines=$(wc -l < "build/quick/$stem.cout" | tr -d ' ')
        printf '%s\n' "$stem: MATCH ($lines lines)"
    else
        printf '%s\n' "$stem: DIFF"
        diff "build/quick/$stem.pyout" "build/quick/$stem.cout" | head -70
        fail=1
    fi
done
exit $fail
