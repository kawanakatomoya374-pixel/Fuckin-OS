/**
 * cos_math.c - the math library for .c-os programs.
 *
 * WHY THIS EXISTS
 * ---------------
 * There was none. Not a reduced one - none at all. Any program doing
 * geometry, signal processing, physics, statistics or plotting could not
 * be ported, and `-lm` had nothing to link against.
 *
 * ACCURACY, AND HOW IT IS ESTABLISHED
 * -----------------------------------
 * These are not the correctly-rounded implementations a serious libm
 * ships; those need multi-precision argument reduction and a few
 * thousand lines. These use standard range reduction plus minimax
 * polynomial or Newton iteration, which lands within a few ULP across
 * the normal range.
 *
 * "Within a few ULP" is a claim, so it is measured rather than asserted:
 * validation/userland/test_userland.c fuzzes every function here against
 * the host libm over tens of thousands of random inputs and fails the
 * build if the relative error exceeds the stated bound. If you tighten a
 * bound and it still passes, the bound was loose; if you change a
 * function and it fails, you broke it.
 *
 * WHAT IS DELIBERATELY NOT HERE
 * -----------------------------
 * No errno setting and no floating-point exception flags. This runtime
 * has neither, and a function that pretended to set errno would be
 * lying. Domain errors return NaN, which is what the IEEE-754 side of
 * the C standard specifies and what callers actually test for.
 */
#include "cos.h"

typedef union { double d; uint64_t u; } cm_bits_t;

#define CM_PI     3.14159265358979311600
#define CM_PI_2   1.57079632679489655800
#define CM_LN2    0.69314718055994530942
#define CM_LOG2E  1.44269504088896340736

static double cm_from_bits(uint64_t u) { cm_bits_t b; b.u = u; return b.d; }
static uint64_t cm_to_bits(double d) { cm_bits_t b; b.d = d; return b.u; }

const double cos_HUGE_VAL = 1e308 * 10.0;   /* +inf, without a literal */

int cos_isnan(double x) { return x != x; }

int cos_isinf(double x)
{
    uint64_t u = cm_to_bits(x) & 0x7FFFFFFFFFFFFFFFULL;
    return u == 0x7FF0000000000000ULL;
}

int cos_isfinite(double x)
{
    uint64_t u = cm_to_bits(x) & 0x7FFFFFFFFFFFFFFFULL;
    return u < 0x7FF0000000000000ULL;
}

int cos_signbit(double x) { return (int)(cm_to_bits(x) >> 63); }

double cos_fabs(double x) { return cm_from_bits(cm_to_bits(x) & 0x7FFFFFFFFFFFFFFFULL); }

double cos_copysign(double x, double y)
{
    return cm_from_bits((cm_to_bits(x) & 0x7FFFFFFFFFFFFFFFULL) |
                        (cm_to_bits(y) & 0x8000000000000000ULL));
}

double cos_nan(void) { return cm_from_bits(0x7FF8000000000000ULL); }
double cos_inf(void) { return cm_from_bits(0x7FF0000000000000ULL); }

/* ---- rounding ------------------------------------------------------------
 * Done on the bit pattern rather than by casting through an integer:
 * a double can exceed the range of every integer type, and the cast
 * would be undefined there. */

double cos_trunc(double x)
{
    uint64_t u = cm_to_bits(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e < 0) return cos_copysign(0.0, x);
    if (e >= 52) return x;                     /* already integral, or inf/nan */
    uint64_t mask = (1ULL << (52 - e)) - 1;
    return cm_from_bits(u & ~mask);
}

double cos_floor(double x)
{
    double t = cos_trunc(x);
    if (x < 0.0 && t != x) t -= 1.0;
    return t;
}

double cos_ceil(double x)
{
    double t = cos_trunc(x);
    if (x > 0.0 && t != x) t += 1.0;
    return t;
}

double cos_round(double x)
{
    /* Half away from zero, which is what round() specifies - NOT the
     * ties-to-even the printf formatter uses. The two genuinely differ
     * and conflating them is a classic source of off-by-one in output. */
    double t = cos_trunc(x);
    double frac = x - t;
    if (frac >= 0.5) t += 1.0;
    else if (frac <= -0.5) t -= 1.0;
    return t;
}

