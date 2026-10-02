/* カーネル/スタンドアロン向け libm/libc 参照実装 (Alpha0.6)
 * ============================================================
 * NO_STDLIB 構成のランタイムは、次の関数を「カーネル側が提供する契約」として
 * 参照する（include/common/python_code_to_c_common.h の宣言）。
 *
 *   libm : floor ceil trunc fabs fmod pow sqrt cbrt hypot copysign ldexp
 *          sin cos tan asin acos atan atan2 exp expm1 log log2 log10 log1p
 *          erf erfc tgamma lgamma
 *   libc : snprintf vsnprintf strtoll strtod
 *
 * 実カーネルは自前の実装（またはlibm相当）へ置き換えるのが本筋だが、
 * `make hobbyos elf` が作る「そのまま組み込める自己完結ELF」と、
 * libcを持たない小さなターゲットのために、ここに参照実装を置く。
 *
 * 精度方針: 組込み用途で実用的な範囲に最適化した級数・近似を使う。倍精度の
 * 最終ビットまでは保証せず、境界（inf/NaN/0/負値）はPythonの期待に合わせて
 * 扱う。ホスト上で libm と比較する回帰（make test-hobbyos-libc）で検証する。
 */

#include "common/python_code_to_c_common.h"

/* テスト用フック: ホストの libm/libc と衝突させずに比較できるよう、シンボル名を
 * 前置する。システムヘッダを取り込んだ後に定義するため、宣言側には影響しない。
 * （tests/test_hobbyos_libc.c が -DP2C_HOBBYOS_LIBC_PREFIX でビルドする。） */
#ifdef P2C_HOBBYOS_LIBC_PREFIX
#define fabs     stub_fabs
#define copysign stub_copysign
#define ldexp    stub_ldexp
#define trunc    stub_trunc
#define floor    stub_floor
#define ceil     stub_ceil
#define fmod     stub_fmod
#define sqrt     stub_sqrt
#define cbrt     stub_cbrt
#define hypot    stub_hypot
#define exp      stub_exp
#define log      stub_log
#define log2     stub_log2
#define log10    stub_log10
#define expm1    stub_expm1
#define log1p    stub_log1p
#define pow      stub_pow
#define sin      stub_sin
#define cos      stub_cos
#define tan      stub_tan
#define atan     stub_atan
#define asin     stub_asin
#define acos     stub_acos
#define atan2    stub_atan2
#define erf      stub_erf
#define erfc     stub_erfc
#define tgamma   stub_tgamma
#define lgamma   stub_lgamma
#define snprintf stub_snprintf
#define strtoll  stub_strtoll
#define strtod   stub_strtod
#endif

/* ── 基本（ビット操作と単純計算で正確に求まるもの） ───────────────── */

/* ビット同一性とゼロ判定（-Wfloat-equal を避けつつ厳密に判定する）。 */
static int p2c_stub_bits_equal(double a, double b) {
    union { double d; uint64_t u; } ua, ub;
    ua.d = a; ub.d = b;
    return ua.u == ub.u;
}
static int p2c_stub_is_zero(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return (u.u & 0x7fffffffffffffffULL) == 0ULL;
}

double fabs(double x) { return x < 0.0 ? -x : x; }

double copysign(double x, double y) {
    /* 符号ビットのみを写す（値の大小に依存しない）。 */
    union { double d; uint64_t u; } xs, ys;
    xs.d = x; ys.d = y;
    xs.u = (xs.u & 0x7fffffffffffffffULL) | (ys.u & 0x8000000000000000ULL);
    return xs.d;
}

double ldexp(double x, int exp) {
    double scale = 1.0;
    int e = exp < 0 ? -exp : exp;
    for (int i = 0; i < e; i++) scale *= 2.0;
    return exp < 0 ? x / scale : x * scale;
}

double trunc(double x) {
    if (x >= 0.0) return (double)(long long)x;
    return -(double)(long long)(-x);
}

