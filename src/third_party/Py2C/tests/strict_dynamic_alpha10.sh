#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
# 追加の厳格プロファイルで CPython 差分コーパスと意味論プローブを走らせる。
# 既定ビルドには影響させず、開発機/CI でのバグ狩りにだけ使う。
#
#   使い方: sh tests/strict_dynamic_alpha10.sh <profile>
#
#   asan-strict  ASan + スタック使用後返却 + 文字列API厳格検査 + 不正ポインタ比較
#   lsan         生成プログラムの解放漏れ（LeakSanitizer 有効）
#   ubsan-deep   UBSan 最大構成（bounds-strict/object-size/builtin/float系）
#   msan         未初期化メモリの読み出し（clang MemorySanitizer）
#   clang-int    clang 整数系（integer/implicit-conversion/unsigned-overflow/local-bounds）
#   harden       最適化 + _FORTIFY_SOURCE=3 + スタック保護で生成プログラムをビルド
set -u

PROFILE=${1:-asan-strict}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-cc}
P2C_COMPILER=${P2C_COMPILER:-./$BD/python-code-to-c}
[ -x "$P2C_COMPILER" ] || {
    printf '%s\n' "strict_dynamic: compiler not built ($P2C_COMPILER)"
    exit 1
}

BASE_CFLAGS="-std=gnu11 -Wall -Wextra -Werror -fno-omit-frame-pointer -g -O1"
# -Wclobbered は GCC が「setjmp と longjmp の間で変更された変数」すべてに発火する
# 保守的警告。生成コードは for ループごとにイテレータ用 setjmp を置くため、ループ内で
# 代入されるユーザー変数（例: value）が軒並み該当する。実際に longjmp をまたいで
# 読まれる値（イテレータの item など）は codegen が volatile を付けており、
# また longjmp 後に読まれない変数は不定値でも意味に影響しない。よって生成コード
# 側では抑制する（詳細は docs/testing/STRICT_BUILD_AND_ANALYSIS_ALPHA1.0.md）。
# clang は未知の警告オプションを -Werror 下でエラーにするため、あわせて
# -Wno-unknown-warning-option を明示する（GCC は元から黙って無視する）。
BASE_CFLAGS="$BASE_CFLAGS -Wno-unknown-warning-option -Wno-clobbered"
TEST_CFLAGS=
TEST_LDFLAGS=
TEST_ENV=

