/* Module table. */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* table module                                                      */
/* ---------------------------------------------------------------- */
static int need_table(VM *vm, SpValue v) {
    if (v.type != T_TABLE) return vm_set_error(vm, E_TYPE, "expected a table, not %s", type_name(v));
    return 0;
}

static int t_keys(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    STable *t = AS_TABLE(argv[0]);
    SArray *a = vm_new_array(vm, t->count);
    SpValue k;
    int64_t c = 0;
    while ((c = table_next(t, c, &k, NULL)) >= 0) a->items[a->len++] = k;
    *ret = v_obj(T_ARRAY, a);
    return 0;
}
static int t_values(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    STable *t = AS_TABLE(argv[0]);
    SArray *a = vm_new_array(vm, t->count);
    SpValue v;
    int64_t c = 0;
    while ((c = table_next(t, c, NULL, &v)) >= 0) a->items[a->len++] = v;
    *ret = v_obj(T_ARRAY, a);
    return 0;
}
static int t_items(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    STable *t = AS_TABLE(argv[0]);
    SArray *a = vm_new_array(vm, t->count);
    SpValue k, v;
    int64_t c = 0;
    while ((c = table_next(t, c, &k, &v)) >= 0) {
        SArray *pair = vm_new_array(vm, 2);
        pair->items[0] = k;
        pair->items[1] = v;
        pair->len = 2;
        a->items[a->len++] = v_obj(T_ARRAY, pair);
    }
    *ret = v_obj(T_ARRAY, a);
    return 0;
}
static int t_copy(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    STable *t = AS_TABLE(argv[0]);
    STable *n = vm_new_table(vm, t->count);
    SpValue k, v;
    int64_t c = 0;
    while ((c = table_next(t, c, &k, &v)) >= 0)
        if (table_set(vm, n, k, v)) return 1;
    *ret = v_obj(T_TABLE, n);
    return 0;
}
static int t_merge(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0]) || need_table(vm, argv[1])) return 1;
    STable *d = AS_TABLE(argv[0]), *s = AS_TABLE(argv[1]);
    SpValue k, v;
    int64_t c = 0;
    while ((c = table_next(s, c, &k, &v)) >= 0)
        if (table_set(vm, d, k, v)) return 1;
    *ret = argv[0];
    return 0;
}
static int t_clear(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    if (need_table(vm, argv[0])) return 1;
    STable *t = AS_TABLE(argv[0]);
    for (int64_t i = 0; i < t->nent; i++) { t->ents[i].key = v_nil(); t->ents[i].val = v_nil(); }
    for (int64_t i = 0; i < t->idxcap; i++) t->idx[i] = -1;
    t->nent = 0;
    t->count = 0;
    t->version++;
    return 0;
}
static int t_reserve(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    int64_t n;
    if (need_table(vm, argv[0]) || need_int(vm, argv[1], "capacity", &n)) return 1;
    if (n < 0) return vm_set_error(vm, E_VALUE, "capacity must not be negative");
    return table_reserve(vm, AS_TABLE(argv[0]), n);
}
static int t_get(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    SpValue out;
    int found = 0;
    if (table_get(vm, AS_TABLE(argv[0]), argv[1], &out, &found)) return 1;
    *ret = found ? out : argv[2];
    return 0;
}
static int t_get_required(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_table(vm, argv[0])) return 1;
    int found = 0;
    if (table_get(vm, AS_TABLE(argv[0]), argv[1], ret, &found)) return 1;
    if (!found) {
        Out m = {0};
        fmt_value(vm, argv[1], 0, 1, &m);
        char buf[200];
        Out w = {0};
        w.dst = buf;
        w.cap = sizeof buf - 1;
        fmt_value(vm, argv[1], 0, 1, &w);
        buf[w.len < w.cap ? w.len : w.cap] = 0;
        return vm_set_error(vm, E_KEY, "key %s not found", buf);
    }
    return 0;
}
const NativeDesc table_fns[] = {
    {"keys", 1, 1, t_keys}, {"values", 1, 1, t_values}, {"items", 1, 1, t_items},
    {"copy", 1, 1, t_copy}, {"merge", 2, 2, t_merge}, {"clear", 1, 1, t_clear},
    {"reserve", 2, 2, t_reserve}, {"get", 3, 3, t_get},
    {"get_required", 2, 2, t_get_required}, {NULL, 0, 0, NULL}};