double floor(double x) {
    double t = trunc(x);
    return (x < 0.0 && !p2c_stub_bits_equal(t, x)) ? t - 1.0 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return (x > 0.0 && !p2c_stub_bits_equal(t, x)) ? t + 1.0 : t;
}

double fmod(double x, double y) {
    double q;
    if (y > 1.7976931348623157e308 || y < -1.7976931348623157e308) return 0.0; /* y=inf */
    if (p2c_stub_is_zero(x) || x != x) return x;                                /* 0 または NaN */
    q = trunc(x / y);
    return x - q * y;
}

/* √x （ニュートン法）。x<0 は NaN、x==0 は 0、inf は inf を返す。 */
double sqrt(double x) {
    double g;
    if (x < 0.0) {
        union { double d; uint64_t u; } n;
        n.u = 0x7ff8000000000000ULL; /* quiet NaN */
        return n.d;
    }
    if (p2c_stub_is_zero(x) || x > 1.7976931348623157e308) return x;
    g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 64; i++) {
        double next = 0.5 * (g + x / g);
        if (p2c_stub_bits_equal(next, g)) break;
        g = next;
    }
    return g;
}

/* ∛x （ニュートン法）。負値は符号を分離して計算する。 */
double cbrt(double x) {
    double g, a;
    int neg = x < 0.0;
    if (p2c_stub_is_zero(x)) return x;
    a = neg ? -x : x;
    g = a > 1.0 ? a : 1.0;
    for (int i = 0; i < 64; i++) {
        double next = (2.0 * g + a / (g * g)) / 3.0;
        if (p2c_stub_bits_equal(next, g)) break;
        g = next;
    }
    return neg ? -g : g;
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }

/* ── 超越関数（級数 + 範囲縮小） ───────────────────────────────────── */

#define P2C_STUB_PI 3.14159265358979323846
#define P2C_STUB_LN2 0.69314718055994530942

static int p2c_stub_is_nan(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return ((u.u >> 52) & 0x7ffULL) == 0x7ffULL && (u.u & 0xfffffffffffffULL) != 0ULL;
}
static int p2c_stub_is_inf(double x) {
    union { double d; uint64_t u; } u;
    u.d = x;
    return ((u.u >> 52) & 0x7ffULL) == 0x7ffULL && (u.u & 0xfffffffffffffULL) == 0ULL;
}
static double p2c_stub_nan(void) {
    union { double d; uint64_t u; } u;
    u.u = 0x7ff8000000000000ULL;
    return u.d;
}
static double p2c_stub_inf(void) { return 1.7976931348623157e308 * 2.0; }

/* e^x: x = n*ln2 + r とし、e^r をマクローリン級数で求めて 2^n を掛ける。 */
double exp(double x) {
    double r, term, sum;
    int n;
    if (p2c_stub_is_nan(x)) return x;
    if (x > 709.782712893384) return p2c_stub_inf();
    if (x < -745.1332191019411) return 0.0;
    n = (int)(x / P2C_STUB_LN2 + (x >= 0.0 ? 0.5 : -0.5));
    r = x - (double)n * P2C_STUB_LN2;
    term = 1.0; sum = 1.0;
    for (int i = 1; i <= 24; i++) { term *= r / (double)i; sum += term; }
    return ldexp(sum, n);
}

/* log(x): x = m * 2^k (m∈[1,2)) へ正規化し、log(m) を atanh 級数で求める。 */
double log(double x) {
    int k = 0;
    double z, z2, sum, term;
    if (p2c_stub_is_nan(x)) return x;
    if (x < 0.0) return p2c_stub_nan();
    if (p2c_stub_is_zero(x)) return -p2c_stub_inf();
    if (p2c_stub_is_inf(x)) return x;
    while (x >= 2.0) { x /= 2.0; k++; }
    while (x < 1.0) { x *= 2.0; k--; }
    z = (x - 1.0) / (x + 1.0);
    z2 = z * z;
    sum = 0.0; term = z;
    for (int i = 1; i <= 41; i += 2) { sum += term / (double)i; term *= z2; }
    return 2.0 * sum + (double)k * P2C_STUB_LN2;
}

