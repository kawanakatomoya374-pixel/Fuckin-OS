#!/bin/sh
# fuzz.sh [iterations] [seed] - builds the loader with AddressSanitizer + UBSan next to the simulated MMU and runs fuzz_loader over the fixtures
# (run.sh builds the fixtures first). Findings: a sanitizer report, a crash, or "did not return"; the input is /tmp/cos_fuzz_cur.bin.
set -e
cd "$(dirname "$0")"
[ -f build/pie_exec.elf ] || sh run.sh >/dev/null
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
mkdir -p build/fuzz
for f in cos_elf cos_elf_link; do gcc -c -o build/fuzz/$f.o build/$f.c -I build -DCOS_ELF_HOSTTEST -std=gnu11 -w $SAN; done
gcc -c -o build/fuzz/sim_mmu.o sim_mmu.c -I build -std=gnu11 -w $SAN
gcc -o build/fuzz/fuzz_loader fuzz_loader.c build/fuzz/sim_mmu.o build/fuzz/cos_elf.o build/fuzz/cos_elf_link.o -I build -std=gnu11 -w $SAN
if [ "$1" = "--replay" ]; then ASAN_OPTIONS=detect_leaks=0 ./build/fuzz/fuzz_loader --replay "$2"; else ASAN_OPTIONS=detect_leaks=0 ./build/fuzz/fuzz_loader "${1:-20000}" "${2:-1}"; fi
