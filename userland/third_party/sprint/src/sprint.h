/* Sprint C embedding API (SPEC section 20, C API major 2).
 *
 * This is the only public header. Link with libsprint.a (-lm).
 * Every function takes an SpVM that must be used from one thread at a time
 * (except sp_interrupt, which is thread- and signal-safe).
 */
#ifndef SPRINT_H
#define SPRINT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_API_MAJOR 2
#define SP_API_MINOR 0

#ifdef SP_EMBED_INTERNAL /* building libsprint itself: reuse the internal types */
typedef struct VM SpVM;
typedef struct Obj SpObj;
typedef struct Program SpProgram;
#else
typedef struct SpVM SpVM;
typedef struct SpObj SpObj;
typedef struct SpProgram SpProgram;
#endif
typedef struct SpRef SpRef;

enum {
    SP_T_NIL = 0, SP_T_BOOL = 1, SP_T_INT = 2, SP_T_FLOAT = 3,
    SP_T_STRING = 4, SP_T_ARRAY = 5, SP_T_TABLE = 6, SP_T_FUNCTION = 7,
    SP_T_CLASS = 8, SP_T_INSTANCE = 9, SP_T_MODULE = 10, SP_T_ITERATOR = 11,
    SP_T_COROUTINE = 12, SP_T_ERROR = 13, SP_T_NATIVE = 14
};

#ifndef SP_EMBED_INTERNAL
typedef struct SpValue {
    uint8_t type;
    uint8_t _pad[7];
    union { int64_t i; double f; int b; SpObj *o; } as;
} SpValue; /* sizeof == 16 */
#endif

typedef enum SpStatus {
    SP_OK = 0,
    SP_E_RUNTIME = 1,     /* unhandled catchable error: see sp_last_error() */
    SP_E_CONFIG = 2,
    SP_E_PERMISSION = 3,  /* unhandled PermissionError */
    SP_E_IMPORT = 4,
    SP_E_COMPILE = 12,    /* see sp_diagnostic*() */
    SP_E_LOAD = 13,
    SP_E_MEMORY = 14,
    SP_E_PANIC = 101,
    SP_E_TIMEOUT = 124,
    SP_E_INTERRUPT = 130,
    SP_EXIT = 256         /* sys.exit: see sp_exit_code() */
} SpStatus;

enum {
    SP_CAP_STDIO_READ = 1u << 0, SP_CAP_STDIO_WRITE = 1u << 1,
    SP_CAP_FS_READ    = 1u << 2, SP_CAP_FS_WRITE    = 1u << 3,
    SP_CAP_CLOCK      = 1u << 4, SP_CAP_RANDOM      = 1u << 5,
    SP_CAP_FFI        = 1u << 6 /* foreign function interface (SPEC 15.15); never with deterministic mode */
};

/* new_size == 0 frees; ptr == NULL allocates. Reserved: this implementation
 * accepts only alloc == NULL (the C library allocator); anything else makes
 * sp_vm_new return SP_E_CONFIG. */
typedef void *(*SpAllocFn)(void *ud, void *ptr, size_t old_size, size_t new_size);
/* stream: 1 = stdout, 2 = stderr. Returns the number of bytes written. */
typedef size_t (*SpWriteFn)(void *ud, int stream, const char *data, size_t len);
/* Reads at most cap bytes. 0 = EOF, negative = error (IOError). */
typedef ptrdiff_t (*SpReadFn)(void *ud, char *buf, size_t cap);
/* Resolves a dotted module name to source text. Return SP_OK, or SP_E_IMPORT
 * when the module does not exist. *src must stay valid until sp_compile returns. */
typedef SpStatus (*SpLoaderFn)(void *ud, const char *dotted_name, const char **src,
                               size_t *len, const char **display_path);

typedef struct SpLimits {
    uint64_t max_source_bytes, max_heap_bytes, max_string_bytes;
    uint32_t max_module_count, max_call_depth, max_operand_stack, max_handlers;
    uint32_t max_defer_records, max_defer_values;
    uint32_t max_array_length, max_table_entries, max_trace_frames;
    uint32_t max_native_reentry, max_native_roots, stdout_buffer_bytes;
    uint64_t max_runtime_ms, max_instructions;
} SpLimits;

typedef struct SpConfig {
    uint16_t api_major, api_minor; /* set by sp_config_init */
    SpAllocFn alloc; void *alloc_ud;
    SpLimits limits;
    uint32_t capabilities;
    const char *const *read_roots;  size_t read_root_count;
    const char *const *write_roots; size_t write_root_count;
    SpWriteFn write; SpReadFn read; void *io_ud;
    SpLoaderFn loader; void *loader_ud;
    int deterministic;
    int no_stdlib; /* reserved (must be 0) */
} SpConfig;

void sp_config_init(SpConfig *cfg); /* defaults; no capabilities */
SpStatus sp_vm_new(const SpConfig *cfg, SpVM **out);
void sp_vm_free(SpVM *vm);
void sp_interrupt(SpVM *vm); /* thread-safe; run stops with SP_E_INTERRUPT */

