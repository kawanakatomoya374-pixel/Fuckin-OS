/* typed arrays (SPEC 15.20): fixed-length arrays of one machine number type, stored unboxed.
 * They are native objects; the VM reaches them through typed_get / typed_set / typed_len and the `for` loop through
 * typed_iter. Nothing here runs Sprint code. */
#include "lib_int.h"

/* header is 16 bytes so the payload keeps the 16-byte alignment vm_mem gives */
typedef struct TArr {
    int64_t len;
    int64_t kind;
    unsigned char b[];
} TArr;

static const unsigned char tsize[TK_COUNT] = {1, 1, 2, 2, 4, 4, 8, 4, 8};
static const char *const tname[TK_COUNT] = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "f32", "f64"};
#define MAX_TYPED_BYTES ((int64_t)1 << 31)

_Static_assert(offsetof(TArr, len) == 0 && offsetof(TArr, b) == TYPED_DATA_OFF, "the JIT assumes this layout");
static int tm_fill(VM *vm, int argc, SpValue *argv, SpValue *ret);
static int tm_copy(VM *vm, int argc, SpValue *argv, SpValue *ret);
static int tm_slice(VM *vm, int argc, SpValue *argv, SpValue *ret);
static int tm_to_array(VM *vm, int argc, SpValue *argv, SpValue *ret);
static int tm_kind(VM *vm, int argc, SpValue *argv, SpValue *ret);
static int tm_set_from(VM *vm, int argc, SpValue *argv, SpValue *ret);

static const NativeDesc typed_methods[] = {
    {"fill", 2, 2, tm_fill}, {"copy", 1, 1, tm_copy}, {"slice", 2, 3, tm_slice},
    {"to_array", 1, 1, tm_to_array}, {"kind", 1, 1, tm_kind}, {"set_from", 3, 3, tm_set_from},
    {NULL, 0, 0, NULL}};

#define TCLASS(n) {n, typed_methods, NULL, NULL}
const NativeClass typed_classes[TK_COUNT] = {
    TCLASS("typed.i8"), TCLASS("typed.u8"), TCLASS("typed.i16"), TCLASS("typed.u16"), TCLASS("typed.i32"),
    TCLASS("typed.u32"), TCLASS("typed.i64"), TCLASS("typed.f32"), TCLASS("typed.f64")};


static TArr *tarr(SpValue v) { return (TArr *)AS_NATIVE(v)->data; }

int64_t typed_len(SpValue v) { return tarr(v)->len; }

/* element kind as a small int (0..8); the JIT and the VM use it to pick a load/store */
int typed_kind(SpValue v) { return (int)tarr(v)->kind; }

static SpValue load_el(const TArr *t, int64_t i) {
    const unsigned char *p = t->b + (size_t)i * tsize[t->kind];
    switch ((TKind)t->kind) {
    case TK_I8: { int8_t x; memcpy(&x, p, 1); return v_int(x); }
    case TK_U8: return v_int(*p);
    case TK_I16: { int16_t x; memcpy(&x, p, 2); return v_int(x); }
    case TK_U16: { uint16_t x; memcpy(&x, p, 2); return v_int(x); }
    case TK_I32: { int32_t x; memcpy(&x, p, 4); return v_int(x); }
    case TK_U32: { uint32_t x; memcpy(&x, p, 4); return v_int(x); }
    case TK_F32: { float x; memcpy(&x, p, 4); return v_float((double)x); }
    case TK_F64: { double x; memcpy(&x, p, 8); return v_float(x); }
    case TK_I64: case TK_COUNT: default: { int64_t x; memcpy(&x, p, 8); return v_int(x); }
    }
}

/* ints are truncated to the element width (like C and JS typed arrays); floats accept int or float. Returns 0 when v has
 * the wrong type (nothing is stored). */
static int store_raw(TArr *t, int64_t i, SpValue v) {
    unsigned char *p = t->b + (size_t)i * tsize[t->kind];
    if (t->kind == TK_F32 || t->kind == TK_F64) {
        double d;
        if (v.type == T_FLOAT) d = v.as.f;
        else if (v.type == T_INT) d = (double)v.as.i;
        else return 0;
        if (t->kind == TK_F32) { float f = (float)d; memcpy(p, &f, 4); }
        else memcpy(p, &d, 8);
        return 1;
    }
    if (v.type != T_INT) return 0;
    int64_t x = v.as.i;
    switch (tsize[t->kind]) {
    case 1: { uint8_t y = (uint8_t)x; memcpy(p, &y, 1); break; }
    case 2: { uint16_t y = (uint16_t)x; memcpy(p, &y, 2); break; }
    case 4: { uint32_t y = (uint32_t)x; memcpy(p, &y, 4); break; }
    default: memcpy(p, &x, 8); break;
    }
    return 1;
}