case "$PROFILE" in
    asan-strict)
        # 既定は軽量構成（use-after-scope + 厳格文字列検査 + 不正ポインタ比較）。
        # ASAN_UAR=1 を付けると detect_stack_use_after_return=1 が有効になり、
        # スタック使用後返却まで検出できるが、実行が 10 倍以上遅くなる
        # （偽スタックを保持するため）ので、絞った確認にだけ使う。
        # 注意: このランタイムの GC は「実際のスタックを保守的に走査する」方式
        # なので、ASan の偽スタック（detect_stack_use_after_return）が有効だと
        # フレームがヒープ上に移り、生存ローカルを発見できず生成プログラムが
        # 誤動作する。収集頻度の高い差分コーパスでは明示的に無効化する
        # （ASAN_UAR=1 は GC を走らせない回帰専用）。
        if [ "${ASAN_UAR:-0}" = "1" ]; then
            UAR_OPT="detect_stack_use_after_return=1:"
        else
            UAR_OPT="detect_stack_use_after_return=0:"
        fi
        if [ "${ASAN_PTR_PAIRS:-0}" = "1" ]; then
            PTR_PAIRS_OPT="detect_invalid_pointer_pairs=2:"
        else
            PTR_PAIRS_OPT=
        fi
        TEST_CFLAGS="$BASE_CFLAGS -fsanitize=address -fsanitize-address-use-after-scope"
        TEST_LDFLAGS="-fsanitize=address"
        TEST_ENV="ASAN_OPTIONS=${UAR_OPT}${PTR_PAIRS_OPT}strict_string_checks=1:halt_on_error=1:abort_on_error=1:detect_leaks=0"
        ;;
    lsan)
        TEST_CFLAGS="$BASE_CFLAGS -fsanitize=address -fsanitize-address-use-after-scope"
        TEST_LDFLAGS="-fsanitize=address"
        TEST_ENV="ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1"
        ;;
    ubsan-deep)
        # GCC が解釈できる最大構成（function/return は clang 専用のため含めない）。
        LIST="-fsanitize=undefined,bounds-strict,object-size,builtin,float-cast-overflow,float-divide-by-zero,nonnull-attribute,returns-nonnull-attribute,pointer-overflow,shift,bool,enum,unreachable,vla-bound,alignment,integer-divide-by-zero"
        TEST_CFLAGS="$BASE_CFLAGS $LIST -fno-sanitize-recover=all"
        TEST_LDFLAGS="$LIST -fno-sanitize-recover=all"
        TEST_ENV="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:abort_on_error=1"
        ;;
    msan)
        TEST_CFLAGS="-std=gnu11 -Wall -Wextra -Werror -fno-omit-frame-pointer -g -O1 -fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE"
        TEST_LDFLAGS="-fsanitize=memory -pie"
        TEST_ENV="MSAN_OPTIONS=halt_on_error=1:exit_code=86"
        ;;
    clang-int)
        # 整数系のみ（address を混ぜると ASan の分だけ遅くなるため分離している）。
        TEST_CFLAGS="$BASE_CFLAGS -fsanitize=integer,implicit-conversion,unsigned-integer-overflow,local-bounds -fno-sanitize-recover=all"
        TEST_LDFLAGS="-fsanitize=integer,implicit-conversion,unsigned-integer-overflow,local-bounds -fno-sanitize-recover=all"
        TEST_ENV="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:abort_on_error=1"
        ;;
    harden)
        TEST_CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fstrict-flex-arrays=3 -fcf-protection=full"
        TEST_LDFLAGS="-Wl,-z,relro,-z,now,-z,noexecstack"
        TEST_ENV=
        ;;
    opt3)
        # 高最適化での意味論一致（未初期化値や longjmp 跨ぎの値など、
        # 最適化でのみ表面化する不具合を差分で検出する）。
        TEST_CFLAGS="-std=gnu11 -Wall -Wextra -Werror -Wno-clobbered -O3 -fno-strict-aliasing"
        TEST_LDFLAGS=
        TEST_ENV=
        ;;
    *)
        printf '%s\n' "strict_dynamic: unknown profile '$PROFILE'" >&2
        exit 2
        ;;
esac

printf '%s\n' "strict_dynamic[$PROFILE] cc=$CC"
printf '  cflags: %s\n' "$TEST_CFLAGS"
[ -n "$TEST_ENV" ] && printf '  env:    %s\n' "$TEST_ENV"

# 走らせる対象: conf（CPython差分コーパス）/ probe（意味論プローブ）/ both。
HARNESSES=${2:-both}
case "$HARNESSES" in
    conf | probe | both) ;;
    *)
        printf '%s\n' "strict_dynamic: harness must be conf|probe|both (got '$HARNESSES')" >&2
        exit 2
        ;;
esac

P2C_TEST_CFLAGS=$TEST_CFLAGS
P2C_TEST_LDFLAGS=$TEST_LDFLAGS
P2C_TEST_ENV=$TEST_ENV
P2C_COMPILER=$P2C_COMPILER
export P2C_TEST_CFLAGS P2C_TEST_LDFLAGS P2C_TEST_ENV P2C_COMPILER CC

rc=0
if [ "$HARNESSES" != "probe" ]; then
    sh tests/conformance_regression.sh || rc=1
fi
if [ "$HARNESSES" != "conf" ]; then
    sh tests/semantic_probe_alpha10.sh || rc=1
fi

if [ "$rc" -eq 0 ]; then
    printf '%s\n' "strict_dynamic_ok: $PROFILE ($HARNESSES) passed"
else
    printf '%s\n' "strict_dynamic_failed: $PROFILE ($HARNESSES) (see report above)" >&2
fi
exit $rc
