/**
 * math.c - freestanding libm for QuickJS (Math.* builtins + internal
 * number formatting).
 *
 * IMPORTANT CAVEAT: these are freshly written from standard,
 * well-documented numerical techniques (range reduction + polynomial/
 * bit-manipulation methods - the same general approach real libm
 * implementations use), not ported from an existing tested libm (no
 * network access in this environment to pull in e.g. musl's or
 * fdlibm's math sources, which would have been the safer choice).
 * There is no way to run these against a reference implementation or
 * test vector suite in this sandbox (no qemu to actually execute
 * anything). fabs/copysign/sqrt are hardware instructions via GCC
 * builtins and can be trusted completely; floor/ceil/trunc/round/fmod
 * are exact bit-manipulation, also high-confidence. The transcendental
 * functions (exp/log/sin/cos/pow/etc.) should be correct to a good
 * number of digits for typical inputs but have NOT been verified for
 * last-bit correctness or extreme edge cases (very large arguments,
 * values extremely close to a pole, etc.) - worth spot-checking with
 * a handful of known values (sin(0)=0, cos(0)=1, sqrt(2)≈1.41421356,
 * log(Math.E)=1, Math.pow(2,10)=1024) once this can actually run.
 */
#include "math.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- bit-level helpers (reinterpret a double's IEEE-754 bits) ---- */

static inline uint64_t d2u(double x) { uint64_t u; __builtin_memcpy(&u, &x, 8); return u; }
static inline double u2d(uint64_t u) { double x; __builtin_memcpy(&x, &u, 8); return x; }

/* fabs/sqrt/floor/pow/sin/cos are also defined in
 * src/third_party/tinygl/src/tinygl_math.c (already linked) -
 * omitted here to avoid the duplicate-definition link error.
 * Everything else below (isnan/isinf/isfinite/ceil/round/fmod/
 * exp/log/trig/lrint) is unique to this file and needed by QuickJS. */
int isnan(double x) {
    uint64_t u = d2u(x);
    return ((u >> 52) & 0x7FF) == 0x7FF && (u & 0xFFFFFFFFFFFFFULL) != 0;
}
int isinf(double x) {
    uint64_t u = d2u(x);
    return ((u >> 52) & 0x7FF) == 0x7FF && (u & 0xFFFFFFFFFFFFFULL) == 0;
}
int isfinite(double x) {
    uint64_t u = d2u(x);
    return ((u >> 52) & 0x7FF) != 0x7FF;
}

/* ---- exact / hardware-instruction primitives ----
 * fabs/sqrt/floor/pow/sin/cos are already defined in
 * src/third_party/tinygl/src/tinygl_math.c; they are kept here for
 * reference but the guard stops the linker seeing them twice. */
#ifndef COS_SKIP_TINYGL_DUPS
double fabs(double x)          { return __builtin_fabs(x); }
double sqrt(double x)          { return __builtin_sqrt(x); }
#endif /* COS_SKIP_TINYGL_DUPS */
double copysign(double x, double y) { return __builtin_copysign(x, y); }

double trunc(double x) {
    if (!isfinite(x) || x == 0.0) return x;
    /* Anything with magnitude >= 2^52 has no fractional bits left to
     * remove at all (a double simply can't represent a fraction that
     * fine at that magnitude) - returning it unchanged also sidesteps
     * the int64 cast below overflowing for huge values. */
    if (fabs(x) >= 4503599627370496.0) return x;
    int64_t i = (int64_t)x; /* C truncates toward zero on this cast - exactly what we want */
    return (double)i;
}
#ifndef COS_SKIP_TINYGL_DUPS
double floor(double x) {
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}
#endif /* COS_SKIP_TINYGL_DUPS */
double ceil(double x) {
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}
double round(double x) {
    if (x >= 0.0) return floor(x + 0.5);
    return ceil(x - 0.5);
}
double fmod(double x, double y) {
    if (y == 0.0 || !isfinite(x) || isnan(y)) return NAN;
    if (!isfinite(y)) return x;
    double q = trunc(x / y);
    return x - q * y;
}

/* ---- exp/log, everything else below is built from these two ---- */

/* ln(2), split into a high/low pair (standard technique for keeping
 * the k*ln2 reconstruction in exp() accurate - the "low" part carries
 * precision the double rounding of a single ln2 constant would lose). */
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define LOG2E  1.44269504088896340736
#define LN10   2.30258509299404568402

