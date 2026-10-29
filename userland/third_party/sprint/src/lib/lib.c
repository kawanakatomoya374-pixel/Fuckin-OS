/* Standard library core: argument helpers, the prelude and the builtin module registry (SPEC 15). */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* argument helpers                                                  */
/* ---------------------------------------------------------------- */
int sp_same(SpValue a, SpValue b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case T_NIL: return 1;
    case T_BOOL: return a.as.b == b.as.b;
    case T_INT: return a.as.i == b.as.i;
    case T_FLOAT: return memcmp(&a.as.f, &b.as.f, sizeof a.as.f) == 0;
    default: return a.as.o == b.as.o;
    }
}

int need_int(VM *vm, SpValue v, const char *what, int64_t *out) {
    if (v.type != T_INT) return vm_set_error(vm, E_TYPE, "%s must be int, not %s", what, type_name(v));
    *out = v.as.i;
    return 0;
}
int need_str(VM *vm, SpValue v, const char *what) {
    if (v.type != T_STRING) return vm_set_error(vm, E_TYPE, "%s must be string, not %s", what, type_name(v));
    return 0;
}
int need_num(VM *vm, SpValue v, const char *what, double *out) {
    if (v.type == T_INT) *out = (double)v.as.i;
    else if (v.type == T_FLOAT) *out = v.as.f;
    else return vm_set_error(vm, E_TYPE, "%s must be a number, not %s", what, type_name(v));
    return 0;
}
SpValue vstr(VM *vm, const char *s, size_t n) { return v_obj(T_STRING, vm_new_string(vm, s, n)); }

int find_bytes(const char *h, size_t hl, const char *nd, size_t nl, size_t start) {
    if (nl == 0) return start <= hl ? (int)start : -1;
    if (hl < nl) return -1;
    for (size_t i = start; i + nl <= hl; i++)
        if (h[i] == nd[0] && memcmp(h + i, nd, nl) == 0) return (int)i;
    return -1;
}

/* ---------------------------------------------------------------- */
/* prelude                                                           */
/* ---------------------------------------------------------------- */
void sink_stdout(void *ud, const char *s, size_t n) { vm_out_write((VM *)ud, s, n); }

static int n_print(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    if (!(vm->caps & CAP_STDIO_WRITE)) return vm_set_error(vm, E_PERM, "stdout is not permitted");
    Out o = {0};
    o.sink = sink_stdout;
    o.ud = vm;
    for (int i = 0; i < argc; i++) {
        if (i) out_put(&o, " ", 1);
        fmt_value(vm, argv[i], 0, 0, &o);
    }
    out_put(&o, "\n", 1);
    return 0;
}

static int n_len(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    switch (argv[0].type) {
    case T_STRING: *ret = v_int(AS_STR(argv[0])->len); return 0;
    case T_ARRAY: *ret = v_int(AS_ARRAY(argv[0])->len); return 0;
    case T_TABLE: *ret = v_int(AS_TABLE(argv[0])->count); return 0;
    case T_NATIVE:
        if (typed_is(argv[0])) { *ret = v_int(typed_len(argv[0])); return 0; }
        return vm_set_error(vm, E_TYPE, "object of type '%s' has no len()", type_name(argv[0]));
    default: return vm_set_error(vm, E_TYPE, "object of type '%s' has no len()", type_name(argv[0]));
    }
}

static int n_type(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    *ret = v_obj(T_STRING, vm->type_names[argv[0].type]);
    return 0;
}

static int n_to_string(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    *ret = v_obj(T_STRING, vm_to_string(vm, argv[0]));
    return 0;
}

static int parse_int_text(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    unsigned base = 10;
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    else if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; i += 2; }
    else if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'O')) { base = 8; i += 2; }
    if (i >= n) return 0;
    uint64_t v = 0;
    int digits = 0, last_us = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (c == '_') {
            if (!digits || last_us) return 0;
            last_us = 1;
            continue;
        }
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
        else return 0;
        if (d >= base) return 0;
        if (v > (UINT64_MAX - d) / base) return 0;
        v = v * base + d;
        digits++;
        last_us = 0;
    }
    if (!digits || last_us) return 0;
    if (neg) {
        if (v > (uint64_t)INT64_MAX + 1u) return 0;
        *out = v == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX) return 0;
        *out = (int64_t)v;
    }
    return 1;
}

