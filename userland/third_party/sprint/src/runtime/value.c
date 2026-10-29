/* Values, objects, strings, arrays, equality and ordering. */
#include "value_int.h"

/* ---------------------------------------------------------------- */
/* error names                                                       */
/* ---------------------------------------------------------------- */
static const struct { int code; const char *name; } kind_names[] = {
    {E_TYPE, "TypeError"}, {E_NAME, "NameError"}, {E_INDEX, "IndexError"},
    {E_KEY, "KeyError"}, {E_MATCH, "MatchError"}, {E_ARITH, "ArithmeticError"},
    {E_IMPORT, "ImportError"}, {E_IO, "IOError"}, {E_CONST, "ConstError"},
    {E_ASSERT, "AssertionError"}, {E_RUNTIME, "RuntimeError"}, {E_PARSE, "ParseError"},
    {E_PERM, "PermissionError"}, {E_LEX, "LexError"}, {E_VALUE, "ValueError"},
    {E_OVERRIDE, "OverrideError"}, {E_LOAD, "LoadError"}, {E_MEMORY, "MemoryError"},
    {E_TIMEOUT, "TimeoutError"}, {E_PANIC, "Panic"},
};

const char *err_kind_name(int code) {
    for (size_t i = 0; i < sizeof kind_names / sizeof kind_names[0]; i++)
        if (kind_names[i].code == code) return kind_names[i].name;
    return "Error";
}

/* kinds the program may construct with error(kind, ...) -- catchable runtime kinds only */
int err_kind_code(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof kind_names / sizeof kind_names[0]; i++) {
        if (strlen(kind_names[i].name) == len && memcmp(kind_names[i].name, name, len) == 0) {
            switch (kind_names[i].code) {
            case E_TYPE: case E_NAME: case E_INDEX: case E_KEY: case E_MATCH: case E_ARITH:
            case E_IO: case E_ASSERT: case E_RUNTIME: case E_PERM: case E_VALUE:
                return kind_names[i].code;
            default:
                return -1; /* diagnostic / fatal kind: not constructible */
            }
        }
    }
    return 0; /* user-defined kind */
}

/* ---------------------------------------------------------------- */
/* memory                                                            */
/* ---------------------------------------------------------------- */
void limits_default(Limits *l) {
    memset(l, 0, sizeof *l);
    l->max_heap_bytes = 268435456ull;
    l->max_call_depth = 1024;
    l->max_operand_stack = 65536;
    l->max_handlers = 1024;
    l->max_trace_frames = 64;
    l->stdout_buffer_bytes = 65536;
    l->max_string_bytes = 67108864ull;
    l->max_array_length = 16777216ull;
    l->max_table_entries = 16777216ull;
    l->max_instructions = 0;
    l->max_defer_records = 1024;
    l->max_defer_values = 8192;
}

void *vm_mem(VM *vm, void *p, size_t old, size_t nw) {
    if (nw == 0) {
        free(p);
        vm->bytes -= old;
        return NULL;
    }
    if (nw > old && vm->bytes + (nw - old) > vm->lim.max_heap_bytes)
        vm_fatal(vm, RS_MEMORY, "heap limit exceeded (%llu bytes)", (unsigned long long)vm->lim.max_heap_bytes);
    void *q = realloc(p, nw);
    if (!q) vm_fatal(vm, RS_MEMORY, "out of memory");
    vm->bytes = vm->bytes - old + nw;
    return q;
}

/* Small objects come from per-VM size-class pools (no malloc/free per object).
 * Chunks are only returned to the system in vm_free. */
enum { POOL_MAX = SP_POOL_MAX, POOL_CHUNK = 65536 };

static void *pool_alloc(VM *vm, size_t size) {
    size_t c = (size + 15) >> 4;
    void *p = vm->pool_free[c];
    if (p) { vm->pool_free[c] = *(void **)p; return p; }
    size_t csz = c << 4;
    if ((size_t)(vm->pool_end - vm->pool_cur) < csz) {
        char *ch = malloc(POOL_CHUNK);
        if (!ch) vm_fatal(vm, RS_MEMORY, "out of memory");
        if (vm->npool_chunks == vm->cappool_chunks) {
            int nc = vm->cappool_chunks ? vm->cappool_chunks * 2 : 16;
            void **np = realloc(vm->pool_chunks, (size_t)nc * sizeof *np);
            if (!np) { free(ch); vm_fatal(vm, RS_MEMORY, "out of memory"); }
            vm->pool_chunks = np;
            vm->cappool_chunks = nc;
        }
        vm->pool_chunks[vm->npool_chunks++] = ch;
        vm->pool_cur = ch;
        vm->pool_end = ch + POOL_CHUNK;
    }
    p = vm->pool_cur;
    vm->pool_cur += csz;
    return p;
}

