#!/usr/bin/env bash
# The Py2C command line on a running guest, the source tree in /apps/Py2C and the full tcc build of Py2C through C-OS Studio (tools/p2ccli_peer.py).
exec env PEER=tools/p2ccli_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
