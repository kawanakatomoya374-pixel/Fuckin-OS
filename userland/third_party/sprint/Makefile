# Sprint 2.0 reference implementation
#
#   make            strict release build (-Werror, hardened)
#   make debug      strict build with ASan + UBSan, no optimisation
#   make analyze    GCC static analyzer (-fanalyzer), also -Werror
#   make test       run regression tests against the release binary
#   make test-san   run regression tests against the sanitizer binary
#   make clean
#
# Every warning is an error. Flags that are deliberately NOT enabled are listed
# at the bottom with the reason.

CC       ?= gcc
BUILD    ?= build
.DEFAULT_GOAL := all
GEN      := $(BUILD)/gen
STDSRC   := $(wildcard stdlib/*.sp)
# gui is kept as ordered parts (stdlib/gui/NN_*.sp) and embedded as one module
GUIPARTS := $(sort $(wildcard stdlib/gui/*.sp))

# ---- language / diagnostics -------------------------------------------------
STD      := -std=c11 -pedantic-errors
WARN     := -Werror -Wall -Wextra \
            -Wconversion -Wsign-conversion -Wdouble-promotion -Warith-conversion \
            -Wshadow -Wundef -Wpointer-arith -Wcast-align=strict -Wcast-qual \
            -Wwrite-strings -Wvla -Walloca -Wredundant-decls -Wnested-externs \
            -Wstrict-prototypes -Wold-style-definition \
            -Wmissing-prototypes -Wmissing-declarations \
            -Wformat=2 -Wformat-signedness -Wformat-overflow=2 -Wformat-truncation=2 \
            -Wnull-dereference -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
            -Wimplicit-fallthrough=5 -Wswitch-default -Wjump-misses-init \
            -Wbad-function-cast -Wstrict-aliasing=2 -Warray-bounds=2 \
            -Wstringop-overflow=4 -Wshift-overflow=2 -Wunused-macros \
            -Wmissing-include-dirs -Wtrampolines -Wunsafe-loop-optimizations \
            -Wstack-protector -Winvalid-pch -Wattribute-alias=2 \
            -Wfloat-equal -Wswitch-enum -Wpacked -Wdate-time -Wdisabled-optimization \
            -Wmultistatement-macros -Wunused-const-variable=2 -Wuninitialized \
            -Wmaybe-uninitialized -Warray-compare -Wc11-c2x-compat -Wcomment \
            -Wtrigraphs -Wstrict-flex-arrays -Wfree-nonheap-object -Wdangling-else \
            -Wsizeof-array-argument -Wsizeof-pointer-memaccess -Wpointer-compare \
            -Wint-in-bool-context -Wmisleading-indentation -Wparentheses \
            -Wframe-larger-than=65536 -Wlarger-than=8388608 \
            -Wsuggest-attribute=format -Wsuggest-attribute=noreturn \
            -Wno-error=unsafe-loop-optimizations

# ---- hardening ---------------------------------------------------------------
HARDEN   := -fstack-protector-strong -fstack-clash-protection -fcf-protection=full \
            -D_FORTIFY_SOURCE=3 -fno-common -fno-strict-overflow -fwrapv-pointer \
            -fno-delete-null-pointer-checks -ftrivial-auto-var-init=zero \
            -fstrict-flex-arrays=3 -fPIE
LDHARDEN := -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

INC      := -Isrc
comma    := ,
TUNE     := $(if $(filter x86_64,$(shell uname -m)),-mtune-ctrl=^sse_unaligned_load_optimal$(comma)^sse_unaligned_store_optimal,)
CFLAGS_REL := $(STD) $(WARN) $(HARDEN) -O2 $(TUNE) -g $(INC) -DNDEBUG_SP
CFLAGS_DBG := $(STD) $(WARN) $(HARDEN) -O0 -g3 $(INC) \
              -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -fno-omit-frame-pointer
LDFLAGS_DBG := -fsanitize=address,undefined

FRONT_SRC   := src/front/lexer.c src/front/parser.c src/front/parse_expr.c src/front/compiler.c src/front/cmp_util.c src/front/cmp_expr.c src/front/cmp_stmt.c src/front/cmp_class.c src/front/cmp_import.c src/front/disasm.c src/front/loader.c
RUNTIME_SRC := src/runtime/value.c src/runtime/table.c src/runtime/format.c src/runtime/gc.c src/runtime/vm_core.c src/runtime/vm.c src/runtime/image.c src/runtime/sha256.c src/runtime/verify.c src/runtime/embed.c src/runtime/jit.c
LIB_SRC     := src/lib/lib.c src/lib/lib_math.c src/lib/lib_str.c src/lib/lib_array.c src/lib/lib_table.c src/lib/lib_sys.c src/lib/lib_co.c src/lib/lib_ext.c src/lib/lib_gfx.c src/lib/font_ja.c src/lib/lib_ffi.c src/lib/lib_typed.c
LIBSRC := $(FRONT_SRC) $(RUNTIME_SRC) $(LIB_SRC) $(GEN)/bundled.c
CLISRC := cli/main.c cli/tool_fmt.c cli/tool_lint.c cli/repl.c
ALLSRC := $(LIBSRC) $(CLISRC)

REL_OBJ := $(patsubst %.c,$(BUILD)/rel/%.o,$(ALLSRC))
DBG_OBJ := $(patsubst %.c,$(BUILD)/dbg/%.o,$(ALLSRC))

$(GEN)/gui.sp: $(GUIPARTS)
	@mkdir -p $(GEN)
	cat $(GUIPARTS) > $@

$(GEN)/bundled.c: tools/embedsrc.c $(STDSRC) $(GEN)/gui.sp Makefile
	@mkdir -p $(GEN)
	$(CC) -O1 -o $(GEN)/embedsrc tools/embedsrc.c
	$(GEN)/embedsrc $@ gui=$(GEN)/gui.sp $(foreach f,$(STDSRC),$(basename $(notdir $(f)))=$(f))

.PHONY: all debug analyze test test-san test-embed fuzz bench clean lib
all: $(BUILD)/sprint

lib: $(BUILD)/libsprint.a

$(BUILD)/libsprint.a: $(filter-out $(BUILD)/rel/cli/%,$(REL_OBJ))
	rm -f $@
	ar rcs $@ $^

debug: $(BUILD)/sprint-san

$(BUILD)/sprint: $(REL_OBJ)
	$(CC) $(CFLAGS_REL) $(LDHARDEN) -o $@ $^ -lm -ldl

$(BUILD)/sprint-san: $(DBG_OBJ)
	$(CC) $(CFLAGS_DBG) -o $@ $^ -lm -ldl $(LDFLAGS_DBG)

# The interpreter loop is the one file where -fno-strict-overflow costs ~30% (it blocks loop and branch folding in the
# dispatch switch). vm.c does not rely on signed wrap-around: every checked operation uses unsigned or builtin overflow
# helpers, and the UBSan build runs the whole suite. vm.c alone therefore uses the compiler default.
$(BUILD)/rel/src/runtime/vm.o: CFLAGS_REL += -fstrict-overflow

$(BUILD)/rel/%.o: %.c src/sp.h src/sprint.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_REL) -MMD -c -o $@ $<

$(BUILD)/dbg/%.o: %.c src/sp.h src/sprint.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DBG) -MMD -c -o $@ $<

# image fuzzer (ASan + UBSan): make fuzz IMAGE=x.spc ITER=20000 SEED=1
IMAGE ?= build/fuzz-seed.spc
ITER  ?= 5000
SEED  ?= 1
fuzz: $(BUILD)/sprint-san
	$(CC) $(CFLAGS_DBG) -o $(BUILD)/fuzz-image tools/fuzz_image.c $(filter-out $(BUILD)/dbg/cli/%,$(DBG_OBJ)) -lm -ldl $(LDFLAGS_DBG)
	@for t in class coroutine defer2 closures match sort hof iter errors cls_super2 json; do \
	  $(BUILD)/sprint-san compile -o $(BUILD)/fuzz-seed.spc $$(ls tests/cases/*/$$t.sp) && \
	  ASAN_OPTIONS=detect_leaks=0 $(BUILD)/fuzz-image $(BUILD)/fuzz-seed.spc $(ITER) $(SEED) || exit 1; \
	done