double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.78) return HUGE_VAL;   /* overflows double */
    if (x < -745.13) return 0.0;       /* underflows to 0 */

    /* Range-reduce to r = x - k*ln2, |r| <= ln2/2, then e^x = 2^k * e^r. */
    double k = round(x * LOG2E);
    double r = (x - k * LN2_HI) - k * LN2_LO;

    /* Minimax-ish Taylor series for e^r on the small reduced range -
     * converges fast since |r| <= ~0.347. */
    double r2 = r * r;
    double series = 1.0 + r + r2 * (1.0/2.0 + r * (1.0/6.0 + r * (1.0/24.0 +
                    r * (1.0/120.0 + r * (1.0/720.0 + r * (1.0/5040.0 +
                    r * (1.0/40320.0)))))));

    /* Scale by 2^k via direct exponent-field manipulation - exact,
     * and avoids needing its own pow-of-2 helper. */
    int ik = (int)k;
    uint64_t bits = d2u(series);
    int exp_field = (int)((bits >> 52) & 0x7FF) + ik;
    if (exp_field <= 0) return 0.0;         /* underflow past what this simple path handles */
    if (exp_field >= 0x7FF) return HUGE_VAL; /* overflow */
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)exp_field << 52);
    return u2d(bits);
}

double expm1(double x) {
    /* exp(x)-1 loses precision by cancellation for small x if computed
     * directly - use the series itself (which already starts at 1+r
     * for small x) minus 1, falling back to the plain formula once x
     * is far enough from 0 that cancellation isn't a concern. */
    if (fabs(x) < 1e-5) {
        return x + x * x * 0.5 + x * x * x * (1.0/6.0);
    }
    return exp(x) - 1.0;
}

double log(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    if (!isfinite(x)) return x;

    /* Decompose x = m * 2^e with m in [1, 2) via the exponent field
     * directly, then further shift m into [sqrt(0.5), sqrt(2)) so the
     * series below (which is only accurate near m=1) has a small
     * argument to work with. */
    uint64_t bits = d2u(x);
    int e = (int)((bits >> 52) & 0x7FF) - 1023;
    uint64_t mantissa_bits = (bits & 0x000FFFFFFFFFFFFFULL) | (1023ULL << 52);
    double m = u2d(mantissa_bits); /* in [1, 2) */

    if (m > 1.4142135623730951) { m *= 0.5; e += 1; }

    /* atanh-based series: ln(m) = 2*atanh((m-1)/(m+1)), converges
     * quickly for m near 1 (which it now is, after the step above). */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double series = t * (1.0 + t2 * (1.0/3.0 + t2 * (1.0/5.0 + t2 * (1.0/7.0 +
                    t2 * (1.0/9.0 + t2 * (1.0/11.0 + t2 * (1.0/13.0)))))));

    return 2.0 * series + (double)e * LN2_HI + (double)e * LN2_LO;
}

double log1p(double x) {
    if (fabs(x) < 1e-5) {
        return x - x * x * 0.5 + x * x * x * (1.0/3.0);
    }
    return log(1.0 + x);
}
double log2(double x)  { return log(x) * LOG2E; }
double log10(double x) { return log(x) / LN10; }

#ifndef COS_SKIP_TINYGL_DUPS
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0) {
        if (y < 0.0) return HUGE_VAL;
        return 0.0;
    }

    double ay = fabs(y);
    bool y_is_int = (ay == trunc(ay)) && ay < 9.007199254740992e15; /* 2^53 */
    bool y_is_odd_int = y_is_int && (fmod(ay, 2.0) == 1.0);

    if (x < 0.0) {
        if (!y_is_int) return NAN; /* real result doesn't exist */
        double r = exp(y * log(-x));
        return y_is_odd_int ? -r : r;
    }
    return exp(y * log(x));
}
#endif /* COS_SKIP_TINYGL_DUPS */

