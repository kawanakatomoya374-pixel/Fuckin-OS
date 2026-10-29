/* Standard library, part 2: json, random, fs (SPEC 15.7, 15.10, 15.11). */
#define _XOPEN_SOURCE 700
#include "sp.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* helpers                                                           */
/* ---------------------------------------------------------------- */
static SpValue vstr(VM *vm, const char *s, size_t n) { return v_obj(T_STRING, vm_new_string(vm, s, n)); }

static int need_int(VM *vm, SpValue v, const char *what, int64_t *out) {
    if (v.type != T_INT) return vm_set_error(vm, E_TYPE, "%s must be int, not %s", what, type_name(v));
    *out = v.as.i;
    return 0;
}
static int need_str(VM *vm, SpValue v, const char *what) {
    if (v.type != T_STRING) return vm_set_error(vm, E_TYPE, "%s must be string, not %s", what, type_name(v));
    return 0;
}

/* Turn the pending error (set by vm_set_error) into an error value with `data`. */
static int fail_data(VM *vm, SpValue data) {
    int code = vm->pend_code;
    const char *kn = err_kind_name(code);
    SString *kind = vm_new_string(vm, kn, strlen(kn));
    SString *msg = vm_new_string(vm, vm->pend_msg, strlen(vm->pend_msg));
    SError *e = vm_make_error(vm, code, kind, msg);
    e->data = data;
    vm->pend_obj = v_obj(T_ERROR, e);
    return 1;
}

/* ================================================================ */
/* random                                                            */
/* ================================================================ */
static uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t rng_next(VM *vm) {
    uint64_t *s = vm->rng;
    uint64_t result = rotl64(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

static void rng_seed(VM *vm, uint64_t x) {
    for (int i = 0; i < 4; i++) {
        x += 0x9e3779b97f4a7c15u;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
        vm->rng[i] = z ^ (z >> 31);
    }
    vm->rng_seeded = 1;
}

static int rng_ready(VM *vm) {
    if (vm->rng_seeded) return 0;
    if (!(vm->caps & CAP_RANDOM) || vm->deterministic)
        return vm_set_error(vm, E_PERM, "random is not seeded; call random.seed(n) or run with --allow-random");
    uint64_t seed = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(&seed, sizeof seed, 1, f) != 1) seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    if (f) (void)fclose(f);
    rng_seed(vm, seed);
    return 0;
}

static int64_t rng_int(VM *vm, int64_t lo, int64_t hi) {
    uint64_t range = (uint64_t)hi - (uint64_t)lo;
    uint64_t x;
    if (range == UINT64_MAX) {
        x = rng_next(vm);
    } else {
        uint64_t m = range + 1u;
        uint64_t rem = (0u - m) % m; /* 2^64 mod m */
        do x = rng_next(vm); while (rem != 0 && x >= 0u - rem);
        x %= m;
    }
    return (int64_t)((uint64_t)lo + x);
}

static int r_seed(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    int64_t n;
    if (need_int(vm, argv[0], "seed", &n)) return 1;
    rng_seed(vm, (uint64_t)n);
    return 0;
}
static int r_int(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t lo, hi;
    if (need_int(vm, argv[0], "lo", &lo) || need_int(vm, argv[1], "hi", &hi)) return 1;
    if (lo > hi) return vm_set_error(vm, E_VALUE, "random.int(): lo must be <= hi");
    if (rng_ready(vm)) return 1;
    *ret = v_int(rng_int(vm, lo, hi));
    return 0;
}
static int r_float(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv;
    if (rng_ready(vm)) return 1;
    *ret = v_float(ldexp((double)(rng_next(vm) >> 11), -53));
    return 0;
}
static int r_shuffle(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    if (argv[0].type != T_ARRAY) return vm_set_error(vm, E_TYPE, "shuffle() requires an array");
    if (rng_ready(vm)) return 1;
    SArray *a = AS_ARRAY(argv[0]);
    for (int64_t i = a->len - 1; i >= 1; i--) {
        int64_t j = rng_int(vm, 0, i);
        SpValue t = a->items[i];
        a->items[i] = a->items[j];
        a->items[j] = t;
    }
    return 0;
}
static int r_choice(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_ARRAY) return vm_set_error(vm, E_TYPE, "choice() requires an array");
    SArray *a = AS_ARRAY(argv[0]);
    if (a->len == 0) return vm_set_error(vm, E_INDEX, "choice() from an empty array");
    if (rng_ready(vm)) return 1;
    *ret = a->items[rng_int(vm, 0, a->len - 1)];
    return 0;
}
const NativeDesc lib_random_fns[] = {
    {"seed", 1, 1, r_seed}, {"int", 2, 2, r_int}, {"float", 0, 0, r_float},
    {"shuffle", 1, 1, r_shuffle}, {"choice", 1, 1, r_choice}, {NULL, 0, 0, NULL}};

