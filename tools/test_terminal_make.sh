#!/bin/sh
# test_terminal_make.sh - host test for the Makefile engine (studio/make.c) and the Terminal's make runner
# (terminal/tmake.c). No QEMU: both are built as ordinary Linux programs on top of studio/host/host_shim.c, and the
# C-OS compiler is replaced by a gcc wrapper that takes tcc-style arguments.
#
#   sh tools/test_terminal_make.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S="$ROOT/userland/programs/studio"; T="$ROOT/userland/programs/terminal"
W="${TMPDIR:-/tmp}/cos_make_test.$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -I$ROOT/userland/include -I$S -I$T"
gcc $FLAGS "$S/host/maketest.c" "$S/make.c" "$S/host/host_shim.c" -o "$W/maketest"
gcc $FLAGS "$T/host/tmtest.c" "$T/tmake.c" "$S/make.c" "$S/host/host_shim.c" -o "$W/tmtest"
printf '#!/bin/sh\nexec gcc -w "$@"\n' > "$W/fakecc.sh"; chmod +x "$W/fakecc.sh"
pass=0; fail=0
check() {   # check "description" "fixed string" "text"
    if printf '%s\n' "$3" | grep -qF -- "$2"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $1 (wanted: $2)"; printf '%s\n' "$3" | sed 's/^/    | /'; fi
}
absent() {  # absent "description" "fixed string" "text"
    if printf '%s\n' "$3" | grep -qF -- "$2"; then fail=$((fail+1)); echo "FAIL: $1 (unexpected: $2)"; else pass=$((pass+1)); fi
}

# ---- engine: conditionals, functions, static pattern rules, overrides ----
P="$W/p1"; mkdir -p "$P/src"; echo 'int main(){return 0;}' > "$P/src/a.c"; echo 'int f(){return 1;}' > "$P/src/b.c"
cat > "$P/Makefile" <<'MK'
NAME ?= demo
DEBUG ?= 0
ifeq ($(DEBUG),1)
CFLAGS += -g
else ifeq ($(DEBUG),2)
CFLAGS += -g3
else
CFLAGS += -O2
endif
ifdef NOPE
CFLAGS += -DNOPE
endif
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
INCS := $(foreach d,include extra,-I$(d))
define BANNER
line one
endef
rev = $(2) $(1)
.DEFAULT_GOAL := all
all: $(NAME).c-os
$(NAME).c-os: $(OBJS)
	$(CC) $^ -o $@
	@echo $(call rev,a,b) $(words $(SRCS)) $(@F) $(if $(filter 1,$(DEBUG)),dbg,rel) $(shell pwd)
$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@
clean:
	$(RM) $(OBJS)
MK
o=$("$W/maketest" "$P")
check "default goal"            "goal=all steps=4" "$o"
check "else branch"             "-O2 -Iinclude -Iextra -c src/a.c -o src/a.o" "$o"
check "static pattern rule"     "-c src/b.c -o src/b.o" "$o"
check "call/words/if/shell"     "b a 2 demo.c-os rel $P" "$o"
absent "NOPE not defined"       "-DNOPE" "$o"
o=$("$W/maketest" "$P" DEBUG=1); check "command-line var in ifeq" "-g -Iinclude" "$o"; absent "O2 skipped" "-O2" "$o"
o=$("$W/maketest" "$P" DEBUG=2); check "else ifeq"               "-g3 -Iinclude" "$o"
o=$("$W/maketest" "$P" clean);   check "clean uses \$(RM)"       "rm -f src/a.o src/b.o" "$o"
o=$("$W/maketest" "$P" nosuch 2>&1 || true); check "unknown target" "No rule to make target 'nosuch'" "$o"
printf 'a:\n\t@echo hi\n$(error boom: now)\n' > "$P/Makefile"
o=$("$W/maketest" "$P" 2>&1 || true); check "\$(error)" "ERROR: boom: now" "$o"
printf 'ifeq (a,b)\nall:\nendif\nendif\n' > "$P/Makefile"
o=$("$W/maketest" "$P" 2>&1 || true); check "stray endif" "extraneous 'endif'" "$o"

# ---- make help ----
cat > "$P/Makefile" <<'MK'
NAME ?= demo ## program name
JOBS = 4
##@ Build
all: $(NAME) ## build everything
$(NAME): main.c
	$(CC) main.c -o $@
# remove the products
clean:
	$(RM) $(NAME)
.PHONY: all clean help test
test: all
	./$(NAME)