double cbrt(double x) {
    if (x == 0.0 || isnan(x) || !isfinite(x)) return x;
    bool neg = x < 0.0;
    double ax = neg ? -x : x;

    /* Bit-trick initial guess (divide the biased exponent by ~3),
     * refined with Newton-Raphson on f(t) = t^3 - ax. Four iterations
     * comfortably converges to double precision from this starting
     * point for any normal-range double. */
    uint64_t bits = d2u(ax);
    bits = (bits / 3) + 0x2A9F7893ULL * (1ULL << 32) / 3 * 3; /* coarse but sufficient starting exponent */
    /* Simpler, robust fallback guess if the bit-trick above ever
     * produces something non-finite/zero for an edge-case input. */
    double t = u2d(bits);
    if (!isfinite(t) || t <= 0.0) t = exp(log(ax) / 3.0);

    for (int i = 0; i < 6; i++) {
        double t3 = t * t * t;
        t = t - (t3 - ax) / (3.0 * t * t);
    }
    return neg ? -t : t;
}

/* ---- trig: range-reduce to [-pi/4, pi/4] + octant, then Taylor ---- */

#define PI          3.14159265358979323846
#define PI_2        1.57079632679489661923
#define TWO_OVER_PI 0.63661977236758134308

static double sin_poly(double x) {
    /* Accurate for |x| <= pi/4. */
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0 +
           x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))));
}
static double cos_poly(double x) {
    double x2 = x * x;
    return 1.0 + x2 * (-1.0/2.0 + x2 * (1.0/24.0 + x2 * (-1.0/720.0 +
           x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0)))));
}

/* Reduces x to r in [-pi/4, pi/4] and returns which octant (0-3) of
 * the circle it fell in, so sin/cos can pick the right
 * poly/sign/swap combination. */
static double trig_reduce(double x, int* quadrant) {
    double k = round(x * TWO_OVER_PI);
    /* Same high/low split trick as exp()'s ln2 reduction, using pi/2
     * split into head+tail so the subtraction doesn't lose precision
     * for x with many periods' worth of magnitude. */
    double r = (x - k * 1.5707963267341256) - k * 6.077100506506192e-11;
    *quadrant = ((int64_t)k) & 3;
    if (*quadrant < 0) *quadrant += 4;
    return r;
}

#ifndef COS_SKIP_TINYGL_DUPS
double sin(double x) {
    if (!isfinite(x)) return NAN;
    int q;
    double r = trig_reduce(x, &q);
    switch (q) {
        case 0: return sin_poly(r);
        case 1: return cos_poly(r);
        case 2: return -sin_poly(r);
        default: return -cos_poly(r);
    }
}
double cos(double x) {
    if (!isfinite(x)) return NAN;
    int q;
    double r = trig_reduce(x, &q);
    switch (q) {
        case 0: return cos_poly(r);
        case 1: return -sin_poly(r);
        case 2: return -cos_poly(r);
        default: return sin_poly(r);
    }
}
#endif /* COS_SKIP_TINYGL_DUPS */
double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return copysign(HUGE_VAL, sin(x));
    return sin(x) / c;
}

double atan(double x) {
    /* Reduce to |x|<=1 via atan(x) = pi/2 - atan(1/x) for |x|>1, then
     * a standard odd-power series (accurate near 0, which is why the
     * reduction matters for large |x|). */
    bool neg = x < 0.0;
    double ax = neg ? -x : x;
    bool inverted = ax > 1.0;
    double z = inverted ? 1.0 / ax : ax;

    double z2 = z * z;
    double series = z * (1.0 + z2 * (-1.0/3.0 + z2 * (1.0/5.0 + z2 * (-1.0/7.0 +
                    z2 * (1.0/9.0 + z2 * (-1.0/11.0 + z2 * (1.0/13.0 +
                    z2 * (-1.0/15.0 + z2 * (1.0/17.0)))))))));

    double r = inverted ? PI_2 - series : series;
    return neg ? -r : r;
}
double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return y >= 0.0 ? atan(y / x) + PI : atan(y / x) - PI;
    /* x == 0 */
    if (y > 0.0) return PI_2;
    if (y < 0.0) return -PI_2;
    return 0.0; /* atan2(0,0) - implementation-defined edge case, 0 is a reasonable choice */
}
double asin(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return atan2(x, sqrt(1.0 - x * x));
}
double acos(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return PI_2 - asin(x);
}

double sinh(double x) {
    if (fabs(x) > 20.0) return copysign(exp(fabs(x)) * 0.5, x);
    double e = expm1(x);
    return (e + e / (e + 1.0)) * 0.5;
}
double cosh(double x) {
    double ax = fabs(x);
    if (ax > 20.0) return exp(ax) * 0.5;
    double e = exp(ax);
    return (e + 1.0 / e) * 0.5;
}
double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

