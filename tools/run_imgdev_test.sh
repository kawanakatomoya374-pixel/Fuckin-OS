#!/usr/bin/env bash
# The kernel's JPEG/PNG/BMP decoders on a running guest: big, progressive, EXIF-rotated, RLE, damaged and hostile files (tools/imgdev).
exec env PEER=tools/imgdev/peer.py bash "$(dirname "$0")/run_terminal_test.sh"
