#!/usr/bin/env bash
# Py2C is cleaned and rebuilt inside the guest with C-OS's own tcc (started from Py2C Studio's Help menu) and the new build is used (tools/selfhost_py2c_peer.py).
exec env PEER=tools/selfhost_py2c_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
