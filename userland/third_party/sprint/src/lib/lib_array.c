/* Array methods and method lookup (SPEC 8). */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* array methods                                                     */
/* ---------------------------------------------------------------- */
static int am_append(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return array_push(vm, AS_ARRAY(argv[0]), argv[1]); }
static int am_extend(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    if (argv[1].type != T_ARRAY) return vm_set_error(vm, E_TYPE, "extend() requires an array");
    SArray *a = AS_ARRAY(argv[0]), *b = AS_ARRAY(argv[1]);
    int64_t n = b->len;
    array_reserve(vm, a, a->len + n);
    for (int64_t i = 0; i < n; i++) a->items[a->len++] = b->items[i];
    return 0;
}
static int am_insert(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    int64_t i;
    if (need_int(vm, argv[1], "index", &i)) return 1;
    SArray *a = AS_ARRAY(argv[0]);
    if (i < 0 || i > a->len) return vm_set_error(vm, E_INDEX, "insert() index out of range");
    array_reserve(vm, a, a->len + 1);
    memmove(a->items + i + 1, a->items + i, (size_t)(a->len - i) * sizeof(SpValue));
    a->items[i] = argv[2];
    a->len++;
    return 0;
}
static int am_pop(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SArray *a = AS_ARRAY(argv[0]);
    if (a->len == 0) return vm_set_error(vm, E_INDEX, "pop() from an empty array");
    *ret = a->items[--a->len];
    return 0;
}
static int am_remove_at(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t i;
    if (need_int(vm, argv[1], "index", &i)) return 1;
    SArray *a = AS_ARRAY(argv[0]);
    if (i < 0 || i >= a->len) return vm_set_error(vm, E_INDEX, "remove_at() index out of range");
    *ret = a->items[i];
    memmove(a->items + i, a->items + i + 1, (size_t)(a->len - i - 1) * sizeof(SpValue));
    a->len--;
    return 0;
}
static int am_clear(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)vm; (void)argc; (void)ret; AS_ARRAY(argv[0])->len = 0; return 0; }
static int am_reserve(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    int64_t n;
    if (need_int(vm, argv[1], "capacity", &n)) return 1;
    if (n < 0) return vm_set_error(vm, E_VALUE, "capacity must not be negative");
    array_reserve(vm, AS_ARRAY(argv[0]), n);
    return 0;
}
static int am_capacity(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)vm; (void)argc; *ret = v_int(AS_ARRAY(argv[0])->cap); return 0; }
static int am_slice(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    SArray *a = AS_ARRAY(argv[0]);
    int64_t s, e = a->len;
    if (need_int(vm, argv[1], "start", &s)) return 1;
    if (argc > 2 && need_int(vm, argv[2], "end", &e)) return 1;
    if (s < 0 || e < 0 || s > a->len || e > a->len) return vm_set_error(vm, E_INDEX, "slice out of range for array of length %lld", (long long)a->len);
    if (s > e) return vm_set_error(vm, E_VALUE, "slice start is after end");
    SArray *r = vm_new_array(vm, e - s);
    for (int64_t i = s; i < e; i++) r->items[r->len++] = a->items[i];
    *ret = v_obj(T_ARRAY, r);
    return 0;
}
static int am_copy(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SArray *a = AS_ARRAY(argv[0]);
    SArray *r = vm_new_array(vm, a->len);
    for (int64_t i = 0; i < a->len; i++) r->items[r->len++] = a->items[i];
    *ret = v_obj(T_ARRAY, r);
    return 0;
}
static int am_index_of(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)vm; (void)argc;
    SArray *a = AS_ARRAY(argv[0]);
    *ret = v_int(-1);
    for (int64_t i = 0; i < a->len; i++) if (value_eq(a->items[i], argv[1])) { *ret = v_int(i); break; }
    return 0;
}
static int am_contains(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    SpValue r;
    (void)am_index_of(vm, argc, argv, &r);
    *ret = v_bool(r.as.i >= 0);
    return 0;
}
/* x in c: element of an array, substring of a string, key of a table (SPEC 7.12) */
int n_in(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SpValue a[2] = {argv[1], argv[0]};
    switch (argv[1].type) {
    case T_ARRAY: return am_contains(vm, 2, a, ret);
    case T_STRING: return sm_contains(vm, 2, a, ret);
    case T_TABLE: return n_has(vm, 2, a, ret);
    default: return vm_set_error(vm, E_TYPE, "'in' requires an array, string or table, not %s", type_name(argv[1]));
    }
}
static int am_reverse(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)vm; (void)argc; (void)ret;
    SArray *a = AS_ARRAY(argv[0]);
    for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) { SpValue t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t; }
    return 0;
}
static int am_join(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "separator")) return 1;
    return arr_join(vm, AS_ARRAY(argv[0]), AS_STR(argv[1]), ret);
}