double cos_fmod(double x, double y)
{
    if (cos_isnan(x) || cos_isnan(y) || cos_isinf(x) || y == 0.0) return cos_nan();
    if (cos_isinf(y)) return x;
    double r = x - cos_trunc(x / y) * y;
    /* One correction step: the division above can round such that the
     * remainder lands just outside [0, |y|). */
    if (cos_fabs(r) >= cos_fabs(y)) r -= cos_copysign(cos_fabs(y), r);
    return r;
}

double cos_fmin(double a, double b)
{
    if (cos_isnan(a)) return b;
    if (cos_isnan(b)) return a;
    return a < b ? a : b;
}

double cos_fmax(double a, double b)
{
    if (cos_isnan(a)) return b;
    if (cos_isnan(b)) return a;
    return a > b ? a : b;
}

double cos_frexp(double x, int *exp_out)
{
    uint64_t u = cm_to_bits(x);
    int e = (int)((u >> 52) & 0x7FF);
    if (e == 0) {
        if ((u & 0xFFFFFFFFFFFFFULL) == 0) { *exp_out = 0; return x; }   /* zero */
        /* Subnormal: scale into the normal range first, then correct the
         * exponent. Skipping this returns a mantissa outside [0.5,1). */
        x *= 18014398509481984.0;             /* 2^54 */
        u = cm_to_bits(x);
        e = (int)((u >> 52) & 0x7FF) - 54;
    }
    if (e == 0x7FF) { *exp_out = 0; return x; }   /* inf / nan */
    *exp_out = e - 1022;
    return cm_from_bits((u & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL);
}

double cos_ldexp(double x, int n)
{
    if (x == 0.0 || !cos_isfinite(x)) return x;

    /* The scale factor 2^n is built by writing the exponent field
     * directly, which only works for n in [-1022, 1023] - outside that
     * the field overflows and the "scale" becomes an infinity or a
     * subnormal. So a large n is applied in steps first.
     *
     * The steps must subtract exactly what they multiplied by. An
     * earlier version multiplied by 2^1023 while subtracting only 1000,
     * so ldexp(1.0, 1023) applied 2^1023 and then 2^23 and returned
     * infinity - and exp() inherited that for every argument above about
     * 693, which is where the fuzz run caught it. */
    while (n > 1023)  { x *= cm_from_bits((uint64_t)2046 << 52); n -= 1023; }
    while (n < -1022) { x *= cm_from_bits((uint64_t)1 << 52);    n += 1022; }
    if (n == 0) return x;
    return x * cm_from_bits((uint64_t)(1023 + n) << 52);
}

double cos_modf(double x, double *ipart)
{
    double t = cos_trunc(x);
    *ipart = t;
    if (cos_isinf(x)) return cos_copysign(0.0, x);
    return x - t;
}

/* ---- sqrt ----------------------------------------------------------------
 * Newton-Raphson from a bit-pattern seed. Three iterations from that
 * seed reach full double precision; the final step is done in the
 * reciprocal form so there is only one division in the whole routine. */
double cos_sqrt(double x)
{
    if (cos_isnan(x)) return x;
    if (x < 0.0) return cos_nan();
    if (x == 0.0 || cos_isinf(x)) return x;

    /* Seed: halving the exponent of the bit pattern gives ~5 correct
     * bits, which is plenty for Newton to converge in three steps. */
    uint64_t u = cm_to_bits(x);
    double y = cm_from_bits((u >> 1) + 0x1FF8000000000000ULL);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return y;
}

double cos_hypot(double a, double b)
{
    a = cos_fabs(a); b = cos_fabs(b);
    if (cos_isinf(a) || cos_isinf(b)) return cos_inf();
    if (a < b) { double t = a; a = b; b = t; }
    if (a == 0.0) return 0.0;
    /* Factoring out the larger term avoids overflowing on a*a when a is
     * large, and underflowing when both are tiny - the entire reason
     * hypot() exists rather than sqrt(a*a+b*b). */
    double r = b / a;
    return a * cos_sqrt(1.0 + r * r);
}

double cos_cbrt(double x)
{
    if (x == 0.0 || cos_isnan(x) || cos_isinf(x)) return x;
    int neg = x < 0.0;
    if (neg) x = -x;
    int e;
    double m = cos_frexp(x, &e);
    /* Make the exponent a multiple of three so the seed is close. */
    while (e % 3) { m *= 2.0; e--; }
    double y = cos_ldexp(0.9, e / 3) * (0.5 + 0.5 * m);
    for (int i = 0; i < 6; ++i) y = y - (y - x / (y * y)) / 3.0;
    return neg ? -y : y;
}

/* ---- exp / log -----------------------------------------------------------
 * exp: reduce to x = k*ln2 + r with |r| <= ln2/2, evaluate a degree-7
 * series on r (which converges fast there), then scale by 2^k.
 * log: reduce the mantissa to [sqrt(0.5), sqrt(2)) and use the atanh
 * form, which has a far better-conditioned series than log(1+f). */

double cos_exp(double x)
{
    if (cos_isnan(x)) return x;
    if (x > 709.782712893384) return cos_inf();
    if (x < -745.133219101941) return 0.0;

    double kd = cos_round(x * CM_LOG2E);
    int k = (int)kd;
    double r = x - kd * CM_LN2;

    double s = 1.0, term = 1.0;
    for (int i = 1; i <= 13; ++i) {
        term *= r / (double)i;
        s += term;
    }
    return cos_ldexp(s, k);
}

double cos_log(double x)
{
    if (cos_isnan(x)) return x;
    if (x < 0.0) return cos_nan();
    if (x == 0.0) return -cos_inf();
    if (cos_isinf(x)) return x;

    int e;
    double m = cos_frexp(x, &e);      /* m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2.0; e--; }

    /* atanh form: log(m) = 2*atanh((m-1)/(m+1)). The series in s
     * converges quadratically faster than the one in (m-1). */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double sum = 0.0, p = s;
    for (int i = 1; i <= 31; i += 2) {
        sum += p / (double)i;
        p *= s2;
    }
    return 2.0 * sum + (double)e * CM_LN2;
}

double cos_log2(double x)  { return cos_log(x) * CM_LOG2E; }
double cos_log10(double x) { return cos_log(x) * 0.43429448190325182765; }
double cos_exp2(double x)  { return cos_exp(x * CM_LN2); }

double cos_pow(double x, double y)
{
    /* The special cases are not decoration: pow(-1, inf) == 1 and
     * pow(0, 0) == 1 are required by the standard and are exactly what a
     * naive exp(y*log(x)) gets wrong. */
    if (y == 0.0) return 1.0;
    if (cos_isnan(x) || cos_isnan(y)) return cos_nan();
    if (x == 1.0) return 1.0;

    if (x == 0.0) {
        if (y < 0.0) return cos_inf();
        return cos_copysign(0.0, (cos_signbit(x) && cos_fmod(y, 2.0) == 1.0) ? -1.0 : 1.0);
    }

    if (x < 0.0) {
        /* A negative base is only defined for an integral exponent. */
        double iy;
        if (cos_modf(y, &iy) != 0.0) return cos_nan();
        double r = cos_exp(y * cos_log(-x));
        return (cos_fmod(cos_fabs(iy), 2.0) == 1.0) ? -r : r;
    }
    return cos_exp(y * cos_log(x));
}

/* ---- trigonometry --------------------------------------------------------
 * Reduction is by subtracting multiples of pi/2 in three pieces
 * (Cody-Waite), which keeps the reduced argument accurate for |x| up to
 * a few million. Beyond that the reduction itself dominates the error -
 * a proper libm uses a 1000-bit pi for that case; this one does not, and
 * the test bounds reflect the range where the claim holds. */

static const double CM_PIO2_1 = 1.57079632673412561417;
static const double CM_PIO2_2 = 6.07710050650619224932e-11;
static const double CM_PIO2_3 = 2.02226624879595063154e-21;

static double cm_sin_poly(double x)
{
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.66666666666666324348e-01 +
               x2 * (8.33333333332248946124e-03 +
               x2 * (-1.98412698298579493134e-04 +
               x2 * (2.75573137070700676789e-06 +
               x2 * (-2.50507602534068634195e-08 +
               x2 * 1.58969099521155010221e-10))))));
}