/* ================================================================ */
/* json                                                              */
/* ================================================================ */
#define JSON_MAX_DEPTH 512

typedef struct JParse {
    VM *vm;
    const char *s;
    size_t n, p;
    int failed;
} JParse;

static int j_fail(JParse *J, const char *msg) {
    VM *vm = J->vm;
    int line = 1, col = 1;
    for (size_t i = 0; i < J->p && i < J->n; i++) {
        if (J->s[i] == '\n') { line++; col = 1; }
        else if (((unsigned char)J->s[i] & 0xC0u) != 0x80u) col++;
    }
    (void)vm_set_error(vm, E_VALUE, "invalid JSON at line %d, column %d: %s", line, col, msg);
    STable *d = vm_new_table(vm, 2);
    table_set_str(vm, d, "line", 4, v_int(line));
    table_set_str(vm, d, "column", 6, v_int(col));
    (void)fail_data(vm, v_obj(T_TABLE, d));
    J->failed = 1;
    return 1;
}

static void j_ws(JParse *J) {
    while (J->p < J->n && (J->s[J->p] == ' ' || J->s[J->p] == '\t' || J->s[J->p] == '\n' || J->s[J->p] == '\r')) J->p++;
}

static int hex4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A') + 10u;
        else return 0;
        v = v * 16u + d;
    }
    *out = v;
    return 1;
}

