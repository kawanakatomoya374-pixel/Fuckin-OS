#!/bin/sh
# Build Studio as a host Linux program (testing only). Usage: sh host/build.sh [out]
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/studio_host}
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -Wno-format-truncation \
    -I../../include -I. \
    main.c demo.c ui.c dialogs.c edview.c term.c make.c build.c project.c search.c complete.c index.c settings.c editor.c theme.c buffer.c highlight.c studio_fonts.c \
    host/host_shim.c ../../lib/cos_ui.c ../../lib/cos_ui_fonts.c -lm -o "$OUT"
echo "built $OUT"
