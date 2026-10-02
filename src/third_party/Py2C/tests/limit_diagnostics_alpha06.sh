#!/bin/sh
# 固定上限の診断と -D 上書きの検証（Alpha0.6）
#   1) 既定の上限: 通知は出ず、結果は CPython と一致する
#   2) -DP2C_MAX_CLASS_REGISTRY=2: 「上限に達した」と必ず通知する（黙って飛ばさない）
#   3) -DP2C_MRO_MAX_NAMES=2: C3 を計算できないと必ず通知する（黙って違う結果を返さない）
#   4) 広げた上限 (-DP2C_MAX_CLASS_REGISTRY=1024, -DP2C_MRO_MAX_NAMES=32): 通知なし・結果一致
#
# 通知はランタイムの他のメッセージと同じく p2c_platform_write（= stdout）へ出るため、
# プログラム自身の出力と比較するときは "p2c: " 行を除いてから比べる。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

PY=${PYTHON:-python3}
CC=${CC:-cc}
OUT=build/limits
SRCS="src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c"

fail() {
    printf 'limit_diagnostics: FAIL: %s\n' "$1"
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT" || exit 1

[ -x ./build/python-code-to-c ] || fail "compiler not built (run: make all)"

./build/python-code-to-c tests/limit_diagnostics_alpha06.py -o "$OUT/limit.c" >/dev/null 2>&1 \
    || fail "transpile tests/limit_diagnostics_alpha06.py"

"$PY" tests/limit_diagnostics_alpha06.py > "$OUT/expected.txt" 2>&1 \
    || fail "reference run with $PY"
EXPECTED=$(cat "$OUT/expected.txt")
[ -n "$EXPECTED" ] || fail "reference output is empty"

build() {
    name=$1
    shift
    # shellcheck disable=SC2086
    "$CC" -I./include -std=gnu11 -O1 "$@" "$OUT/limit.c" $SRCS -lm -o "$OUT/$name" \
        2> "$OUT/$name.build.log" || return 1
    return 0
}

# --- 1) 既定の上限 --------------------------------------------------------
build default || fail "build (default limits); see $OUT/default.build.log"
"$OUT/default" > "$OUT/default.out" 2> "$OUT/default.err"
cat "$OUT/default.out" "$OUT/default.err" > "$OUT/default.all"
if grep -q 'limit exceeded' "$OUT/default.all"; then
    fail "default: unexpected limit notification: $(grep 'limit exceeded' "$OUT/default.all")"
fi
grep -v '^p2c: ' "$OUT/default.out" > "$OUT/default.prog"
[ "$(cat "$OUT/default.prog")" = "$EXPECTED" ] || fail "default: stdout differs from CPython"
printf 'limit_diagnostics: 1/4 default limits OK (no notification, CPython-identical)\n'

# --- 2) クラスレジストリを絞る（無言スキップ廃止の確認） -------------------
build reg2 -DP2C_MAX_CLASS_REGISTRY=2 || fail "build (-DP2C_MAX_CLASS_REGISTRY=2)"
"$OUT/reg2" > "$OUT/reg2.out" 2> "$OUT/reg2.err"
cat "$OUT/reg2.out" "$OUT/reg2.err" > "$OUT/reg2.all"
grep -q 'limit exceeded: class registry' "$OUT/reg2.all" \
    || fail "registry: expected 'limit exceeded: class registry' in the output"
grep -q 'P2C_MAX_CLASS_REGISTRY' "$OUT/reg2.all" \
    || fail "registry: notification should mention the -D override"
printf 'limit_diagnostics: 2/4 class registry overflow is reported (not skipped)\n'

# --- 3) MRO 上限を絞る（結果は同じか、さもなくば必ず通知） -----------------
build mro2 -DP2C_MRO_MAX_NAMES=2 || fail "build (-DP2C_MRO_MAX_NAMES=2)"
"$OUT/mro2" > "$OUT/mro2.out" 2> "$OUT/mro2.err"
cat "$OUT/mro2.out" "$OUT/mro2.err" > "$OUT/mro2.all"
grep -q 'limit exceeded: C3 MRO' "$OUT/mro2.all" \
    || fail "mro2: expected 'limit exceeded: C3 MRO' in the output"
grep -v '^p2c: ' "$OUT/mro2.out" > "$OUT/mro2.prog"
if [ "$(cat "$OUT/mro2.prog")" = "$EXPECTED" ]; then
    printf 'limit_diagnostics: 3/4 MRO fallback announced and still CPython-identical\n'
elif grep -Eq 'Error|unhandled|Aborted' "$OUT/mro2.all"; then
    printf 'limit_diagnostics: 3/4 MRO fallback announced and degraded loudly (no silent answer)\n'
else
    fail "mro2: silent divergence from CPython with no limit notification"
fi

# --- 4) 上限を広げる（上書きが効くこと） ----------------------------------
build wide -DP2C_MAX_CLASS_REGISTRY=1024 -DP2C_MRO_MAX_NAMES=32 \
    || fail "build (-DP2C_MAX_CLASS_REGISTRY=1024 -DP2C_MRO_MAX_NAMES=32)"
"$OUT/wide" > "$OUT/wide.out" 2> "$OUT/wide.err"
cat "$OUT/wide.out" "$OUT/wide.err" > "$OUT/wide.all"
if grep -q 'limit exceeded' "$OUT/wide.all"; then
    fail "wide: unexpected limit notification: $(grep 'limit exceeded' "$OUT/wide.all")"
fi
grep -v '^p2c: ' "$OUT/wide.out" > "$OUT/wide.prog"
[ "$(cat "$OUT/wide.prog")" = "$EXPECTED" ] || fail "wide: stdout differs from CPython"
printf 'limit_diagnostics: 4/4 raised limits work (-D overrides honoured)\n'

printf 'limit_diagnostics_ok: 4 cases passed\n'
