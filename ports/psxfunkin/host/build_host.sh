#!/bin/sh
# Builds the port against the PSXFunkin sources in $UP (default /home/user/psxf/src_repo) as a headless PC program: build_host.sh [out]
# The only change to the upstream sources is in boot/mem.h (see tools/patch_upstream.sh); they are copied to a work directory first.
UP=${UP:-/home/user/psxf/src_repo}; HERE=$(cd "$(dirname "$0")/.." && pwd); OUT=${1:-/tmp/psxf_host}; W=${WORK:-/tmp/psxf_src}
sh "$HERE/tools/patch_upstream.sh" "$UP" "$W" || exit 1
SRCS="$W/src/boot/main.c $W/src/boot/mutil.c $W/src/boot/random.c $W/src/boot/archive.c $W/src/boot/font.c $W/src/boot/trans.c $W/src/boot/loadscr.c $W/src/boot/menu.c $W/src/boot/stage.c $W/src/boot/animation.c $W/src/boot/character.c $W/src/boot/object.c $W/src/boot/object/combo.c $W/src/boot/object/splash.c $W/src/menu/menu.c $W/src/week1/week1.c $W/src/week2/week2.c $W/src/week3/week3.c $W/src/week4/week4.c $W/src/week6/week6.c"
gcc -O2 -g -w -DPSXF_PC -DPSXF_STDMEM -I"$W/src/boot" -I"$W/src" -I"$HERE" -I/home/user/Claude-Workspace/userland/third_party \
  $SRCS "$HERE"/backend/*.c "$HERE"/host/plat_host.c -lm -o "$OUT"
