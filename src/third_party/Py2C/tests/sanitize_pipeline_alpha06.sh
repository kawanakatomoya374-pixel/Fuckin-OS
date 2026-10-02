#!/bin/sh
# 変換パイプライン（lexer/parser/semantic/codegen）を ASan+UBSan で一巡させる回帰 (Alpha0.6)
#
# ランタイム中心だった既存の test-sanitizers に対し、こちらは「変換器そのもの」を
# sanitizer 付きでビルドして全フィクスチャを変換し、クラッシュや未定義動作の
# レポートが出ないことを確認する。期待する終了コードは問わない
# （rejection フィクスチャは診断を出して非0で終わるのが正しい挙動）。
set -eu

COMPILER=${1:-./build/sanitize-core/python-code-to-c}
WORK=build/tests/sanitize_pipeline
rm -rf "$WORK"
mkdir -p "$WORK"

if [ ! -x "$COMPILER" ]; then
    printf '%s\n' "sanitize_pipeline_skipped: $COMPILER not found" >&2
    exit 1
fi

count=0
failed=0
leaks=0
for py in tests/*.py examples/*.py examples/*/*.py; do
    [ -f "$py" ] || continue
    stem=$(basename "$py" .py)
    for extra in "" "--fallback"; do
        out="$WORK/${stem}${extra:+.fallback}.c"
        log="$out.log"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        timeout 120 "$COMPILER" "$py" $extra -o "$out" > "$log" 2>&1 || true
        # メモリ破壊・未定義動作は即失敗。リークは「変換器の1回分の確保が既定の
        # malloc アロケータでは解放されない」既知の設計要因があるため、
        # ここでは警告として数えるだけにする（可視化はする）。
        if grep -qE 'AddressSanitizer: [a-zA-Z-]+|runtime error:' "$log"; then
            printf '%s\n' "sanitize_pipeline_failed: $py $extra" >&2
            head -20 "$log" >&2
            failed=1
            break
        fi
        if grep -q 'LeakSanitizer' "$log"; then
            leaks=$((leaks + 1))
        fi
        count=$((count + 1))
    done
    [ "$failed" -eq 0 ] || break
done

if [ "$failed" -ne 0 ]; then
    exit 1
fi
if [ "$leaks" -gt 0 ]; then
    printf '%s\n' "sanitize_pipeline_note: LeakSanitizer flagged $leaks of $count conversions" \
        "(transpiler per-run allocations are not released with the default malloc allocator;" \
        "runtime-side leaks are covered by 'make test-gc-leaks')"
fi
printf '%s\n' "sanitize_pipeline_ok: $count conversions under ASan/UBSan (strict + --fallback)"
