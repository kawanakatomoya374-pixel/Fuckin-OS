#!/bin/sh
# build_studio.sh - build everything C-OS Studio needs, as real C-OS ELF programs:
#
#   build/studio/studio.c-os          the IDE (installed as "C-OS Studio.c-os")                   (size budget: 10 MB)
#   build/studio/tcc.c-os             TinyCC 0.9.28rc, on-device compiler
#   build/studio/sdk/include/...      headers a program compiled on the device sees
#   build/studio/sdk/lib/...          cos_crt0.o, libcos.a, libtcc1.a
#
# The host TinyCC built here is used ONLY to produce libtcc1.a (TinyCC's own runtime
# support: 128-bit division, va_list helpers...). It is target-independent ELF x86-64
# code with no libc dependency; the compiler that ships is tcc.c-os.
set -e
COS_ROOT="${COS_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$COS_ROOT"
OUT=build/studio
TINYC=src/third_party/tinyc
STUDIO=userland/programs/studio
TCCDIR=userland/programs/tcc
MAX_STUDIO_BYTES=$((10 * 1024 * 1024))          # design doc: was 2 MB, raised to 10 MB
mkdir -p "$OUT/sdk/include" "$OUT/sdk/lib" "$OUT/host_tcc"

# 0. the C-OS runtime (crt0 + libcos.a) that both tcc.c-os and the SDK use
[ -f build/userland/libcos.a ] || make -s userland >/dev/null

# 1. host TinyCC -> libtcc1.a (and tccdefs_.h generation tool)
if [ ! -f "$OUT/host_tcc/libtcc1.a" ]; then
    rm -rf "$OUT/host_tcc"; mkdir -p "$OUT/host_tcc"
    cp -r "$TINYC"/. "$OUT/host_tcc/"
    ( cd "$OUT/host_tcc" && ./configure --cc=gcc >/dev/null 2>&1 && make tcc libtcc1.a >/dev/null 2>&1 )
fi
cp "$OUT/host_tcc/libtcc1.a" "$OUT/sdk/lib/libtcc1.a"

# 1b. runmain.o - the tiny entry stub `tcc -run` needs (see userland/programs/tcc/runmain.c
# for why this is a from-scratch replacement rather than upstream's lib/runmain.c). Plain
# x86-64 SysV code with no OS-specific symbols beyond main(), so the host compiler that
# already built libtcc1.a above produces an identical, ABI-correct object.
gcc -c -O2 -fno-stack-protector -o "$OUT/sdk/lib/runmain.o" userland/programs/tcc/runmain.c

# 2. tccdefs_.h (TinyCC embeds its predefined-macro header as a C string)
mkdir -p "$OUT/gen"
gcc -DC2STR -o "$OUT/gen/c2str" "$TINYC/conftest.c"
"$OUT/gen/c2str" "$TINYC/include/tccdefs.h" "$OUT/gen/tccdefs_.h"

# 3. tcc.c-os
CC="${CC:-gcc}" tools/cos-cc -O2 -w \
    -I "$TCCDIR" -I "$TCCDIR/compat" -I "$OUT/gen" -I "$TINYC" \
    -o "$OUT/tcc.c-os" "$TCCDIR/tcc_cos.c" "$TCCDIR/tcc_shim.c"

# 4. SDK: the headers a program compiled ON the device can include
cp userland/include/cos.h userland/include/cos_ui.h "$OUT/sdk/include/"
cp -r userland/include/libc/. "$OUT/sdk/include/"
cp "$TINYC"/include/stdarg.h "$TINYC"/include/stddef.h "$TINYC"/include/stdbool.h "$TINYC"/include/float.h \
   "$TINYC"/include/stdalign.h "$TINYC"/include/stdnoreturn.h "$TINYC"/include/varargs.h "$OUT/sdk/include/"
cp userland/sdk/include-extra/*.h "$OUT/sdk/include/"
cp "$TCCDIR"/compat/fcntl.h "$TCCDIR"/compat/signal.h "$TCCDIR"/compat/dlfcn.h "$OUT/sdk/include/"
mkdir -p "$OUT/sdk/include/sys"; cp "$TCCDIR"/compat/sys/*.h "$OUT/sdk/include/sys/" 2>/dev/null || true
cp build/userland/cos_crt0.o build/userland/libcos.a "$OUT/sdk/lib/"

# 5. C-OS Studio itself
CC="${CC:-gcc}" tools/cos-cc -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation \
    -I "$STUDIO" \
    -o "$OUT/studio.c-os" \
    $STUDIO/main.c $STUDIO/ui.c $STUDIO/dialogs.c $STUDIO/edview.c $STUDIO/term.c $STUDIO/make.c \
    $STUDIO/build.c $STUDIO/project.c $STUDIO/search.c $STUDIO/complete.c $STUDIO/index.c $STUDIO/settings.c \
    $STUDIO/editor.c $STUDIO/theme.c $STUDIO/buffer.c $STUDIO/highlight.c $STUDIO/studio_fonts.c \
    userland/lib/cos_ui.c userland/lib/cos_ui_fonts.c

sz=$(stat -c %s "$OUT/studio.c-os")
echo "C-OS Studio.c-os : $sz bytes (budget $MAX_STUDIO_BYTES)"
echo "tcc.c-os         : $(stat -c %s "$OUT/tcc.c-os") bytes"
[ "$sz" -le "$MAX_STUDIO_BYTES" ] || { echo "ERROR: Studio exceeds the 10 MB budget" >&2; exit 1; }
