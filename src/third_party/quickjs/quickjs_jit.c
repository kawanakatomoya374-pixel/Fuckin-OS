/*
 * quickjs_jit.c - Copy-and-Patch JIT compiler for QuickJS on C-OS
 */

#include "quickjs_jit.h"
#include "quickjs.h"
#include "quickjs-c-atomics.h"
#include "../include/memory.h"
#include "../include/serial.h"

/* Include opcode definitions */
#define DEF(id, size, n_pop, n_push, f) OP_ ## id,
#define def(id, size, n_pop, n_push, f)
typedef enum {
#include "quickjs-opcode.h"
    OP_COUNT
} OPCodeEnum;
#undef DEF
#undef def

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* JIT Configuration */
#define JIT_CODE_CACHE_SIZE     (2 * 1024 * 1024)
#define JIT_MAX_FUNCTIONS       1024
#define JIT_HOT_THRESHOLD       100
#define JIT_MAX_PATCH_SIZE      4096
#define JIT_ALIGNMENT           16

/* x86_64 opcodes */
#define X64_MOV_RAX_IMM     0xB8
#define X64_PUSH_RBP        0x55
#define X64_LEAVE           0xC9
#define X64_RET             0xC3
#define X64_NOP             0x90

/* JIT Code Cache */
typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    uint8_t *alloc_ptr;
} jit_code_cache_t;

/* Compiled Function Entry */
typedef struct {
    void *bc;  /* Opaque pointer to avoid including quickjs internals */
    uint8_t *native_code;
    size_t native_size;
    uint32_t invoke_count;
    bool compiled;
    bool compiling;
} jit_function_entry_t;

/* JIT Context */
typedef struct {
    jit_code_cache_t code_cache;
    jit_function_entry_t functions[JIT_MAX_FUNCTIONS];
    size_t function_count;
    bool initialized;
    uint32_t stats_compiled;
    uint32_t stats_executed_jit;
    uint32_t stats_executed_interp;
} jit_context_t;

static jit_context_t g_jit_ctx;

static bool jit_code_cache_init(jit_code_cache_t *cache)
{
    cache->base = (uint8_t *)kmalloc(JIT_CODE_CACHE_SIZE);
    if (!cache->base) {
        serial_puts("[JIT] Failed to allocate code cache\n");
        return false;
    }
    cache->size = JIT_CODE_CACHE_SIZE;
    cache->used = 0;
    cache->alloc_ptr = cache->base;
    serial_puts("[JIT] Code cache allocated (2MB)\n");
    return true;
}

bool jit_init(void)
{
    if (g_jit_ctx.initialized) {
        return true;
    }
    
    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    
    if (!jit_code_cache_init(&g_jit_ctx.code_cache)) {
        return false;
    }
    
    g_jit_ctx.initialized = true;
    serial_puts("[JIT] Global JIT context initialized\n");
    return true;
}

void *jit_try_compile(const JITBytecodeInfo *info)
{
    (void)info;
    /* No code generation yet - see jit_profile_call() below. This still
     * returns NULL so every call falls through to the interpreter; the
     * profiling layer is deliberately landed first and separately, because
     * without real measurements of which functions are actually hot there
     * is no way to tell whether a compiler is worth its complexity, nor
     * anything to validate its output against. */
    return NULL;
}

/* ---- Tier-up profiling -------------------------------------------------
 *
 * Records per-function call counts and bytecode size, keyed by the
 * JSFunctionBytecode pointer, so the real hot-function distribution of
 * actual pages can be measured before any compiler exists.
 *
 * Deliberately allocation-free and lock-free on the hot path: this is
 * called on EVERY interpreted JS call, so it must not perturb the very
 * numbers it is trying to collect. A fixed-size open-addressed table with
 * linear probing and a hard probe limit means a bounded, predictable cost
 * per call and no behaviour change when the table fills - entries are
 * simply not tracked past that point, which skews coverage but never
 * correctness.
 *
 * The pointer is used only as an identity key and is never dereferenced
 * here, so a freed JSFunctionBytecode cannot cause a use-after-free
 * through this table. A recycled allocation could merge two functions'
 * counts, which is acceptable for profiling and noted rather than
 * pretended away. */
#define JIT_PROFILE_SLOTS 1024u
#define JIT_PROFILE_MAX_PROBE 8u

