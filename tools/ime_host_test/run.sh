#!/usr/bin/env bash
# Builds and runs the host IME test:  tools/ime_host_test/run.sh "kyouhaiitenkidesune " "watashinonamaehananakadesu "
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gcc -O2 -w -I"$ROOT/tools/ime_host_test" -I"$ROOT/userland/include" -o /tmp/ime_test "$ROOT/tools/ime_host_test/test.c" "$ROOT/userland/lib/cosime.c" "$ROOT/userland/lib/stbi_all.c"
/tmp/ime_test "$ROOT/userland/third_party/mozc_dict/cosime.dic" "$@"