void *new_obj(VM *vm, size_t size, int type, int with_id) {
    Obj *o;
    if (size <= POOL_MAX) {
        if (vm->bytes + size > vm->lim.max_heap_bytes)
            vm_fatal(vm, RS_MEMORY, "heap limit exceeded (%llu bytes)", (unsigned long long)vm->lim.max_heap_bytes);
        o = pool_alloc(vm, size);
        vm->bytes += size;
    } else {
        o = vm_mem(vm, NULL, 0, size);
    }
    memset(o, 0, size);
    o->type = (uint8_t)type;
    o->id = with_id ? ++vm->next_id : 0u;
    o->next = vm->objs;
    vm->objs = o;
    return o;
}

/* size of the object header block as allocated by new_obj */
static size_t obj_hdr_size(const Obj *o) {
    switch (o->type) {
    case T_STRING: return offsetof(SString, data) + ((const SString *)o)->len + 1;
    case T_ARRAY: return sizeof(SArray);
    case T_TABLE: return sizeof(STable);
    case T_FUNCTION: return sizeof(SFunc);
    case T_CELL: return sizeof(SCell);
    case T_CLASS: return sizeof(SClass);
    case T_COROUTINE: return sizeof(SCoroutine);
    case T_INSTANCE: return offsetof(SInstance, slots) + (size_t)((const SInstance *)o)->nslots * sizeof(SpValue);
    case T_MODULE: return sizeof(SModule);
    case T_ITERATOR: return sizeof(SIterator);
    case T_ERROR: return sizeof(SError);
    case T_NATIVE: return sizeof(SNativeObj);
    default: return sizeof(Obj);
    }
}

void obj_release_block(VM *vm, Obj *o) {
    size_t hs = obj_hdr_size(o);
    if (hs <= POOL_MAX) {
        size_t c = (hs + 15) >> 4;
        *(void **)(void *)o = vm->pool_free[c];
        vm->pool_free[c] = o;
    } else {
        free(o);
    }
}

/* ---------------------------------------------------------------- */
/* strings                                                           */
/* ---------------------------------------------------------------- */
/* FNV-1a, byte at a time. A word-at-a-time version used fewer instructions but was measurably slower (~6%) on the
 * string-key benchmark: the hash runs right after the string bytes were written with narrow stores, and wide loads
 * over those stores cannot be store-forwarded. Do not "optimise" this without timing it (tools/bench/tbl.sp). */
uint32_t sp_hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

SString *vm_alloc_string(VM *vm, size_t len) {
    if (len > vm->lim.max_string_bytes)
        vm_fatal(vm, RS_MEMORY, "string length limit exceeded");
    SString *s = new_obj(vm, offsetof(SString, data) + len + 1, T_STRING, 0);
    s->len = (uint32_t)len;
    s->data[len] = 0;
    return s;
}

uint32_t str_hash_calc(const char *s, size_t n) {
    uint32_t h = sp_hash_bytes(s, n);
    return h ? h : 1;
}

uint32_t str_hash_slow(SString *s) {
    s->hash = str_hash_calc(s->data, s->len);
    return s->hash;
}

void string_finish(SString *s) {
    if (s->len <= 48) (void)str_hash_slow(s);
    else s->hash = 0;
}

SString *vm_new_string(VM *vm, const char *src, size_t len) {
    SString *s = vm_alloc_string(vm, len);
    if (len) memcpy(s->data, src, len);
    string_finish(s);
    return s;
}

SpValue vm_perm_string(VM *vm, const char *s, size_t len) {
    return v_obj(T_STRING, vm_new_string(vm, s, len));
}

/* ---------------------------------------------------------------- */
/* arrays                                                            */
/* ---------------------------------------------------------------- */
SArray *vm_new_array(VM *vm, int64_t cap) {
    if ((uint64_t)cap > vm->lim.max_array_length) vm_fatal(vm, RS_MEMORY, "array length limit exceeded");
    SArray *a = new_obj(vm, sizeof *a, T_ARRAY, 1);
    if (cap > 0) {
        a->items = vm_mem(vm, NULL, 0, (size_t)cap * sizeof(SpValue));
        a->cap = cap;
    }
    return a;
}

int array_reserve(VM *vm, SArray *a, int64_t need) {
    if (need <= a->cap) return 0;
    if ((uint64_t)need > vm->lim.max_array_length) vm_fatal(vm, RS_MEMORY, "array length limit exceeded");
    int64_t nc = a->cap ? a->cap + a->cap / 2 : 4;
    if (nc < need) nc = need;
    a->items = vm_mem(vm, a->items, (size_t)a->cap * sizeof(SpValue), (size_t)nc * sizeof(SpValue));
    a->cap = nc;
    return 0;
}