typedef struct {
    const void *key;      /* JSFunctionBytecode*, identity only */
    uint32_t call_count;
    uint32_t byte_code_len;
    uint16_t arg_count;
    uint16_t stack_size;
} jit_profile_entry_t;

static jit_profile_entry_t g_jit_profile[JIT_PROFILE_SLOTS];
static uint32_t g_jit_profile_live;
static uint32_t g_jit_profile_dropped;
static uint64_t g_jit_profile_calls;

static inline uint32_t jit_profile_hash(const void *key)
{
    /* Pointers are allocator-aligned, so the low bits carry little
     * information; mix them up before masking. */
    uint64_t v = (uint64_t)(uintptr_t)key;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (uint32_t)(v & (JIT_PROFILE_SLOTS - 1u));
}

void jit_profile_call(const void *fn_bytecode, uint32_t byte_code_len,
                      uint16_t arg_count, uint16_t stack_size)
{
    if (fn_bytecode == NULL) return;
    ++g_jit_profile_calls;

    uint32_t slot = jit_profile_hash(fn_bytecode);
    for (uint32_t probe = 0; probe < JIT_PROFILE_MAX_PROBE; ++probe) {
        jit_profile_entry_t *e = &g_jit_profile[(slot + probe) & (JIT_PROFILE_SLOTS - 1u)];
        if (e->key == fn_bytecode) { ++e->call_count; return; }
        if (e->key == NULL) {
            e->key = fn_bytecode;
            e->call_count = 1;
            e->byte_code_len = byte_code_len;
            e->arg_count = arg_count;
            e->stack_size = stack_size;
            ++g_jit_profile_live;
            return;
        }
    }
    ++g_jit_profile_dropped;
}

void jit_profile_report(void)
{
    if (g_jit_profile_calls == 0) return;

    serial_puts("[JITPROF] total_calls=");
    serial_putdec(g_jit_profile_calls);
    serial_puts(" tracked_functions=");
    serial_putdec(g_jit_profile_live);
    serial_puts(" dropped_by_probe_limit=");
    serial_putdec(g_jit_profile_dropped);
    serial_puts("\n");

    /* Report the busiest functions by selection pass rather than sorting:
     * the table is small and this runs once, so an O(n*k) scan avoids
     * needing scratch memory. */
    uint32_t reported = 0;
    uint32_t ceiling = 0xFFFFFFFFu;
    while (reported < 10u) {
        uint32_t best = 0;
        int best_idx = -1;
        for (uint32_t i = 0; i < JIT_PROFILE_SLOTS; ++i) {
            if (g_jit_profile[i].key == NULL) continue;
            uint32_t c = g_jit_profile[i].call_count;
            if (c < ceiling && c > best) { best = c; best_idx = (int)i; }
        }
        if (best_idx < 0) break;
        jit_profile_entry_t *e = &g_jit_profile[best_idx];
        serial_puts("[JITPROF]   calls=");
        serial_putdec(e->call_count);
        serial_puts(" bytecode_len=");
        serial_putdec(e->byte_code_len);
        serial_puts(" args=");
        serial_putdec(e->arg_count);
        serial_puts(" stack=");
        serial_putdec(e->stack_size);
        serial_puts("\n");
        ceiling = best;
        ++reported;
        /* Skip any ties at this count on the next pass by zeroing the key
         * we just reported; the table is rebuilt per boot anyway. */
        e->key = NULL;
    }
}

void jit_get_stats(uint32_t *compiled, uint32_t *jit_exec, uint32_t *interp_exec)
{
    if (compiled) *compiled = g_jit_ctx.stats_compiled;
    if (jit_exec) *jit_exec = g_jit_ctx.stats_executed_jit;
    if (interp_exec) *interp_exec = g_jit_ctx.stats_executed_interp;
}

void jit_shutdown(void)
{
    if (g_jit_ctx.initialized && g_jit_ctx.code_cache.base) {
        kfree(g_jit_ctx.code_cache.base);
        memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
        serial_puts("[JIT] JIT context cleaned up\n");
    }
}

/* Legacy wrapper for compatibility */
void *jit_try_execute(void *bc)
{
    if (!g_jit_ctx.initialized || !bc) {
        return NULL;
    }
    
    /* In a real implementation, we would extract bytecode info from bc */
    JITBytecodeInfo info = {0};
    return jit_try_compile(&info);
}

int jit_global_init(void)
{
    return jit_init() ? 0 : -1;
}

void jit_global_cleanup(void)
{
    jit_shutdown();
}
