#!/bin/sh
# run.sh - host tests for src/netsurf/cos_ns_font.c (CSS-aware text
# metrics and glyph synthesis).
#
# Compiles the REAL cos_ns_font.c against a stub glyph driver whose fake
# font has ink on every row and edge column, so a resampling bug shows up
# as wrong ink rather than as silently nothing. What these protect is
# GEOMETRY, not glyph beauty: that font-size becomes real pixels, that
# measurement (used by layout) and drawing (the text plotter) agree on
# every character's advance, and that no input overruns the mask buffer.
#
# Usage: sh validation/nsfont/run.sh
set -e
cd "$(dirname "$0")"
mkdir -p build
cp ../../src/netsurf/cos_ns_font.c ../../src/netsurf/cos_ns_font.h build/
cp -r stub/. build/

echo "[1/2] compiling cos_ns_font.c against a stub glyph driver"
gcc -o build/test_nsfont test_nsfont.c build/cos_ns_font.c fake_font.c \
    -I build -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1

echo "[2/2] running geometry tests"
./build/test_nsfont
