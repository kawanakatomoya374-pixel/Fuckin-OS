/* C embedding API (SPEC 20): a thin, checked facade over the interpreter. */
#define SP_EMBED_INTERNAL 1
#include "sp.h"
#include "sprint.h"
#include <stdarg.h>
#include <time.h>

_Static_assert(sizeof(SpValue) == 16, "SpValue must be 16 bytes");
_Static_assert(offsetof(SpValue, as) == 8, "SpValue payload offset");

struct HostDiag {
    SpDiagnostic d;
    char kind[24], message[256], file[256];
};
struct SpRef {
    HostRef h;
};
struct SpTracer {
    VM *vm;
};

/* host iterator / native classes get an internal class record */
typedef struct HIC {
    IterClass ic; /* must be first */
    NativeClass nc;
    const SpIterClass *h;
} HIC;
typedef struct HNC {
    NativeClass nc; /* must be first */
    const SpNativeClass *h;
} HNC;

typedef struct HostCfg {
    SpLoaderFn loader;
    void *loader_ud;
    uint64_t max_source_bytes;
    uint32_t max_module_count;
    uint64_t max_runtime_ms;
    uint64_t max_instructions;
    int ran;
    void **classes; /* HIC / HNC records, freed with the VM */
    int nclasses, capclasses;
    Program **progs; /* programs not yet released by the host */
    int nprogs, capprogs;
} HostCfg;

static HostCfg *cfg_of(const VM *vm) { return vm->hcfg; }

static int prog_track(VM *vm, Program *p) {
    HostCfg *hc = cfg_of(vm);
    if (hc->nprogs == hc->capprogs) {
        int nc = hc->capprogs ? hc->capprogs * 2 : 4;
        Program **np = realloc(hc->progs, sizeof *np * (size_t)nc);
        if (!np) return 0;
        hc->progs = np;
        hc->capprogs = nc;
    }
    hc->progs[hc->nprogs++] = p;
    return 1;
}
static void prog_untrack(VM *vm, const Program *p) {
    HostCfg *hc = cfg_of(vm);
    for (int i = 0; i < hc->nprogs; i++)
        if (hc->progs[i] == p) {
            hc->progs[i] = hc->progs[--hc->nprogs];
            return;
        }
}

/* ------------------------------------------------------------------ */
/* error / fatal plumbing                                             */
/* ------------------------------------------------------------------ */
typedef struct Guard {
    jmp_buf jb;
    int armed;
} Guard;

static void guard_enter(VM *vm, Guard *g) {
    memcpy(&g->jb, &vm->fatal_jb, sizeof g->jb);
    g->armed = vm->fatal_armed;
    vm->fatal_armed = 1;
}
static void guard_leave(VM *vm, Guard *g) {
    memcpy(&vm->fatal_jb, &g->jb, sizeof g->jb);
    vm->fatal_armed = g->armed;
}
static SpStatus fatal_status(const VM *vm) {
    switch (vm->fatal_status) {
    case RS_PANIC: return SP_E_PANIC;
    case RS_TIMEOUT: return SP_E_TIMEOUT;
    case RS_INTERRUPT: return SP_E_INTERRUPT;
    case RS_EXIT: return SP_EXIT;
    case RS_OK: case RS_ERROR: case RS_PERM: case RS_MEMORY: default: return SP_E_MEMORY;
    }
}
/* Allocating APIs run under a guard so that a heap-limit failure becomes a
 * status code instead of a longjmp through the host. */
#define GUARD(vm)                                                 \
    Guard sv__;                                                   \
    guard_enter((vm), &sv__);                                     \
    if (setjmp((vm)->fatal_jb) != 0) {                            \
        guard_leave((vm), &sv__);                                 \
        return fatal_status(vm);                                  \
    }
#define UNGUARD(vm) guard_leave((vm), &sv__)

static int pending(const VM *vm) { return vm->pend_code != 0 || vm->pend_obj.type != T_NIL; }

static SP_PRINTF(3, 4) SpStatus rt_fail(VM *vm, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(vm->pend_msg, sizeof vm->pend_msg, fmt, ap);
    va_end(ap);
    vm->pend_code = code;
    vm->pend_obj = v_nil();
    return code == E_PERM ? SP_E_PERMISSION : SP_E_RUNTIME;
}

/* hidden-allocation rule (SPEC 20.5, 20.6): natives without SP_NATIVE_ALLOCATES
 * must not call allocating APIs. */
static SpStatus check_alloc(VM *vm) {
    if ((vm->in_host || vm->in_iter) && !(vm->hflags & SP_NATIVE_ALLOCATES))
        return rt_fail(vm, E_RUNTIME, "allocation inside a native function without SP_NATIVE_ALLOCATES");
    return SP_OK;
}

/* values created while a native runs are GC roots until it returns */
static SpStatus keep(VM *vm, SpValue v) {
    if (!vm->in_host && !vm->in_iter) return SP_OK;
    if (v.type < T_STRING) return SP_OK;
    if (vm->nnroot >= vm->maxnroot) vm_fatal(vm, RS_MEMORY, "native root limit exceeded");
    if (vm->nnroot == vm->capnroot) {
        int nc = vm->capnroot ? vm->capnroot * 2 : 64;
        if (nc > vm->maxnroot) nc = vm->maxnroot;
        SpValue *np = realloc(vm->nroot_stack, sizeof(SpValue) * (size_t)nc);
        if (!np) vm_fatal(vm, RS_MEMORY, "out of memory");
        vm->nroot_stack = np;
        vm->capnroot = nc;
    }
    vm->nroot_stack[vm->nnroot++] = v;
    return SP_OK;
}

