#!/bin/sh
# Builds the Sprint interpreter (https://github.com/kawanakatomoya374-pixel/Sprint, a C11 stack VM) as a C-OS program: build_cos.sh OUT.c-os [SPRINT_SRC_DIR]
# Two small shims (shim.h: timespec_get; shim.c: getline) fill gaps in the C-OS libc. SP_JIT=1 (second argument of the environment, see README) enables the loop JIT.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=$1; SRC=${2:-$ROOT/userland/third_party/sprint}; W=${WORK:-/tmp/sprint_work}
rm -rf "$W"; mkdir -p "$W"; cp -r "$SRC/." "$W/" || exit 1
cd "$W" || exit 1
cp "$HERE/cosgui.c" "$HERE/cosapi.c" "$HERE/smooth.c" src/lib/
python3 - <<'PYEOF'      # gfx text goes through the smooth font unless Settings chose the retro one
import re
p = "src/lib/lib_gfx.c"; s = open(p).read()
s = s.replace("static void draw_text(Canvas *c, int x, int y, const SString *s, uint32_t col, int scale) {\n", "int sp_smooth_mode(void);\nvoid sp_smooth_draw(uint32_t *px, int w, int h, int cx0, int cy0, int cx1, int cy1, int x, int y, const char *s, size_t n, uint32_t col, int scale);\nint sp_smooth_advance(const char *s, size_t n, int scale);\nstatic void draw_text(Canvas *c, int x, int y, const SString *s, uint32_t col, int scale) {\n    if (sp_smooth_mode() && c->alpha == 255 && scale <= 8) { sp_smooth_draw(c->px, c->w, c->h, c->cx0, c->cy0, c->cx1, c->cy1, x, y, s->data, s->len, col, scale); return; }\n", 1)
s = s.replace("static int64_t text_advance(const SString *s, int scale) {\n", "static int64_t text_advance(const SString *s, int scale) {\n    if (sp_smooth_mode() && scale <= 8) return sp_smooth_advance(s->data, s->len, scale);\n", 1)
assert "sp_smooth_draw(c->px" in s and "sp_smooth_advance(s->data" in s
open(p, "w").write(s)
PYEOF
cp "$HERE/cosgui.sp" stdlib/cosgui.sp       # the C-OS window backend: native _cosgui + the Sprint module cosgui
sed -i 's|^static const ModConst no_consts|extern const NativeDesc lib_cosgui_fns[], lib_cos_fns[];\nstatic const ModConst no_consts|; s|{"_ffi", lib_ffi_fns, no_consts},|{"_ffi", lib_ffi_fns, no_consts}, {"_cosgui", lib_cosgui_fns, no_consts}, {"_cos", lib_cos_fns, no_consts},|' src/lib/lib.c
make build/gen/bundled.c >/tmp/sprint_bundled.log 2>&1 || { cat /tmp/sprint_bundled.log; echo "bundled.c failed"; exit 1; }
SRCS="$(find src -name '*.c' | grep -v 'jit/' | tr '\n' ' ') build/gen/bundled.c $(ls cli/*.c | tr '\n' ' ') $HERE/shim.c"
# the loop JIT (copy-and-patch, x86-64) needs only mmap/mprotect(PROT_EXEC), which C-OS has: its platform guard names Linux, so SP_COS is added to it. SP_JIT=0 builds without it.
sed -i 's|defined(__linux__) \&\& !defined(SP_NO_JIT)|(defined(__linux__) \|\| defined(SP_COS)) \&\& !defined(SP_NO_JIT)|' src/sp.h
JITFLAGS="-DSP_COS"; [ "${SP_JIT:-1}" = 0 ] && JITFLAGS="-DSP_NO_JIT"
if [ "$JITFLAGS" = "-DSP_COS" ]; then
    # the C-OS libc has a smaller jmp_buf than glibc, so VM's fields move: jit_layout.h holds the offsets measured with cos-cc (tools/jitlayout.c), and the stencils are assembled with them
    cp "$HERE/jit_layout.h" src/runtime/jit/layout.h
    mkdir -p build/jitgen && gcc -O1 -o build/jitgen/jitgen tools/jitgen.c && gcc -O2 -c -x assembler-with-cpp -Isrc/runtime/jit -o build/jitgen/stencils.o src/runtime/jit/stencils.S \
        && build/jitgen/jitgen build/jitgen/stencils.o > src/runtime/jit/stencils.h || { echo "stencils failed"; exit 1; }
fi
[ -f "$ROOT/build/libbearssl_user.a" ] || make -C "$ROOT" build/libbearssl_user.a >/dev/null || exit 1
exec "$ROOT/tools/cos-cc" -O2 -w -std=gnu11 -I"$ROOT/src/third_party/bearssl/inc" -I"$ROOT/build/update" -Isrc -DNDEBUG_SP $JITFLAGS -include "$HERE/shim.h" -o "$OUT" $SRCS "$ROOT/build/libbearssl_user.a"
