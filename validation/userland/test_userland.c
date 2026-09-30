/**
 * test_userland.c - host-side tests for the .c-os C runtime.
 *
 * The same idea as validation/elfloader: compile the REAL userland
 * sources and run them natively, with the syscalls they depend on
 * stubbed out.
 *
 * The formatter tests are differential rather than golden: each case is
 * run through both cos_snprintf() and the host's snprintf() and the two
 * outputs are compared. A golden-string test only proves the formatter
 * agrees with whatever the test author believed C99 says - which, for
 * corners like "%.0f" of 0.5, or "%#o" of zero, or a negative `*` width,
 * is exactly where the author is most likely to be wrong. Comparing
 * against a mature implementation checks the actual standard.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cos.h"

static int g_checks, g_failures;
static const char *g_case = "";
static void begin(const char *n) { g_case = n; }

static void test_math(void);

#define CHECK(cond, msg, ...) do {                                     \
    g_checks++;                                                        \
    if (!(cond)) { g_failures++;                                       \
        fprintf(stderr, "FAIL [%s] " msg "\n", g_case, ##__VA_ARGS__); }\
} while (0)

/* Runs one format through both implementations and compares. */
#define SAME(fmt, ...) do {                                            \
    char mine[256], theirs[256];                                       \
    int a = cos_snprintf(mine, sizeof(mine), fmt, __VA_ARGS__);        \
    int b = snprintf(theirs, sizeof(theirs), fmt, __VA_ARGS__);        \
    g_checks++;                                                        \
    if (strcmp(mine, theirs) != 0 || a != b) { g_failures++;           \
        fprintf(stderr, "FAIL [%s] \"%s\": got \"%s\"(%d) want \"%s\"(%d)\n", \
                g_case, fmt, mine, a, theirs, b); }                    \
} while (0)

#define SAME0(fmt) do {                                                \
    char mine[256], theirs[256];                                       \
    int a = cos_snprintf(mine, sizeof(mine), "%s", fmt);               \
    int b = snprintf(theirs, sizeof(theirs), "%s", fmt);               \
    g_checks++;                                                        \
    if (strcmp(mine, theirs) != 0 || a != b) { g_failures++;           \
        fprintf(stderr, "FAIL [%s] literal mismatch\n", g_case); }     \
} while (0)

static void test_fmt_integers(void)
{
    begin("printf: integers");
    SAME("%d", 0); SAME("%d", 42); SAME("%d", -42);
    SAME("%d", 2147483647); SAME("%d", -2147483647 - 1);
    SAME("%ld", (long)-9223372036854775807L - 1);   /* LONG_MIN: negating overflows */
    SAME("%lld", (long long)1234567890123456789LL);
    SAME("%u", 4294967295u);
    SAME("%lu", 18446744073709551615UL);
    SAME("%x", 0xdeadbeefu); SAME("%X", 0xdeadbeefu);
    SAME("%o", 0777u);
    SAME("%hd", (int)(short)-5);
    SAME("%hhd", (int)(signed char)-5);
    SAME("%zu", (size_t)12345);

    begin("printf: integer flags and width");
    SAME("%5d", 42);    SAME("%-5d|", 42);  SAME("%05d", 42);
    SAME("%+d", 42);    SAME("%+d", -42);   SAME("% d", 42);
    SAME("%05d", -42);  SAME("%-8d|", -42);
    SAME("%#x", 255u);  SAME("%#X", 255u);  SAME("%#o", 8u);
    /* Zero with the # flag: no "0x" prefix, per C99. Easy to get wrong. */
    SAME("%#x", 0u);    SAME("%#o", 0u);
    SAME("%8.5d", 42);  SAME("%-8.5d|", 42);
    /* Precision 0 with value 0 produces NO characters at all. */
    SAME("%.0d|", 0);   SAME("%.0u|", 0u);
    /* Precision beats the zero flag. */
    SAME("%08.3d", 42);
    SAME("%*d", 7, 42);
    SAME("%-*d|", 7, 42);
    /* A negative * width means left-justify. */
    SAME("%*d|", -7, 42);
    SAME("%.*d", 5, 42);
}