int array_push(VM *vm, SArray *a, SpValue v) {
    if (a->len == a->cap) array_reserve(vm, a, a->len + 1);
    a->items[a->len++] = v;
    return 0;
}


/* ---------------------------------------------------------------- */
/* misc objects                                                      */
/* ---------------------------------------------------------------- */
SCell *vm_new_cell(VM *vm, SpValue v) {
    SCell *c = new_obj(vm, sizeof *c, T_CELL, 0);
    c->v = v;
    return c;
}

SFunc *vm_new_native(VM *vm, const NativeDesc *d) {
    SFunc *f = new_obj(vm, sizeof *f, T_FUNCTION, 1);
    f->nat = d;
    return f;
}

SModule *vm_new_module(VM *vm, const char *name) {
    SModule *m = new_obj(vm, sizeof *m, T_MODULE, 1);
    m->name = vm_new_string(vm, name, strlen(name));
    m->exports = vm_new_table(vm, 0);
    return m;
}

SClass *vm_new_class(VM *vm, SString *name, SClass *parent) {
    SClass *c = new_obj(vm, sizeof *c, T_CLASS, 1);
    c->name = name;
    c->parent = parent;
    c->fields = vm_new_table(vm, 4);
    c->methods = vm_new_table(vm, 4);
    return c;
}

SInstance *vm_new_instance(VM *vm, SClass *cls) {
    size_t n = (size_t)cls->nfields;
    SInstance *o = new_obj(vm, offsetof(SInstance, slots) + n * sizeof(SpValue), T_INSTANCE, 1);
    o->cls = cls;
    o->nslots = cls->nfields;
    return o;
}

SIterator *vm_new_iterator(VM *vm, const IterClass *cls, SpValue held, int64_t a, int64_t b, int64_t c) {
    SIterator *it = new_obj(vm, sizeof *it, T_ITERATOR, 1);
    it->cls = cls;
    it->held = held;
    it->a = a;
    it->b = b;
    it->c = c;
    return it;
}

SCoroutine *vm_new_coroutine(VM *vm, SFunc *fn, int64_t slots) {
    int64_t nf = slots / 8 > 16 ? slots / 8 : 16;
    int64_t nh = slots / 16 > 16 ? slots / 16 : 16;
    int64_t nd = slots / 16 > 8 ? slots / 16 : 8;
    int64_t nv = slots / 4 > 32 ? slots / 4 : 32;
    size_t sz = (size_t)slots * sizeof(SpValue) + (size_t)nf * sizeof(Frame) + (size_t)nh * sizeof(Handler) +
                (size_t)nd * sizeof(DeferRec) + (size_t)nv * sizeof(SpValue);
    SCoroutine *co = new_obj(vm, sizeof *co, T_COROUTINE, 1);
    co->fn = fn;
    char *blk = vm_mem(vm, NULL, 0, sz);
    memset(blk, 0, sz);
    co->block = blk;
    co->block_size = sz;
    ExecState *s = &co->st;
    s->stack = (SpValue *)(void *)blk;
    s->sp = s->stack;
    s->stack_end = s->stack + slots;
    blk += (size_t)slots * sizeof(SpValue);
    s->frames = (Frame *)(void *)blk;
    s->max_frames = (int)nf;
    blk += (size_t)nf * sizeof(Frame);
    s->handlers = (Handler *)(void *)blk;
    s->hcap = (int)nh;
    blk += (size_t)nh * sizeof(Handler);
    s->drecs = (DeferRec *)(void *)blk;
    s->drcap = (int)nd;
    blk += (size_t)nd * sizeof(DeferRec);
    s->dvals = (SpValue *)(void *)blk;
    s->dvcap = (int)nv;
    co->status = CO_SUSPENDED;
    return co;
}

void vm_co_release(VM *vm, SCoroutine *co) {
    if (co->block) {
        vm_mem(vm, co->block, co->block_size, 0);
        co->block = NULL;
        co->block_size = 0;
    }
    memset(&co->st, 0, sizeof co->st);
}

void exec_save(const VM *vm, ExecState *st) {
    st->stack = vm->stack;
    st->stack_end = vm->stack_end;
    st->sp = vm->sp;
    st->frames = vm->frames;
    st->nframes = vm->nframes;
    st->max_frames = vm->max_frames;
    st->handlers = vm->handlers;
    st->nh = vm->nh;
    st->hcap = vm->hcap;
    st->drecs = vm->drecs;
    st->ndr = vm->ndr;
    st->drcap = vm->drcap;
    st->dvals = vm->dvals;
    st->ndv = vm->ndv;
    st->dvcap = vm->dvcap;
}