/* ------------------------------------------------------------------ */
/* configuration / VM lifetime                                        */
/* ------------------------------------------------------------------ */
void sp_config_init(SpConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->api_major = SP_API_MAJOR;
    cfg->api_minor = SP_API_MINOR;
    Limits l;
    limits_default(&l);
    SpLimits *s = &cfg->limits;
    s->max_source_bytes = 16777216u;
    s->max_heap_bytes = l.max_heap_bytes;
    s->max_string_bytes = l.max_string_bytes;
    s->max_module_count = 1024;
    s->max_call_depth = l.max_call_depth;
    s->max_operand_stack = l.max_operand_stack;
    s->max_handlers = l.max_handlers;
    s->max_defer_records = l.max_defer_records;
    s->max_defer_values = l.max_defer_values;
    s->max_array_length = (uint32_t)l.max_array_length;
    s->max_table_entries = (uint32_t)l.max_table_entries;
    s->max_trace_frames = l.max_trace_frames;
    s->max_native_reentry = 32;
    s->max_native_roots = 256;
    s->stdout_buffer_bytes = l.stdout_buffer_bytes;
}

static int limits_ok(const SpLimits *s) {
    return s->max_source_bytes && s->max_string_bytes && s->max_module_count && s->max_call_depth &&
           s->max_operand_stack && s->max_handlers && s->max_defer_records && s->max_defer_values &&
           s->max_array_length && s->max_table_entries && s->max_trace_frames && s->max_native_reentry &&
           s->max_native_roots && s->stdout_buffer_bytes >= 256;
}

SpStatus sp_vm_new(const SpConfig *cfg, SpVM **out) {
    *out = NULL;
    if (!cfg || cfg->api_major != SP_API_MAJOR || cfg->alloc != NULL || cfg->no_stdlib) return SP_E_CONFIG;
    if (!limits_ok(&cfg->limits)) return SP_E_CONFIG;
    if (cfg->capabilities & ~63u) return SP_E_CONFIG;
    Limits l;
    limits_default(&l);
    const SpLimits *s = &cfg->limits;
    l.max_heap_bytes = s->max_heap_bytes;
    l.max_string_bytes = s->max_string_bytes;
    l.max_call_depth = s->max_call_depth;
    l.max_operand_stack = s->max_operand_stack;
    l.max_handlers = s->max_handlers;
    l.max_defer_records = s->max_defer_records;
    l.max_defer_values = s->max_defer_values;
    l.max_array_length = s->max_array_length;
    l.max_table_entries = s->max_table_entries;
    l.max_trace_frames = s->max_trace_frames;
    l.stdout_buffer_bytes = s->stdout_buffer_bytes;
    l.max_instructions = s->max_instructions;
    VM *vm = vm_new(&l, (int)cfg->capabilities);
    if (!vm) return SP_E_MEMORY;
    HostCfg *hc = calloc(1, sizeof *hc);
    if (!hc) { vm_free(vm); return SP_E_MEMORY; }
    hc->loader = cfg->loader;
    hc->loader_ud = cfg->loader_ud;
    hc->max_source_bytes = s->max_source_bytes;
    hc->max_module_count = s->max_module_count;
    hc->max_runtime_ms = s->max_runtime_ms;
    hc->max_instructions = s->max_instructions;
    vm->hcfg = hc;
    vm->deterministic = cfg->deterministic != 0;
    vm->hwrite = cfg->write;
    vm->hread = cfg->read;
    vm->hio_ud = cfg->io_ud;
    vm->max_reentry = (int)(s->max_native_reentry > 1000 ? 1000 : s->max_native_reentry);
    vm->maxnroot = (int)(s->max_native_roots > 1000000 ? 1000000 : s->max_native_roots);
    for (size_t i = 0; i < cfg->read_root_count; i++)
        if (lib_allow_dir(vm, 0, cfg->read_roots[i]) != 0) { sp_vm_free(vm); return SP_E_CONFIG; }
    for (size_t i = 0; i < cfg->write_root_count; i++)
        if (lib_allow_dir(vm, 1, cfg->write_roots[i]) != 0) { sp_vm_free(vm); return SP_E_CONFIG; }
    *out = vm;
    return SP_OK;
}

void sp_vm_free(SpVM *vm) {
    if (!vm) return;
    HostCfg *hc = cfg_of(vm);
    vm_free(vm); /* runs native-object release callbacks, which may use the class records */
    if (!hc) return;
    for (int i = 0; i < hc->nclasses; i++) free(hc->classes[i]);
    free(hc->classes);
    for (int i = 0; i < hc->nprogs; i++) program_free(hc->progs[i]);
    free(hc->progs);
    free(hc);
}