help: ## show this help
	@grep -E '^[a-z]+:.*##' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*##"} {print $$1 $$2}'
MK
echo 'int main(){return 0;}' > "$P/main.c"
o=$("$W/maketest" "$P" help)
check "help is built in (help recipe needs awk)" "[builtin help]" "$o"
check "help: section"           "Build:" "$o"
check "help: ## doc"            "all          - build everything  [default]" "$o"
check "help: comment above"     "clean        - remove the products" "$o"
check "help: undocumented phony" "test         - (needs: all)" "$o"
check "help: variable with doc" "NAME         - program name  (default: demo)" "$o"
absent "help: undocumented plain var" "JOBS" "$o"
printf 'help:\n\t@echo "targets: all"\n\t@printf "bye\\n"\nall:\n\t@echo ok\n' > "$P/Makefile"
o=$("$W/maketest" "$P" help); check "echo-only help recipe runs as written" "goal=help steps=2" "$o"
printf 'all:\n\t@echo ok\n' > "$P/Makefile"
o=$("$W/maketest" "$P" help); check "no help rule -> listing" "[builtin help]" "$o"

# ---- the Terminal's runner ----
Q="$W/proj"; mkdir -p "$Q/src" "$Q/sub"
printf '#include <stdio.h>\nint f(void);\nint main(void){ printf("hello %%d\\n", f()); return 0; }\n' > "$Q/src/main.c"
printf 'int f(void){return 42;}\n' > "$Q/src/lib.c"
printf 'all:\n\t@echo in sub\n\t@mkdir -p out\n\t@echo x > out/f.txt\n\t@echo y >> out/f.txt\n' > "$Q/sub/Makefile"
cat > "$Q/Makefile" <<'MK'
NAME ?= demo ## program name
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
all: $(NAME).c-os ## build the program
$(NAME).c-os: $(OBJS)
	$(CC) $^ -o $@
src/%.o: src/%.c
	$(CC) -c $< -o $@
run: all ## build and run
	./$(NAME).c-os
subdir: ## build in sub/
	$(MAKE) -C sub
fail:
	false
	@echo not reached
soft:
	-false
	@echo reached
clean: ## remove products
	$(RM) src/*.o $(NAME).c-os
	$(RM) -r sub/out
.PHONY: all run subdir fail soft clean help
help:
	@grep -E '^[a-z]+:.*##' Makefile
MK
cat > "$W/cmds" <<EOF
cd $Q
make help
make -n
make
make
make run
make subdir
make fail
make soft
make clean
make nosuch
make NAME=other -s all
./other.c-os
tcc src/main.c src/lib.c -o t.c-os
./t.c-os
make -f nofile
EOF
o=$(TERMINAL_TCC="$W/fakecc.sh" "$W/tmtest" < "$W/cmds" 2>&1)
check "announces the Makefile"   "Makefile found: Makefile - 6 targets, default 'all'." "$o"
check "make help listing"        "run          - build and run" "$o"
check "dry run shows the commands" "tcc -c src/lib.c -o src/lib.o" "$o"
check "builds with the compiler" "make: done (all)" "$o"
check "up to date the 2nd time"  "make: 'all' is up to date." "$o"
check "runs the program"         "hello 42" "$o"
check "nested make"              "make: Entering directory '$Q/sub'" "$o"
check "nested make output"       "in sub" "$o"
check "leaving directory"        "make: Leaving directory '$Q/sub'" "$o"
check "failing command stops"    "make: *** [fail] Error 1" "$o"
absent "after the failure"       "not reached" "$o"
check "- ignores an error"       "make: [soft] Error 1 (ignored)" "$o"
check "- keeps going"            "reached" "$o"
check "clean"                    "make: done (clean)" "$o"
check "no rule"                  "make: *** No rule to make target 'nosuch'.  Stop." "$o"
check "var override + -s"        "[started other.c-os" "$o"
check "program output"           "[exit code 0]" "$o"
check "tcc passthrough"          "$ tcc src/main.c src/lib.c -o t.c-os" "$o"
check "missing -f file"          "make: *** nofile: No such file.  Stop." "$o"
printf 'cd %s\nmake subdir\n' "$Q" | TERMINAL_TCC="$W/fakecc.sh" "$W/tmtest" >/dev/null 2>&1 || true
if printf 'x\ny\n' | cmp -s - "$Q/sub/out/f.txt"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: > and >> redirection in recipes"; fi

echo "terminal make: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
