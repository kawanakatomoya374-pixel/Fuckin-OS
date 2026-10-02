# Portable freestanding integration template for a hobby OS.
PROJECT_ROOT ?= .
CC ?= cc
AR ?= ar
WARN_CFLAGS ?= -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wundef -Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal
FREESTANDING_ARCH_CFLAGS ?=
EXTRA_CFLAGS ?=
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector $(FREESTANDING_ARCH_CFLAGS)
CFLAGS ?= $(FREESTANDING_CFLAGS) -std=c11 -O2 $(WARN_CFLAGS) $(EXTRA_CFLAGS)
CPPFLAGS ?= -I$(PROJECT_ROOT)/include -DPYTHON_CODE_TO_C_NO_STDLIB
BUILD ?= $(PROJECT_ROOT)/build/freestanding
CORE_DIRS := common lexer parser semantic codegen runtime core platform modules
CORE_SRC := $(foreach d,$(CORE_DIRS),$(wildcard $(PROJECT_ROOT)/src/$(d)/*.c))
CORE_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.c,$(BUILD)/%.o,$(CORE_SRC))

.PHONY: all clean print-config
all: $(BUILD)/libpython-code-to-c-core.a

print-config:
	@printf '%s\n' "freestanding_config: CC=$(CC) AR=$(AR) BUILD=$(BUILD)"
	@printf '%s\n' "freestanding_cflags: $(CFLAGS)"
	@printf '%s\n' "freestanding_cppflags: $(CPPFLAGS)"

$(BUILD)/libpython-code-to-c-core.a: $(CORE_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/%.o: $(PROJECT_ROOT)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

# An outer build may supply CC, AR, CFLAGS, CPPFLAGS, BUILD, EXTRA_CFLAGS, and
# FREESTANDING_ARCH_CFLAGS.  For an x86_64 kernel, set
# FREESTANDING_ARCH_CFLAGS=-mno-red-zone when its ABI requires it.