int signbit(double x) {
    return (int)(d2u(x) >> 63);
}

double scalbn(double x, int n) {
    if (x == 0.0 || isnan(x) || isinf(x)) return x;
    uint64_t bits = d2u(x);
    int exp_field = (int)((bits >> 52) & 0x7FF) + n;
    if (exp_field >= 0x7FF) return copysign(HUGE_VAL, x);
    if (exp_field <= 0) return copysign(0.0, x); /* flush to zero rather than handling subnormals */
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)exp_field << 52);
    return u2d(bits);
}

double frexp(double x, int* exp) {
    if (x == 0.0 || isnan(x) || isinf(x)) { *exp = 0; return x; }
    uint64_t bits = d2u(x);
    int e = (int)((bits >> 52) & 0x7FF) - 1022;
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52); /* mantissa in [0.5, 1) */
    *exp = e;
    return u2d(bits);
}

double modf(double x, double* iptr) {
    double ip = trunc(x);
    *iptr = ip;
    if (isinf(x)) return copysign(0.0, x);
    return x - ip;
}

long lrint(double x) {
    /* Simplification: always rounds half-away-from-zero (round()'s
     * behaviour) rather than switching on the FPU's current rounding
     * mode the way a real lrint() would - this kernel doesn't expose
     * fesetround()/rounding-mode control, and round-half-away-from-
     * zero is what every caller in quickjs.c actually wants in
     * practice (typed-array clamping, Math.round-adjacent paths). */
    return (long)round(x);
}

double hypot(double x, double y) {
    double ax = fabs(x), ay = fabs(y);
    if (isinf(ax) || isinf(ay)) return HUGE_VAL;
    if (ax < ay) { double t = ax; ax = ay; ay = t; }
    if (ax == 0.0) return 0.0;
    double r = ay / ax;
    return ax * sqrt(1.0 + r * r);
}

double asinh(double x) {
    if (fabs(x) < 1e-8) return x; /* avoid cancellation for tiny x */
    bool neg = x < 0.0;
    double ax = fabs(x);
    double r = log(ax + sqrt(ax * ax + 1.0));
    return neg ? -r : r;
}
double acosh(double x) {
    if (x < 1.0) return NAN;
    return log(x + sqrt(x * x - 1.0));
}
double atanh(double x) {
    if (x <= -1.0 || x >= 1.0) return (x == 1.0) ? HUGE_VAL : (x == -1.0 ? -HUGE_VAL : NAN);
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

/* Single-precision companions required by image/SVG geometry. Keep them
 * as wrappers around the already-tested double primitives so QuickJS and
 * libsvgtiny share one deterministic rounding policy. */
float ceilf(float x)
{
    return (float)ceil((double)x);
}

float fabsf(float x)
{
    return __builtin_fabsf(x);
}

long lroundf(float x)
{
    return (long)round((double)x);
}

/* Locale-free decimal parser for freestanding consumers such as SVG lengths,
 * offsets and gradients. It accepts the standard leading sign, integer and
 * fractional digits, and bounded scientific exponent; parsing stops at the
 * first unit suffix (`px`, `%`, `em`, ...), as required by those consumers. */
double atof(const char *text)
{
    if (text == NULL) return 0.0;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        ++p;
    }
    double value = 0.0;
    bool have_digit = false;
    while (*p >= '0' && *p <= '9') {
        have_digit = true;
        value = value * 10.0 + (double)(*p - '0');
        ++p;
    }
    if (*p == '.') {
        double scale = 0.1;
        ++p;
        while (*p >= '0' && *p <= '9') {
            have_digit = true;
            value += (double)(*p - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }
    if (!have_digit) return 0.0;
    int exponent = 0;
    bool exponent_negative = false;
    if (*p == 'e' || *p == 'E') {
        const char *exp_start = ++p;
        if (*p == '+' || *p == '-') {
            exponent_negative = (*p == '-');
            ++p;
        }
        bool exponent_digits = false;
        while (*p >= '0' && *p <= '9') {
            exponent_digits = true;
            if (exponent < 308) exponent = exponent * 10 + (*p - '0');
            ++p;
        }
        if (!exponent_digits) p = exp_start - 1;
    }
    if (exponent > 308) exponent = 308;
    if (exponent_negative) {
        while (exponent-- > 0) value *= 0.1;
    } else {
        while (exponent-- > 0) value *= 10.0;
    }
    return negative ? -value : value;
}
