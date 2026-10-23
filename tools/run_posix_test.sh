#!/usr/bin/env bash
# POSIX layer of libcos on the guest: the Terminal runs tools/posixtest/posix_test.c-os.
exec env TERMINAL_TEST_NETWORK= PEER=tools/posix_peer.py bash "$(dirname "$0")/run_terminal_test.sh"
