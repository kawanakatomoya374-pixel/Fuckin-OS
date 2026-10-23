#!/bin/sh
# mc_hostbench.sh VERSION [FRAMES] - builds mc-classic-c VERSION for the Linux host with a headless GLFW (userland/programs/mcclassic/host) and
# runs it for FRAMES frames, printing the time per frame; MC_PPM=out.ppm keeps the last picture, MC_PROF=1 builds with -pg (gprof). For profiling
# mcgl.c with the real game; the C-OS build is tools/mc_build.sh.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=$1; export MC_FRAMES=${2:-120}
M=$ROOT/userland/programs/mcclassic; GAME=$ROOT/userland/third_party/mc-classic-c/$VER
W=${MC_WORK:-/tmp/mc_host}; mkdir -p "$W/inc"; ln -sfn "$M/compat/GL" "$W/inc/GL"; ln -sfn "$M/compat/GLFW" "$W/inc/GLFW"
SRCS=$(cd "$GAME/src" && sed -e 's/#.*//' Makefile | awk '/^SRC *=/{f=1} f{print} f&&!/\\$/{exit}' | sed -e 's/^SRC *=//' -e 's/\\//g' | tr -s ' \t\n' ' ')
cd "$GAME/src"
gcc ${MC_OPT:--O2} ${MC_PROF:+-pg -fno-inline-functions} -w -std=gnu99 -DM_PI=3.14159265358979323846 $MC_DEFS -I"$W/inc" -I. -o "$W/$VER" $SRCS "$M/mcgl.c" "$M/host/glfw_host.c" -lm -lz -lpthread
cd "$GAME"
"$W/$VER" 2>&1 | tail -${MC_TAIL:-8}
