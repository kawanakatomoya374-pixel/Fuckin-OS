# Backward-compatible x86_64-oriented entry point.
# New integrations should use freestanding-c11.mk and opt into target flags explicitly.
FREESTANDING_ARCH_CFLAGS ?= -mno-red-zone
include $(dir $(lastword $(MAKEFILE_LIST)))freestanding-c11.mk
