/* Module str and the string methods (SPEC 8). */
#include "lib_int.h"

static int cp_next(VM *vm, SIterator *it, SpValue out[2]) {
    (void)vm;
    const SString *s = AS_STR(it->held);
    if ((uint64_t)it->a >= s->len) return 1;
    const unsigned char *p = (const unsigned char *)s->data + it->a;
    int64_t cp;
    int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if (p[0] < 0xE0) { cp = ((int64_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    else if (p[0] < 0xF0) { cp = ((int64_t)(p[0] & 0x0F) << 12) | ((int64_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else { cp = ((int64_t)(p[0] & 0x07) << 18) | ((int64_t)(p[1] & 0x3F) << 12) | ((int64_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
    it->a += n;
    out[0] = v_int(cp);
    return 0;
}
static const IterClass cp_class = {"codepoints", 1, cp_next};

/* ---------------------------------------------------------------- */
/* str                                                               */
/* ---------------------------------------------------------------- */
static int format_pass(VM *vm, const SString *t, int argc, SpValue *args, Out *o) {
    int next = 0;
    for (uint32_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        if (c == '{') {
            if (i + 1 < t->len && t->data[i + 1] == '{') { out_put(o, "{", 1); i++; continue; }
            if (i + 1 < t->len && t->data[i + 1] == '}') {
                if (next >= argc) return vm_set_error(vm, E_VALUE, "format(): not enough arguments for template");
                fmt_value(vm, args[next++], 0, 0, o);
                i++;
                continue;
            }
            return vm_set_error(vm, E_VALUE, "format(): '{' must be followed by '}' or '{'");
        }
        if (c == '}') {
            if (i + 1 < t->len && t->data[i + 1] == '}') { out_put(o, "}", 1); i++; continue; }
            return vm_set_error(vm, E_VALUE, "format(): single '}' in template");
        }
        out_put(o, &c, 1);
    }
    if (next != argc) return vm_set_error(vm, E_VALUE, "format(): too many arguments for template");
    return 0;
}

static int s_format(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    if (need_str(vm, argv[0], "format template")) return 1;
    const SString *t = AS_STR(argv[0]);
    Out m = {0};
    if (format_pass(vm, t, argc - 1, argv + 1, &m)) return 1;
    SString *s = vm_alloc_string(vm, m.len);
    Out w = {0};
    w.dst = s->data;
    w.cap = m.len;
    (void)format_pass(vm, t, argc - 1, argv + 1, &w);
    string_finish(s);
    *ret = v_obj(T_STRING, s);
    return 0;
}

static size_t utf8_enc(char *dst, uint32_t cp) {
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) { dst[0] = (char)(0xC0 | (cp >> 6)); dst[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12)); dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18)); dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int s_from_codepoint(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t cp;
    if (need_int(vm, argv[0], "code point", &cp)) return 1;
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return vm_set_error(vm, E_VALUE, "not a Unicode scalar value");
    char b[4];
    size_t n = utf8_enc(b, (uint32_t)cp);
    *ret = vstr(vm, b, n);
    return 0;
}

int arr_join(VM *vm, SArray *a, SString *sep, SpValue *ret) {
    size_t total = 0;
    for (int64_t i = 0; i < a->len; i++) {
        if (a->items[i].type != T_STRING) return vm_set_error(vm, E_TYPE, "join() requires an array of strings");
        total += AS_STR(a->items[i])->len;
        if (i) total += sep->len;
    }
    SString *s = vm_alloc_string(vm, total);
    size_t p = 0;
    for (int64_t i = 0; i < a->len; i++) {
        if (i) { memcpy(s->data + p, sep->data, sep->len); p += sep->len; }
        SString *e = AS_STR(a->items[i]);
        memcpy(s->data + p, e->data, e->len);
        p += e->len;
    }
    string_finish(s);
    *ret = v_obj(T_STRING, s);
    return 0;
}

static int s_join(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_ARRAY) return vm_set_error(vm, E_TYPE, "join() requires an array");
    if (need_str(vm, argv[1], "separator")) return 1;
    return arr_join(vm, AS_ARRAY(argv[0]), AS_STR(argv[1]), ret);
}

const NativeDesc str_fns[] = {
    {"format", 1, -1, s_format}, {"from_codepoint", 1, 1, s_from_codepoint},
    {"join", 2, 2, s_join}, {NULL, 0, 0, NULL}};


/* ---------------------------------------------------------------- */
/* string methods                                                    */
/* ---------------------------------------------------------------- */
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static int is_boundary(const SString *s, int64_t i) {
    return i == 0 || i == (int64_t)s->len || (i > 0 && i < (int64_t)s->len && (((unsigned char)s->data[i]) & 0xC0) != 0x80);
}

static int sm_slice(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    SString *s = AS_STR(argv[0]);
    int64_t a, b = (int64_t)s->len;
    if (need_int(vm, argv[1], "start", &a)) return 1;
    if (argc > 2 && need_int(vm, argv[2], "end", &b)) return 1;
    if (a < 0 || b < 0 || a > (int64_t)s->len || b > (int64_t)s->len)
        return vm_set_error(vm, E_INDEX, "slice out of range for string of length %u", s->len);
    if (a > b) return vm_set_error(vm, E_VALUE, "slice start is after end");
    if (!is_boundary(s, a) || !is_boundary(s, b)) return vm_set_error(vm, E_VALUE, "slice boundary is inside a UTF-8 character");
    *ret = vstr(vm, s->data + a, (size_t)(b - a));
    return 0;
}
static int sm_find(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    SString *s = AS_STR(argv[0]);
    int64_t st = 0;
    if (need_str(vm, argv[1], "substring")) return 1;
    if (argc > 2 && need_int(vm, argv[2], "start", &st)) return 1;
    if (st < 0 || st > (int64_t)s->len) return vm_set_error(vm, E_INDEX, "find start out of range");
    SString *sub = AS_STR(argv[1]);
    *ret = v_int(find_bytes(s->data, s->len, sub->data, sub->len, (size_t)st));
    return 0;
}
int sm_contains(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "substring")) return 1;
    SString *s = AS_STR(argv[0]), *sub = AS_STR(argv[1]);
    *ret = v_bool(find_bytes(s->data, s->len, sub->data, sub->len, 0) >= 0);
    return 0;
}
static int sm_starts_with(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "prefix")) return 1;
    SString *s = AS_STR(argv[0]), *p = AS_STR(argv[1]);
    *ret = v_bool(p->len <= s->len && memcmp(s->data, p->data, p->len) == 0);
    return 0;
}
static int sm_ends_with(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "suffix")) return 1;
    SString *s = AS_STR(argv[0]), *p = AS_STR(argv[1]);
    *ret = v_bool(p->len <= s->len && memcmp(s->data + (s->len - p->len), p->data, p->len) == 0);
    return 0;
}
static int sm_count(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "substring")) return 1;
    SString *s = AS_STR(argv[0]), *sub = AS_STR(argv[1]);
    if (sub->len == 0) return vm_set_error(vm, E_VALUE, "count() of an empty substring");
    int64_t n = 0;
    size_t p = 0;
    for (;;) {
        int f = find_bytes(s->data, s->len, sub->data, sub->len, p);
        if (f < 0) break;
        n++;
        p = (size_t)f + sub->len;
    }
    *ret = v_int(n);
    return 0;
}
static int sm_split(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "separator")) return 1;
    SString *s = AS_STR(argv[0]), *sep = AS_STR(argv[1]);
    if (sep->len == 0) return vm_set_error(vm, E_VALUE, "split() separator must not be empty");
    SArray *a = vm_new_array(vm, 4);
    *ret = v_obj(T_ARRAY, a);
    size_t p = 0;
    for (;;) {
        int f = find_bytes(s->data, s->len, sep->data, sep->len, p);
        size_t e = f < 0 ? s->len : (size_t)f;
        array_push(vm, a, vstr(vm, s->data + p, e - p));
        if (f < 0) break;
        p = e + sep->len;
    }
    return 0;
}
static int sm_split_lines(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SString *s = AS_STR(argv[0]);
    SArray *a = vm_new_array(vm, 4);
    *ret = v_obj(T_ARRAY, a);
    size_t p = 0;
    while (p < s->len) {
        int f = find_bytes(s->data, s->len, "\n", 1, p);
        size_t e = f < 0 ? s->len : (size_t)f;
        array_push(vm, a, vstr(vm, s->data + p, e - p));
        p = e + 1;
    }
    return 0;
}
static int trim_impl(VM *vm, SpValue *argv, SpValue *ret, int left, int right) {
    SString *s = AS_STR(argv[0]);
    size_t b = 0, e = s->len;
    if (left) while (b < e && is_ws(s->data[b])) b++;
    if (right) while (e > b && is_ws(s->data[e - 1])) e--;
    *ret = (b == 0 && e == s->len) ? argv[0] : vstr(vm, s->data + b, e - b);
    return 0;
}
static int sm_trim(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return trim_impl(vm, argv, ret, 1, 1); }
static int sm_trim_start(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return trim_impl(vm, argv, ret, 1, 0); }
static int sm_trim_end(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return trim_impl(vm, argv, ret, 0, 1); }