static size_t put_utf8(char *d, uint32_t cp) {
    if (cp < 0x80u) { d[0] = (char)cp; return 1; }
    if (cp < 0x800u) { d[0] = (char)(0xC0u | (cp >> 6)); d[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    if (cp < 0x10000u) {
        d[0] = (char)(0xE0u | (cp >> 12)); d[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); d[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    d[0] = (char)(0xF0u | (cp >> 18)); d[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    d[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); d[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* parses a string literal at J->p (on the opening quote) */
static int j_string(JParse *J, SpValue *out) {
    VM *vm = J->vm;
    J->p++;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    if (!buf) vm_fatal(vm, RS_MEMORY, "out of memory");
    for (;;) {
        if (len + 8 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); vm_fatal(vm, RS_MEMORY, "out of memory"); }
            buf = nb;
        }
        if (J->p >= J->n) { free(buf); return j_fail(J, "unterminated string"); }
        unsigned char c = (unsigned char)J->s[J->p];
        if (c == '"') { J->p++; break; }
        if (c < 0x20) { free(buf); return j_fail(J, "control character in string"); }
        if (c != '\\') { buf[len++] = (char)c; J->p++; continue; }
        J->p++;
        if (J->p >= J->n) { free(buf); return j_fail(J, "unterminated escape"); }
        char e = J->s[J->p++];
        switch (e) {
        case '"': buf[len++] = '"'; break;
        case '\\': buf[len++] = '\\'; break;
        case '/': buf[len++] = '/'; break;
        case 'b': buf[len++] = '\b'; break;
        case 'f': buf[len++] = '\f'; break;
        case 'n': buf[len++] = '\n'; break;
        case 'r': buf[len++] = '\r'; break;
        case 't': buf[len++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (J->p + 4 > J->n || !hex4(J->s + J->p, &cp)) { free(buf); return j_fail(J, "bad \\u escape"); }
            J->p += 4;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                uint32_t lo;
                if (J->p + 6 > J->n || J->s[J->p] != '\\' || J->s[J->p + 1] != 'u' || !hex4(J->s + J->p + 2, &lo) ||
                    lo < 0xDC00u || lo > 0xDFFFu) {
                    free(buf);
                    return j_fail(J, "unpaired surrogate");
                }
                J->p += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                free(buf);
                return j_fail(J, "unpaired surrogate");
            }
            len += put_utf8(buf + len, cp);
            break;
        }
        default: free(buf); J->p--; return j_fail(J, "bad escape");
        }
    }
    *out = vstr(vm, buf, len);
    free(buf);
    return 0;
}

static int j_value(JParse *J, int depth, SpValue *out);

static int j_number(JParse *J, SpValue *out) {
    size_t st = J->p;
    int isint = 1;
    if (J->p < J->n && J->s[J->p] == '-') J->p++;
    if (J->p >= J->n || J->s[J->p] < '0' || J->s[J->p] > '9') return j_fail(J, "bad number");
    if (J->s[J->p] == '0') {
        J->p++;
        if (J->p < J->n && J->s[J->p] >= '0' && J->s[J->p] <= '9') return j_fail(J, "leading zero");
    } else {
        while (J->p < J->n && J->s[J->p] >= '0' && J->s[J->p] <= '9') J->p++;
    }
    if (J->p < J->n && J->s[J->p] == '.') {
        isint = 0;
        J->p++;
        if (J->p >= J->n || J->s[J->p] < '0' || J->s[J->p] > '9') return j_fail(J, "bad fraction");
        while (J->p < J->n && J->s[J->p] >= '0' && J->s[J->p] <= '9') J->p++;
    }
    if (J->p < J->n && (J->s[J->p] == 'e' || J->s[J->p] == 'E')) {
        isint = 0;
        J->p++;
        if (J->p < J->n && (J->s[J->p] == '+' || J->s[J->p] == '-')) J->p++;
        if (J->p >= J->n || J->s[J->p] < '0' || J->s[J->p] > '9') return j_fail(J, "bad exponent");
        while (J->p < J->n && J->s[J->p] >= '0' && J->s[J->p] <= '9') J->p++;
    }
    char tmp[512];
    size_t len = J->p - st;
    if (len >= sizeof tmp) return j_fail(J, "number too long");
    memcpy(tmp, J->s + st, len);
    tmp[len] = 0;
    if (isint) {
        errno = 0;
        char *end;
        long long v = strtoll(tmp, &end, 10);
        if (errno == 0) { *out = v_int((int64_t)v); return 0; }
    }
    errno = 0;
    double d = strtod(tmp, NULL);
    if (isinf(d)) { J->p = st; return j_fail(J, "number out of range"); }
    *out = v_float(d);
    return 0;
}

static int j_lit(JParse *J, const char *lit, SpValue v, SpValue *out) {
    size_t l = strlen(lit);
    if (J->p + l > J->n || memcmp(J->s + J->p, lit, l) != 0) return j_fail(J, "unexpected token");
    J->p += l;
    *out = v;
    return 0;
}

static int j_value(JParse *J, int depth, SpValue *out) {
    VM *vm = J->vm;
    j_ws(J);
    if (J->p >= J->n) return j_fail(J, "unexpected end of input");
    char c = J->s[J->p];
    if (c == '"') return j_string(J, out);
    if (c == '-' || (c >= '0' && c <= '9')) return j_number(J, out);
    if (c == 't') return j_lit(J, "true", v_bool(1), out);
    if (c == 'f') return j_lit(J, "false", v_bool(0), out);
    if (c == 'n') return j_lit(J, "null", v_nil(), out);
    if (c != '[' && c != '{') return j_fail(J, "unexpected character");
    if (depth >= JSON_MAX_DEPTH) return j_fail(J, "nesting too deep");
    J->p++;
    if (c == '[') {
        SArray *a = vm_new_array(vm, 4);
        *out = v_obj(T_ARRAY, a);
        j_ws(J);
        if (J->p < J->n && J->s[J->p] == ']') { J->p++; return 0; }
        for (;;) {
            SpValue e;
            if (j_value(J, depth + 1, &e)) return 1;
            (void)array_push(vm, a, e);
            j_ws(J);
            if (J->p >= J->n) return j_fail(J, "unterminated array");
            if (J->s[J->p] == ',') { J->p++; continue; }
            if (J->s[J->p] == ']') { J->p++; return 0; }
            return j_fail(J, "expected ',' or ']'");
        }
    }
    STable *t = vm_new_table(vm, 4);
    *out = v_obj(T_TABLE, t);
    j_ws(J);
    if (J->p < J->n && J->s[J->p] == '}') { J->p++; return 0; }
    for (;;) {
        j_ws(J);
        if (J->p >= J->n || J->s[J->p] != '"') return j_fail(J, "expected string key");
        SpValue k, v;
        if (j_string(J, &k)) return 1;
        j_ws(J);
        if (J->p >= J->n || J->s[J->p] != ':') return j_fail(J, "expected ':'");
        J->p++;
        if (j_value(J, depth + 1, &v)) return 1;
        (void)table_set(vm, t, k, v);
        j_ws(J);
        if (J->p >= J->n) return j_fail(J, "unterminated object");
        if (J->s[J->p] == ',') { J->p++; continue; }
        if (J->s[J->p] == '}') { J->p++; return 0; }
        return j_fail(J, "expected ',' or '}'");
    }
}

static int json_parse(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_str(vm, argv[0], "json.parse() input")) return 1;
    SString *s = AS_STR(argv[0]);
    JParse J = {vm, s->data, s->len, 0, 0};
    SpValue v = v_nil();
    if (j_value(&J, 0, &v)) return 1;
    j_ws(&J);
    if (J.p != J.n) return j_fail(&J, "unexpected trailing characters");
    *ret = v;
    return 0;
}

typedef struct JBuf {
    char *d;
    size_t len, cap;
} JBuf;

static void jb_put(VM *vm, JBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = realloc(b->d, nc);
        if (!nd) { free(b->d); vm_fatal(vm, RS_MEMORY, "out of memory"); }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
}

static void jb_str(VM *vm, JBuf *b, const SString *s) {
    jb_put(vm, b, "\"", 1);
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        char esc[8];
        switch (c) {
        case '"': jb_put(vm, b, "\\\"", 2); break;
        case '\\': jb_put(vm, b, "\\\\", 2); break;
        case '\b': jb_put(vm, b, "\\b", 2); break;
        case '\t': jb_put(vm, b, "\\t", 2); break;
        case '\n': jb_put(vm, b, "\\n", 2); break;
        case '\f': jb_put(vm, b, "\\f", 2); break;
        case '\r': jb_put(vm, b, "\\r", 2); break;
        default:
            if (c < 0x20) {
                (void)snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                jb_put(vm, b, esc, 6);
            } else {
                jb_put(vm, b, (const char *)&c, 1);
            }
        }
    }
    jb_put(vm, b, "\"", 1);
}

static void jb_nl(VM *vm, JBuf *b, int indent, int depth) {
    if (!indent) return;
    jb_put(vm, b, "\n", 1);
    for (int i = 0; i < depth * indent; i++) jb_put(vm, b, " ", 1);
}

static int j_emit(VM *vm, JBuf *b, SpValue v, int indent, int depth) {
    char num[48];
    switch (v.type) {
    case T_NIL: jb_put(vm, b, "null", 4); return 0;
    case T_BOOL: if (v.as.b) jb_put(vm, b, "true", 4); else jb_put(vm, b, "false", 5); return 0;
    case T_INT: {
        int k = snprintf(num, sizeof num, "%lld", (long long)v.as.i);
        jb_put(vm, b, num, (size_t)k);
        return 0;
    }
    case T_FLOAT:
        if (isnan(v.as.f) || isinf(v.as.f)) return vm_set_error(vm, E_VALUE, "json.stringify(): NaN and infinity cannot be encoded");
        fmt_float(v.as.f, num);
        jb_put(vm, b, num, strlen(num));
        return 0;
    case T_STRING: jb_str(vm, b, AS_STR(v)); return 0;
    case T_ARRAY: {
        if (depth >= JSON_MAX_DEPTH) return vm_set_error(vm, E_VALUE, "json.stringify(): nesting too deep (cyclic?)");
        SArray *a = AS_ARRAY(v);
        if (a->len == 0) { jb_put(vm, b, "[]", 2); return 0; }
        jb_put(vm, b, "[", 1);
        for (int64_t i = 0; i < a->len; i++) {
            if (i) jb_put(vm, b, ",", 1);
            jb_nl(vm, b, indent, depth + 1);
            if (j_emit(vm, b, a->items[i], indent, depth + 1)) return 1;
        }
        jb_nl(vm, b, indent, depth);
        jb_put(vm, b, "]", 1);
        return 0;
    }
    case T_TABLE: {
        if (depth >= JSON_MAX_DEPTH) return vm_set_error(vm, E_VALUE, "json.stringify(): nesting too deep (cyclic?)");
        STable *t = AS_TABLE(v);
        if (t->count == 0) { jb_put(vm, b, "{}", 2); return 0; }
        jb_put(vm, b, "{", 1);
        int64_t cur = 0;
        int first = 1;
        SpValue k, val;
        for (;;) {
            int64_t nx = table_next(t, cur, &k, &val);
            if (nx < 0) break;
            cur = nx;
            if (k.type != T_STRING) return vm_set_error(vm, E_TYPE, "json.stringify(): table keys must be strings, not %s", type_name(k));
            if (!first) jb_put(vm, b, ",", 1);
            first = 0;
            jb_nl(vm, b, indent, depth + 1);
            jb_str(vm, b, AS_STR(k));
            jb_put(vm, b, indent ? ": " : ":", indent ? 2u : 1u);
            if (j_emit(vm, b, val, indent, depth + 1)) return 1;
        }
        jb_nl(vm, b, indent, depth);
        jb_put(vm, b, "}", 1);
        return 0;
    }
    default:
        return vm_set_error(vm, E_TYPE, "json.stringify(): cannot encode a value of type %s", type_name(v));
    }
}

static int json_stringify(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    int64_t indent = 0;
    if (argc > 1 && need_int(vm, argv[1], "indent", &indent)) return 1;
    if (indent < 0 || indent > 8) return vm_set_error(vm, E_VALUE, "json.stringify(): indent must be 0..8");
    JBuf b = {0, 0, 0};
    int bad = j_emit(vm, &b, argv[0], (int)indent, 0);
    if (!bad) *ret = vstr(vm, b.d ? b.d : "", b.len);
    free(b.d);
    return bad;
}
const NativeDesc lib_json_fns[] = {
    {"parse", 1, 1, json_parse}, {"stringify", 1, 2, json_stringify}, {NULL, 0, 0, NULL}};

/* ================================================================ */
/* fs                                                                */
/* ================================================================ */
static int io_fail(VM *vm, int err, const char *op, const char *path) {
    (void)vm_set_error(vm, E_IO, "%s '%s': %s", op, path, strerror(err));
    STable *d = vm_new_table(vm, 3);
    table_set_str(vm, d, "errno", 5, v_int(err));
    table_set_str(vm, d, "op", 2, vstr(vm, op, strlen(op)));
    table_set_str(vm, d, "path", 4, vstr(vm, path, strlen(path)));
    return fail_data(vm, v_obj(T_TABLE, d));
}

static int within(const char *path, const char *dir) {
    size_t n = strlen(dir);
    if (strcmp(dir, "/") == 0) return 1;
    return strncmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == '/');
}

/* Resolve `path` (which may not exist yet) to an absolute canonical path and
 * check it against the capability. Returns 0, or 1 with an error pending. */
static int fs_check(VM *vm, SpValue pv, int write, const char *op, char *out);
int lib_fs_check(VM *vm, SpValue pv, int write, const char *op, char *out) { return fs_check(vm, pv, write, op, out); }
int lib_io_fail(VM *vm, int err, const char *op, const char *path) { return io_fail(vm, err, op, path); }
static int fs_check(VM *vm, SpValue pv, int write, const char *op, char *out) {
    if (need_str(vm, pv, "path")) return 1;
    const SString *ps = AS_STR(pv);
    if (ps->len == 0 || ps->len >= PATH_MAX - 2 || memchr(ps->data, 0, ps->len))
        return vm_set_error(vm, E_VALUE, "invalid path");
    int cap = write ? CAP_FS_WRITE : CAP_FS_READ;
    if (!(vm->caps & cap))
        return vm_set_error(vm, E_PERM, "%s access is not permitted (use --allow-%s=DIR)", write ? "filesystem write" : "filesystem read", write ? "write" : "read");
    char tmp[PATH_MAX];
    if (realpath(ps->data, tmp)) {
        (void)snprintf(out, PATH_MAX, "%s", tmp);
    } else if (errno == ENOENT) {
        char buf[PATH_MAX];
        (void)snprintf(buf, sizeof buf, "%s", ps->data);
        char *sl = strrchr(buf, '/');
        char dirp[PATH_MAX];
        const char *base;
        if (!sl) { (void)snprintf(dirp, sizeof dirp, "."); base = buf; }
        else if (sl == buf) { (void)snprintf(dirp, sizeof dirp, "/"); base = sl + 1; }
        else { *sl = 0; (void)snprintf(dirp, sizeof dirp, "%s", buf); base = sl + 1; }
        char rd[PATH_MAX];
        if (!realpath(dirp, rd)) return io_fail(vm, errno, op, ps->data);
        size_t rl = strcmp(rd, "/") == 0 ? 0 : strlen(rd), bl = strlen(base);
        if (rl + 1 + bl >= PATH_MAX) return io_fail(vm, ENAMETOOLONG, op, ps->data);
        memcpy(out, rd, rl);
        out[rl] = '/';
        memcpy(out + rl + 1, base, bl + 1);
    } else {
        return io_fail(vm, errno, op, ps->data);
    }
    const char *const *list = (const char *const *)(write ? vm->allow_write : vm->allow_read);
    int cnt = write ? vm->n_allow_write : vm->n_allow_read;
    for (int i = 0; i < cnt; i++)
        if (within(out, list[i])) return 0;
    return vm_set_error(vm, E_PERM, "'%s' is outside the permitted directories", ps->data);
}

static int fs_read_file(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    char rp[PATH_MAX];
    if (fs_check(vm, argv[0], 0, "read", rp)) return 1;
    size_t len = 0;
    char *b = sp_read_file(rp, &len);
    if (!b) return io_fail(vm, errno ? errno : EIO, "read", AS_STR(argv[0])->data);
    size_t bad;
    if (!utf8_validate(b, len, &bad)) {
        free(b);
        return vm_set_error(vm, E_IO, "'%s' is not valid UTF-8", AS_STR(argv[0])->data);
    }
    *ret = vstr(vm, b, len);
    free(b);
    return 0;
}

static int write_impl(VM *vm, SpValue *argv, int append) {
    char rp[PATH_MAX];
    const char *op = append ? "append" : "write";
    if (fs_check(vm, argv[0], 1, op, rp)) return 1;
    if (need_str(vm, argv[1], "data")) return 1;
    const SString *d = AS_STR(argv[1]);
    const char *orig = AS_STR(argv[0])->data;
    if (append) {
        FILE *f = fopen(rp, "ab");
        if (!f) return io_fail(vm, errno, op, orig);
        int ok = fwrite(d->data, 1, d->len, f) == d->len;
        int e = errno;
        if (fclose(f) != 0) ok = 0;
        return ok ? 0 : io_fail(vm, e ? e : EIO, op, orig);
    }
    char tmp[PATH_MAX + 32];
    (void)snprintf(tmp, sizeof tmp, "%.*s.sprint-tmp-%ld", PATH_MAX, rp, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return io_fail(vm, errno, op, orig);
    int ok = fwrite(d->data, 1, d->len, f) == d->len;
    int e = errno;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, rp) != 0) { ok = 0; e = errno; }
    if (!ok) {
        (void)unlink(tmp);
        return io_fail(vm, e ? e : EIO, op, orig);
    }
    return 0;
}
static int fs_write_file(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return write_impl(vm, argv, 0); }
static int fs_append_file(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return write_impl(vm, argv, 1); }