static int n_to_int(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SpValue v = argv[0];
    if (v.type == T_INT) { *ret = v; return 0; }
    if (v.type == T_FLOAT) {
        double f = v.as.f;
        if (isnan(f) || isinf(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0)
            return vm_set_error(vm, E_VALUE, "cannot convert float to int: out of range");
        *ret = v_int((int64_t)f);
        return 0;
    }
    if (v.type == T_STRING) {
        int64_t r;
        if (!parse_int_text(AS_STR(v)->data, AS_STR(v)->len, &r))
            return vm_set_error(vm, E_VALUE, "invalid integer literal: \"%s\"", AS_STR(v)->data);
        *ret = v_int(r);
        return 0;
    }
    return vm_set_error(vm, E_TYPE, "to_int() argument must be int, float or string, not %s", type_name(v));
}

static int n_to_float(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SpValue v = argv[0];
    if (v.type == T_INT) { *ret = v_float((double)v.as.i); return 0; }
    if (v.type == T_FLOAT) { *ret = v; return 0; }
    if (v.type == T_STRING) {
        const SString *s = AS_STR(v);
        if (strcmp(s->data, "inf") == 0) { *ret = v_float(INFINITY); return 0; }
        if (strcmp(s->data, "-inf") == 0) { *ret = v_float(-INFINITY); return 0; }
        if (strcmp(s->data, "nan") == 0) { *ret = v_float((double)NAN); return 0; }
        char buf[128];
        size_t bl = 0;
        int ok = s->len > 0 && s->len < sizeof buf;
        for (size_t i = 0; ok && i < s->len; i++) {
            char c = s->data[i];
            if (c == '_') continue;
            if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) ok = 0;
            else buf[bl++] = c;
        }
        if (ok) {
            buf[bl] = 0;
            char *end;
            double d = strtod(buf, &end);
            if (bl > 0 && *end == 0 && !isinf(d)) { *ret = v_float(d); return 0; }
        }
        return vm_set_error(vm, E_VALUE, "invalid float literal: \"%s\"", s->data);
    }
    return vm_set_error(vm, E_TYPE, "to_float() argument must be int, float or string, not %s", type_name(v));
}

static int range_next(VM *vm, SIterator *it, SpValue out[2]) {
    (void)vm;
    int64_t cur = it->a, stop = it->b, step = it->c;
    if (step > 0 ? cur >= stop : cur <= stop) return 1;
    out[0] = v_int(cur);
    int64_t nx;
    if (__builtin_add_overflow(cur, step, &nx)) nx = stop; /* past the int range: terminate */
    it->a = nx;
    return 0;
}
static const IterClass range_class = {"range", 1, range_next};

static int n_range(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    int64_t v[3] = {0, 0, 1};
    if (argc == 1) {
        if (need_int(vm, argv[0], "range stop", &v[1])) return 1;
    } else {
        if (need_int(vm, argv[0], "range start", &v[0]) || need_int(vm, argv[1], "range stop", &v[1])) return 1;
        if (argc == 3 && need_int(vm, argv[2], "range step", &v[2])) return 1;
    }
    if (v[2] == 0) return vm_set_error(vm, E_VALUE, "range() step must not be zero");
    *ret = v_obj(T_ITERATOR, vm_new_iterator(vm, &range_class, v_nil(), v[0], v[1], v[2]));
    return 0;
}


static int n_error(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    if (need_str(vm, argv[0], "error kind") || need_str(vm, argv[1], "error message")) return 1;
    SString *k = AS_STR(argv[0]);
    int code = err_kind_code(k->data, k->len);
    if (code < 0) return vm_set_error(vm, E_VALUE, "'%s' is not a constructible error kind", k->data);
    if (code == 0) {
        code = E_USER;
        if (k->len == 0 || !(isalpha((unsigned char)k->data[0]) || k->data[0] == '_'))
            return vm_set_error(vm, E_VALUE, "error kind must be an identifier");
    }
    SpValue cause = argc > 2 ? argv[2] : v_nil();
    if (cause.type != T_NIL && cause.type != T_ERROR) return vm_set_error(vm, E_TYPE, "error cause must be an error or nil");
    SError *e = vm_make_error(vm, code, k, AS_STR(argv[1]));
    e->cause = cause;
    if (argc > 3) e->data = argv[3];
    *ret = v_obj(T_ERROR, e);
    return 0;
}

static int n_error_trace(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_ERROR) return vm_set_error(vm, E_TYPE, "error_trace() argument must be an error");
    *ret = v_obj(T_STRING, AS_ERROR(argv[0])->trace);
    (void)vm;
    return 0;
}

