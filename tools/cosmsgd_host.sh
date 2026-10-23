#!/usr/bin/env bash
# Builds userland/programs/cosmsgd for the host (build/cosmsgd_host) so the server can be tested without a virtual machine.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
B=src/third_party/bearssl; OUT=build/cosmsgd_host_obj; mkdir -p "$OUT"
if [ ! -f build/libbearssl_host.a ]; then
  for f in $(find $B/src -name '*.c' ! -name sysrng.c | sort); do o="$OUT/$(echo "$f" | tr / _).o"; [ -f "$o" ] || gcc -O2 -w -I$B/inc -I$B/src -c "$f" -o "$o" & while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do sleep 0.05; done; done; wait
  ar rcs build/libbearssl_host.a "$OUT"/*.o
fi
echo "(host build of cosmsgd)" >&2
gcc -O2 -Wall -Wextra -Wno-unused-parameter -DCOSMSGD_HOST -I$B/inc -o build/cosmsgd_host userland/programs/cosmsgd/main.c userland/programs/cosmsgd/core.c userland/programs/cosmsgd/cert.c build/libbearssl_host.a
