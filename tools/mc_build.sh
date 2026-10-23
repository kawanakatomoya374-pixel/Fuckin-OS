#!/bin/sh
# mc_build.sh SRCROOT VERSION OUT.c-os - builds one version of mc-classic-c for C-OS (see docs/MC_CLASSIC.md).
# SRCROOT/VERSION/src is the game's own tree (its Makefile names the sources), SRCROOT/VERSION/resources its textures.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRCROOT=$1; VER=$2; OUT=$3
case $SRCROOT in /*) ;; *) SRCROOT=$PWD/$SRCROOT ;; esac
case $OUT in /*) ;; *) OUT=$PWD/$OUT ;; esac
GAME=$SRCROOT/$VER; M=$ROOT/userland/programs/mcclassic
WORK=${MC_WORK:-/tmp/mc_build}/$VER$MC_TAG; mkdir -p "$WORK"
# the source list of the game's Makefile (without comments and the optional net/server parts that need sockets)
SRCS=$(cd "$GAME/src" && sed -e 's/#.*//' Makefile | awk '/^SRC *=/{f=1} f{print} f&&!/\\$/{exit}' | sed -e 's/^SRC *=//' -e 's/\\//g' | tr -s ' \t\n' ' ')
python3 "$ROOT/tools/mc_pack.py" "$GAME/resources" "/home/mc/$VER$MC_TAG" "$WORK/pack.c"
cd "$GAME/src"
exec "$ROOT/tools/cos-cc" -O2 -w -std=gnu99 -Dmain=mc_game_main $MC_DEFS -Dstderr=stdout -Dfopen=mc_fopen -DSTBI_NO_STDIO -DSTBI_NO_THREAD_LOCALS -include "$M/mcstbi.h" -Dmalloc=cm_malloc -Dfree=cm_free -Dcalloc=cm_calloc -Drealloc=cm_realloc -I"$ROOT/userland/programs/craft" -DM_PI=3.14159265358979323846 -I. -I"$M/compat" -I"$M" \
    -o "$OUT" $SRCS "$M/mcgl.c" "$M/glfw_cos.c" "$M/mcrt.c" "$M/audio_cos.c" "$ROOT/userland/programs/craft/cmem.c" "$WORK/pack.c" ${MC_EXTRA}
