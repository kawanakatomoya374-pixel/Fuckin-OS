#!/bin/bash
# build_userland.sh - compiles C source into a .c-os executable.
#
# Usage:  tools/build_userland.sh <source.c> [more.c ...] -o <output.c-os>
#
# Before this existed, every .c-os program had to be hand-written x86-64
# assembly - there was no C runtime, so nothing set up main(argc, argv) or
# provided malloc/printf/strings. This links the caller's C code against
# userland/lib (crt0 + libcos) using the same linker script the assembly
# programs already used.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ULIB="$ROOT/userland/lib"
UINC="$ROOT/userland/include"
LDSCRIPT="$ROOT/validation/cos_programs/cos.ld"
OBJDIR="$ROOT/build/userland"

SOURCES=()
OUTPUT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTPUT="$2"; shift 2 ;;
        *)  SOURCES+=("$1"); shift ;;
    esac
done

if [[ ${#SOURCES[@]} -eq 0 || -z "$OUTPUT" ]]; then
    echo "usage: $0 <source.c> [more.c ...] -o <output.c-os>" >&2
    exit 1
fi

mkdir -p "$OBJDIR"

# -fno-pic/-fno-pie/-mcmodel=large: the program is linked at a fixed high
#   address (0x8000000000, see cos.ld). The default small code model
#   assumes everything fits in the low 2GB and produces 32-bit relocations
#   that cannot reach that address - the link fails with "relocation
#   truncated to fit", which is exactly the error the hand-written
#   assembly programs kept hitting when they referenced data directly.
# -mno-red-zone: the kernel's interrupt/signal delivery can write below
#   RSP, which would corrupt a red zone the compiler assumed was safe.
# -fno-builtin: without it GCC can turn a loop into a memset/memcpy call
#   or constant-fold a string function in a way that assumes host libc
#   semantics.
CFLAGS=(
    -std=c11 -ffreestanding -nostdlib -fno-builtin
    -fno-pic -fno-pie -mno-red-zone -mcmodel=large
    -fno-stack-protector -fno-asynchronous-unwind-tables
    -O2 -Wall -Wextra -Wno-unused-parameter
    -I"$UINC"
)

OBJS=()

# crt0
nasm -f elf64 "$ULIB/cos_crt0.S" -o "$OBJDIR/cos_crt0.o"
OBJS+=("$OBJDIR/cos_crt0.o")

# runtime library
gcc "${CFLAGS[@]}" -c "$ULIB/cos_lib.c" -o "$OBJDIR/cos_lib.o"
OBJS+=("$OBJDIR/cos_lib.o")

# caller sources
for src in "${SOURCES[@]}"; do
    base="$(basename "$src" .c)"
    gcc "${CFLAGS[@]}" -c "$src" -o "$OBJDIR/$base.o"
    OBJS+=("$OBJDIR/$base.o")
done

ld -n -T "$LDSCRIPT" -o "$OUTPUT" "${OBJS[@]}"
echo "[USERLAND] built $OUTPUT"