int n_has(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_TABLE) return vm_set_error(vm, E_TYPE, "has() requires a table, not %s", type_name(argv[0]));
    SpValue out;
    int found = 0;
    if (table_get(vm, AS_TABLE(argv[0]), argv[1], &out, &found)) return 1;
    *ret = v_bool(found);
    return 0;
}

static int n_delete(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_TABLE) return vm_set_error(vm, E_TYPE, "delete() requires a table, not %s", type_name(argv[0]));
    SpValue out;
    if (table_get(vm, AS_TABLE(argv[0]), argv[1], &out, NULL)) return 1;
    if (table_set(vm, AS_TABLE(argv[0]), argv[1], v_nil())) return 1;
    *ret = out;
    return 0;
}

static int deep_eq(VM *vm, SpValue a, SpValue b, int depth, int *res) {
    if (depth > 256) return vm_set_error(vm, E_VALUE, "deep_equal: nesting too deep (or cyclic structure)");
    if (a.type == T_ARRAY && b.type == T_ARRAY) {
        SArray *x = AS_ARRAY(a), *y = AS_ARRAY(b);
        if (x == y) { *res = 1; return 0; }
        if (x->len != y->len) { *res = 0; return 0; }
        for (int64_t i = 0; i < x->len; i++) {
            if (deep_eq(vm, x->items[i], y->items[i], depth + 1, res)) return 1;
            if (!*res) return 0;
        }
        *res = 1;
        return 0;
    }
    if (a.type == T_TABLE && b.type == T_TABLE) {
        STable *x = AS_TABLE(a), *y = AS_TABLE(b);
        if (x == y) { *res = 1; return 0; }
        if (x->count != y->count) { *res = 0; return 0; }
        SpValue k, v, w;
        int64_t c = 0;
        while ((c = table_next(x, c, &k, &v)) >= 0) {
            int found = 0;
            if (table_get(vm, y, k, &w, &found)) return 1;
            if (!found) { *res = 0; return 0; }
            if (deep_eq(vm, v, w, depth + 1, res)) return 1;
            if (!*res) return 0;
        }
        *res = 1;
        return 0;
    }
    *res = value_eq(a, b);
    return 0;
}

static int n_deep_equal(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int r = 0;
    if (deep_eq(vm, argv[0], argv[1], 0, &r)) return 1;
    *ret = v_bool(r);
    return 0;
}

static int n_classof(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)vm; (void)argc;
    *ret = argv[0].type == T_INSTANCE ? v_obj(T_CLASS, AS_INSTANCE(argv[0])->cls) : v_nil();
    return 0;
}
static int n_class_name(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_CLASS) return vm_set_error(vm, E_TYPE, "class_name() requires a class, not %s", type_name(argv[0]));
    *ret = v_obj(T_STRING, AS_CLASS(argv[0])->name);
    return 0;
}
static int n_is_instance(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[1].type != T_CLASS) return vm_set_error(vm, E_TYPE, "is_instance() requires a class, not %s", type_name(argv[1]));
    int r = 0;
    if (argv[0].type == T_INSTANCE)
        for (const SClass *k = AS_INSTANCE(argv[0])->cls; k; k = k->parent)
            if (k == AS_CLASS(argv[1])) { r = 1; break; }
    *ret = v_bool(r);
    return 0;
}


static const NativeDesc prelude_fns[] = {
    {"print", 0, -1, n_print},       {"len", 1, 1, n_len},
    {"type", 1, 1, n_type},          {"to_string", 1, 1, n_to_string},
    {"to_int", 1, 1, n_to_int},      {"to_float", 1, 1, n_to_float},
    {"range", 1, 3, n_range},        {"error", 2, 4, n_error},
    {"error_trace", 1, 1, n_error_trace}, {"has", 2, 2, n_has},
    {"delete", 2, 2, n_delete},      {"deep_equal", 2, 2, n_deep_equal},
    {"classof", 1, 1, n_classof},    {"class_name", 1, 1, n_class_name},
    {"is_instance", 2, 2, n_is_instance},
    {"$in", 2, 2, n_in}, /* target of `x in c`; not an identifier, so only the parser can name it */
    {NULL, 0, 0, NULL}};

int lib_prelude_index(const char *name, size_t len) {
    for (int i = 0; prelude_fns[i].name; i++)
        if (prelude_fns[i].name[0] == name[0] && strlen(prelude_fns[i].name) == len && memcmp(prelude_fns[i].name, name, len) == 0) return i;
    return -1;
}
const NativeDesc *lib_prelude_desc(int idx) { return &prelude_fns[idx]; }
SpValue lib_prelude_value(VM *vm, int idx) { return vm->roots[idx]; }


