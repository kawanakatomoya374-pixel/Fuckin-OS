#!/usr/bin/env bash
# Py2C Studio on the guest: edit, convert, build with tcc against the Py2C runtime, run, input, find, save, examples (see tools/py2c_peer.py).
exec env PEER=tools/py2c_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
