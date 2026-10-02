# HobbyOS integration build fragment (Alpha0.6)
#
# カーネルのMakefileから `include templates/hobby_os/hobby_os.mk` するか、
# 下記変数を自分のビルドへコピーしてください。`make -f templates/hobby_os/hobby_os.mk`
# 単体でも、テンプレートと埋込みランタイムのアーカイブを作成できます。

PROJECT_ROOT ?= .
CC ?= cc
AR ?= ar
BUILD ?= $(PROJECT_ROOT)/build/hobby_os

# 厳格な警告基準（プロジェクト本体と同じ）
WARN_CFLAGS ?= -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral \
	-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wundef \
	-Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal

# 自作OS向け構成: 標準Cライブラリなし、カーネルの malloc/free と setjmp/longjmp を使い、
# p2c_embed がプラットフォーム互換フックとヒープを提供する。
HOBBY_OS_DEFINES := -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
	-DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT

HOBBY_OS_ARCH_CFLAGS ?=
HOBBY_OS_EXTRA_CFLAGS ?=
CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 \
	$(WARN_CFLAGS) $(HOBBY_OS_ARCH_CFLAGS) $(HOBBY_OS_EXTRA_CFLAGS)
CPPFLAGS ?= -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT)/templates/hobby_os $(HOBBY_OS_DEFINES)

# 変換済みモジュール（--embed-entry kernel_python_program）を追加する場合は
# GENERATED_MODULE を指定する。指定しなければテンプレートのコンパイルのみ。
GENERATED_MODULE ?=
RUNTIME_SRC := \
	$(PROJECT_ROOT)/src/runtime/python_code_to_c_runtime.c \
	$(PROJECT_ROOT)/src/common/python_code_to_c_common.c \
	$(PROJECT_ROOT)/src/platform/python_code_to_c_platform.c \
	$(PROJECT_ROOT)/src/platform/python_code_to_c_embed.c
TEMPLATE_SRC := $(PROJECT_ROOT)/templates/hobby_os/embed/hobby_os_embed.c
SRC := $(RUNTIME_SRC) $(TEMPLATE_SRC) $(GENERATED_MODULE)
OBJ := $(patsubst $(PROJECT_ROOT)/%.c,$(BUILD)/%.o,$(SRC))

.PHONY: all clean print-config

all: $(BUILD)/libpython-code-to-c-hobby-os.a

print-config:
	@printf '%s\n' "hobby_os_config: CC=$(CC) AR=$(AR) BUILD=$(BUILD)"
	@printf '%s\n' "hobby_os_cflags: $(CFLAGS)"
	@printf '%s\n' "hobby_os_cppflags: $(CPPFLAGS)"

$(BUILD)/libpython-code-to-c-hobby-os.a: $(OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/%.o: $(PROJECT_ROOT)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
