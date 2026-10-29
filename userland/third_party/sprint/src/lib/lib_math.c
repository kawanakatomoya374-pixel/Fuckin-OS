/* Module math. */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* math                                                              */
/* ---------------------------------------------------------------- */
#define MATH1(NAME, EXPR)                                                          \
    static int m_##NAME(VM *vm, int argc, SpValue *argv, SpValue *ret) {           \
        (void)argc;                                                                \
        double x;                                                                  \
        if (need_num(vm, argv[0], #NAME "() argument", &x)) return 1;              \
        *ret = v_float(EXPR);                                                      \
        return 0;                                                                  \
    }
MATH1(exp, exp(x))
MATH1(sin, sin(x))
MATH1(cos, cos(x))
MATH1(tan, tan(x))
MATH1(asin, asin(x))
MATH1(acos, acos(x))
MATH1(atan, atan(x))

static int m_sqrt(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    double x;
    if (need_num(vm, argv[0], "sqrt() argument", &x)) return 1;
    if (x < 0) return vm_set_error(vm, E_VALUE, "sqrt() of a negative number");
    *ret = v_float(sqrt(x));
    return 0;
}
static int m_log(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    double x, b = 0;
    if (need_num(vm, argv[0], "log() argument", &x)) return 1;
    if (x <= 0) return vm_set_error(vm, E_VALUE, "log() of a non-positive number");
    if (argc > 1 && argv[1].type != T_NIL) {
        if (need_num(vm, argv[1], "log() base", &b)) return 1;
        if (b <= 0 || sp_feq(b, 1.0)) return vm_set_error(vm, E_VALUE, "invalid log() base");
        *ret = v_float(log(x) / log(b));
    } else {
        *ret = v_float(log(x));
    }
    return 0;
}
static int m_atan2(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    double y, x;
    if (need_num(vm, argv[0], "atan2() argument", &y) || need_num(vm, argv[1], "atan2() argument", &x)) return 1;
    *ret = v_float(atan2(y, x));
    return 0;
}
static int m_abs(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type == T_INT) {
        if (argv[0].as.i == INT64_MIN) return vm_set_error(vm, E_ARITH, "abs() overflow");
        *ret = v_int(argv[0].as.i < 0 ? -argv[0].as.i : argv[0].as.i);
        return 0;
    }
    double x;
    if (need_num(vm, argv[0], "abs() argument", &x)) return 1;
    *ret = v_float(fabs(x));
    return 0;
}

static int cmp_pick(VM *vm, int argc, SpValue *argv, SpValue *ret, int want_max) {
    SpValue best = argv[0];
    for (int i = 0; i < argc; i++)
        if (!IS_NUM(argv[i])) return vm_set_error(vm, E_TYPE, "min()/max() require numbers, not %s", type_name(argv[i]));
    for (int i = 1; i < argc; i++) {
        int un;
        int c = num_cmp(argv[i], best, &un);
        if (un) { *ret = v_float((double)NAN); return 0; }
        if (want_max ? c > 0 : c < 0) best = argv[i];
    }
    if (IS_FLOAT(best) && isnan(best.as.f)) { *ret = best; return 0; }
    *ret = best;
    return 0;
}
static int m_min(VM *vm, int argc, SpValue *argv, SpValue *ret) { return cmp_pick(vm, argc, argv, ret, 0); }
static int m_max(VM *vm, int argc, SpValue *argv, SpValue *ret) { return cmp_pick(vm, argc, argv, ret, 1); }

static int m_clamp(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    for (int i = 0; i < 3; i++)
        if (!IS_NUM(argv[i])) return vm_set_error(vm, E_TYPE, "clamp() requires numbers");
    int un;
    if (num_cmp(argv[1], argv[2], &un) > 0) return vm_set_error(vm, E_VALUE, "clamp(): lo > hi");
    if (num_cmp(argv[0], argv[1], &un) < 0) *ret = argv[1];
    else if (num_cmp(argv[0], argv[2], &un) > 0) *ret = argv[2];
    else *ret = argv[0];
    return 0;
}

