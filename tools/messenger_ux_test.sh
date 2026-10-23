#!/usr/bin/env bash
# Host test of the Message app's date / quote helpers (pure functions copied from userland/programs/messenger/main.c).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
gcc -Wall -Wextra -o /tmp/messenger_ux_test tools/messenger_ux_test/test.c && /tmp/messenger_ux_test