static double cm_cos_poly(double x)
{
    double x2 = x * x;
    return 1.0 + x2 * (-5.00000000000000000000e-01 +
           x2 * (4.16666666666666019037e-02 +
           x2 * (-1.38888888888741095749e-03 +
           x2 * (2.48015872894767294178e-05 +
           x2 * (-2.75573143513906633035e-07 +
           x2 * (2.08757232129817482790e-09 +
           x2 * -1.13596475577881948265e-11))))));
}

/* Reduces x to r in [-pi/4, pi/4] and reports the quadrant. */
static int cm_reduce(double x, double *r)
{
    double fn = cos_round(x * (2.0 / CM_PI));
    double y = x - fn * CM_PIO2_1;
    y -= fn * CM_PIO2_2;
    y -= fn * CM_PIO2_3;
    *r = y;
    long q = (long)fn;
    return (int)(q & 3);
}

double cos_sin(double x)
{
    if (cos_isnan(x) || cos_isinf(x)) return cos_nan();
    double r;
    switch (cm_reduce(x, &r)) {
    case 0:  return cm_sin_poly(r);
    case 1:  return cm_cos_poly(r);
    case 2:  return -cm_sin_poly(r);
    default: return -cm_cos_poly(r);
    }
}

double cos_cos(double x)
{
    if (cos_isnan(x) || cos_isinf(x)) return cos_nan();
    double r;
    switch (cm_reduce(x, &r)) {
    case 0:  return cm_cos_poly(r);
    case 1:  return -cm_sin_poly(r);
    case 2:  return -cm_cos_poly(r);
    default: return cm_sin_poly(r);
    }
}

