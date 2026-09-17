#ifndef MICROPY_INCLUDED_COS_MPCONFIGPORT_H
#define MICROPY_INCLUDED_COS_MPCONFIGPORT_H

#include <stdint.h>
#include <stddef.h>

// C-OS types.h already defines NORETURN, MicroPython will try to redefine it.
#ifdef NORETURN
#undef NORETURN
#endif
#define MP_NORETURN __attribute__((noreturn))
#define NORETURN MP_NORETURN

#include <alloca.h>

// MicroPython configuration for C-OS
#define MICROPY_ENABLE_GC           (1)
#define MICROPY_HELPER_REPL         (1)
#define MICROPY_HELPER_LEXER_UNIX   (0)
#define MICROPY_ENABLE_SOURCE_LINE  (1)
#define MICROPY_ENABLE_DOC_STRING   (0)
#define MICROPY_ERROR_REPORTING     (MICROPY_ERROR_REPORTING_TERSE)
#define MICROPY_BUILTIN_METHOD_CHECK_SELF_ARG (0)
#define MICROPY_PY_ASYNC_AWAIT      (0)
#define MICROPY_PY_BUILTINS_BYTEARRAY (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW (1)
#define MICROPY_PY_BUILTINS_ENUMERATE (1)
#define MICROPY_PY_BUILTINS_FILTER    (1)
#define MICROPY_PY_BUILTINS_REVERSED  (1)
#define MICROPY_PY_BUILTINS_SET       (1)
#define MICROPY_PY_BUILTINS_SLICE     (1)
#define MICROPY_PY_BUILTINS_PROPERTY  (1)
#define MICROPY_PY_BUILTINS_MIN_MAX   (1)
#define MICROPY_PY_BUILTINS_STR_COUNT (1)
#define MICROPY_PY_BUILTINS_STR_OP_MODULO (1)
#define MICROPY_PY_GC                (1)
#define MICROPY_PY_ARRAY             (1)
#define MICROPY_PY_COLLECTIONS       (1)
#define MICROPY_PY_BUILTINS_FLOAT   (0)
#define MICROPY_PY_BUILTINS_COMPLEX (0)
#define MICROPY_PY_MATH              (0)
#define MICROPY_PY_CMATH             (0)
#define MICROPY_PY_IO                (1)
#define MICROPY_PY_STRUCT            (1)
#define MICROPY_PY_SYS               (1)
#define MICROPY_PY_MACHINE           (0)
#define MICROPY_CPYTHON_COMPAT       (1)
#define MICROPY_LONGINT_IMPL         (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL           (MICROPY_FLOAT_IMPL_NONE)

// VFS support
#define MICROPY_VFS                  (0)
#define MICROPY_READER_VFS           (0)

// Types
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

// Board name
#define MICROPY_HW_BOARD_NAME "C-OS-4.0.7"
#define MICROPY_HW_MCU_NAME   "x86_64"

#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_PORT_ROOT_POINTERS \
    const char *readline_hist[8];

#endif
