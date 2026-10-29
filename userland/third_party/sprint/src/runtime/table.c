/* Ordered tables (insertion-ordered open hashing, SPEC 8.4). */
#include "value_int.h"

/* ---------------------------------------------------------------- */
/* tables                                                            */
/* ---------------------------------------------------------------- */
static uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static uint64_t hash_value(SpValue k) {
    switch (k.type) {
    case T_INT: return mix64((uint64_t)k.as.i);
    case T_FLOAT: { uint64_t b; memcpy(&b, &k.as.f, 8); return mix64(b); }
    case T_BOOL: return k.as.b ? 0x9e3779b97f4a7c15ull : 0x7f4a7c159e3779b9ull;
    case T_STRING: return mix64(str_hash(AS_STR(k)));
    default: return mix64(k.as.o->id * 0x9e3779b97f4a7c15ull);
    }
}

static int key_eq(SpValue a, SpValue b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case T_INT: return a.as.i == b.as.i;
    case T_FLOAT: return sp_feq(a.as.f, b.as.f);
    case T_BOOL: return a.as.b == b.as.b;
    case T_STRING: {
        SString *x = AS_STR(a), *y = AS_STR(b);
        return x == y || (x->len == y->len && str_hash(x) == str_hash(y) && memcmp(x->data, y->data, x->len) == 0);
    }
    default: return a.as.o == b.as.o;
    }
}

/* normalise a key for lookup; returns 0 ok, 1 error (pending) */
static int norm_key(VM *vm, SpValue *k) {
    if (k->type == T_NIL) return vm_set_error(vm, E_TYPE, "table key cannot be nil");
    if (k->type == T_FLOAT) {
        double f = k->as.f;
        if (isnan(f)) return vm_set_error(vm, E_VALUE, "table key cannot be NaN");
        if (sp_feq(f, floor(f)) && f >= -9223372036854775808.0 && f < 9223372036854775808.0)
            *k = v_int((int64_t)f);
    }
    return 0;
}

STable *vm_new_table(VM *vm, int64_t cap) {
    STable *t = new_obj(vm, sizeof *t, T_TABLE, 1);
    if (cap > 0) table_reserve(vm, t, cap);
    return t;
}

static int64_t t_find(const STable *t, SpValue k, uint64_t h, int64_t *slot_out) {
    if (!t->idx) return -1;
    uint64_t mask = (uint64_t)t->idxcap - 1;
    uint64_t i = h & mask;
    for (;;) {
        int32_t e = t->idx[i];
        if (e == -1) return -1;
        if (e >= 0 && key_eq(t->ents[e].key, k)) {
            if (slot_out) *slot_out = (int64_t)i;
            return e;
        }
        i = (i + 1) & mask;
    }
}

static void t_idx_insert(STable *t, int32_t e) {
    uint64_t mask = (uint64_t)t->idxcap - 1;
    uint64_t i = hash_value(t->ents[e].key) & mask;
    while (t->idx[i] >= 0) i = (i + 1) & mask;
    t->idx[i] = e;
}

static void t_rebuild(VM *vm, STable *t, int64_t newcap) {
    if ((uint64_t)newcap > vm->lim.max_table_entries) vm_fatal(vm, RS_MEMORY, "table size limit exceeded");
    int64_t icap = 8;
    while (icap < newcap * 2) icap <<= 1;
    TEntry *ne = vm_mem(vm, NULL, 0, (size_t)newcap * sizeof(TEntry));
    int32_t *ni = vm_mem(vm, NULL, 0, (size_t)icap * sizeof(int32_t));
    int64_t n = 0;
    for (int64_t i = 0; i < t->nent; i++)
        if (t->ents[i].key.type != T_NIL) ne[n++] = t->ents[i];
    if (t->ents) vm_mem(vm, t->ents, (size_t)t->cap * sizeof(TEntry), 0);
    if (t->idx) vm_mem(vm, t->idx, (size_t)t->idxcap * sizeof(int32_t), 0);
    t->ents = ne;
    t->cap = newcap;
    t->nent = n;
    t->idx = ni;
    t->idxcap = icap;
    for (int64_t i = 0; i < icap; i++) ni[i] = -1;
    for (int64_t i = 0; i < n; i++) t_idx_insert(t, (int32_t)i);
}

int table_reserve(VM *vm, STable *t, int64_t n) {
    if (n < t->count) n = t->count;
    if (n > t->cap || !t->ents) t_rebuild(vm, t, n < 4 ? 4 : n);
    return 0;
}