void sp_interrupt(SpVM *vm) { __atomic_store_n(&vm->interrupted, 1, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------ */
/* programs                                                           */
/* ------------------------------------------------------------------ */
static void set_diag(VM *vm, const Diag *d) {
    free(vm->hdiag);
    vm->hdiag = calloc(1, sizeof *vm->hdiag);
    vm->nhdiag = 0;
    if (!vm->hdiag) return;
    struct HostDiag *h = vm->hdiag;
    (void)snprintf(h->kind, sizeof h->kind, "%s", d->kind ? d->kind : "Error");
    (void)snprintf(h->message, sizeof h->message, "%s", d->msg);
    (void)snprintf(h->file, sizeof h->file, "%s", d->file);
    h->d.kind = h->kind;
    h->d.code = d->code;
    h->d.message = h->message;
    h->d.file = h->file;
    h->d.line = (uint32_t)d->line;
    h->d.column = (uint32_t)d->col;
    h->d.end_line = (uint32_t)d->line;
    h->d.end_column = (uint32_t)d->col;
    h->d.offset = d->offset;
    h->d.end_offset = d->offset;
    vm->nhdiag = 1;
}

static void diag_simple(VM *vm, const char *kind, int code, const char *file, const char *msg) {
    Diag d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.code = code;
    d.line = 1;
    d.col = 1;
    (void)snprintf(d.file, sizeof d.file, "%s", file);
    (void)snprintf(d.msg, sizeof d.msg, "%s", msg);
    set_diag(vm, &d);
}

static Program *frontend_host(Front *F, VM *vm, const char *disp, char *src, size_t len) {
    Program *volatile prog = NULL;
    if (setjmp(F->jb) == 0) prog = load_program_host(F, vm, disp, src, len);
    return prog;
}

static int embed_loader_tramp(void *ud, const char *name, const char **src, size_t *len, const char **disp) {
    VM *vm = ud;
    HostCfg *hc = cfg_of(vm);
    SpStatus st = hc->loader(hc->loader_ud, name, src, len, disp);
    if (st == SP_OK && *len > hc->max_source_bytes) return 1;
    return st == SP_OK ? 0 : 1;
}

SpStatus sp_compile(SpVM *vm, const char *entry_module, SpProgram **out) {
    *out = NULL;
    free(vm->hdiag);
    vm->hdiag = NULL;
    vm->nhdiag = 0;
    HostCfg *hc = cfg_of(vm);
    if (!hc || !hc->loader || !entry_module) return SP_E_CONFIG;
    const char *hs = NULL, *disp = NULL;
    size_t hl = 0;
    SpStatus st = hc->loader(hc->loader_ud, entry_module, &hs, &hl, &disp);
    if (st != SP_OK || !hs) {
        char m[200];
        (void)snprintf(m, sizeof m, "module '%s' not found", entry_module);
        diag_simple(vm, "ImportError", E_IMPORT, entry_module, m);
        return SP_E_IMPORT;
    }
    if (!disp) disp = entry_module;
    if (hl > hc->max_source_bytes) {
        diag_simple(vm, "LexError", E_LEX, disp, "source exceeds max_source_bytes");
        return SP_E_COMPILE;
    }
    char *src = malloc(hl + 1);
    if (!src) return SP_E_MEMORY;
    memcpy(src, hs, hl);
    src[hl] = 0;
    hl = sp_normalize(src, hl);
    size_t bad;
    if (!utf8_validate(src, hl, &bad) || memchr(src, 0, hl)) {
        diag_simple(vm, "LexError", E_LEX, disp, "invalid UTF-8 or NUL byte in source");
        free(src);
        return SP_E_COMPILE;
    }
    vm->hloader = NULL; /* installed below through the wrapper */
    Front F;
    memset(&F, 0, sizeof F);
    F.file = disp;
    (void)snprintf(F.diag.file, sizeof F.diag.file, "%s", disp);
    vm->hloader = embed_loader_tramp;
    vm->hloader_ud = vm;
    Program *prog = frontend_host(&F, vm, disp, src, hl); /* takes ownership of src */
    vm->hloader = NULL;
    if (!prog) {
        set_diag(vm, &F.diag);
        return F.diag.code == E_IMPORT ? SP_E_IMPORT : SP_E_COMPILE;
    }
    if (prog->nmods > (int)hc->max_module_count) {
        program_free(prog);
        diag_simple(vm, "ImportError", E_IMPORT, disp, "too many modules");
        return SP_E_IMPORT;
    }
    char verr[256];
    if (verify_program(prog, verr, sizeof verr)) {
        program_free(prog);
        diag_simple(vm, "InternalError", E_LOAD, disp, verr);
        return SP_E_COMPILE;
    }
    if (!prog_track(vm, prog)) { program_free(prog); return SP_E_MEMORY; }
    *out = prog;
    return SP_OK;
}

SpStatus sp_load_image(SpVM *vm, const uint8_t *data, size_t len, SpProgram **out) {
    *out = NULL;
    char err[256];
    Program *p = image_decode(vm, data, len, err, sizeof err);
    if (!p) {
        diag_simple(vm, "LoadError", E_LOAD, "<image>", err);
        return SP_E_LOAD;
    }
    if (!prog_track(vm, p)) { program_free(p); return SP_E_MEMORY; }
    *out = p;
    return SP_OK;
}

SpStatus sp_save_image(SpVM *vm, const SpProgram *p, int debug_info, uint8_t **data, size_t *len) {
    (void)vm;
    (void)debug_info; /* images always carry the compact debug section */
    *data = image_encode(p, len);
    return *data ? SP_OK : SP_E_MEMORY;
}

void sp_free_buffer(SpVM *vm, void *buf) {
    (void)vm;
    free(buf);
}

static double now_s(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void reset_exec(VM *vm) {
    vm->sp = vm->stack;
    vm->nframes = 0;
    vm->nh = 0;
    vm->ndr = 0;
    vm->ndv = 0;
    vm->nnroot = 0;
    vm->native_depth = 0;
    vm->cur_co = NULL;
}

static SpStatus map_result(VM *vm, RunStatus st) {
    switch (st) {
    case RS_OK: return SP_OK;
    case RS_EXIT: return SP_EXIT;
    case RS_PERM: return SP_E_PERMISSION;
    case RS_MEMORY: return SP_E_MEMORY;
    case RS_PANIC: return SP_E_PANIC;
    case RS_TIMEOUT: return SP_E_TIMEOUT;
    case RS_INTERRUPT: return SP_E_INTERRUPT;
    case RS_ERROR:
    default:
        if (vm->uncaught.type == T_ERROR && AS_ERROR(vm->uncaught)->code == E_PERM) return SP_E_PERMISSION;
        return SP_E_RUNTIME;
    }
}

SpStatus sp_run(SpVM *vm, SpProgram *p, int argc, const char *const *argv) {
    HostCfg *hc = cfg_of(vm);
    if (hc->ran || vm->hrun) return SP_E_CONFIG;
    hc->ran = 1;
    p->hostvm = vm;
    vm->hrun = 1;
    vm->interrupted = 0;
    vm->deadline_s = hc->max_runtime_ms ? now_s() + (double)hc->max_runtime_ms / 1000.0 : 0;
    RunStatus st = vm_run_program(vm, p, argc, (char **)(uintptr_t)argv);
    vm->deadline_s = 0;
    vm->hrun = 0;
    SpStatus r = map_result(vm, st);
    reset_exec(vm);
    return r;
}

void sp_program_free(SpVM *vm, SpProgram *p) {
    if (!p) return;
    if (vm->prog == p) {
        vm->prog = NULL;
        free(vm->slots);
        free(vm->links);
        vm->slots = NULL;
        vm->links = NULL;
    }
    prog_untrack(vm, p);
    program_free(p);
}

size_t sp_diagnostic_count(const SpVM *vm) { return (size_t)vm->nhdiag; }
const SpDiagnostic *sp_diagnostic(const SpVM *vm, size_t i) { return i < (size_t)vm->nhdiag ? &vm->hdiag[i].d : NULL; }
SpValue sp_last_error(SpVM *vm) { return vm->uncaught.type == T_ERROR ? vm->uncaught : v_nil(); }
int sp_exit_code(SpVM *vm) { return vm->exit_code; }

/* ------------------------------------------------------------------ */
/* values                                                             */
/* ------------------------------------------------------------------ */
SpStatus sp_new_string(SpVM *vm, const char *utf8, size_t len, SpValue *out) {
    *out = v_nil();
    SpStatus a = check_alloc(vm);
    if (a != SP_OK) return a;
    size_t bad;
    if (!utf8_validate(utf8, len, &bad)) return rt_fail(vm, E_VALUE, "invalid UTF-8 at byte %zu", bad);
    if (len > vm->lim.max_string_bytes) return SP_E_MEMORY;
    GUARD(vm)
    SpValue v = v_obj(T_STRING, vm_new_string(vm, utf8, len));
    UNGUARD(vm);
    (void)keep(vm, v);
    *out = v;
    return SP_OK;
}

const char *sp_string_data(SpValue s, size_t *len) {
    if (s.type != T_STRING) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = AS_STR(s)->len;
    return AS_STR(s)->data;
}

SpStatus sp_new_array(SpVM *vm, size_t capacity, SpValue *out) {
    *out = v_nil();
    SpStatus a = check_alloc(vm);
    if (a != SP_OK) return a;
    if (capacity > vm->lim.max_array_length) return rt_fail(vm, E_VALUE, "array capacity exceeds max_array_length");
    GUARD(vm)
    SpValue v = v_obj(T_ARRAY, vm_new_array(vm, (int64_t)capacity));
    UNGUARD(vm);
    (void)keep(vm, v);
    *out = v;
    return SP_OK;
}

size_t sp_array_len(SpValue a) { return a.type == T_ARRAY ? (size_t)AS_ARRAY(a)->len : 0; }

static SpStatus need(VM *vm, SpValue v, int type, const char *what) {
    if (v.type != type) return rt_fail(vm, E_TYPE, "%s expected, got %s", what, type_name(v));
    return SP_OK;
}

SpStatus sp_array_get(SpVM *vm, SpValue a, int64_t i, SpValue *out) {
    *out = v_nil();
    SpStatus s = need(vm, a, T_ARRAY, "array");
    if (s != SP_OK) return s;
    SArray *arr = AS_ARRAY(a);
    if (i < 0 || i >= arr->len) return rt_fail(vm, E_INDEX, "array index %lld out of range (length %lld)", (long long)i, (long long)arr->len);
    *out = arr->items[i];
    (void)keep(vm, *out);
    return SP_OK;
}

SpStatus sp_array_set(SpVM *vm, SpValue a, int64_t i, SpValue v) {
    SpStatus s = need(vm, a, T_ARRAY, "array");
    if (s != SP_OK) return s;
    SArray *arr = AS_ARRAY(a);
    if (i < 0 || i >= arr->len) return rt_fail(vm, E_INDEX, "array index %lld out of range (length %lld)", (long long)i, (long long)arr->len);
    arr->items[i] = v;
    return SP_OK;
}

SpStatus sp_array_push(SpVM *vm, SpValue a, SpValue v) {
    SpStatus s = need(vm, a, T_ARRAY, "array");
    if (s != SP_OK) return s;
    s = check_alloc(vm);
    if (s != SP_OK) return s;
    if ((uint64_t)AS_ARRAY(a)->len >= vm->lim.max_array_length) return rt_fail(vm, E_MEMORY, "array length limit exceeded");
    GUARD(vm)
    (void)array_push(vm, AS_ARRAY(a), v);
    UNGUARD(vm);
    return SP_OK;
}

SpStatus sp_new_table(SpVM *vm, size_t capacity, SpValue *out) {
    *out = v_nil();
    SpStatus a = check_alloc(vm);
    if (a != SP_OK) return a;
    if (capacity > vm->lim.max_table_entries) return rt_fail(vm, E_VALUE, "table capacity exceeds max_table_entries");
    GUARD(vm)
    SpValue v = v_obj(T_TABLE, vm_new_table(vm, (int64_t)capacity));
    UNGUARD(vm);
    (void)keep(vm, v);
    *out = v;
    return SP_OK;
}

SpStatus sp_table_get(SpVM *vm, SpValue t, SpValue key, SpValue *out, int *found) {
    *out = v_nil();
    if (found) *found = 0;
    SpStatus s = need(vm, t, T_TABLE, "table");
    if (s != SP_OK) return s;
    if (table_get(vm, AS_TABLE(t), key, out, found)) return pending(vm) ? SP_E_RUNTIME : rt_fail(vm, E_TYPE, "invalid table key");
    (void)keep(vm, *out);
    return SP_OK;
}

SpStatus sp_table_set(SpVM *vm, SpValue t, SpValue key, SpValue v) {
    SpStatus s = need(vm, t, T_TABLE, "table");
    if (s != SP_OK) return s;
    s = check_alloc(vm);
    if (s != SP_OK) return s;
    GUARD(vm)
    int bad = table_set(vm, AS_TABLE(t), key, v);
    UNGUARD(vm);
    return bad ? (pending(vm) ? SP_E_RUNTIME : rt_fail(vm, E_TYPE, "invalid table key")) : SP_OK;
}

int sp_table_next(SpValue t, size_t *cursor, SpValue *key, SpValue *val) {
    if (t.type != T_TABLE) return 0;
    int64_t n = table_next(AS_TABLE(t), (int64_t)*cursor, key, val);
    if (n < 0) return 0;
    *cursor = (size_t)n;
    return 1;
}

SpStatus sp_get_member(SpVM *vm, SpValue obj, const char *name, SpValue *out) {
    *out = v_nil();
    size_t nl = strlen(name);
    switch (obj.type) {
    case T_TABLE:
        (void)table_get_str(AS_TABLE(obj), name, nl, out);
        break;
    case T_MODULE:
        if (!table_get_str(AS_MODULE(obj)->exports, name, nl, out))
            return rt_fail(vm, E_NAME, "module '%s' has no export '%s'", AS_MODULE(obj)->name->data, name);
        break;
    case T_INSTANCE: {
        SInstance *in = AS_INSTANCE(obj);
        SpValue slot;
        if (table_get_str(in->cls->fields, name, nl, &slot)) *out = in->slots[slot.as.i];
        else if (!table_get_str(in->cls->methods, name, nl, out))
            return rt_fail(vm, E_NAME, "'%s' has no member '%s'", in->cls->name->data, name);
        break;
    }
    case T_ERROR: {
        SError *e = AS_ERROR(obj);
        if (strcmp(name, "kind") == 0) *out = v_obj(T_STRING, e->kind);
        else if (strcmp(name, "message") == 0) *out = v_obj(T_STRING, e->message);
        else if (strcmp(name, "code") == 0) *out = v_int(e->code);
        else if (strcmp(name, "file") == 0) *out = v_obj(T_STRING, e->file);
        else if (strcmp(name, "line") == 0) *out = v_int(e->line);
        else if (strcmp(name, "column") == 0) *out = v_int(e->col);
        else if (strcmp(name, "trace") == 0) *out = v_obj(T_STRING, e->trace);
        else return rt_fail(vm, E_NAME, "error has no member '%s'", name);
        break;
    }
    case T_NIL: case T_BOOL: case T_INT: case T_FLOAT: case T_STRING: case T_ARRAY: case T_FUNCTION:
    case T_CLASS: case T_ITERATOR: case T_COROUTINE: case T_NATIVE: case T_CELL: case T_UNINIT: default:
        return rt_fail(vm, E_TYPE, "'%s' value has no members", type_name(obj));
    }
    (void)keep(vm, *out);
    return SP_OK;
}

SpStatus sp_call(SpVM *vm, SpValue callee, int argc, const SpValue *argv, SpValue *ret) {
    *ret = v_nil();
    if (vm->in_iter) return rt_fail(vm, E_RUNTIME, "a native iterator must not call Sprint code");
    if (vm->in_host && !(vm->hflags & SP_NATIVE_CALLS_BACK))
        return rt_fail(vm, E_RUNTIME, "sp_call inside a native function without SP_NATIVE_CALLS_BACK");
    if (!vm->prog) return SP_E_CONFIG;
    int top = !vm->hrun;
    HostCfg *hc = cfg_of(vm);
    GUARD(vm)
    if (top) {
        vm->hrun = 1;
        vm->interrupted = 0;
        vm->ibank = hc->max_instructions ? hc->max_instructions : UINT64_MAX;
        vm->ileft = 0;
        vm->deadline_s = hc->max_runtime_ms ? now_s() + (double)hc->max_runtime_ms / 1000.0 : 0;
    }
    SpValue r = v_nil();
    int bad = vm_call(vm, callee, argc, argv, &r);
    if (top) {
        vm->hrun = 0;
        vm->deadline_s = 0;
    }
    UNGUARD(vm);
    if (bad) {
        if (top) {
            /* a top-level failure is reported through sp_last_error */
            if (vm->pend_obj.type != T_NIL) vm->uncaught = vm->pend_obj;
            else vm->uncaught = v_obj(T_ERROR, vm_new_error(vm, vm->pend_code, err_kind_name(vm->pend_code),
                                                           strlen(err_kind_name(vm->pend_code)), vm->pend_msg, strlen(vm->pend_msg)));
            vm->pend_obj = v_nil();
            vm->pend_code = 0;
            reset_exec(vm);
        }
        return SP_E_RUNTIME;
    }
    (void)keep(vm, r);
    *ret = r;
    return SP_OK;
}

SpStatus sp_throw(SpVM *vm, const char *kind, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int code = err_kind_code(kind, strlen(kind));
    if (code) return rt_fail(vm, code, "%s", msg);
    SError *e = vm_new_error(vm, E_USER, kind, strlen(kind), msg, strlen(msg));
    vm->pend_obj = v_obj(T_ERROR, e);
    vm->pend_code = 0;
    return SP_E_RUNTIME;
}

SpStatus sp_panic(SpVM *vm, const char *message) {
    (void)snprintf(vm->fatal_msg, sizeof vm->fatal_msg, "%s", message);
    vm->fatal_status = RS_PANIC;
    return SP_E_PANIC;
}

const char *sp_type_name(SpValue v) { return type_name(v); }

SpStatus sp_ref(SpVM *vm, SpValue v, SpRef **out) {
    *out = NULL;
    SpRef *r = calloc(1, sizeof *r);
    if (!r) return SP_E_MEMORY;
    r->h.v = v;
    r->h.next = vm->hrefs;
    if (vm->hrefs) vm->hrefs->prev = &r->h;
    vm->hrefs = &r->h;
    *out = r;
    return SP_OK;
}
SpValue sp_ref_get(const SpRef *r) { return r->h.v; }
void sp_unref(SpVM *vm, SpRef *r) {
    if (!r) return;
    if (r->h.prev) r->h.prev->next = r->h.next;
    else vm->hrefs = r->h.next;
    if (r->h.next) r->h.next->prev = r->h.prev;
    free(r);
}

/* ------------------------------------------------------------------ */
/* native functions and modules                                       */
/* ------------------------------------------------------------------ */
static const SpNativeDesc *find_hd(const VM *vm, const NativeDesc *nd) {
    for (int i = 0; i < vm->nhmods; i++) {
        const HostMod *m = vm->hmods[i];
        if (nd >= m->fns && nd < m->fns + m->n) return (const SpNativeDesc *)m->hd + (nd - m->fns);
    }
    return NULL;
}

static int host_call(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    const SpNativeDesc *hd = NULL;
    if (argv[-1].type == T_FUNCTION && AS_FUNC(argv[-1])->nat) hd = find_hd(vm, AS_FUNC(argv[-1])->nat);
    if (!hd) return vm_set_error(vm, E_RUNTIME, "host function lost its descriptor");
    if (hd->capability && (vm->caps & (int)hd->capability) != (int)hd->capability)
        return vm_set_error(vm, E_PERM, "%s requires a capability that was not granted", hd->name);
    unsigned sv_flags = vm->hflags;
    int sv_host = vm->in_host, sv_iter = vm->in_iter, sv_root = vm->nnroot;
    vm->hflags = hd->flags;
    vm->in_host = 1;
    vm->in_iter = 0;
    vm->pend_code = 0;
    SpValue r = v_nil();
    SpStatus st = hd->fn(vm, argc, argv, &r);
    vm->hflags = sv_flags;
    vm->in_host = sv_host;
    vm->in_iter = sv_iter;
    vm->nnroot = sv_root;
    switch (st) {
    case SP_OK:
        vm->pend_code = 0;
        *ret = r;
        return 0;
    case SP_E_RUNTIME: case SP_E_PERMISSION: case SP_E_IMPORT:
        if (pending(vm)) return 1;
        return vm_set_error(vm, st == SP_E_PERMISSION ? E_PERM : E_RUNTIME, "%s() failed without reporting an error", hd->name);
    case SP_E_MEMORY: vm_fatal(vm, RS_MEMORY, "%s", vm->fatal_msg[0] ? vm->fatal_msg : "out of memory");
    case SP_E_PANIC: vm_fatal(vm, RS_PANIC, "%s", vm->fatal_msg);
    case SP_E_TIMEOUT: vm_fatal(vm, RS_TIMEOUT, "%s", vm->fatal_msg);
    case SP_E_INTERRUPT: vm_fatal(vm, RS_INTERRUPT, "%s", vm->fatal_msg);
    case SP_EXIT: vm_fatal(vm, RS_EXIT, "exit");
    case SP_E_CONFIG: case SP_E_COMPILE: case SP_E_LOAD: default:
        return vm_set_error(vm, E_RUNTIME, "%s() returned an invalid status (%d)", hd->name, (int)st);
    }
}

SpStatus sp_register_module(SpVM *vm, const char *dotted_name, const SpNativeDesc *fns, size_t count) {
    if (vm->prog || cfg_of(vm)->ran) return SP_E_CONFIG;
    size_t nl = strlen(dotted_name);
    if (nl == 0 || nl > 128 || lib_find_module(vm, dotted_name, nl)) return SP_E_CONFIG;
    for (size_t i = 0; i < nl; i++) {
        char c = dotted_name[i];
        int ok = c == '_' || c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && c >= '0' && c <= '9');
        if (!ok) return SP_E_CONFIG;
    }
    for (size_t i = 0; i < count; i++) {
        if (!fns[i].name || !fns[i].fn || fns[i].min_arity > 255) return SP_E_CONFIG;
        if (fns[i].max_arity != SP_VARIADIC && fns[i].max_arity < fns[i].min_arity) return SP_E_CONFIG;
        for (size_t j = 0; j < i; j++)
            if (strcmp(fns[i].name, fns[j].name) == 0) return SP_E_CONFIG;
    }
    HostMod **nm = realloc(vm->hmods, sizeof(HostMod *) * (size_t)(vm->nhmods + 1));
    if (!nm) return SP_E_MEMORY;
    vm->hmods = nm;
    HostMod *m = calloc(1, sizeof *m);
    if (!m) return SP_E_MEMORY;
    m->name = malloc(nl + 1);
    m->fns = calloc(count + 1, sizeof(NativeDesc));
    SpNativeDesc *hd = calloc(count + 1, sizeof(SpNativeDesc));
    if (!m->name || !m->fns || !hd) {
        free(m->name);
        free(m->fns);
        free(hd);
        free(m);
        return SP_E_MEMORY;
    }
    memcpy(m->name, dotted_name, nl + 1);
    for (size_t i = 0; i < count; i++) {
        hd[i] = fns[i];
        m->fns[i].name = fns[i].name;
        m->fns[i].min_args = fns[i].min_arity;
        m->fns[i].max_args = fns[i].max_arity == SP_VARIADIC ? -1 : fns[i].max_arity;
        m->fns[i].fn = host_call;
    }
    m->hd = hd;
    m->n = (int)count;
    m->md.name = m->name;
    m->md.fns = m->fns;
    m->md.consts = &m->none;
    vm->hmods[vm->nhmods] = m;
    vm->nhmods++;
    return SP_OK;
}

/* ------------------------------------------------------------------ */
/* tracing, iterators, native objects                                 */
/* ------------------------------------------------------------------ */
void sp_trace_value(SpTracer *t, SpValue v) { gc_mark(t->vm, v); }

static int add_class(HostCfg *hc, void *rec) {
    if (hc->nclasses == hc->capclasses) {
        int nc = hc->capclasses ? hc->capclasses * 2 : 8;
        void **np = realloc(hc->classes, sizeof(void *) * (size_t)nc);
        if (!np) return 0;
        hc->classes = np;
        hc->capclasses = nc;
    }
    hc->classes[hc->nclasses++] = rec;
    return 1;
}

/* iterator state lives in a hidden native object held by the iterator */
static const HIC *hic_of_nc(const NativeClass *nc) {
    return (const HIC *)(const void *)((const char *)nc - offsetof(HIC, nc));
}
static void hic_trace(VM *vm, SNativeObj *self) {
    const HIC *c = hic_of_nc(self->cls);
    if (c->h->trace) {
        SpTracer t = {vm};
        c->h->trace(self->data, &t);
    }
}
static int hic_next(VM *vm, SIterator *it, SpValue out[2]) {
    const HIC *c = (const HIC *)(const void *)it->cls;
    SNativeObj *st = AS_NATIVE(it->held);
    unsigned sv_flags = vm->hflags;
    int sv_host = vm->in_host, sv_iter = vm->in_iter, sv_root = vm->nnroot;
    vm->hflags = c->h->allocates ? SP_NATIVE_ALLOCATES : 0;
    vm->in_host = 0;
    vm->in_iter = 1;
    vm->pend_code = 0;
    out[0] = v_nil();
    out[1] = v_nil();
    SpIterResult r = c->h->next(vm, st->data, out);
    vm->hflags = sv_flags;
    vm->in_host = sv_host;
    vm->in_iter = sv_iter;
    vm->nnroot = sv_root;
    if (r == SP_ITER_ITEM) return 0;
    if (r == SP_ITER_END) return 1;
    if (!pending(vm)) (void)vm_set_error(vm, E_RUNTIME, "native iterator failed without reporting an error");
    return 2;
}

__attribute__((noinline)) static const HIC *class_for_iter(VM *vm, const SpIterClass *cls) {
    HostCfg *hc = cfg_of(vm);
    for (int i = 0; i < hc->nclasses; i++) {
        const HIC *cand = hc->classes[i];
        if (cand->ic.next == hic_next && cand->h == cls) return cand;
    }
    HIC *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (!add_class(hc, c)) { free(c); return NULL; }
    c->h = cls;
    c->ic.kind = cls->kind ? cls->kind : "native";
    c->ic.arity = cls->arity;
    c->ic.next = hic_next;
    c->nc.name = "iterstate";
    c->nc.trace = hic_trace;
    return c;
}

SpStatus sp_new_iterator(SpVM *vm, const SpIterClass *cls, const void *init_state, SpValue *out) {
    *out = v_nil();
    SpStatus a = check_alloc(vm);
    if (a != SP_OK) return a;
    if (cls->abi != SP_ITER_ABI || cls->arity < 1 || cls->arity > 2 || !cls->next) return SP_E_CONFIG;
    const HIC *c = class_for_iter(vm, cls);
    if (!c) return SP_E_MEMORY;
    GUARD(vm)
    SNativeObj *st = vm_new_native_obj(vm, &c->nc, cls->state_size);
    if (init_state) memcpy(st->data, init_state, cls->state_size);
    SIterator *it = vm_new_iterator(vm, &c->ic, v_obj(T_NATIVE, st), 0, 0, 0);
    UNGUARD(vm);
    SpValue v = v_obj(T_ITERATOR, it);
    (void)keep(vm, v);
    *out = v;
    return SP_OK;
}

SpIterResult sp_iter_next(SpVM *vm, SpValue it, SpValue out[2]) {
    if (it.type != T_ITERATOR) {
        (void)rt_fail(vm, E_TYPE, "iterator expected, got %s", type_name(it));
        return SP_ITER_FAIL;
    }
    SIterator *i = AS_ITER(it);
    if (i->done) return SP_ITER_END;
    int r = i->cls->next(vm, i, out);
    if (r == 0) {
        (void)keep(vm, out[0]);
        if (i->cls->arity == 2) (void)keep(vm, out[1]);
        return SP_ITER_ITEM;
    }
    i->done = 1;
    return r == 1 ? SP_ITER_END : SP_ITER_FAIL;
}

#define HNC_HDR 16
static void hnc_release(void *data) {
    const HNC *c = *(HNC *const *)data;
    if (c->h->release) c->h->release((char *)data + HNC_HDR);
}
static void hnc_trace(VM *vm, SNativeObj *self) {
    const HNC *c = (const HNC *)(const void *)self->cls;
    if (c->h->trace) {
        SpTracer t = {vm};
        c->h->trace((char *)self->data + HNC_HDR, &t);
    }
}

__attribute__((noinline)) static const HNC *class_for_native(VM *vm, const SpNativeClass *cls) {
    HostCfg *hc = cfg_of(vm);
    for (int i = 0; i < hc->nclasses; i++) {
        const HNC *cand = hc->classes[i];
        if (cand->nc.release == hnc_release && cand->h == cls) return cand;
    }
    HNC *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (!add_class(hc, c)) { free(c); return NULL; }
    c->h = cls;
    c->nc.name = cls->name;
    c->nc.release = hnc_release;
    c->nc.trace = hnc_trace;
    return c;
}

SpStatus sp_new_native(SpVM *vm, const SpNativeClass *cls, SpValue *out, void **self) {
    *out = v_nil();
    if (self) *self = NULL;
    SpStatus a = check_alloc(vm);
    if (a != SP_OK) return a;
    if (cls->abi != SP_NATIVE_ABI || !cls->name) return SP_E_CONFIG;
    const HNC *c = class_for_native(vm, cls);
    if (!c) return SP_E_MEMORY;
    GUARD(vm)
    SNativeObj *o = vm_new_native_obj(vm, &c->nc, cls->size + HNC_HDR);
    UNGUARD(vm);
    memcpy(o->data, &c, sizeof c);
    SpValue v = v_obj(T_NATIVE, o);
    (void)keep(vm, v);
    *out = v;
    if (self) *self = (char *)o->data + HNC_HDR;
    return SP_OK;
}

void *sp_native_data(SpValue v, const SpNativeClass *cls) {
    if (v.type != T_NATIVE) return NULL;
    SNativeObj *o = AS_NATIVE(v);
    if (o->cls->release != hnc_release) return NULL;
    const HNC *c = (const HNC *)(const void *)o->cls;
    return c->h == cls ? (char *)o->data + HNC_HDR : NULL;
}
