#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
# 指定した Python フィクスチャを変換・ビルドして CPython と出力を比較する簡易差分。
#   使い方: sh tests/quick_diff_alpha10.sh tests/foo_alpha10.py [more...]
set -eu
CC=${CC:-cc}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p $BD/quick
fail=0
for f in "$@"; do
    stem=$(basename "$f" .py)
    ./$BD/python-code-to-c "$f" -o "$BD/quick/$stem.c"
    $CC -I./include -std=gnu11 "$BD/quick/$stem.c" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c -lm -o "$BD/quick/$stem"
    python3 "$f" > "$BD/quick/$stem.pyout" 2>&1 || true
    "./$BD/quick/$stem" > "$BD/quick/$stem.cout" 2>&1 || true
    if diff -q "$BD/quick/$stem.pyout" "$BD/quick/$stem.cout" > /dev/null; then
        lines=$(wc -l < "$BD/quick/$stem.cout" | tr -d ' ')
        printf '%s\n' "$stem: MATCH ($lines lines)"
    else
        printf '%s\n' "$stem: DIFF"
        diff "$BD/quick/$stem.pyout" "$BD/quick/$stem.cout" | head -70
        fail=1
    fi
done
exit $fail