static int sort_less(VM *vm, SpValue cmp, SpValue a, SpValue b, int *less) {
    if (cmp.type != T_NIL) {
        SpValue args[2] = {a, b};
        SpValue r;
        if (vm_call(vm, cmp, 2, args, &r)) return 1;
        *less = TRUTHY(r);
        return 0;
    }
    int c, un = 0;
    if (IS_NUM(a) && IS_NUM(b)) c = num_cmp(a, b, &un);
    else if (a.type == T_STRING && b.type == T_STRING) {
        SString *x = AS_STR(a), *y = AS_STR(b);
        size_t n = x->len < y->len ? x->len : y->len;
        c = memcmp(x->data, y->data, n);
        if (c == 0) c = x->len < y->len ? -1 : x->len > y->len;
    } else {
        return vm_set_error(vm, E_TYPE, "cannot sort %s with %s", type_name(a), type_name(b));
    }
    if (un) return vm_set_error(vm, E_TYPE, "cannot sort NaN");
    *less = c < 0;
    return 0;
}

/* Arrays of plain ints sort in place on a temporary key array (equal ints are indistinguishable, so stability is moot):
 * introsort -- median-of-three quicksort, insertion sort for small ranges, heapsort when the recursion gets deep. */
static void keys_heapsort(int64_t *a, int64_t n) {
    for (int64_t s = n / 2 - 1; s >= 0; s--) {
        int64_t i = s;
        for (;;) {
            int64_t l = 2 * i + 1, m = i;
            if (l < n && a[l] > a[m]) m = l;
            if (l + 1 < n && a[l + 1] > a[m]) m = l + 1;
            if (m == i) break;
            int64_t t = a[i]; a[i] = a[m]; a[m] = t;
            i = m;
        }
    }
    for (int64_t e = n - 1; e > 0; e--) {
        int64_t t = a[0]; a[0] = a[e]; a[e] = t;
        int64_t i = 0;
        for (;;) {
            int64_t l = 2 * i + 1, m = i;
            if (l < e && a[l] > a[m]) m = l;
            if (l + 1 < e && a[l + 1] > a[m]) m = l + 1;
            if (m == i) break;
            int64_t u = a[i]; a[i] = a[m]; a[m] = u;
            i = m;
        }
    }
}