/* a TypeError for a rejected element */
static int store_el(VM *vm, TArr *t, int64_t i, SpValue v) {
    if (store_raw(t, i, v)) return 0;
    return vm_set_error(vm, E_TYPE, "typed.%s element must be %s, not %s", tname[t->kind],
                        t->kind == TK_F32 || t->kind == TK_F64 ? "a number" : "an int", type_name(v));
}

/* error-free variants for the JIT helpers: 0 means "not handled here", the interpreter then runs the instruction */
int typed_peek(SpValue o, SpValue k, SpValue *out) {
    if (!typed_is(o)) return 0;
    const TArr *t = tarr(o);
    if (k.type != T_INT || k.as.i < 0 || k.as.i >= t->len) return 0;
    *out = load_el(t, k.as.i);
    return 1;
}
int typed_poke(SpValue o, SpValue k, SpValue v) {
    if (!typed_is(o)) return 0;
    TArr *t = tarr(o);
    if (k.type != T_INT || k.as.i < 0 || k.as.i >= t->len) return 0;
    return store_raw(t, k.as.i, v);
}

int typed_get(VM *vm, SpValue o, SpValue k, SpValue *out) {
    const TArr *t = tarr(o);
    if (k.type != T_INT) return vm_set_error(vm, E_TYPE, "%s index must be int, not %s", type_name(o), type_name(k));
    if (k.as.i < 0 || k.as.i >= t->len)
        return vm_set_error(vm, E_INDEX, "%s index %lld out of range (length %lld)", type_name(o), (long long)k.as.i, (long long)t->len);
    *out = load_el(t, k.as.i);
    return 0;
}

int typed_set(VM *vm, SpValue o, SpValue k, SpValue v) {
    TArr *t = tarr(o);
    if (k.type != T_INT) return vm_set_error(vm, E_TYPE, "%s index must be int, not %s", type_name(o), type_name(k));
    if (k.as.i < 0 || k.as.i >= t->len)
        return vm_set_error(vm, E_INDEX, "%s index %lld out of range (length %lld)", type_name(o), (long long)k.as.i, (long long)t->len);
    return store_el(vm, t, k.as.i, v);
}

static SpValue alloc_typed(VM *vm, TKind kind, int64_t n) {
    SNativeObj *o = vm_new_native_obj(vm, &typed_classes[kind], sizeof(TArr) + (size_t)n * tsize[kind]);
    TArr *t = (TArr *)o->data;
    t->len = n;
    t->kind = kind;
    return v_obj(T_NATIVE, o);
}

/* typed.<kind>(n) or typed.<kind>(array) */
static int make(VM *vm, TKind kind, SpValue arg, SpValue *ret) {
    int64_t n;
    if (arg.type == T_INT) {
        n = arg.as.i;
        if (n < 0) return vm_set_error(vm, E_VALUE, "typed.%s length must not be negative", tname[kind]);
        if (n > MAX_TYPED_BYTES / tsize[kind]) return vm_set_error(vm, E_VALUE, "typed.%s length is too large", tname[kind]);
        *ret = alloc_typed(vm, kind, n);
        return 0;
    }
    if (arg.type == T_ARRAY) {
        n = AS_ARRAY(arg)->len;
        if (n > MAX_TYPED_BYTES / tsize[kind]) return vm_set_error(vm, E_VALUE, "typed.%s length is too large", tname[kind]);
        SpValue r = alloc_typed(vm, kind, n);
        TArr *t = tarr(r);
        const SArray *a = AS_ARRAY(arg);
        for (int64_t i = 0; i < n; i++)
            if (store_el(vm, t, i, a->items[i])) return 1;
        *ret = r;
        return 0;
    }
    return vm_set_error(vm, E_TYPE, "typed.%s() takes a length (int) or an array, not %s", tname[kind], type_name(arg));
}

#define MK(NAME, K) static int tf_##NAME(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return make(vm, K, argv[0], ret); }
MK(i8, TK_I8) MK(u8, TK_U8) MK(i16, TK_I16) MK(u16, TK_U16) MK(i32, TK_I32) MK(u32, TK_U32) MK(i64, TK_I64) MK(f32, TK_F32) MK(f64, TK_F64)

const NativeDesc lib_typed_fns[] = {
    {"i8", 1, 1, tf_i8}, {"u8", 1, 1, tf_u8}, {"i16", 1, 1, tf_i16}, {"u16", 1, 1, tf_u16}, {"i32", 1, 1, tf_i32},
    {"u32", 1, 1, tf_u32}, {"i64", 1, 1, tf_i64}, {"f32", 1, 1, tf_f32}, {"f64", 1, 1, tf_f64}, {NULL, 0, 0, NULL}};