analyze: $(GEN)/bundled.c
	@for f in $(ALLSRC); do \
	  echo "analyze $$f"; \
	  $(CC) $(STD) $(WARN) -O1 $(INC) -fanalyzer -fstrict-flex-arrays=3 -Wno-analyzer-malloc-leak \
	    -fsyntax-only $$f || exit 1; \
	done

# shared libraries used by the ffi tests (the C++ one only when a C++ compiler exists)
FFI_LIBS := $(BUILD)/libctest.so $(if $(shell command -v c++ 2>/dev/null),$(BUILD)/libcxxtest.so)
$(BUILD)/libctest.so: tests/ffi/ctest.c
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -o $@ $<
$(BUILD)/libcxxtest.so: tests/ffi/cxxtest.cpp
	@mkdir -p $(BUILD)
	c++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

test: $(BUILD)/sprint $(FFI_LIBS)
	@SPRINT=$(BUILD)/sprint sh tests/run_tests.sh

test-san: $(BUILD)/sprint-san $(FFI_LIBS)
	@SPRINT=$(BUILD)/sprint-san sh tests/run_tests.sh

# C embedding API: build the host programs against the library (release and
# ASan/UBSan builds) and run them.
$(BUILD)/libsprint-san.a: $(filter-out $(BUILD)/dbg/cli/%,$(DBG_OBJ))
	rm -f $@
	ar rcs $@ $^