static void keys_sort(int64_t *a, int64_t n) {
    typedef struct { int64_t lo, hi; int d; } Rng;
    Rng stk[96];
    int sp = 0, d0 = 0;
    for (int64_t m = n; m > 1; m >>= 1) d0 += 2;
    stk[sp++] = (Rng){0, n - 1, d0};
    while (sp) {
        Rng r = stk[--sp];
        int64_t lo = r.lo, hi = r.hi;
        int d = r.d;
        while (hi - lo > 16) {
            if (d-- == 0) { keys_heapsort(a + lo, hi - lo + 1); hi = lo; break; }
            int64_t x = a[lo], y = a[lo + (hi - lo) / 2], z = a[hi], t;
            if (x > y) { t = x; x = y; y = t; }
            if (y > z) { t = y; y = z; z = t; }
            if (x > y) { t = x; x = y; y = t; }
            int64_t i = lo, j = hi;
            while (i <= j) {
                while (a[i] < y) i++;
                while (a[j] > y) j--;
                if (i <= j) { t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
            }
            if (j - lo < hi - i) { stk[sp++] = (Rng){i, hi, d}; hi = j; }
            else { stk[sp++] = (Rng){lo, j, d}; lo = i; }
        }
        for (int64_t i = lo + 1; i <= hi; i++) {
            int64_t v = a[i], j = i - 1;
            while (j >= lo && a[j] > v) { a[j + 1] = a[j]; j--; }
            a[j + 1] = v;
        }
    }
}

/* give an array's storage back at once (the temporaries of sort would otherwise stay until the next collection) */
static void arr_release(VM *vm, SArray *x) {
    x->items = vm_mem(vm, x->items, (size_t)x->cap * sizeof(SpValue), 0);
    x->cap = 0;
    x->len = 0;
}

/* Stable merge sort on a work copy; the temporary arrays sit on the operand stack so that the GC sees them while cmp
 * runs. A comparison function is user code, which may change the array: that case keeps a snapshot to detect it. */
static int am_sort(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    SpValue cmp = argc > 1 ? argv[1] : v_nil();
    if (cmp.type != T_NIL && cmp.type != T_FUNCTION && cmp.type != T_CLASS)
        return vm_set_error(vm, E_TYPE, "sort() comparison must be a function, not %s", type_name(cmp));
    SArray *a = AS_ARRAY(argv[0]);
    int64_t n = a->len;
    if (n < 2) return 0;
    if (cmp.type == T_NIL) {
        int64_t i = 0;
        while (i < n && a->items[i].type == T_INT) i++;
        if (i == n) {
            int64_t *keys = vm_mem(vm, NULL, 0, (size_t)n * sizeof *keys);
            for (i = 0; i < n; i++) keys[i] = a->items[i].as.i;
            keys_sort(keys, n);
            for (i = 0; i < n; i++) a->items[i] = v_int(keys[i]);
            (void)vm_mem(vm, keys, (size_t)n * sizeof *keys, 0);
            return 0;
        }
    }
    int user = cmp.type != T_NIL;
    SpValue *base = vm->sp;
    SArray *snap = user ? vm_new_array(vm, n) : NULL, *work = vm_new_array(vm, n), *scratch = vm_new_array(vm, n);
    int nroot = 0;
    if (user) base[nroot++] = v_obj(T_ARRAY, snap);
    base[nroot++] = v_obj(T_ARRAY, work);
    base[nroot++] = v_obj(T_ARRAY, scratch);
    vm->sp = base + nroot;
    for (int64_t i = 0; i < n; i++) work->items[i] = a->items[i];
    if (user) for (int64_t i = 0; i < n; i++) snap->items[i] = a->items[i];
    if (user) snap->len = n;
    work->len = scratch->len = n;
    int err = 0;
    SpValue *src = work->items, *dst = scratch->items;
    for (int64_t w = 1; w < n && !err; w *= 2) {
        for (int64_t lo = 0; lo < n && !err; lo += 2 * w) {
            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi && !err) {
                int less = 0;
                if (sort_less(vm, cmp, src[j], src[i], &less)) { err = 1; break; }
                dst[k++] = less ? src[j++] : src[i++];
            }
            if (err) break;
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        SpValue *t = src; src = dst; dst = t;
    }
    vm->sp = base;
    if (err) return 1;
    int changed = a->len != n;
    for (int64_t i = 0; user && !changed && i < n; i++)
        if (!sp_same(a->items[i], snap->items[i])) changed = 1;
    if (changed) return vm_set_error(vm, E_RUNTIME, "array was modified during sort()");
    for (int64_t i = 0; i < n; i++) a->items[i] = src[i];
    arr_release(vm, work);
    arr_release(vm, scratch);
    if (user) arr_release(vm, snap);
    return 0;
}

/* Higher-order methods. The callback is user code (SPEC App. B); the array is
 * re-checked against its current length on every step, like `for`. */
enum { H_EACH, H_MAP, H_FILTER, H_ANY, H_ALL, H_FIND, H_REDUCE };

static int hof(VM *vm, SpValue *argv, SpValue *ret, int mode) {
    SpValue fn = argv[1];
    if (fn.type != T_FUNCTION && fn.type != T_CLASS)
        return vm_set_error(vm, E_TYPE, "callback must be a function, not %s", type_name(fn));
    SArray *a = AS_ARRAY(argv[0]);
    SpValue *base = vm->sp;
    SArray *out = NULL;
    if (mode == H_MAP || mode == H_FILTER) {
        out = vm_new_array(vm, mode == H_MAP ? a->len : 4);
        base[0] = v_obj(T_ARRAY, out);
        vm->sp = base + 1;
    }
    SpValue acc = mode == H_REDUCE ? argv[2] : v_nil();
    if (mode == H_REDUCE) {
        base[0] = acc; /* keep the accumulator rooted */
        vm->sp = base + 1;
    }
    *ret = v_nil();
    if (mode == H_ANY) *ret = v_bool(0);
    if (mode == H_ALL) *ret = v_bool(1);
    int err = 0;
    for (int64_t i = 0; i < a->len && !err; i++) {
        SpValue item = a->items[i], args[2], r;
        int n = 1;
        args[0] = item;
        if (mode == H_REDUCE) { args[0] = acc; args[1] = item; n = 2; }
        if (vm_call(vm, fn, n, args, &r)) { err = 1; break; }
        switch (mode) {
        case H_EACH: break;
        case H_MAP: err = array_push(vm, out, r); break;
        case H_FILTER: if (sp_truthy(r)) err = array_push(vm, out, item); break;
        case H_ANY: if (sp_truthy(r)) { *ret = v_bool(1); i = a->len; } break;
        case H_ALL: if (!sp_truthy(r)) { *ret = v_bool(0); i = a->len; } break;
        case H_FIND: if (sp_truthy(r)) { *ret = item; i = a->len; } break;
        case H_REDUCE: acc = r; base[0] = acc; break;
        default: break;
        }
    }
    vm->sp = base;
    if (err) return 1;
    if (mode == H_MAP || mode == H_FILTER) *ret = v_obj(T_ARRAY, out);
    if (mode == H_REDUCE) *ret = acc;
    return 0;
}
static int am_each(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_EACH); }
static int am_map(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_MAP); }
static int am_filter(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_FILTER); }
static int am_any(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_ANY); }
static int am_all(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_ALL); }
static int am_find(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_FIND); }
static int am_reduce(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return hof(vm, argv, ret, H_REDUCE); }

