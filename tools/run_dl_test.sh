#!/usr/bin/env bash
# Loader checks that need a running system (process teardown, shared pages, dlopen/dlclose, the dl API): the Terminal runs tools/dltest/dl_main.c-os.
exec env PEER=tools/dl_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