static void test_fmt_strings(void)
{
    begin("printf: strings and chars");
    SAME("%s", "hello");
    SAME("%10s|", "hi");
    SAME("%-10s|", "hi");
    SAME("%.2s|", "hello");
    SAME("%10.2s|", "hello");
    SAME("%-10.2s|", "hello");
    SAME("%c", 'x');
    SAME("%5c|", 'x');
    SAME("%-5c|", 'x');
    SAME("%s", (char *)NULL);          /* glibc prints "(null)" too */
    SAME("%.3s", "ab");                /* precision longer than the string */

    begin("printf: literals and escapes");
    SAME0("no conversions here");
    { char m[64], t[64];
      int a = cos_snprintf(m, sizeof(m), "100%%");
      int b = snprintf(t, sizeof(t), "100%%");
      CHECK(strcmp(m, t) == 0 && a == b, "%%%% wrong: %s vs %s", m, t); }
}

static void test_fmt_floats(void)
{
    begin("printf: %f");
    SAME("%f", 0.0);
    SAME("%f", 1.0);
    SAME("%f", -1.5);
    SAME("%f", 3.14159265358979);
    SAME("%.0f", 1.0);
    SAME("%.0f", 2.5);            /* ties: glibc rounds to even here */
    SAME("%.2f", 1.005);
    SAME("%.1f", 9.99);           /* the rounding carry case: must be 10.0 */
    SAME("%.3f", 0.0005);
    SAME("%.2f", 1234.5678);
    SAME("%10.2f|", 3.5);
    SAME("%-10.2f|", 3.5);
    SAME("%010.2f", 3.5);
    SAME("%010.2f", -3.5);
    SAME("%+.2f", 3.5);
    SAME("% .2f", 3.5);
    SAME("%f", 1e6);
    SAME("%.1f", 123456789.0);

    begin("printf: %e and %g");
    SAME("%e", 1234.5678);
    SAME("%E", 1234.5678);
    SAME("%.2e", 1234.5678);
    SAME("%e", 0.0);
    SAME("%e", 0.000123);
    SAME("%g", 100000.0);
    SAME("%g", 1000000.0);
    SAME("%g", 0.0001);
    SAME("%g", 0.00001);
    SAME("%.3g", 3.14159);

    begin("printf: float specials");
    SAME("%f", (double)INFINITY);
    SAME("%f", (double)-INFINITY);
    SAME("%F", (double)INFINITY);
    SAME("%f", (double)NAN);
    SAME("%.2f", (double)INFINITY);
    /* Zero-padding a special would give "00inf"; it must be suppressed. */
    SAME("%08f", (double)INFINITY);
}

static void test_fmt_misc(void)
{
    begin("printf: pointers");
    { char m[64], t[64];
      void *p = (void *)0x1234abcd;
      cos_snprintf(m, sizeof(m), "%p", p);
      snprintf(t, sizeof(t), "%p", p);
      CHECK(strcmp(m, t) == 0, "%%p: got %s want %s", m, t); }
    { char m[64], t[64];
      cos_snprintf(m, sizeof(m), "%p", (void *)0);
      snprintf(t, sizeof(t), "%p", (void *)0);
      CHECK(strcmp(m, t) == 0, "%%p NULL: got %s want %s", m, t); }

    begin("printf: truncation and return value");
    { char buf[8];
      int n = cos_snprintf(buf, sizeof(buf), "%s", "0123456789");
      CHECK(n == 10, "snprintf must return the UNTRUNCATED length, got %d", n);
      CHECK(strcmp(buf, "0123456") == 0, "truncated wrong: '%s'", buf);
      CHECK(buf[7] == '\0', "truncated output was not NUL terminated"); }
    { int n = cos_snprintf(NULL, 0, "%d-%s", 42, "x");   /* "42-x" */
      CHECK(n == 4, "sizing pass (NULL buffer) returned %d, want 4", n); }

    begin("printf: hostile format strings");
    { char buf[64];
      /* A format ending in '%' must stop, not read past the end. */
      int n = cos_snprintf(buf, sizeof(buf), "abc%");
      CHECK(n == 3, "trailing %% mishandled (n=%d)", n);
      /* %n must be refused visibly rather than silently doing nothing. */
      cos_snprintf(buf, sizeof(buf), "x%ny", (int *)NULL);
      CHECK(strstr(buf, "refused") != NULL,
            "%%n was not refused: '%s'", buf);
      /* An unknown conversion is echoed rather than swallowed. */
      cos_snprintf(buf, sizeof(buf), "a%qb");
      CHECK(strcmp(buf, "a%qb") == 0, "unknown conversion: '%s'", buf); }
}