static const NativeDesc array_methods[] = {
    {"each", 2, 2, am_each}, {"map", 2, 2, am_map}, {"filter", 2, 2, am_filter},
    {"any", 2, 2, am_any}, {"all", 2, 2, am_all}, {"find", 2, 2, am_find}, {"reduce", 3, 3, am_reduce},
    {"append", 2, 2, am_append}, {"extend", 2, 2, am_extend}, {"insert", 3, 3, am_insert},
    {"pop", 1, 1, am_pop}, {"remove_at", 2, 2, am_remove_at}, {"clear", 1, 1, am_clear},
    {"reserve", 2, 2, am_reserve}, {"capacity", 1, 1, am_capacity}, {"slice", 2, 3, am_slice},
    {"copy", 1, 1, am_copy}, {"index_of", 2, 2, am_index_of}, {"contains", 2, 2, am_contains},
    {"reverse", 1, 1, am_reverse}, {"join", 2, 2, am_join}, {"sort", 1, 2, am_sort},
    {NULL, 0, 0, NULL}};

/* Open-addressing tables over the method name hash, built on first use. */
enum { MT_SIZE = 128 };
static const NativeDesc *mtab[2][MT_SIZE];
static atomic_int mtab_state; /* 0 = unbuilt, 1 = building, 2 = ready (thread-safe lazy init) */

