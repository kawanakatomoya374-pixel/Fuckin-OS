#!/bin/sh
# run.sh - build the ELF loader fixtures and run the conformance tests.
#
# This compiles src/kernel/cos_elf.c UNMODIFIED against a simulated MMU
# (sim_mmu.c) and runs it over binaries produced by the host's ordinary
# gcc/ld. That is the point: the thing under test is the shipping loader
# source, and the inputs are real toolchain output rather than
# hand-written fixtures that only exercise what the loader already does.
#
# Usage:   sh validation/elfloader/run.sh
#          COS_ELF_VERBOSE=1 sh validation/elfloader/run.sh   # loader logs
set -e

cd "$(dirname "$0")"
mkdir -p build
rm -f build/*.o build/test_loader

# Stage the real loader where its `#include "mm/paging.h"` resolves to
# the stub rather than the kernel's own header (a quoted include searches
# the including file's directory first, so the sources have to sit beside
# the stubs - they are copied, never edited).
cp ../../src/kernel/cos_elf.c ../../src/kernel/cos_elf.h \
   ../../src/kernel/cos_elf_link.c ../../src/kernel/cos_elf_link.h build/
cp -r stub/. build/

CF="-ffreestanding -fno-stack-protector -nostdlib -O1 -g0"
WARN="-std=gnu11 -Wall -Wextra -Wno-unused-parameter"

echo "[1/3] building ELF fixtures with $(gcc -dumpversion)"
# Classic statically linked non-PIE: the only shape the old loader took.
gcc $CF -no-pie -fno-pie -Wl,-e,_start \
    -o build/static_exec.elf fixtures/static_exec.c
# Position-independent executable, relocations as RELA.
gcc $CF -pie -fpie -Wl,-e,_start -Wl,--no-dynamic-linker \
    -o build/pie_exec.elf fixtures/pie_exec.c
# The same program with DT_RELR instead - note this has RELASZ == 0, so a
# loader that only reads DT_RELA applies zero relocations and "succeeds".
gcc $CF -pie -fpie -Wl,-e,_start -Wl,--no-dynamic-linker \
    -Wl,-z,pack-relative-relocs -o build/pie_relr.elf fixtures/pie_exec.c
# PT_TLS + exported ifunc + __tls_get_addr import.
gcc $CF -shared -fPIC -o build/tls_lib.so fixtures/tls_lib.c
# A dependency pair with a real cross-object JUMP_SLOT and a weak import.
gcc $CF -shared -fPIC -Wl,-soname,base_lib.c-osll \
    -o build/base_lib.so fixtures/base_lib.c
gcc $CF -shared -fPIC -Wl,-soname,dep_lib.c-osll \
    -o build/dep_lib.so fixtures/dep_lib.c -L build -l:base_lib.so
# Two definitions of one name at different versions.
gcc $CF -shared -fPIC -Wl,--version-script=fixtures/ver_lib.map \
    -Wl,-soname,ver_lib.c-osll -o build/ver_lib.so fixtures/ver_lib.c
# A standalone R_X86_64_IRELATIVE.
gcc $CF -pie -fpie -Wl,-e,_start -Wl,--no-dynamic-linker \
    -o build/ifunc_exec.elf fixtures/ifunc_exec.c
# The end-to-end case: a dynamically linked PIE with two DT_NEEDED
# libraries, its own PT_TLS, a constructor, and an R_X86_64_COPY of a
# data object defined in one of the libraries. This is the shape an
# ordinary externally-built program has, and the shape the old loader
# refused outright.
gcc $CF -pie -fpie -Wl,-e,_start -Wl,--no-dynamic-linker \
    -o build/dyn_exec.elf fixtures/dyn_exec.c \
    -L build -l:dep_lib.so -l:base_lib.so

# The documented build path, exercised as a fixture: userland runtime,
# then a program built through tools/cos-cc exactly as a user would.
echo "      building the C-OS userland runtime and a cos-cc program"
COS_ROOT="$(cd ../.. && pwd)"
mkdir -p "$COS_ROOT/build/userland"
UCF="-ffreestanding -nostdlib -fno-stack-protector -fno-builtin \
     -fno-asynchronous-unwind-tables -fPIC -I $COS_ROOT/userland/include"
for u in cos_lib cos_rtld cos_fmt cos_math cos_sync; do
    gcc $UCF -c "$COS_ROOT/userland/lib/$u.c" -o "$COS_ROOT/build/userland/$u.o"
done
gcc -c "$COS_ROOT/userland/lib/cos_crt0.S" -o "$COS_ROOT/build/userland/cos_crt0.o"
ar rcs "$COS_ROOT/build/userland/libcos.a" \
       "$COS_ROOT/build/userland/cos_lib.o"  "$COS_ROOT/build/userland/cos_rtld.o" \
       "$COS_ROOT/build/userland/cos_fmt.o"  "$COS_ROOT/build/userland/cos_math.o" \
       "$COS_ROOT/build/userland/cos_sync.o"
COS_ROOT="$COS_ROOT" "$COS_ROOT/tools/cos-cc" \
    -o build/cosapp.c-os fixtures/cosapp.c >/dev/null

# The on-device compiler's output: TinyCC (src/third_party/tinyc) built for the
# host, then used exactly as Studio will use it on the device - static ET_EXEC
# placed in the program region (PML4[1]), linked against libcos.a.
echo "      building TinyCC from src/third_party/tinyc and a tcc-built program"
TCCB=build/tcc
rm -rf "$TCCB"; mkdir -p "$TCCB"
cp -r "$COS_ROOT/src/third_party/tinyc/." "$TCCB/"
( cd "$TCCB" && ./configure --cc=gcc >/dev/null 2>&1 && make tcc libtcc1.a >/dev/null 2>&1 ) || echo "      (TinyCC host build failed - tcc fixture skipped)"
if [ -x "$TCCB/tcc" ]; then
    # a shared library from the same compiler: what `tcc -shared -o x.c-osll x.c` produces on the device
    "$TCCB/tcc" -B"$TCCB" -shared -nostdlib -o build/tcc_lib.so fixtures/tcclib.c || echo "      (tcc -shared failed)"
    "$TCCB/tcc" -B"$TCCB" -I"$TCCB/include" -nostdinc -I"$COS_ROOT/userland/include" \
        -static -nostdlib -Wl,-Ttext=0x8000001000 -o build/tccapp.c-os fixtures/tccapp.c \
        "$COS_ROOT/build/userland/cos_crt0.o" "$COS_ROOT/build/userland/cos_rtld.o" \
        "$COS_ROOT/build/userland/libcos.a" "$TCCB/libtcc1.a"
fi

echo "[2/3] compiling the loader against the simulated MMU"
gcc -c -o build/cos_elf.o      build/cos_elf.c      -I build -DCOS_ELF_HOSTTEST $WARN -O1
gcc -c -o build/cos_elf_link.o build/cos_elf_link.c -I build -DCOS_ELF_HOSTTEST $WARN -O1
gcc -c -o build/sim_mmu.o      sim_mmu.c            -I build $WARN -O1
gcc -o build/test_loader test_loader.c \
    build/sim_mmu.o build/cos_elf.o build/cos_elf_link.o -I build $WARN -O1

echo "[3/3] running conformance tests"
./build/test_loader
