/*
 * quickjs_jit.h - Copy-and-Patch JIT compiler header for QuickJS on C-OS
 */

#ifndef QUICKJS_JIT_H
#define QUICKJS_JIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JIT opaque handle - internal structure hidden from users */
typedef struct JITCompiledFunc JITCompiledFunc;

/* Bytecode info structure for JIT compilation */
typedef struct {
    const uint8_t *bytecode;
    int bytecode_len;
    uint16_t stack_size;
    uint16_t arg_count;
    uint16_t var_count;
} JITBytecodeInfo;

/**
 * Initialize the JIT system.
 * Returns true on success, false on failure.
 */
bool jit_init(void);

/**
 * Try to compile and execute a function using JIT.
 * Pass extracted bytecode info instead of full JSFunctionBytecode.
 * Returns pointer to native code if available, NULL to fall back to interpreter.
 */
void *jit_try_compile(const JITBytecodeInfo *info);

/**
 * Get JIT statistics.
 */
void jit_get_stats(uint32_t *compiled, uint32_t *jit_exec, uint32_t *interp_exec);

/**
 * Record one interpreted call of a function, keyed by its
 * JSFunctionBytecode pointer (used as an identity only, never
 * dereferenced). Called from the interpreter's function entry, so it is
 * allocation-free, lock-free and bounded-cost by design.
 */
void jit_profile_call(const void *fn_bytecode, uint32_t byte_code_len,
                      uint16_t arg_count, uint16_t stack_size);

/**
 * Dump the collected hot-function profile to the serial log. Intended to
 * be called once, after a representative workload has run.
 */
void jit_profile_report(void);

/**
 * Shutdown the JIT system and free resources.
 */
void jit_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_JIT_H */