static void mtab_build(void) {
    int exp = 0;
    if (!atomic_compare_exchange_strong(&mtab_state, &exp, 1)) {
        while (atomic_load_explicit(&mtab_state, memory_order_acquire) != 2) { /* another VM thread is building */ }
        return;
    }
    for (int t = 0; t < 2; t++) {
        const NativeDesc *tbl = t == 0 ? array_methods : string_methods;
        for (const NativeDesc *d = tbl; d->name; d++) {
            uint32_t i = sp_hash_bytes(d->name, strlen(d->name)) & (MT_SIZE - 1);
            while (mtab[t][i]) i = (i + 1) & (MT_SIZE - 1);
            mtab[t][i] = d;
        }
    }
    atomic_store_explicit(&mtab_state, 2, memory_order_release);
}

static const NativeDesc *find_method(int type, const char *name, size_t len, uint32_t hash) {
    if (atomic_load_explicit(&mtab_state, memory_order_acquire) != 2) mtab_build();
    const NativeDesc *const *tab = mtab[type == T_ARRAY ? 0 : 1];
    for (uint32_t i = hash & (MT_SIZE - 1); tab[i]; i = (i + 1) & (MT_SIZE - 1))
        if (strncmp(tab[i]->name, name, len) == 0 && tab[i]->name[len] == 0) return tab[i];
    return NULL;
}

int sp_invoke_method_exists(int type, const char *name, size_t len) { return find_method(type, name, len, sp_hash_bytes(name, len)) != NULL; }

int lib_invoke_builtin_method_s(VM *vm, SpValue recv, const SString *name, int argc, SpValue *argv, SpValue *ret, int *found) {
    const NativeClass *ncls = recv.type == T_NATIVE ? AS_NATIVE(recv)->cls : NULL;
    size_t slot = (((uintptr_t)name >> 4) ^ ((uintptr_t)ncls >> 4) ^ (size_t)recv.type) & 255u;
    struct MCacheEnt *e = &vm->mcache[slot];
    const NativeDesc *d;
    if (e->name == name && e->ncls == ncls && e->type == recv.type) {
        d = e->d;
    } else {
        d = NULL;
        if (ncls) {
            for (const NativeDesc *m = ncls->methods; m && m->name; m++)
                if (strncmp(m->name, name->data, name->len) == 0 && m->name[name->len] == 0) { d = m; break; }
        } else {
            d = find_method(recv.type, name->data, name->len, str_hash_peek(name));
        }
        if (d) { e->name = name; e->ncls = ncls; e->type = recv.type; e->d = d; }
    }
    *found = d != NULL;
    if (!d) return 0;
    if (argc < d->min_args || argc > d->max_args)
        return vm_set_error(vm, E_TYPE, "%s.%s() called with %d argument%s", type_name(recv), d->name, argc - 1, argc - 1 == 1 ? "" : "s");
    return d->fn(vm, argc, argv, ret);
}

int lib_invoke_builtin_method(VM *vm, SpValue recv, const char *name, size_t len, uint32_t hash, int argc,
                              SpValue *argv, SpValue *ret, int *found) {
    const NativeDesc *d = NULL;
    if (recv.type == T_NATIVE) {
        for (const NativeDesc *m = AS_NATIVE(recv)->cls->methods; m && m->name; m++)
            if (strncmp(m->name, name, len) == 0 && m->name[len] == 0) { d = m; break; }
    } else {
        d = find_method(recv.type, name, len, hash);
    }
    *found = d != NULL;
    if (!d) return 0;
    if (argc < d->min_args || argc > d->max_args)
        return vm_set_error(vm, E_TYPE, "%s.%s() called with %d argument%s", type_name(recv), d->name, argc - 1, argc - 1 == 1 ? "" : "s");
    return d->fn(vm, argc, argv, ret);
}
