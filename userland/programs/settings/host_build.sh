#!/bin/sh
set -e
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -o /tmp/settings_host \
    -I../../include main.c ../studio/host/host_shim.c ../../lib/cos_ui.c ../../lib/cos_ui_fonts.c -lm
echo "built /tmp/settings_host"
