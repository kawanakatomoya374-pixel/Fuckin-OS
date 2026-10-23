#!/usr/bin/env bash
# Host test of the end-to-end encryption code (userland/programs/messenger/e2e.inc): C self-test + interoperability with Python's `cryptography`.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
tools/cosmsgd_host.sh >/dev/null 2>&1 || true            # makes build/libbearssl_host.a
B=src/third_party/bearssl
gcc -O2 -Wall -Wextra -Wno-unused-function -I$B/inc -o build/messenger_e2e_test tools/messenger_e2e_test/test.c build/libbearssl_host.a || exit 1
build/messenger_e2e_test
build/messenger_e2e_test emit > build/messenger_e2e_emit.txt
python3 tools/messenger_e2e_test.py build/messenger_e2e_emit.txt