test-embed: $(BUILD)/libsprint.a $(BUILD)/libsprint-san.a
	$(CC) $(STD) $(WARN) $(HARDEN) -O2 $(INC) -o $(BUILD)/embed_test tests/embed/embed_test.c $(BUILD)/libsprint.a -lm -ldl
	$(CC) $(STD) $(WARN) $(HARDEN) -O0 -g $(INC) -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $(BUILD)/embed_test-san tests/embed/embed_test.c $(BUILD)/libsprint-san.a -lm -ldl -fsanitize=address,undefined
	$(BUILD)/embed_test
	$(BUILD)/embed_test-san
	$(CC) $(STD) $(WARN) $(HARDEN) -O2 $(INC) -o $(BUILD)/embed_demo examples/embed/host.c $(BUILD)/libsprint.a -lm -ldl
	$(BUILD)/embed_demo > $(BUILD)/embed_demo.out
	@cmp $(BUILD)/embed_demo.out tests/embed/host_demo.out && echo "embed demo ok"

clean:
	rm -rf $(BUILD)

-include $(REL_OBJ:.o=.d) $(DBG_OBJ:.o=.d)
include os/os.mk

# Deliberately not enabled:
#  -Wpadded            SpValue's explicit padding is part of the ABI (SPEC 6.2)
#  -Wtraditional, -Wsystem-headers   obsolete / noise from libc
#  -fanalyzer in the normal build    slow; run `make analyze` instead
#  -Wstrict-overflow                 deprecated in favour of -fno-strict-overflow (enabled above)
#  -Winline                          noise; inlining is the optimiser's business
# Exact floating-point comparison is required by the language (SPEC 7.5/7.6), so
# -Wfloat-equal stays on and each intentional site carries a local, commented pragma.

bench: $(BUILD)/sprint
	tools/bench/run.sh $(BUILD)/sprint

# ---- Copy-and-patch JIT: regenerate the committed stencil table (needs gcc + the assembler; not part of normal builds)
jit-regen: tools/jitgen.c tools/jitlayout.c src/runtime/jit/stencils.S
	$(CC) -O1 $(INC) -o $(GEN)/jitlayout tools/jitlayout.c
	$(GEN)/jitlayout > src/runtime/jit/layout.h
	$(CC) -O1 -o $(GEN)/jitgen tools/jitgen.c
	$(CC) -O2 -c -x assembler-with-cpp -Isrc/runtime/jit -o $(GEN)/stencils.o src/runtime/jit/stencils.S
	$(GEN)/jitgen $(GEN)/stencils.o > src/runtime/jit/stencils.h
.PHONY: jit-regen

# Whole suite with the JIT compiling every loop at its first back edge (the output must equal the interpreter's).
test-jit: $(BUILD)/sprint $(FFI_LIBS)
	@SPRINT_JIT_THRESHOLD=1 SPRINT=$(BUILD)/sprint sh tests/run_tests.sh
.PHONY: test-jit