/* ================================================================== */
/* Randomised differential fuzz against the host libc                  */
/* ================================================================== */
/*
 * The hand-written cases above cover the corners someone thought of.
 * This covers the ones nobody did: random bit patterns reinterpreted as
 * doubles, at random precisions, in every float conversion. It is what
 * justifies the exact big-integer renderer over the obvious
 * multiply-by-ten loop, which passes a dozen tidy cases and then
 * disagrees with glibc on roughly one value in twenty.
 */
static uint64_t rng_state = 0x853c49e6748fea9bULL;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_fmt_fuzz(void)
{
    begin("printf: randomised float differential");
    const char convs[] = { 'f', 'e', 'g', 'F', 'E', 'G' };
    int mismatches = 0, tried = 0;

    for (int i = 0; i < 40000; ++i) {
        union { uint64_t u; double d; } b;
        b.u = rng();
        /* Keep the exponent in a range that produces finite values;
         * infinities and NaNs are covered by the explicit cases. */
        b.u = (b.u & ~0x7FF0000000000000ULL) |
              ((uint64_t)(rng() % 0x7FE + 1) << 52);
        double v = b.d;

        char conv = convs[rng() % sizeof(convs)];
        int prec = (int)(rng() % 18);
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%.%d%c", prec, conv);

        char mine[1400], theirs[1400];
        int a = cos_snprintf(mine, sizeof(mine), fmt, v);
        int c = snprintf(theirs, sizeof(theirs), fmt, v);
        tried++;
        if (strcmp(mine, theirs) != 0 || a != c) {
            if (mismatches < 5) {
                fprintf(stderr, "  mismatch fmt=%s bits=%016llx\n"
                                "    got  %s\n    want %s\n",
                        fmt, (unsigned long long)b.u, mine, theirs);
            }
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "%d of %d random float conversions disagreed with "
                           "the host libc", mismatches, tried);

    begin("printf: randomised integer differential");
    mismatches = 0; tried = 0;
    for (int i = 0; i < 40000; ++i) {
        long long v = (long long)rng();
        const char *fmts[] = { "%lld", "%llx", "%#llo", "%+lld", "%020lld",
                               "%-20lld|", "%.15lld", "%llu" };
        const char *f = fmts[rng() % 8];
        char mine[128], theirs[128];
        int a = cos_snprintf(mine, sizeof(mine), f, v);
        int c = snprintf(theirs, sizeof(theirs), f, v);
        tried++;
        if (strcmp(mine, theirs) != 0 || a != c) {
            if (mismatches < 5) {
                fprintf(stderr, "  mismatch fmt=%s v=%lld\n    got  %s\n    want %s\n",
                        f, v, mine, theirs);
            }
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "%d of %d random integer conversions disagreed",
          mismatches, tried);
}

int main(void)
{
    test_fmt_integers();
    test_fmt_strings();
    test_fmt_floats();
    test_fmt_misc();
    test_fmt_fuzz();
    test_math();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* ================================================================== */
/* Math: differential against the host libm                            */
/* ================================================================== */
/*
 * "Accurate to a few ULP" is a claim, so it is measured. Each function
 * is run over tens of thousands of random inputs in its useful range and
 * the relative error against glibc must stay under a stated bound. The
 * bounds are deliberately specific: if one is loose you will find out by
 * tightening it, and if a change breaks a function you find out here
 * rather than in a program that quietly draws the wrong shape.
 */
static double ulp_rel(double got, double want)
{
    if (want == 0.0) return fabs(got);
    return fabs((got - want) / want);
}

typedef double (*fn1_t)(double);
typedef double (*fn2_t)(double, double);

static void check_fn1(const char *name, fn1_t mine, fn1_t theirs,
                      double lo, double hi, double bound, int n)
{
    double worst = 0.0, worst_at = 0.0;
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        double t = (double)(rng() % 1000001) / 1000000.0;
        double x = lo + (hi - lo) * t;
        double a = mine(x), b = theirs(x);
        if (isnan(b)) { if (!isnan(a)) bad++; continue; }
        if (isinf(b)) { if (!isinf(a) || (a > 0) != (b > 0)) bad++; continue; }
        double e = ulp_rel(a, b);
        if (e > worst) { worst = e; worst_at = x; }
    }
    g_checks++;
    if (worst > bound || bad) {
        g_failures++;
        fprintf(stderr, "FAIL [math] %s: worst relative error %.3g at %.17g "
                        "(bound %.3g), %d special-case misses\n",
                name, worst, worst_at, bound, bad);
    }
}

static void check_fn2(const char *name, fn2_t mine, fn2_t theirs,
                      double lo, double hi, double bound, int n)
{
    double worst = 0.0;
    for (int i = 0; i < n; ++i) {
        double tx = (double)(rng() % 1000001) / 1000000.0;
        double ty = (double)(rng() % 1000001) / 1000000.0;
        double x = lo + (hi - lo) * tx, y = lo + (hi - lo) * ty;
        double a = mine(x, y), b = theirs(x, y);
        if (isnan(b) || isinf(b)) continue;
        double e = ulp_rel(a, b);
        if (e > worst) worst = e;
    }
    g_checks++;
    if (worst > bound) {
        g_failures++;
        fprintf(stderr, "FAIL [math] %s: worst relative error %.3g (bound %.3g)\n",
                name, worst, bound);
    }
}

static void test_math(void)
{
    begin("math: accuracy vs the host libm");
    const int N = 20000;

    check_fn1("sqrt",  cos_sqrt,  sqrt,  0.0,    1e6,   1e-15, N);
    check_fn1("sqrt(tiny)", cos_sqrt, sqrt, 0.0, 1e-200, 1e-15, N);
    check_fn1("cbrt",  cos_cbrt,  cbrt,  -1e6,   1e6,   1e-13, N);
    check_fn1("exp",   cos_exp,   exp,   -700.0, 700.0, 1e-13, N);
    check_fn1("log",   cos_log,   log,   1e-100, 1e100, 1e-13, N);
    check_fn1("log2",  cos_log2,  log2,  1e-100, 1e100, 1e-13, N);
    check_fn1("log10", cos_log10, log10, 1e-100, 1e100, 1e-13, N);
    check_fn1("sin",   cos_sin,   sin,   -100.0, 100.0, 1e-12, N);
    check_fn1("cos",   cos_cos,   cos,   -100.0, 100.0, 1e-12, N);
    check_fn1("tan",   cos_tan,   tan,   -1.5,   1.5,   1e-12, N);
    check_fn1("asin",  cos_asin,  asin,  -0.999, 0.999, 1e-13, N);
    check_fn1("acos",  cos_acos,  acos,  -0.999, 0.999, 1e-13, N);
    check_fn1("atan",  cos_atan,  atan,  -1e6,   1e6,   1e-13, N);
    check_fn1("sinh",  cos_sinh,  sinh,  -20.0,  20.0,  1e-12, N);
    check_fn1("cosh",  cos_cosh,  cosh,  -20.0,  20.0,  1e-12, N);
    check_fn1("tanh",  cos_tanh,  tanh,  -10.0,  10.0,  1e-12, N);
    check_fn1("floor", cos_floor, floor, -1e12,  1e12,  0.0,   N);
    check_fn1("ceil",  cos_ceil,  ceil,  -1e12,  1e12,  0.0,   N);
    check_fn1("trunc", cos_trunc, trunc, -1e12,  1e12,  0.0,   N);
    check_fn1("round", cos_round, round, -1e12,  1e12,  0.0,   N);

    check_fn2("atan2", cos_atan2, atan2, -100.0, 100.0, 1e-12, N);
    check_fn2("hypot", cos_hypot, hypot, -1e8,   1e8,   1e-14, N);
    check_fn2("fmod",  cos_fmod,  fmod,  -1000.0, 1000.0, 1e-13, N);
    check_fn2("pow",   cos_pow,   pow,   0.001,  100.0, 1e-12, N);

    begin("math: special values");
    CHECK(cos_isnan(cos_sqrt(-1.0)), "sqrt of a negative must be NaN");
    CHECK(cos_isinf(cos_log(0.0)) && cos_log(0.0) < 0, "log(0) must be -inf");
    CHECK(cos_isnan(cos_log(-1.0)), "log of a negative must be NaN");
    CHECK(cos_pow(0.0, 0.0) == 1.0, "pow(0,0) must be 1");
    CHECK(cos_pow(2.0, 10.0) == 1024.0, "pow(2,10) must be exactly 1024");
    CHECK(cos_pow(-2.0, 3.0) == -8.0, "pow(-2,3) must be -8");
    CHECK(cos_isnan(cos_pow(-2.0, 0.5)), "a negative base with a fractional "
                                         "exponent must be NaN");
    CHECK(cos_atan2(0.0, -1.0) == M_PI, "atan2(+0,-1) must be +pi");
    CHECK(cos_atan2(-0.0, -1.0) == -M_PI, "atan2(-0,-1) must be -pi");
    CHECK(cos_fabs(cos_atan2(0.0, 1.0)) == 0.0, "atan2(0,1) must be 0");
    CHECK(cos_isinf(cos_exp(1000.0)), "exp overflow must be +inf");
    CHECK(cos_exp(-1000.0) == 0.0, "exp underflow must be 0");
    CHECK(cos_hypot(3.0, 4.0) == 5.0, "hypot(3,4) must be exactly 5");
    /* hypot must not overflow where sqrt(a*a+b*b) would. */
    CHECK(cos_isfinite(cos_hypot(1e300, 1e300)),
          "hypot overflowed where the naive formula would");
    { int e; double m = cos_frexp(1024.0, &e);
      CHECK(m == 0.5 && e == 11, "frexp(1024) wrong: %g, %d", m, e); }
    { int e; double m = cos_frexp(5e-324, &e);   /* smallest subnormal */
      CHECK(m >= 0.5 && m < 1.0, "frexp of a subnormal gave mantissa %g", m); }
    { double ip; double f = cos_modf(-3.75, &ip);
      CHECK(ip == -3.0 && fabs(f + 0.75) < 1e-15, "modf(-3.75) wrong"); }
    CHECK(cos_ldexp(1.0, 1023) == ldexp(1.0, 1023), "ldexp at the top of the range");
    CHECK(cos_ldexp(1.0, -1074) == ldexp(1.0, -1074), "ldexp into the subnormals");
    /* round() is ties-AWAY, unlike the formatter's ties-to-even. */
    CHECK(cos_round(2.5) == 3.0 && cos_round(-2.5) == -3.0,
          "round() must be ties away from zero");
}