static int stat_impl(VM *vm, SpValue *argv, SpValue *ret, int kind) {
    char rp[PATH_MAX];
    if (fs_check(vm, argv[0], 0, "stat", rp)) return 1;
    struct stat st;
    int ok = stat(rp, &st) == 0;
    *ret = v_bool(ok && (kind == 0 || (kind == 1 && S_ISREG(st.st_mode)) || (kind == 2 && S_ISDIR(st.st_mode))));
    return 0;
}
static int fs_exists(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return stat_impl(vm, argv, ret, 0); }
static int fs_is_file(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return stat_impl(vm, argv, ret, 1); }
static int fs_is_dir(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; return stat_impl(vm, argv, ret, 2); }

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int fs_list_dir(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    char rp[PATH_MAX];
    if (fs_check(vm, argv[0], 0, "list", rp)) return 1;
    DIR *d = opendir(rp);
    if (!d) return io_fail(vm, errno, "list", AS_STR(argv[0])->data);
    size_t cap = 16, n = 0;
    char **names = malloc(cap * sizeof *names);
    if (!names) { (void)closedir(d); vm_fatal(vm, RS_MEMORY, "out of memory"); }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n == cap) {
            cap *= 2;
            char **nn = realloc(names, cap * sizeof *names);
            if (!nn) { (void)closedir(d); vm_fatal(vm, RS_MEMORY, "out of memory"); }
            names = nn;
        }
        size_t l = strlen(de->d_name) + 1;
        names[n] = malloc(l);
        if (!names[n]) { (void)closedir(d); vm_fatal(vm, RS_MEMORY, "out of memory"); }
        memcpy(names[n], de->d_name, l);
        n++;
    }
    (void)closedir(d);
    qsort(names, n, sizeof *names, cmp_names);
    SArray *a = vm_new_array(vm, (int64_t)n);
    *ret = v_obj(T_ARRAY, a);
    for (size_t i = 0; i < n; i++) {
        (void)array_push(vm, a, vstr(vm, names[i], strlen(names[i])));
        free(names[i]);
    }
    free(names);
    return 0;
}