static int to_int_checked(VM *vm, double f, SpValue *ret) {
    if (isnan(f) || isinf(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0)
        return vm_set_error(vm, E_VALUE, "value out of int range");
    *ret = v_int((int64_t)f);
    return 0;
}
#define MATHI(NAME, EXPR)                                                          \
    static int m_##NAME(VM *vm, int argc, SpValue *argv, SpValue *ret) {           \
        (void)argc;                                                                \
        if (argv[0].type == T_INT) { *ret = argv[0]; return 0; }                   \
        double x;                                                                  \
        if (need_num(vm, argv[0], #NAME "() argument", &x)) return 1;              \
        return to_int_checked(vm, EXPR, ret);                                      \
    }
MATHI(floor, floor(x))
MATHI(ceil, ceil(x))
MATHI(trunc, trunc(x))
MATHI(round, rint(x))

#define MATHB(NAME, EXPR)                                                          \
    static int m_##NAME(VM *vm, int argc, SpValue *argv, SpValue *ret) {           \
        (void)argc;                                                                \
        double x;                                                                  \
        if (need_num(vm, argv[0], #NAME "() argument", &x)) return 1;              \
        *ret = v_bool(EXPR);                                                       \
        return 0;                                                                  \
    }
MATHB(is_nan, isnan(x))
MATHB(is_inf, isinf(x))
MATHB(is_finite, isfinite(x))

static int m_is_close(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    double a, b, rel = 1e-9, abs_t = 0.0;
    if (need_num(vm, argv[0], "is_close() argument", &a) || need_num(vm, argv[1], "is_close() argument", &b)) return 1;
    if (argc > 2 && need_num(vm, argv[2], "rel_tol", &rel)) return 1;
    if (argc > 3 && need_num(vm, argv[3], "abs_tol", &abs_t)) return 1;
    double diff = fabs(a - b);
    double m = fmax(fabs(a), fabs(b));
    *ret = v_bool(sp_feq(a, b) || diff <= fmax(rel * m, abs_t));
    return 0;
}

const NativeDesc math_fns[] = {
    {"abs", 1, 1, m_abs}, {"min", 1, -1, m_min}, {"max", 1, -1, m_max},
    {"min2", 2, 2, m_min}, {"max2", 2, 2, m_max}, {"clamp", 3, 3, m_clamp},
    {"floor", 1, 1, m_floor}, {"ceil", 1, 1, m_ceil}, {"trunc", 1, 1, m_trunc},
    {"round", 1, 1, m_round}, {"sqrt", 1, 1, m_sqrt}, {"exp", 1, 1, m_exp},
    {"log", 1, 2, m_log}, {"sin", 1, 1, m_sin}, {"cos", 1, 1, m_cos}, {"tan", 1, 1, m_tan},
    {"asin", 1, 1, m_asin}, {"acos", 1, 1, m_acos}, {"atan", 1, 1, m_atan},
    {"atan2", 2, 2, m_atan2}, {"is_nan", 1, 1, m_is_nan}, {"is_inf", 1, 1, m_is_inf},
    {"is_finite", 1, 1, m_is_finite}, {"is_close", 2, 4, m_is_close}, {NULL, 0, 0, NULL}};
const int math_fns_count = (int)(sizeof math_fns / sizeof math_fns[0]) - 1;

/* the VM's shortcut for math.sqrt(x) etc. with a float argument: no call frame, no error path. 0 = not handled */
int math_fast1(const NativeDesc *d, double x, double *out) {
    NativeFn f = d->fn;
    if (f == m_sqrt) { if (x < 0) return 0; *out = sqrt(x); return 1; }
    if (f == m_sin) { *out = sin(x); return 1; }
    if (f == m_cos) { *out = cos(x); return 1; }
    if (f == m_exp) { *out = exp(x); return 1; }
    if (f == m_tan) { *out = tan(x); return 1; }
    if (f == m_atan) { *out = atan(x); return 1; }
    return 0;
}
/* natives the JIT may call through a helper and, when they report an error, simply run again in the interpreter:
 * pure (no output, no allocation, no state), so a second call is indistinguishable */
int math_pure_native(const NativeDesc *d) {
    NativeFn f = d->fn;
    return f == m_abs || f == m_min || f == m_max || f == m_clamp || f == m_floor || f == m_ceil || f == m_trunc ||
           f == m_round || f == m_log || f == m_asin || f == m_acos || f == m_atan2 || f == m_is_nan || f == m_is_inf ||
           f == m_is_finite || f == m_is_close;
}
const ModConst math_consts[] = {
    {"pi", 1, 0, 3.14159265358979323846}, {"e", 1, 0, 2.71828182845904523536},
    {"inf", 1, 0, (double)INFINITY}, {"nan", 1, 0, (double)NAN},
    {"int_max", 0, INT64_MAX, 0}, {"int_min", 0, INT64_MIN, 0}, {NULL, 0, 0, 0}};
