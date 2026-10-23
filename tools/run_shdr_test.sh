#!/usr/bin/env bash
# Single-header libraries built with the on-device TinyCC: SDK examples and Py2C's single header (tools/shdr_peer.py).
exec env PEER=tools/shdr_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