void exec_load(VM *vm, const ExecState *st) {
    vm->stack = st->stack;
    vm->stack_end = st->stack_end;
    vm->sp = st->sp;
    vm->frames = st->frames;
    vm->nframes = st->nframes;
    vm->max_frames = st->max_frames;
    vm->handlers = st->handlers;
    vm->nh = st->nh;
    vm->hcap = st->hcap;
    vm->drecs = st->drecs;
    vm->ndr = st->ndr;
    vm->drcap = st->drcap;
    vm->dvals = st->dvals;
    vm->ndv = st->ndv;
    vm->dvcap = st->dvcap;
}

void vm_add_root(VM *vm, SpValue v) {
    if (vm->nroots == vm->caproots) {
        vm->caproots = vm->caproots ? vm->caproots * 2 : 64;
        vm->roots = realloc(vm->roots, (size_t)vm->caproots * sizeof(SpValue));
        if (!vm->roots) exit(14);
    }
    vm->roots[vm->nroots++] = v;
}

/* closure creation lives in vm.c (needs frame access) */
SFunc *vm_new_closure_obj(VM *vm, Proto *p, int nup) {
    SFunc *f = new_obj(vm, sizeof *f, T_FUNCTION, 1);
    f->proto = p;
    f->nup = nup;
    if (nup > 0) {
        f->up = vm_mem(vm, NULL, 0, (size_t)nup * sizeof(SCell *));
        memset(f->up, 0, (size_t)nup * sizeof(SCell *));
    }
    return f;
}

SNativeObj *vm_new_native_obj(VM *vm, const NativeClass *cls, size_t size) {
    void *data = vm_mem(vm, NULL, 0, size ? size : 1);
    memset(data, 0, size ? size : 1);
    SNativeObj *o = new_obj(vm, sizeof *o, T_NATIVE, 1);
    o->cls = cls;
    o->size = size ? size : 1;
    o->data = data;
    return o;
}

SError *vm_new_error_obj(VM *vm) { return new_obj(vm, sizeof(SError), T_ERROR, 1); }

/* ---------------------------------------------------------------- */
/* semantics                                                         */
/* ---------------------------------------------------------------- */
const char *type_name(SpValue v) {
    static const char *names[] = {"nil", "bool", "int", "float", "string", "array", "table",
                                  "function", "class", "instance", "module", "iterator",
                                  "coroutine", "error", "native", "cell", "uninit"};
    if (v.type == T_NATIVE && v.as.o != NULL) return AS_NATIVE(v)->cls->name;
    return v.type <= T_UNINIT ? names[v.type] : "?";
}

int num_cmp(SpValue a, SpValue b, int *unordered) {
    *unordered = 0;
    if (a.type == T_INT && b.type == T_INT)
        return a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
    if (a.type == T_FLOAT && b.type == T_FLOAT) {
        if (isnan(a.as.f) || isnan(b.as.f)) { *unordered = 1; return 0; }
        return a.as.f < b.as.f ? -1 : a.as.f > b.as.f;
    }
    /* mixed: exact comparison */
    int swap = 0;
    int64_t i;
    double d;
    if (a.type == T_INT) { i = a.as.i; d = b.as.f; }
    else { i = b.as.i; d = a.as.f; swap = 1; }
    if (isnan(d)) { *unordered = 1; return 0; }
    int r; /* compare i with d */
    if (d >= 9223372036854775808.0) r = -1;
    else if (d < -9223372036854775808.0) r = 1;
    else {
        double fl = floor(d);
        int64_t fi = (int64_t)fl;
        if (i < fi) r = -1;
        else if (i > fi) r = 1;
        else r = (d > fl) ? -1 : 0;
    }
    return swap ? -r : r;
}

int value_eq(SpValue a, SpValue b) {
    if (IS_NUM(a) && IS_NUM(b)) {
        int un;
        int c = num_cmp(a, b, &un);
        return !un && c == 0;
    }
    if (a.type != b.type) return 0;
    switch (a.type) {
    case T_NIL: return 1;
    case T_BOOL: return a.as.b == b.as.b;
    case T_STRING: {
        SString *x = AS_STR(a), *y = AS_STR(b);
        return x == y || (x->len == y->len && str_hash(x) == str_hash(y) && memcmp(x->data, y->data, x->len) == 0);
    }
    default: return a.as.o == b.as.o;
    }
}