/* ---- programs ---- */
SpStatus sp_compile(SpVM *vm, const char *entry_module, SpProgram **out);
SpStatus sp_load_image(SpVM *vm, const uint8_t *data, size_t len, SpProgram **out);
SpStatus sp_save_image(SpVM *vm, const SpProgram *p, int debug_info, uint8_t **data, size_t *len);
void sp_free_buffer(SpVM *vm, void *buf);
SpStatus sp_run(SpVM *vm, SpProgram *p, int argc, const char *const *argv);
void sp_program_free(SpVM *vm, SpProgram *p);

typedef struct SpDiagnostic {
    const char *kind; int code; const char *message; const char *file;
    uint32_t line, column, end_line, end_column;
    uint64_t offset, end_offset;
} SpDiagnostic;
size_t sp_diagnostic_count(const SpVM *vm);
const SpDiagnostic *sp_diagnostic(const SpVM *vm, size_t i);
SpValue sp_last_error(SpVM *vm);
int sp_exit_code(SpVM *vm);

/* ---- values ---- */
static inline SpValue sp_nil(void) { SpValue v; memset(&v, 0, sizeof v); return v; }
static inline SpValue sp_bool(int b) { SpValue v = sp_nil(); v.type = SP_T_BOOL; v.as.b = b != 0; return v; }
static inline SpValue sp_int(int64_t i) { SpValue v = sp_nil(); v.type = SP_T_INT; v.as.i = i; return v; }
static inline SpValue sp_float(double f) { SpValue v = sp_nil(); v.type = SP_T_FLOAT; v.as.f = f; return v; }

SpStatus sp_new_string(SpVM *vm, const char *utf8, size_t len, SpValue *out);
const char *sp_string_data(SpValue s, size_t *len); /* NUL-terminated */
SpStatus sp_new_array(SpVM *vm, size_t capacity, SpValue *out);
size_t sp_array_len(SpValue a);
SpStatus sp_array_get(SpVM *vm, SpValue a, int64_t i, SpValue *out);
SpStatus sp_array_set(SpVM *vm, SpValue a, int64_t i, SpValue v);
SpStatus sp_array_push(SpVM *vm, SpValue a, SpValue v);
SpStatus sp_new_table(SpVM *vm, size_t capacity, SpValue *out);
SpStatus sp_table_get(SpVM *vm, SpValue t, SpValue key, SpValue *out, int *found);
SpStatus sp_table_set(SpVM *vm, SpValue t, SpValue key, SpValue v);
int sp_table_next(SpValue t, size_t *cursor, SpValue *key, SpValue *val);
SpStatus sp_get_member(SpVM *vm, SpValue obj, const char *name, SpValue *out);
SpStatus sp_call(SpVM *vm, SpValue callee, int argc, const SpValue *argv, SpValue *ret);
SpStatus sp_throw(SpVM *vm, const char *kind, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
SpStatus sp_panic(SpVM *vm, const char *message);
const char *sp_type_name(SpValue v);

SpStatus sp_ref(SpVM *vm, SpValue v, SpRef **out);
SpValue sp_ref_get(const SpRef *r);
void sp_unref(SpVM *vm, SpRef *r);

/* ---- native functions and modules ---- */
typedef SpStatus (*SpNativeFn)(SpVM *vm, int argc, const SpValue *argv, SpValue *ret);
#define SP_VARIADIC 0xFFFFu
enum { SP_NATIVE_ALLOCATES = 1u << 0, SP_NATIVE_CALLS_BACK = 1u << 1 };

typedef struct SpNativeDesc {
    const char *name;
    uint16_t min_arity, max_arity;
    uint32_t capability;
    uint32_t flags;
    SpNativeFn fn;
} SpNativeDesc;

SpStatus sp_register_module(SpVM *vm, const char *dotted_name, const SpNativeDesc *fns, size_t count);

/* ---- native iterators (SPEC 20.6) ---- */
typedef enum SpIterResult { SP_ITER_ITEM = 0, SP_ITER_END = 1, SP_ITER_FAIL = 2 } SpIterResult;
typedef struct SpTracer SpTracer;
void sp_trace_value(SpTracer *t, SpValue v);

typedef struct SpIterClass {
    uint32_t abi; /* SP_ITER_ABI */
    const char *kind;
    uint8_t arity;     /* 1 or 2 */
    uint8_t allocates;
    size_t state_size;
    SpIterResult (*next)(SpVM *vm, void *state, SpValue out[2]);
    void (*trace)(void *state, SpTracer *t);
} SpIterClass;
#define SP_ITER_ABI 2u
SpStatus sp_new_iterator(SpVM *vm, const SpIterClass *cls, const void *init_state, SpValue *out);
SpIterResult sp_iter_next(SpVM *vm, SpValue it, SpValue out[2]);

/* ---- native objects (SPEC 20.7) ---- */
typedef struct SpNativeClass {
    uint32_t abi; /* SP_NATIVE_ABI */
    const char *name;
    size_t size;
    void (*trace)(void *self, SpTracer *t);
    void (*release)(void *self);
} SpNativeClass;
#define SP_NATIVE_ABI 2u
SpStatus sp_new_native(SpVM *vm, const SpNativeClass *cls, SpValue *out, void **self);
void *sp_native_data(SpValue v, const SpNativeClass *cls);

#ifdef __cplusplus
}
#endif
#endif /* SPRINT_H */
