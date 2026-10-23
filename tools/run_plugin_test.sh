#!/usr/bin/env bash
# Plugin experiment: C-OS Message and the File Manager gain and lose features while they run (see tools/plugin_peer.py, docs/plugins.md).
exec env PEER=tools/plugin_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
