#!/bin/sh
# patch_upstream.sh UPSTREAM WORK: copies the PSXFunkin sources (src/ with the generated src/iso headers) to WORK and applies the one change the port needs:
# the allocator macros of boot/mem.h (PSXF_STDMEM) call psxf_alloc / psxf_free (backend/mem.c), which ignore a pointer that is not a heap block: the PlayStation build frees
# the chart of a stage, which is an array inside the program.
UP=$1; W=$2
rm -rf "$W/src"; mkdir -p "$W"; cp -r "$UP/src" "$W/src" || exit 1
python3 - "$W/src/boot/mem.h" <<'PY' || exit 1
import sys
p = sys.argv[1]; s = open(p).read()
a = "#define Mem_Alloc malloc\n#define Mem_Free free"
assert a in s, "mem.h has changed"
s = s.replace(a, "void *psxf_alloc(size_t size);\nvoid psxf_free(void *ptr);\n#define Mem_Alloc psxf_alloc\n#define Mem_Free psxf_free")
open(p, "w").write(s)
PY