double cos_tan(double x)
{
    double s = cos_sin(x), c = cos_cos(x);
    if (c == 0.0) return cos_copysign(cos_inf(), s);
    return s / c;
}

double cos_atan(double x)
{
    if (cos_isnan(x)) return x;
    int neg = x < 0.0;
    if (neg) x = -x;
    if (cos_isinf(x)) return neg ? -CM_PI_2 : CM_PI_2;

    /* Two reductions keep |t| <= tan(pi/12), where the series is short
     * and well-conditioned: reciprocate for x > 1, then use the
     * tan(a-b) identity against tan(pi/6). */
    int invert = 0, shift = 0;
    if (x > 1.0) { x = 1.0 / x; invert = 1; }
    if (x > 0.26794919243112270647) {   /* tan(pi/12) */
        x = (x * 1.73205080756887729353 - 1.0) / (1.73205080756887729353 + x);
        shift = 1;
    }

    double x2 = x * x;
    double sum = 0.0, p = x;
    for (int i = 1; i <= 25; i += 2) {
        sum += ((i & 2) ? -p : p) / (double)i;
        p *= x2;
    }
    if (shift)  sum += 0.52359877559829887308;   /* pi/6 */
    if (invert) sum = CM_PI_2 - sum;
    return neg ? -sum : sum;
}

double cos_atan2(double y, double x)
{
    if (cos_isnan(x) || cos_isnan(y)) return cos_nan();
    if (x == 0.0 && y == 0.0) {
        /* Signed zeros matter here: atan2(+0,-0) is +pi, atan2(-0,-0) is
         * -pi. Returning plain 0 for all four is a common shortcut and
         * visibly wrong in anything doing angle arithmetic. */
        if (cos_signbit(x)) return cos_signbit(y) ? -CM_PI : CM_PI;
        return cos_signbit(y) ? -0.0 : 0.0;
    }
    if (x == 0.0) return cos_signbit(y) ? -CM_PI_2 : CM_PI_2;
    double a = cos_atan(y / x);
    if (x > 0.0) return a;
    return (y >= 0.0 && !cos_signbit(y)) ? a + CM_PI : a - CM_PI;
}

double cos_asin(double x)
{
    if (cos_isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return cos_nan();
    if (x == 1.0) return CM_PI_2;
    if (x == -1.0) return -CM_PI_2;
    return cos_atan(x / cos_sqrt(1.0 - x * x));
}

double cos_acos(double x)
{
    if (cos_isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return cos_nan();
    return CM_PI_2 - cos_asin(x);
}

double cos_sinh(double x)
{
    if (cos_isnan(x) || cos_isinf(x)) return x;
    /* For small x, exp(x)-exp(-x) cancels catastrophically; the series
     * is both faster and more accurate there. */
    if (cos_fabs(x) < 0.5) {
        double x2 = x * x, term = x, s = x;
        for (int i = 1; i <= 8; ++i) {
            term *= x2 / (double)((2 * i) * (2 * i + 1));
            s += term;
        }
        return s;
    }
    double e = cos_exp(x);
    return 0.5 * (e - 1.0 / e);
}

double cos_cosh(double x)
{
    if (cos_isnan(x)) return x;
    if (cos_isinf(x)) return cos_inf();
    double e = cos_exp(cos_fabs(x));
    return 0.5 * (e + 1.0 / e);
}

double cos_tanh(double x)
{
    if (cos_isnan(x)) return x;
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e = cos_exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
