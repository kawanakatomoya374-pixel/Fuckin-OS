CC ?= gcc
CFLAGS ?= -Wall -Wextra -I./include -std=c11 -O2
LDLIBS ?= -lm
RUNTIME_SRCS := src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c
