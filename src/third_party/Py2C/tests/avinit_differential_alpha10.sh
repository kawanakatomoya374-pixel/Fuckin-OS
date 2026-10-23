#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
# 未初期化スタック（自動変数の初期化漏れ）読み出しを差分で検出する。
# 変換器を通常ビルドと -ftrivial-auto-var-init=pattern（スタックを 0xFE で埋める）
# ビルドの2通り用意し、同じコーパスを変換する。生成Cが1バイトでも違えば、
# 未初期化データが出力へ影響している（= 実バグ）。
#
#   使い方: sh tests/avinit_differential_alpha10.sh <通常変換器> <pattern変換器>
#   AVINIT_RUN=1 を付けると、生成プログラム自体も両構成でビルド・実行して
#   標準出力と終了コードを比較する（ランタイム側の未初期化読み出しも検出）。
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

REF=${1:-./$BD/python-code-to-c}
PAT=${2:-./$BD/avinit/python-code-to-c}
RUN_PROGRAMS=${AVINIT_RUN:-0}
OUT=$BD/avinit-diff
CC=${CC:-cc}
SRCS="src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c"

[ -x "$REF" ] || { printf '%s\n' "avinit: missing reference compiler: $REF"; exit 1; }
[ -x "$PAT" ] || { printf '%s\n' "avinit: missing pattern compiler: $PAT"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/ref" "$OUT/pat"

total=0
bad=0
checked=0
for f in tests/*.py examples/*.py; do
    [ -f "$f" ] || continue
    stem=$(basename "$f" .py)
    case "$stem" in *asan* | *diagnostic*) continue ;; esac
    total=$((total + 1))
    # 変換自体できない例（診断専用フィクスチャ）は比較対象から外す。
    if ! "$REF" "$f" -o "$OUT/ref/$stem.c" > /dev/null 2> "$OUT/ref/$stem.err"; then
        continue
    fi
    checked=$((checked + 1))
    if ! "$PAT" "$f" -o "$OUT/pat/$stem.c" > /dev/null 2> "$OUT/pat/$stem.err"; then
        printf 'AVINIT %-42s PATTERN-CONVERT-FAIL\n' "$f"
        bad=$((bad + 1))
        continue
    fi
    if ! diff -q "$OUT/ref/$stem.c" "$OUT/pat/$stem.c" > /dev/null; then
        printf 'AVINIT %-42s OUTPUT-DIFFERS (未初期化データが生成Cに影響)\n' "$f"
        diff "$OUT/ref/$stem.c" "$OUT/pat/$stem.c" | head -6 | sed 's/^/    /'
        bad=$((bad + 1))
        continue
    fi
    if [ "$RUN_PROGRAMS" = "1" ]; then
        # shellcheck disable=SC2086
        $CC -I./include -std=gnu11 -O1 "$OUT/ref/$stem.c" $SRCS -lm -o "$OUT/ref/$stem" 2> /dev/null || continue
        # shellcheck disable=SC2086
        $CC -I./include -std=gnu11 -O1 -ftrivial-auto-var-init=pattern "$OUT/ref/$stem.c" $SRCS -lm -o "$OUT/pat/$stem" 2> /dev/null || continue
        ref_rc=0; pat_rc=0
        "$OUT/ref/$stem" > "$OUT/ref/$stem.out" 2>&1 || ref_rc=$?
        "$OUT/pat/$stem" > "$OUT/pat/$stem.out" 2>&1 || pat_rc=$?
        if [ "$ref_rc" != "$pat_rc" ]; then
            printf 'AVINIT %-42s EXIT-CODE-DIFFERS (ref=%s pattern=%s)\n' "$f" "$ref_rc" "$pat_rc"
            bad=$((bad + 1))
            continue
        fi
        if ! diff -q "$OUT/ref/$stem.out" "$OUT/pat/$stem.out" > /dev/null; then
            printf 'AVINIT %-42s STDOUT-DIFFERS (未初期化データが実行結果に影響)\n' "$f"
            diff "$OUT/ref/$stem.out" "$OUT/pat/$stem.out" | head -6 | sed 's/^/    /'
            bad=$((bad + 1))
        fi
    fi
done

printf 'avinit_differential: %d files (%d convertible), %d suspicious\n' "$total" "$checked" "$bad"
[ "$bad" -eq 0 ]