double log2(double x) { return log(x) / P2C_STUB_LN2; }
double log10(double x) { return log(x) / 2.30258509299404568402; }
double expm1(double x) { return exp(x) - 1.0; }
double log1p(double x) { return log(1.0 + x); }

/* atan 級数（|a| <= 0.5 で十分な精度）。 */
static double p2c_stub_atan_series(double a) {
    double a2 = a * a, sum = a, term = a;
    for (int i = 1; i <= 40; i++) {
        term *= -a2;
        sum += term / (double)(2 * i + 1);
    }
    return sum;
}

/* sin/cos: π/2 の倍数へ範囲縮小してテイラー級数。 */
static double p2c_stub_sin_core(double r) {
    double term = r, sum = r;
    double r2 = r * r;
    for (int i = 1; i <= 10; i++) {
        term *= -r2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}
static double p2c_stub_cos_core(double r) {
    double term = 1.0, sum = 1.0;
    double r2 = r * r;
    for (int i = 1; i <= 10; i++) {
        term *= -r2 / (double)((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}
static void p2c_stub_reduce_pi2(double x, int *quadrant, double *r) {
    double q = trunc(x / (P2C_STUB_PI / 2.0));
    double n = q;
    int quad = (int)fmod(n, 4.0);
    *quadrant = ((quad % 4) + 4) % 4;
    *r = x - q * (P2C_STUB_PI / 2.0);
}
double sin(double x) {
    int quadrant; double r;
    if (p2c_stub_is_nan(x) || p2c_stub_is_inf(x)) return p2c_stub_nan();
    p2c_stub_reduce_pi2(x, &quadrant, &r);
    switch (quadrant) {
        case 0: return p2c_stub_sin_core(r);
        case 1: return p2c_stub_cos_core(r);
        case 2: return -p2c_stub_sin_core(r);
        default: return -p2c_stub_cos_core(r);
    }
}
double cos(double x) {
    int quadrant; double r;
    if (p2c_stub_is_nan(x) || p2c_stub_is_inf(x)) return p2c_stub_nan();
    p2c_stub_reduce_pi2(x, &quadrant, &r);
    switch (quadrant) {
        case 0: return p2c_stub_cos_core(r);
        case 1: return -p2c_stub_sin_core(r);
        case 2: return -p2c_stub_cos_core(r);
        default: return p2c_stub_sin_core(r);
    }
}
double tan(double x) {
    double c = cos(x);
    if (p2c_stub_is_zero(c)) return p2c_stub_inf();
    return sin(x) / c;
}

/* atan: |x|>1 は π/2-atan(1/x)、|x|>0.5 は半角公式で収束域へ落とす。 */
double atan(double x) {
    int neg = x < 0.0, invert = 0, doubled = 0;
    double a, result;
    if (p2c_stub_is_nan(x)) return x;
    a = neg ? -x : x;
    if (a > 1.0) { a = 1.0 / a; invert = 1; }
    if (a > 0.5) { a = a / (1.0 + sqrt(1.0 + a * a)); doubled = 1; }
    result = doubled ? 2.0 * p2c_stub_atan_series(a) : p2c_stub_atan_series(a);
    if (invert) result = P2C_STUB_PI / 2.0 - result;
    return neg ? -result : result;
}

double asin(double x) {
    if (x > 1.0 || x < -1.0) return p2c_stub_nan();
    if (p2c_stub_is_zero(x)) return 0.0;
    return atan(x / sqrt(1.0 - x * x));
}
double acos(double x) { return P2C_STUB_PI / 2.0 - asin(x); }

double atan2(double y, double x) {
    if (p2c_stub_is_zero(x)) {
        if (y > 0.0) return P2C_STUB_PI / 2.0;
        if (y < 0.0) return -P2C_STUB_PI / 2.0;
        return 0.0;
    }
    if (x > 0.0) return atan(y / x);
    return y >= 0.0 ? atan(y / x) + P2C_STUB_PI : atan(y / x) - P2C_STUB_PI;
}

/* erf: Abramowitz-Stegun 7.1.26（絶対誤差 < 1.5e-7）。 */
double erf(double x) {
    double t, absx, poly;
    int neg = x < 0.0;
    const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741;
    const double a4 = -1.453152027, a5 = 1.061405429, p = 0.3275911;
    if (x < 0.0) x = -x;
    absx = x;
    t = 1.0 / (1.0 + p * absx);
    poly = ((((a5 * t + a4) * t + a3) * t + a2) * t + a1) * t;
    {
        double r = 1.0 - poly * exp(-absx * absx);
        return neg ? -r : r;
    }
}
double erfc(double x) { return 1.0 - erf(x); }

/* gamma/lgamma: Stirling近似（x>=8）と漸化式 gamma(x)=gamma(x+1)/x の併用。 */
static double p2c_stub_lgamma_large(double x) {
    return 0.91893853320467274178 + (x - 0.5) * log(x) - x
         + 1.0 / (12.0 * x) - 1.0 / (360.0 * x * x * x) + 1.0 / (1260.0 * x * x * x * x * x);
}
double lgamma(double x) {
    double acc = 0.0;
    if (p2c_stub_is_nan(x)) return x;
    if (x <= 0.0 && p2c_stub_bits_equal(trunc(x), x)) return p2c_stub_inf(); /* 非正の整数 */
    while (x < 8.0) { acc -= log(x); x += 1.0; }
    return p2c_stub_lgamma_large(x) + acc;
}
double tgamma(double x) {
    double l;
    if (x < 0.5 && !p2c_stub_bits_equal(trunc(x), x)) {
        /* 反射公式: Γ(x)Γ(1-x) = π/sin(πx) */
        double s = sin(P2C_STUB_PI * x);
        if (p2c_stub_is_zero(s)) return p2c_stub_inf();
        return P2C_STUB_PI / (s * tgamma(1.0 - x));
    }
    l = lgamma(x);
    return exp(l);
}

/* ── 最小 libc（書式化と数値変換） ──────────────────────────────────
 * ランタイムが使う仕様（%d %i %u %x %X %o %c %s %p %f %e %g %%、
 * フラグ -+0#空白、幅、精度、長さ l/ll/z/h）だけを実装する。
 * %f/%e/%g は固定小数の桁生成による近似で、倍精度の全桁は保証しない。 */

typedef struct {
    char *out;      /* 出力先（NULLなら長さだけ数える） */
    size_t cap;     /* 出力先の容量 */
    size_t len;     /* 書き込んだ（または書き込むべき）文字数 */
} P2C_StubSink;

static void p2c_stub_putc(P2C_StubSink *sink, char c) {
    if (sink->out && sink->len + 1u < sink->cap) sink->out[sink->len] = c;
    sink->len++;
}
static void p2c_stub_puts(P2C_StubSink *sink, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) p2c_stub_putc(sink, s[i]);
}
static void p2c_stub_pad(P2C_StubSink *sink, char c, int count) {
    for (int i = 0; i < count; i++) p2c_stub_putc(sink, c);
}

/* 符号なし整数を基数baseで出力する（大文字指定に対応）。 */
static void p2c_stub_put_uint(P2C_StubSink *sink, unsigned long long value, unsigned base, int upper) {
    char buf[32];
    size_t n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0ULL) { p2c_stub_putc(sink, '0'); return; }
    while (value > 0ULL && n < sizeof(buf)) { buf[n++] = digits[value % base]; value /= base; }
    while (n > 0) p2c_stub_putc(sink, buf[--n]);
}
static size_t p2c_stub_uint_digits(unsigned long long value, unsigned base) {
    size_t n = 1;
    while (value >= (unsigned long long)base) { value /= (unsigned long long)base; n++; }
    return n;
}

/* 自前の文字列比較を使う（libcのstrlen等はランタイム側の実装と衝突しない）。 */
static size_t p2c_stub_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* 浮動小数を固定小数で出力する（precision桁、丸めは最近傍）。 */
static void p2c_stub_put_float(P2C_StubSink *sink, double value, int precision, int force_point) {
    unsigned long long scale = 1ULL;
    unsigned long long ipart, fpart;
    int neg = value < 0.0;
    if (neg) value = -value;
    if (precision < 0) precision = 6;
    for (int i = 0; i < precision; i++) scale *= 10ULL;
    {
        double scaled = value * (double)scale + 0.5;
        if (scaled > 1.8e19) scaled = 1.8e19;   /* 64ビット上限の防御 */
        ipart = (unsigned long long)(scaled / (double)scale);
        fpart = (unsigned long long)(scaled - (double)ipart * (double)scale);
    }
    if (neg) p2c_stub_putc(sink, '-');
    p2c_stub_put_uint(sink, ipart, 10u, 0);
    if (precision > 0 || force_point) p2c_stub_putc(sink, '.');
    for (int i = precision - 1; i >= 0; i--) {
        unsigned long long div = 1ULL;
        for (int j = 0; j < i; j++) div *= 10ULL;
        p2c_stub_putc(sink, (char)('0' + (int)((fpart / div) % 10ULL)));
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    P2C_StubSink sink;
    /* format は呼び出し側が有効なC文字列を渡す契約（libcの nonnull 指定と同じ）。 */
    const char *p = format;
    sink.out = (size > 0) ? str : NULL;
    sink.cap = size;
    sink.len = 0;
    while (*p) {
        int left = 0, zero = 0, plus = 0, space = 0;
        int width = 0, precision = -1, length = 0; /* 0=int 1=long 2=longlong */
        P2C_StubSink item;
        char itembuf[512];
        if (*p != '%') { p2c_stub_putc(&sink, *p++); continue; }
        p++;
        for (;; p++) {
            if (*p == '-') { left = 1; zero = 0; }
            else if (*p == '0') { if (!left) zero = 1; }
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else break;
        }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == '.') {
            precision = 0;
            p++;
            while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); p++; }
        }
        if (*p == 'l') { length = 1; p++; if (*p == 'l') { length = 2; p++; } }
        else if (*p == 'z') { length = 2; p++; }
        else if (*p == 'h') { p++; }
        item.out = itembuf;
        item.cap = sizeof(itembuf);
        item.len = 0;
        switch (*p) {
            case 'd': case 'i': {
                long long v = (length == 2) ? va_arg(ap, long long) : (long long)va_arg(ap, int);
                unsigned long long mag = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
                if (v < 0) p2c_stub_putc(&item, '-');
                else if (plus) p2c_stub_putc(&item, '+');
                else if (space) p2c_stub_putc(&item, ' ');
                if (zero && precision < 0) {
                    int digits = (int)p2c_stub_uint_digits(mag, 10u);
                    int need = width - digits - (int)item.len;
                    p2c_stub_pad(&item, '0', need > 0 ? need : 0);
                }
                p2c_stub_put_uint(&item, mag, 10u, 0);
                break;
            }
            case 'u': {
                unsigned long long v = (length == 2) ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
                p2c_stub_put_uint(&item, v, 10u, 0);
                break;
            }
            case 'x': case 'X': {
                unsigned long long v = (length == 2) ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
                if (zero && precision < 0) {
                    int digits = (int)p2c_stub_uint_digits(v, 16u);
                    int need = width - digits;
                    p2c_stub_pad(&item, '0', need > 0 ? need : 0);
                }
                p2c_stub_put_uint(&item, v, 16u, *p == 'X');
                break;
            }
            case 'o': {
                unsigned long long v = (unsigned long long)va_arg(ap, unsigned int);
                p2c_stub_put_uint(&item, v, 8u, 0);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                p2c_stub_putc(&item, c);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                size_t n = p2c_stub_strlen(s);
                if (precision >= 0 && (size_t)precision < n) n = (size_t)precision;
                if (zero && precision < 0) {
                    int need = width - (int)n;
                    p2c_stub_pad(&item, '0', need > 0 ? need : 0);
                }
                if (s) p2c_stub_puts(&item, s, n);
                break;
            }
            case 'p': {
                unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                p2c_stub_putc(&item, '0');
                p2c_stub_putc(&item, 'x');
                p2c_stub_put_uint(&item, v, 16u, 0);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                double v = va_arg(ap, double);
                int prec = precision < 0 ? 6 : precision;
                int upper = (*p == 'E' || *p == 'F' || *p == 'G');
                if (v != v) { p2c_stub_puts(&item, upper ? "NAN" : "nan", 3u); break; }
                if (v > 1.7976931348623157e308) { p2c_stub_puts(&item, upper ? "INF" : "inf", 3u); break; }
                if (v < -1.7976931348623157e308) { p2c_stub_puts(&item, upper ? "-INF" : "-inf", 4u); break; }
                p2c_stub_put_float(&item, v, prec, (*p == 'e' || *p == 'E' || *p == 'g' || *p == 'G'));
                break;
            }
            case '%': p2c_stub_putc(&item, '%'); break;
            default:
                if (*p == '\0') { p--; }
                else { p2c_stub_putc(&item, '%'); p2c_stub_putc(&item, *p); }
                break;
        }
        if (!left && width > (int)item.len) p2c_stub_pad(&sink, zero ? '0' : ' ', width - (int)item.len);
        p2c_stub_puts(&sink, itembuf, item.len);
        if (left && width > (int)item.len) p2c_stub_pad(&sink, ' ', width - (int)item.len);
        if (*p) p++;
    }
    if (sink.out) {
        size_t term = sink.len < sink.cap ? sink.len : (sink.cap > 0u ? sink.cap - 1u : 0u);
        sink.out[term] = '\0';
    }
    return (int)sink.len;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

/* 文字列→整数（10進/16進/8進/2進、符号と前置空白を許容）。 */
long long strtoll(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    int neg = 0;
    unsigned long long acc = 0ULL;
    if (!p) { if (endptr) *endptr = (char*)nptr; return 0; }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) { base = 10; }
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        acc = acc * (unsigned long long)base + (unsigned long long)d;
    }
    if (endptr) *endptr = (char*)p;
    return neg ? -(long long)acc : (long long)acc;
}