int table_get(VM *vm, STable *t, SpValue key, SpValue *out, int *found) {
    if (norm_key(vm, &key)) return 1;
    int64_t e = t_find(t, key, hash_value(key), NULL);
    if (e >= 0) { *out = t->ents[e].val; if (found) *found = 1; }
    else { *out = v_nil(); if (found) *found = 0; }
    return 0;
}

/* Same as table_get_str for a key that is already an SString (uses its cached hash). */
int table_get_sstr(const STable *t, const SString *key, SpValue *out) {
    if (!t->idx) return 0;
    uint32_t h32 = key->hash ? key->hash : str_hash_calc(key->data, key->len);
    uint64_t mask = (uint64_t)t->idxcap - 1;
    uint64_t i = mix64(h32) & mask;
    for (;;) {
        int32_t e = t->idx[i];
        if (e == -1) return 0;
        if (e >= 0) {
            const TEntry *te = &t->ents[e];
            if (te->key.type == T_STRING) {
                const SString *ks = AS_STR(te->key);
                if (ks == key || ((ks->hash == h32 || !ks->hash) && ks->len == key->len && memcmp(ks->data, key->data, key->len) == 0)) {
                    if (out) *out = te->val;
                    return 1;
                }
            }
        }
        i = (i + 1) & mask;
    }
}
int64_t table_find_sstr(const STable *t, const SString *key) {
    if (!t->idx) return -1;
    uint32_t h32 = key->hash ? key->hash : str_hash_calc(key->data, key->len);
    uint64_t mask = (uint64_t)t->idxcap - 1;
    uint64_t i = mix64(h32) & mask;
    for (;;) {
        int32_t e = t->idx[i];
        if (e == -1) return -1;
        if (e >= 0) {
            const TEntry *te = &t->ents[e];
            if (te->key.type == T_STRING) {
                const SString *ks = AS_STR(te->key);
                if (ks == key || ((ks->hash == h32 || !ks->hash) && ks->len == key->len && memcmp(ks->data, key->data, key->len) == 0))
                    return e;
            }
        }
        i = (i + 1) & mask;
    }
}
int table_get_str(STable *t, const char *s, size_t len, SpValue *out) {
    if (!t->idx) return 0;
    uint32_t h32 = str_hash_calc(s, len);
    uint64_t h = mix64(h32);
    uint64_t mask = (uint64_t)t->idxcap - 1;
    uint64_t i = h & mask;
    for (;;) {
        int32_t e = t->idx[i];
        if (e == -1) return 0;
        if (e >= 0) {
            SpValue k = t->ents[e].key;
            if (k.type == T_STRING && (AS_STR(k)->hash == h32 || !AS_STR(k)->hash) && AS_STR(k)->len == len &&
                memcmp(AS_STR(k)->data, s, len) == 0) {
                if (out) *out = t->ents[e].val;
                return 1;
            }
        }
        i = (i + 1) & mask;
    }
}

int table_set(VM *vm, STable *t, SpValue key, SpValue val) {
    if (norm_key(vm, &key)) return 1;
    uint64_t h = hash_value(key);
    int64_t slot = -1;
    int64_t e = t_find(t, key, h, &slot);
    if (val.type == T_NIL) { /* delete */
        if (e >= 0) {
            t->ents[e].key = v_nil();
            t->ents[e].val = v_nil();
            t->idx[slot] = -2;
            t->count--;
            t->version++;
        }
        return 0;
    }
    if (e >= 0) { t->ents[e].val = val; return 0; }
    if (t->nent == t->cap) {
        int64_t nc = t->count * 2 > t->cap ? (t->cap ? t->cap * 2 : 4) : (t->cap ? t->cap : 4);
        t_rebuild(vm, t, nc);
    }
    t->ents[t->nent].key = key;
    t->ents[t->nent].val = val;
    t_idx_insert(t, (int32_t)t->nent);
    t->nent++;
    t->count++;
    t->version++;
    return 0;
}

void table_set_str(VM *vm, STable *t, const char *s, size_t len, SpValue v) {
    SpValue k = v_obj(T_STRING, vm_new_string(vm, s, len));
    table_set(vm, t, k, v);
}

/* cursor iteration over live entries; returns next cursor or -1 at end */
int64_t table_next(STable *t, int64_t cursor, SpValue *k, SpValue *v) {
    while (cursor < t->nent) {
        if (t->ents[cursor].key.type != T_NIL) {
            if (k) *k = t->ents[cursor].key;
            if (v) *v = t->ents[cursor].val;
            return cursor + 1;
        }
        cursor++;
    }
    return -1;
}
