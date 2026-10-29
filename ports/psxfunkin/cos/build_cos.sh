#!/bin/sh
# Builds the C-OS program: build_cos.sh OUT.c-os   (UP = the PSXFunkin tree, default /home/user/psxf/src_repo, with its tools run; see README.md)
UP=${UP:-/home/user/psxf/src_repo}; HERE=$(cd "$(dirname "$0")/.." && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=$1; W=${WORK:-/tmp/psxf_src_cos}
sh "$HERE/tools/patch_upstream.sh" "$UP" "$W" || exit 1
SRCS="$W/src/boot/main.c $W/src/boot/mutil.c $W/src/boot/random.c $W/src/boot/archive.c $W/src/boot/font.c $W/src/boot/trans.c $W/src/boot/loadscr.c $W/src/boot/menu.c $W/src/boot/stage.c $W/src/boot/animation.c $W/src/boot/character.c $W/src/boot/object.c $W/src/boot/object/combo.c $W/src/boot/object/splash.c $W/src/menu/menu.c $W/src/week1/week1.c $W/src/week2/week2.c $W/src/week3/week3.c $W/src/week4/week4.c $W/src/week6/week6.c"
exec "$ROOT/tools/cos-cc" -O2 -w -std=gnu99 -DPSXF_PC -DPSXF_STDMEM -I"$W/src/boot" -I"$W/src" -I"$HERE" -I"$ROOT/userland/third_party" \
    -o "$OUT" $SRCS "$HERE"/backend/*.c "$HERE"/cos/plat_cos.c
