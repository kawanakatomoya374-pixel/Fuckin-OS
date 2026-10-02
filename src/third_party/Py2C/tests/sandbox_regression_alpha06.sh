#!/bin/sh
# サンドボックス（実行予算）の回帰 (Alpha0.6)
#
#   1) ステップ予算: ループ後退エッジで予算を超えると SandboxError で止まる（ハングしない）
#   2) 確保予算: オブジェクト確保が上限を超えると SandboxError で止まる
#   3) 無制限（既定）では何も起きない
#   4) 生成Cの while ループに計装（p2c_sandbox_tick）が入っていること
set -eu

CC=${CC:-cc}
mkdir -p build/tests
WORK=build/tests/sandbox
mkdir -p "$WORK"

cat > "$WORK/probe.c" <<'EOF'
/* サンドボックス予算の実挙動を確かめる小さなドライバ。 */
#include "runtime/python_code_to_c_runtime.h"
#include <stdio.h>

static int caught_ticks = 0;
static int caught_allocs = 0;

int main(void) {
    P2C_SandboxLimits limits;
    P2C_ExceptFrame frame;

    /* ── 1) ステップ予算 ── */
    limits.max_ticks = 1000;
    limits.max_allocs = 0;
    p2c_sandbox_reset();
    p2c_sandbox_set(&limits);
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        for (int i = 0; i < 100000; i++) p2c_sandbox_tick();
        printf("sandbox_failed: step budget did not fire\n");
        return 1;
    }
    p2c_exc_stack = frame.prev;
    if (p2c_exc_name_match(frame.exc, "SandboxError")) caught_ticks = 1;
    if (!caught_ticks) { printf("sandbox_failed: wrong exception for step budget\n"); return 2; }
    if (p2c_sandbox_ticks() < 1000) { printf("sandbox_failed: tick counter not advanced\n"); return 3; }

    /* ── 2) 確保予算 ── */
    limits.max_ticks = 0;
    limits.max_allocs = 50;
    p2c_sandbox_reset();
    p2c_sandbox_set(&limits);
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    if (setjmp(frame.env) == 0) {
        for (int i = 0; i < 10000; i++) (void)p2c_obj_from_int(i);
        printf("sandbox_failed: allocation budget did not fire\n");
        return 4;
    }
    p2c_exc_stack = frame.prev;
    if (p2c_exc_name_match(frame.exc, "SandboxError")) caught_allocs = 1;
    if (!caught_allocs) { printf("sandbox_failed: wrong exception for allocation budget\n"); return 5; }

    /* ── 3) 無制限に戻す ── */
    p2c_sandbox_set(NULL);
    for (int i = 0; i < 100000; i++) p2c_sandbox_tick();
    /* 予算違反の回数は参考値（リセットのタイミングに依存するため合否には使わない）。 */

    printf("sandbox_ok: ticks=%llu allocs=%llu violations=%llu\n",
           (unsigned long long)p2c_sandbox_ticks(),
           (unsigned long long)p2c_sandbox_allocs(),
           (unsigned long long)p2c_sandbox_violations());
    return 0;
}
EOF

$CC -I./include -std=c11 -O2 -Wall -Wextra -Werror \
    "$WORK/probe.c" src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c \
    src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c \
    -DPYTHON_CODE_TO_C_NO_PYGAME -lm -o "$WORK/probe"
"$WORK/probe"

# ── 4) 生成Cの while ループ計装 ──
printf '%s\n' 'n = 0' 'while n < 3:' '    n = n + 1' 'print(n)' > "$WORK/loop.py"
./build/python-code-to-c "$WORK/loop.py" -o "$WORK/loop.c"
if ! grep -q 'p2c_sandbox_tick();' "$WORK/loop.c"; then
    printf '%s\n' 'sandbox_failed: generated while-loop is not instrumented' >&2
    exit 7
fi
printf '%s\n' 'sandbox_regression_ok'
