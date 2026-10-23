#!/bin/sh
# Builds the host test programs into $1 (default: tools/imgtest/build).
# SAN=1 (default) builds with AddressSanitizer + UBSan, SAN=0 builds plain -O2.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HERE/build}
SAN=${SAN:-1}
mkdir -p "$OUT"
APPS=$ROOT/src/apps
SRCS="$APPS/png_decoder.c $APPS/jpeg_codec.c $APPS/bmp_codec.c $APPS/image_scale.c $HERE/shim/shim.c"
CF="-std=gnu11 -g -Wall -Wextra -I$HERE/shim -I$APPS"
if [ "$SAN" = 1 ]; then
  SF="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
else
  SF="-O2"
fi
gcc $CF $SF "$HERE/imgdec.c" $SRCS -o "$OUT/imgdec"
gcc $CF $SF "$HERE/fuzz.c" $SRCS -o "$OUT/fuzz"
# speed benchmark is always built optimised, without sanitizers
gcc $CF -O2 "$HERE/bench.c" $SRCS -o "$OUT/bench"