/* ---- methods ------------------------------------------------------------------------- */
static int self_typed(VM *vm, SpValue v) {
    if (!typed_is(v)) return vm_set_error(vm, E_TYPE, "expected a typed array, not %s", type_name(v));
    return 0;
}

static int tm_fill(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (self_typed(vm, argv[0])) return 1;
    TArr *t = tarr(argv[0]);
    if (t->len > 0) {
        if (store_el(vm, t, 0, argv[1])) return 1;
        for (int64_t i = 1; i < t->len; i++) memcpy(t->b + (size_t)i * tsize[t->kind], t->b, tsize[t->kind]);
    } else if (argv[1].type != T_INT && !(argv[1].type == T_FLOAT && (t->kind == TK_F32 || t->kind == TK_F64))) {
        return vm_set_error(vm, E_TYPE, "typed.%s element must be %s, not %s", tname[t->kind],
                            t->kind == TK_F32 || t->kind == TK_F64 ? "a number" : "an int", type_name(argv[1]));
    }
    *ret = v_nil();
    return 0;
}

static int tm_slice(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    if (self_typed(vm, argv[0])) return 1;
    const TArr *t = tarr(argv[0]);
    int64_t s, e = t->len;
    if (need_int(vm, argv[1], "start", &s)) return 1;
    if (argc > 2 && need_int(vm, argv[2], "end", &e)) return 1;
    if (s < 0 || e < 0 || s > t->len || e > t->len)
        return vm_set_error(vm, E_INDEX, "slice out of range for %s of length %lld", type_name(argv[0]), (long long)t->len);
    if (s > e) return vm_set_error(vm, E_VALUE, "slice start is after end");
    SpValue r = alloc_typed(vm, (TKind)t->kind, e - s);
    t = tarr(argv[0]); /* no GC moves objects, but keep the pointer fresh */
    memcpy(tarr(r)->b, t->b + (size_t)s * tsize[t->kind], (size_t)(e - s) * tsize[t->kind]);
    *ret = r;
    return 0;
}

static int tm_copy(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (self_typed(vm, argv[0])) return 1;
    const TArr *t = tarr(argv[0]);
    SpValue r = alloc_typed(vm, (TKind)t->kind, t->len);
    memcpy(tarr(r)->b, t->b, (size_t)t->len * tsize[t->kind]);
    *ret = r;
    return 0;
}

static int tm_to_array(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (self_typed(vm, argv[0])) return 1;
    const TArr *t = tarr(argv[0]);
    SArray *a = vm_new_array(vm, t->len);
    for (int64_t i = 0; i < t->len; i++) a->items[a->len++] = load_el(t, i);
    *ret = v_obj(T_ARRAY, a);
    return 0;
}

static int tm_kind(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (self_typed(vm, argv[0])) return 1;
    *ret = vstr(vm, tname[tarr(argv[0])->kind], strlen(tname[tarr(argv[0])->kind]));
    return 0;
}

/* dst.set_from(src, offset): copy all of src (same kind, or a plain array of numbers) into dst starting at offset */
static int tm_set_from(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (self_typed(vm, argv[0])) return 1;
    TArr *d = tarr(argv[0]);
    int64_t off;
    if (need_int(vm, argv[2], "offset", &off)) return 1;
    int64_t n;
    if (argv[1].type == T_ARRAY) n = AS_ARRAY(argv[1])->len;
    else if (typed_is(argv[1])) n = tarr(argv[1])->len;
    else return vm_set_error(vm, E_TYPE, "set_from() source must be a typed array or an array, not %s", type_name(argv[1]));
    if (off < 0 || off > d->len || n > d->len - off)
        return vm_set_error(vm, E_INDEX, "set_from() does not fit: offset %lld + %lld elements into length %lld", (long long)off, (long long)n, (long long)d->len);
    if (argv[1].type == T_ARRAY) {
        const SArray *a = AS_ARRAY(argv[1]);
        for (int64_t i = 0; i < n; i++)
            if (store_el(vm, d, off + i, a->items[i])) return 1;
    } else {
        const TArr *s = tarr(argv[1]);
        if (s->kind == d->kind) {
            memmove(d->b + (size_t)off * tsize[d->kind], s->b, (size_t)n * tsize[d->kind]);
        } else {
            for (int64_t i = 0; i < n; i++)
                if (store_el(vm, d, off + i, load_el(s, i))) return 1;
        }
    }
    *ret = v_nil();
    return 0;
}
