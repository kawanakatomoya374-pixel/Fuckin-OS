#!/bin/sh
# Builds the data setup program: build_cos.sh OUT.c-os
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); OUT=$1
[ -f "$ROOT/build/libbearssl_user.a" ] || make -C "$ROOT" build/libbearssl_user.a >/dev/null || exit 1
exec "$ROOT/tools/cos-cc" -O2 -w -std=gnu99 -I"$ROOT/src/third_party/bearssl/inc" -I"$HERE" -o "$OUT" "$HERE/main.c" "$ROOT/build/libbearssl_user.a"