/* ---------------------------------------------------------------- */
/* module registry                                                   */
/* ---------------------------------------------------------------- */
static const ModConst no_consts[] = {{NULL, 0, 0, 0}};
static const ModConst sys_consts[] = {{"args", 2, 0, 0}, {"platform", 2, 0, 0}, {"version", 2, 0, 0}, {NULL, 0, 0, 0}};
static const ModDesc modules[] = {
    {"math", math_fns, math_consts}, {"str", str_fns, no_consts}, {"table", table_fns, no_consts},
    {"io", io_fns, no_consts}, {"sys", sys_fns, sys_consts}, {"time", time_fns, no_consts},
    {"gc", gc_fns, no_consts}, {"coroutine", co_fns, no_consts},
    {"json", lib_json_fns, no_consts}, {"gfx", lib_gfx_fns, no_consts}, {"random", lib_random_fns, no_consts}, {"fs", lib_fs_fns, no_consts}, {"_ffi", lib_ffi_fns, no_consts}, {"typed", lib_typed_fns, no_consts}, {NULL, NULL, NULL}};

const ModDesc *lib_find_module(const VM *vm, const char *name, size_t len) {
    for (int i = 0; modules[i].name; i++)
        if (strlen(modules[i].name) == len && memcmp(modules[i].name, name, len) == 0) return &modules[i];
    if (vm)
        for (int i = 0; i < vm->nhmods; i++)
            if (strlen(vm->hmods[i]->name) == len && memcmp(vm->hmods[i]->name, name, len) == 0) return &vm->hmods[i]->md;
    return NULL;
}

int lib_module_has(const ModDesc *m, const char *member, size_t len) {
    for (const NativeDesc *f = m->fns; f->name; f++)
        if (strlen(f->name) == len && memcmp(f->name, member, len) == 0) return 1;
    for (const ModConst *c = m->consts; c->name; c++)
        if (strlen(c->name) == len && memcmp(c->name, member, len) == 0) return 1;
    return 0;
}

SpValue lib_module_value(VM *vm, const char *name) {
    for (int i = 0; i < vm->nroots; i++)
        if (vm->roots[i].type == T_MODULE && strcmp(AS_MODULE(vm->roots[i])->name->data, name) == 0)
            return vm->roots[i];
    const ModDesc *d = lib_find_module(vm, name, strlen(name));
    SModule *m = vm_new_module(vm, name);
    SpValue mv = v_obj(T_MODULE, m);
    vm_add_root(vm, mv);
    for (const NativeDesc *f = d->fns; f->name; f++)
        table_set_str(vm, m->exports, f->name, strlen(f->name), v_obj(T_FUNCTION, vm_new_native(vm, f)));
    for (const ModConst *c = d->consts; c->name; c++) {
        SpValue v;
        if (c->kind == 0) v = v_int(c->i);
        else if (c->kind == 1) v = v_float(c->f);
        else if (strcmp(name, "sys") == 0 && strcmp(c->name, "args") == 0) {
            SArray *a = vm_new_array(vm, vm->prog_argc);
            for (int i = 0; i < vm->prog_argc; i++) a->items[a->len++] = vstr(vm, vm->prog_argv[i], strlen(vm->prog_argv[i]));
            v = v_obj(T_ARRAY, a);
        } else if (strcmp(c->name, "platform") == 0) {
#if defined(__linux__)
            v = vstr(vm, vm->deterministic ? "other" : "linux", 5);
#elif defined(__APPLE__)
            v = vstr(vm, vm->deterministic ? "other" : "macos", 5);
#else
            v = vstr(vm, "other", 5);
#endif
        } else {
            v = vstr(vm, "2.31.0", 6);
        }
        table_set_str(vm, m->exports, c->name, strlen(c->name), v);
    }
    return mv;
}

SpValue lib_module_export(VM *vm, const char *module, const char *member, int *found) {
    SpValue mv = lib_module_value(vm, module);
    SpValue out = v_nil();
    *found = table_get_str(AS_MODULE(mv)->exports, member, strlen(member), &out);
    return out;
}

void lib_init(VM *vm) {
    for (int i = 0; prelude_fns[i].name; i++) vm_add_root(vm, v_obj(T_FUNCTION, vm_new_native(vm, &prelude_fns[i])));
}
