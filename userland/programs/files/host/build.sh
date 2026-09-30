#!/bin/sh
# Build Files as a host Linux program (testing only). Usage: sh host/build.sh [out]
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/files_host}
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -Wno-format-truncation -Wno-stringop-truncation \
    -I../../include -I. \
    main.c core.c ui.c input.c stbi_impl.c host/host_extra.c ../studio/host/host_shim.c \
    ../../lib/cos_ui.c ../../lib/cos_ui_fonts.c -lm -o "$OUT"
echo "built $OUT"