/* 文字列→浮動小数（符号・整数部・小数部・指数部）。 */
double strtod(const char *nptr, char **endptr) {
    const char *p = nptr;
    int neg = 0;
    double value = 0.0, frac_scale = 0.1;
    int exp_neg = 0, exp_val = 0;
    if (!p) { if (endptr) *endptr = (char*)nptr; return 0.0; }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    while (*p >= '0' && *p <= '9') { value = value * 10.0 + (double)(*p - '0'); p++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { value += (double)(*p - '0') * frac_scale; frac_scale *= 0.1; p++; }
    }
    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        p++;
        if (*p == '+' || *p == '-') { exp_neg = (*p == '-'); p++; }
        if (*p < '0' || *p > '9') { p = save; }
        else {
            while (*p >= '0' && *p <= '9') { exp_val = exp_val * 10 + (*p - '0'); p++; }
            if (exp_val > 308) exp_val = 308;   /* 10^308 を超える桁は飽和させる */
            {
                double scale = 1.0;
                for (int i = 0; i < exp_val; i++) scale *= 10.0;
                value = exp_neg ? value / scale : value * scale;
            }
        }
    }
    if (endptr) *endptr = (char*)p;
    return neg ? -value : value;
}


double pow(double x, double y) {
    double result = 1.0, base;
    long long n;
    if (p2c_stub_is_nan(y)) return y;
    if (p2c_stub_is_zero(y)) return 1.0;
    if (x > 0.0) return exp(y * log(x));
    if (p2c_stub_is_zero(x)) return y > 0.0 ? 0.0 : p2c_stub_inf();
    n = (long long)y;
    if (!p2c_stub_bits_equal((double)n, y)) return p2c_stub_nan(); /* 負の底の非整数乗 */
    base = n < 0 ? 1.0 / x : x;
    if (n < 0) n = -n;
    while (n > 0) {
        if (n & 1LL) result *= base;
        base *= base;
        n >>= 1;
    }
    return result;
}
