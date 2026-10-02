PROJECT_ROOT ?= .
CC ?= cc
AR ?= ar
BUILD ?= $(PROJECT_ROOT)/build/baremetal
WARN_CFLAGS ?= -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wundef -Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal
BAREMETAL_ARCH_CFLAGS ?=
EXTRA_CFLAGS ?=
CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) $(BAREMETAL_ARCH_CFLAGS) $(EXTRA_CFLAGS)
CPPFLAGS ?= -I$(PROJECT_ROOT)/include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME
RUNTIME_SRC := $(PROJECT_ROOT)/src/common/python_code_to_c_common.c $(PROJECT_ROOT)/src/runtime/python_code_to_c_runtime.c $(PROJECT_ROOT)/src/platform/python_code_to_c_platform.c
APP_SRC := $(PROJECT_ROOT)/examples/baremetal/python_code_to_c_baremetal.c $(PROJECT_ROOT)/examples/baremetal/baremetal_hello_program.c $(PROJECT_ROOT)/examples/baremetal/baremetal_start.c
SRC := $(RUNTIME_SRC) $(APP_SRC)
OBJ := $(patsubst $(PROJECT_ROOT)/%.c,$(BUILD)/%.o,$(SRC))

.PHONY: all clean print-config
all: $(BUILD)/libpython-code-to-c-baremetal-example.a

print-config:
	@printf '%s\n' "baremetal_config: CC=$(CC) AR=$(AR) BUILD=$(BUILD)"
	@printf '%s\n' "baremetal_cflags: $(CFLAGS)"
	@printf '%s\n' "baremetal_cppflags: $(CPPFLAGS)"

$(BUILD)/libpython-code-to-c-baremetal-example.a: $(OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/%.o: $(PROJECT_ROOT)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
