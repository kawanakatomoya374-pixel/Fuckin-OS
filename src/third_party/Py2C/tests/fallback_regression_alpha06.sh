#!/bin/sh
# 未対応構文のフォールバック（--fallback）の回帰 (Alpha0.6)
#
#   1) 既定（strict）: 未対応構文は位置付きの明確な診断で失敗する
#   2) --fallback: 変換とビルドは成功し、「到達しなければ」そのまま動く
#   3) --fallback: 到達した場合は実行時に NotImplementedError になる
set -eu

P2C_COMPILER=${P2C_COMPILER:-./build/python-code-to-c}
CC=${CC:-cc}
mkdir -p build/tests
WORK=build/tests/fallback
mkdir -p "$WORK"

compile_and_run() {
    # $1: 生成C, $2: 出力バイナリ（pgameモジュールは不要なので無効化）
    $CC -I./include -DPYTHON_CODE_TO_C_NO_PYGAME "$1" src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c -lm -o "$2"
}

# ── 1) strict: 明確な診断 ─────────────────────────────────────────────
printf '%s\n' 'value = b"abc"' > "$WORK/strict.py"
if "$P2C_COMPILER" "$WORK/strict.py" -o "$WORK/strict.c" > "$WORK/strict.out" 2>&1; then
    printf '%s\n' 'fallback_regression_failed: strict mode accepted an unsupported construct' >&2
    exit 1
fi
if ! grep -q 'unsupported syntax: bytes literals' "$WORK/strict.out"; then
    printf '%s\n' 'fallback_regression_failed: strict diagnostic message changed' >&2
    cat "$WORK/strict.out" >&2
    exit 1
fi

# ── 2) fallback: 到達しない行はそのまま動く ──────────────────────────
printf '%s\n' \
    'if False:' \
    '    value = b"abc"' \
    'print("fallback_reached_ok")' > "$WORK/skip.py"
"$P2C_COMPILER" "$WORK/skip.py" --fallback -o "$WORK/skip.c"
if ! grep -q 'p2c_fallback_expr' "$WORK/skip.c"; then
    printf '%s\n' 'fallback_regression_failed: no runtime stub was generated' >&2
    exit 1
fi
compile_and_run "$WORK/skip.c" "$WORK/skip.bin"
"$WORK/skip.bin" > "$WORK/skip.out" 2>&1
if ! grep -q '^fallback_reached_ok$' "$WORK/skip.out"; then
    printf '%s\n' 'fallback_regression_failed: program with unreached stub did not run' >&2
    cat "$WORK/skip.out" >&2
    exit 1
fi

# ── 3) fallback: 到達したら NotImplementedError ──────────────────────
printf '%s\n' 'value = b"abc"' 'print("should not print")' > "$WORK/reach.py"
"$P2C_COMPILER" "$WORK/reach.py" --fallback -o "$WORK/reach.c"
compile_and_run "$WORK/reach.c" "$WORK/reach.bin"
if "$WORK/reach.bin" > "$WORK/reach.out" 2>&1; then
    printf '%s\n' 'fallback_regression_failed: stub did not raise at runtime' >&2
    exit 1
fi
if ! grep -q 'NotImplementedError' "$WORK/reach.out"; then
    printf '%s\n' 'fallback_regression_failed: runtime stub did not report NotImplementedError' >&2
    cat "$WORK/reach.out" >&2
    exit 1
fi

printf '%s\n' 'fallback_regression_ok: strict diagnostics, unreached stub runs, reached stub raises NotImplementedError'
