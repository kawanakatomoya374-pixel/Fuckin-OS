#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build/python-code-to-c"
CC_BIN=${CC:-cc}
CFLAGS_TEST=${CFLAGS:-"-Wall -Wextra -Werror -std=c11 -O2"}
LDLIBS_TEST=${LDLIBS:--lm}
TMP=${TMPDIR:-/tmp}/p2c-audit-$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP"

compile_and_run() {
    name=$1
    "$BIN" "$ROOT/tests/$name.py" -o "$TMP/$name.c"
    $CC_BIN -I"$ROOT/include" $CFLAGS_TEST "$TMP/$name.c" \
        "$ROOT/src/runtime/python_code_to_c_runtime.c" \
        "$ROOT/src/common/python_code_to_c_common.c" \
        "$ROOT/src/platform/python_code_to_c_platform.c" \
        "$ROOT/src/platform/python_code_to_c_platform_hosted.c" \
        "$ROOT/src/platform/python_code_to_c_gui.c" \
        "$ROOT/src/modules/python_code_to_c_pygame.c" \
        $LDLIBS_TEST -o "$TMP/$name"
    "$TMP/$name" > "$TMP/$name.out"
}

compile_and_run audit_with_semantics
printf '%s\n' \
'bool 7 5 fallback' \
'short False True []' \
'for_break [1]' \
"for_done [1, 2, 'else']" \
"with_raises ['enter', 'body', 'exit', 'after']" > "$TMP/audit_with_semantics.expected"
diff -u "$TMP/audit_with_semantics.expected" "$TMP/audit_with_semantics.out"

compile_and_run audit_augassign_semantics
printf '%s\n' "5 [13] ['box', 'list', 'index']" > "$TMP/audit_augassign_semantics.expected"
diff -u "$TMP/audit_augassign_semantics.expected" "$TMP/audit_augassign_semantics.out"

compile_and_run delete_alpha06
printf '%s\n' 'attr_deleted' '[10, 30] missing 1' > "$TMP/delete_alpha06.expected"
diff -u "$TMP/delete_alpha06.expected" "$TMP/delete_alpha06.out"

compile_and_run bitops_alpha06
printf '%s\n' '1 7 6 -6' '48 16 -5' '34' > "$TMP/bitops_alpha06.expected"
diff -u "$TMP/bitops_alpha06.expected" "$TMP/bitops_alpha06.out"

# C77: finally内のreturn/break/continueは、finally本体を実行したうえで
# 進行中の制御フローを上書きする（Pythonと同じ）。以前は安全側に倒して
# 意味解析エラーにしていたが、生成Cがクリーンアップフレームで正しく扱えるように
# なったため受理し、CPythonと出力が一致することを回帰として固定する。
compile_and_run finally_control_flow_alpha06
python3 "$ROOT/tests/finally_control_flow_alpha06.py" > "$TMP/finally_control_flow_alpha06.expected"
diff -u "$TMP/finally_control_flow_alpha06.expected" "$TMP/finally_control_flow_alpha06.out"
printf '%s\n' 'C77: finally control-flow semantics passed'

compile_and_run nonlocal_audit_alpha06
printf '%s\n' '12 14 14' > "$TMP/nonlocal_audit_alpha06.expected"
diff -u "$TMP/nonlocal_audit_alpha06.expected" "$TMP/nonlocal_audit_alpha06.out"
printf '%s\n' 'A01: nonlocal shared-cell semantics passed'

# GCH-010: 式の途中で例外が脱出してもTLSの一時値（binopの左オペランド、
# f-stringビルダ）が残らない。以前は65回目／33回目でネスト上限の
# RuntimeErrorを誤発火し、except ValueErrorを素通りしていた。
compile_and_run gc_temp_roots_alpha06
printf '%s\n' '200 200' '30002' '[1, 2, 3]' 'sum=7' > "$TMP/gc_temp_roots_alpha06.expected"
diff -u "$TMP/gc_temp_roots_alpha06.expected" "$TMP/gc_temp_roots_alpha06.out"
printf '%s\n' 'GCH-010: escaped expression unwind passed'

"$BIN" --c11 "$ROOT/tests/c11_safe.py" -o "$TMP/c11_safe.c"
$CC_BIN -std=c11 -pedantic-errors -Wall -Wextra -Werror -I"$ROOT/include" "$TMP/c11_safe.c" \
    "$ROOT/src/runtime/python_code_to_c_runtime.c" \
    "$ROOT/src/common/python_code_to_c_common.c" \
    "$ROOT/src/platform/python_code_to_c_platform.c" \
    "$ROOT/src/platform/python_code_to_c_platform_hosted.c" \
    "$ROOT/src/platform/python_code_to_c_gui.c" \
    "$ROOT/src/modules/python_code_to_c_pygame.c" \
    -lm -o "$TMP/c11_safe"
"$TMP/c11_safe" > "$TMP/c11_safe.out"
printf '49\n' > "$TMP/c11_safe.expected"
diff -u "$TMP/c11_safe.expected" "$TMP/c11_safe.out"
printf '%s\n' 'C78: strict ISO C11 accepted path passed'

if "$BIN" --c11 "$ROOT/tests/complex_alpha06.py" -o "$TMP/c11_complex.c" > "$TMP/c11_complex.out" 2>&1; then
    echo 'expected strict ISO C11 diagnostic was not emitted' >&2
    exit 1
fi
grep -F 'strict ISO C11 mode' "$TMP/c11_complex.out" >/dev/null
printf '%s\n' 'C79: strict ISO C11 rejection path passed'

echo 'audit_regression_ok'