static int case_impl(VM *vm, SpValue *argv, SpValue *ret, int upper) {
    SString *s = AS_STR(argv[0]);
    int change = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) { change = 1; break; }
    }
    if (!change) { *ret = argv[0]; return 0; }
    SString *r = vm_alloc_string(vm, s->len);
    for (uint32_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        r->data[i] = c;
    }
    string_finish(r);
    *ret = v_obj(T_STRING, r);
    return 0;
}
static int sm_upper(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return case_impl(vm, argv, ret, 1); }
static int sm_lower(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return case_impl(vm, argv, ret, 0); }

static int sm_replace(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[1], "old") || need_str(vm, argv[2], "new")) return 1;
    SString *s = AS_STR(argv[0]), *o = AS_STR(argv[1]), *n = AS_STR(argv[2]);
    if (o->len == 0) return vm_set_error(vm, E_VALUE, "replace() of an empty substring");
    size_t cnt = 0, p = 0;
    for (;;) {
        int f = find_bytes(s->data, s->len, o->data, o->len, p);
        if (f < 0) break;
        cnt++;
        p = (size_t)f + o->len;
    }
    if (cnt == 0) { *ret = argv[0]; return 0; }
    size_t total = s->len - cnt * o->len + cnt * n->len;
    SString *r = vm_alloc_string(vm, total);
    size_t w = 0;
    p = 0;
    for (;;) {
        int f = find_bytes(s->data, s->len, o->data, o->len, p);
        size_t e = f < 0 ? s->len : (size_t)f;
        memcpy(r->data + w, s->data + p, e - p);
        w += e - p;
        if (f < 0) break;
        memcpy(r->data + w, n->data, n->len);
        w += n->len;
        p = e + o->len;
    }
    string_finish(r);
    *ret = v_obj(T_STRING, r);
    return 0;
}
static int sm_repeat(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t n;
    if (need_int(vm, argv[1], "count", &n)) return 1;
    if (n < 0) return vm_set_error(vm, E_VALUE, "repeat() count must not be negative");
    SString *s = AS_STR(argv[0]);
    if (s->len && (uint64_t)n > vm->lim.max_string_bytes / s->len) vm_fatal(vm, RS_MEMORY, "string length limit exceeded");
    SString *r = vm_alloc_string(vm, s->len * (size_t)n);
    for (int64_t i = 0; i < n; i++) memcpy(r->data + (size_t)i * s->len, s->data, s->len);
    string_finish(r);
    *ret = v_obj(T_STRING, r);
    return 0;
}
static int sm_char_count(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)vm; (void)argc;
    SString *s = AS_STR(argv[0]);
    int64_t n = 0;
    for (uint32_t i = 0; i < s->len; i++) if ((((unsigned char)s->data[i]) & 0xC0) != 0x80) n++;
    *ret = v_int(n);
    return 0;
}
static int sm_char_at(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t i;
    if (need_int(vm, argv[1], "index", &i)) return 1;
    SString *s = AS_STR(argv[0]);
    if (i < 0 || i >= (int64_t)s->len) return vm_set_error(vm, E_INDEX, "char_at() index out of range");
    if (!is_boundary(s, i)) return vm_set_error(vm, E_VALUE, "char_at() index is not on a character boundary");
    const unsigned char *p = (const unsigned char *)s->data + i;
    uint32_t cp;
    if (p[0] < 0x80) cp = p[0];
    else if (p[0] < 0xE0) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3Fu);
    else if (p[0] < 0xF0) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3Fu);
    else cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3Fu);
    *ret = v_int(cp);
    return 0;
}
static int sm_is_boundary(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t i;
    if (need_int(vm, argv[1], "index", &i)) return 1;
    SString *s = AS_STR(argv[0]);
    *ret = v_bool(i >= 0 && i <= (int64_t)s->len && is_boundary(s, i));
    return 0;
}

static int sm_codepoints(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    *ret = v_obj(T_ITERATOR, vm_new_iterator(vm, &cp_class, argv[0], 0, 0, 0));
    return 0;
}

const NativeDesc string_methods[] = {
    {"slice", 2, 3, sm_slice}, {"find", 2, 3, sm_find}, {"contains", 2, 2, sm_contains},
    {"starts_with", 2, 2, sm_starts_with}, {"ends_with", 2, 2, sm_ends_with},
    {"count", 2, 2, sm_count}, {"split", 2, 2, sm_split}, {"split_lines", 1, 1, sm_split_lines},
    {"trim", 1, 1, sm_trim}, {"trim_start", 1, 1, sm_trim_start}, {"trim_end", 1, 1, sm_trim_end},
    {"upper", 1, 1, sm_upper}, {"lower", 1, 1, sm_lower}, {"replace", 3, 3, sm_replace},
    {"repeat", 2, 2, sm_repeat}, {"char_count", 1, 1, sm_char_count}, {"char_at", 2, 2, sm_char_at},
    {"is_boundary", 2, 2, sm_is_boundary}, {"codepoints", 1, 1, sm_codepoints}, {NULL, 0, 0, NULL}};
