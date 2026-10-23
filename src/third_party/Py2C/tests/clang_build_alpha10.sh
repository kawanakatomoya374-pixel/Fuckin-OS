#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
# clang を「第二のコンパイラ」として使う厳格ゲート。
# GCC の WARN_CFLAGS から clang 非対応フラグを除いた同水準の基準で変換器本体を
# ビルドし、その変換器で CPython 差分コーパスと意味論プローブを走らせる。
# （-Wconversion/-Wcast-qual 等の解釈が GCC と異なるため、警告の抜けを相互補完する）
#
#   使い方: sh tests/clang_build_alpha10.sh          # ビルド + コーパス
#           BUILD_ONLY=1 sh tests/clang_build_alpha10.sh
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

CLANG=${CLANG:-clang}
BUILD=${CLANG_BUILD:-$BD/clangfull}
OBJ=${CLANG_OBJ:-obj/clangfull}
BUILD_ONLY=${BUILD_ONLY:-0}

CLANG_STRICT="-Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral \
-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wundef \
-Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla \
-Wpointer-arith -Wcast-align -Wnested-externs -Wno-unknown-warning-option"

command -v "$CLANG" > /dev/null || { printf '%s\n' "clang not found: $CLANG"; exit 1; }

make BUILD="$BUILD" OBJ="$OBJ" CC="$CLANG" \
    CFLAGS="-std=c11 -O2 -ftrivial-auto-var-init=zero -fstack-protector-strong -D_FORTIFY_SOURCE=3 $CLANG_STRICT" \
    LDFLAGS="-Wl,-z,relro,-z,now,-z,noexecstack" all || exit 1
printf '%s\n' "clang_build_ok: $BUILD/python-code-to-c"

[ "$BUILD_ONLY" = "1" ] && exit 0

P2C_COMPILER=./$BUILD/python-code-to-c sh tests/conformance_regression.sh || exit 1
P2C_COMPILER=./$BUILD/python-code-to-c sh tests/semantic_probe_alpha10.sh || exit 1
printf '%s\n' 'clang_build_corpus_ok: clang-built converter matches CPython on corpus + probes'