static int fs_make_dir(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    char rp[PATH_MAX];
    if (fs_check(vm, argv[0], 1, "mkdir", rp)) return 1;
    if (mkdir(rp, 0777) != 0) return io_fail(vm, errno, "mkdir", AS_STR(argv[0])->data);
    return 0;
}
static int fs_remove(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    char rp[PATH_MAX];
    if (fs_check(vm, argv[0], 1, "remove", rp)) return 1;
    struct stat st;
    if (lstat(rp, &st) != 0) return io_fail(vm, errno, "remove", AS_STR(argv[0])->data);
    int r = S_ISDIR(st.st_mode) ? rmdir(rp) : unlink(rp);
    if (r != 0) return io_fail(vm, errno, "remove", AS_STR(argv[0])->data);
    return 0;
}
const NativeDesc lib_fs_fns[] = {
    {"read_file", 1, 1, fs_read_file}, {"write_file", 2, 2, fs_write_file},
    {"append_file", 2, 2, fs_append_file}, {"exists", 1, 1, fs_exists},
    {"is_file", 1, 1, fs_is_file}, {"is_dir", 1, 1, fs_is_dir},
    {"list_dir", 1, 1, fs_list_dir}, {"make_dir", 1, 1, fs_make_dir},
    {"remove", 1, 1, fs_remove}, {NULL, 0, 0, NULL}};

/* ---- capability configuration (called by the host before running) ---- */
int lib_allow_dir(VM *vm, int write, const char *dir) {
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) return -1;
    char ***list = write ? &vm->allow_write : &vm->allow_read;
    int *cnt = write ? &vm->n_allow_write : &vm->n_allow_read;
    char **nl = realloc(*list, sizeof(char *) * (size_t)(*cnt + 1));
    if (!nl) return -1;
    *list = nl;
    size_t l = strlen(rp) + 1;
    nl[*cnt] = malloc(l);
    if (!nl[*cnt]) return -1;
    memcpy(nl[*cnt], rp, l);
    (*cnt)++;
    vm->caps |= write ? CAP_FS_WRITE : CAP_FS_READ;
    return 0;
}

void lib_ext_free(VM *vm) {
    for (int i = 0; i < vm->n_allow_read; i++) free(vm->allow_read[i]);
    for (int i = 0; i < vm->n_allow_write; i++) free(vm->allow_write[i]);
    free(vm->allow_read);
    free(vm->allow_write);
    vm->allow_read = vm->allow_write = NULL;
    vm->n_allow_read = vm->n_allow_write = 0;
}
