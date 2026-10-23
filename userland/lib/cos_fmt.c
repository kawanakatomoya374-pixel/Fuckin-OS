/**
 * cos_fmt.c - the printf engine for .c-os programs.
 *
 * WHAT THIS REPLACES
 * ------------------
 * The previous formatter handled `%s %c %d %i %u %x %X %p %%` and the
 * `l` length modifier, and nothing else. No flags, no field width, no
 * precision, no floating point. That is enough to print a diagnostic
 * line and not enough to port anything: `%-20s`, `%08x`, `%.3f` and
 * `%5.2f` are not exotic, they are what ordinary C code is written with,
 * and a formatter that silently emits the literal text `%-20s` turns a
 * missing feature into corrupted output.
 *
 * This is a complete C99 formatter minus `%n` (deliberately - see below)
 * and the wide-character conversions (there is no wchar support in this
 * runtime to convert to).
 *
 * WHY %n IS NOT HERE
 * ------------------
 * `%n` writes an integer through a pointer taken from the argument list.
 * It is the classic format-string exploit primitive, it is
 * vanishingly rarely used on purpose, and glibc itself refuses it in
 * writable format strings. Supporting it would add an
 * arbitrary-write gadget to every program that ever passes a
 * user-controlled string as a format. It is rejected rather than
 * ignored, so a program using it fails loudly at the call instead of
 * silently doing nothing.
 *
 * FLOATING POINT
 * --------------
 * %f/%F/%e/%E/%g/%G are implemented with integer arithmetic on the
 * decomposed double, not by calling into libm: the formatter has to work
 * in a program that never linked the math library, and pulling pow()
 * into every printf would be both a layering inversion and a needless
 * dependency. Accuracy is good to the 17 significant digits a double
 * carries; the long-double conversions (%Lf) are accepted and formatted
 * as double, which is what x86-64 code overwhelmingly means anyway.
 */
#include "cos.h"

/* ---- output sink --------------------------------------------------------
 * One sink type for every entry point, so snprintf, printf and fprintf
 * cannot drift apart in their handling of width, precision or
 * truncation. `len` counts what WOULD have been written, which is what
 * snprintf must return. */
typedef struct {
    char  *buf;        /* NULL for a counting-only pass          */
    size_t cap;        /* bytes available in buf, including NUL  */
    size_t len;        /* characters produced so far             */
    void (*flush)(const char *s, size_t n, void *ctx);
    void  *ctx;
} cos_sink_t;

static void sink_putn(cos_sink_t *o, const char *s, size_t n)
{
    if (o->flush) {
        o->flush(s, n, o->ctx);
        o->len += n;
        return;
    }
    if (o->buf) {
        for (size_t i = 0; i < n; ++i) {
            if (o->len + i + 1 < o->cap) o->buf[o->len + i] = s[i];
        }
    }
    o->len += n;
}

static void sink_put(cos_sink_t *o, char c) { sink_putn(o, &c, 1); }

static void sink_pad(cos_sink_t *o, char c, long n)
{
    char block[16];
    for (int i = 0; i < 16; ++i) block[i] = c;
    while (n > 0) {
        long k = n > 16 ? 16 : n;
        sink_putn(o, block, (size_t)k);
        n -= k;
    }
}

/* ---- conversion specification ------------------------------------------ */
#define FL_LEFT   0x01   /* -  */
#define FL_PLUS   0x02   /* +  */
#define FL_SPACE  0x04   /* ' ' */
#define FL_ALT    0x08   /* #  */
#define FL_ZERO   0x10   /* 0  */

typedef struct {
    unsigned flags;
    long     width;
    long     prec;       /* -1 when absent */
    int      lenmod;     /* 0 none, 1 h, 2 hh, 3 l, 4 ll, 5 z, 6 t, 7 j, 8 L */
} cos_spec_t;

/* Emits `body` (already converted) with the spec's padding rules.
 * `prefix` is the sign or "0x" that must sit INSIDE zero padding - the
 * difference between "-0042" and "00-42" is exactly this. */
static void emit_padded(cos_sink_t *o, const cos_spec_t *sp,
                        const char *prefix, size_t plen,
                        const char *body, size_t blen,
                        long zero_pad)
{
    long total = (long)plen + (long)blen + (zero_pad > 0 ? zero_pad : 0);
    long pad = sp->width - total;

    if (!(sp->flags & FL_LEFT) && !(sp->flags & FL_ZERO) && pad > 0) {
        sink_pad(o, ' ', pad);
    }
    if (plen) sink_putn(o, prefix, plen);
    /* Zero padding from the ZERO flag only applies when there is no
     * explicit precision: C says the flag is ignored for d/i/o/u/x/X
     * with a precision, because the precision already specifies the
     * minimum digit count. */
    if (!(sp->flags & FL_LEFT) && (sp->flags & FL_ZERO) && pad > 0) {
        sink_pad(o, '0', pad);
    }
    if (zero_pad > 0) sink_pad(o, '0', zero_pad);
    sink_putn(o, body, blen);
    if ((sp->flags & FL_LEFT) && pad > 0) sink_pad(o, ' ', pad);
}

/* ---- integers ----------------------------------------------------------- */

static size_t u64_to_digits(uint64_t v, unsigned base, bool upper,
                            char *out /* >= 64 */)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *d = upper ? up : lo;
    char tmp[64];
    size_t n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v % base]; v /= base; }
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

static void fmt_integer(cos_sink_t *o, const cos_spec_t *sp, uint64_t mag,
                        bool negative, unsigned base, bool upper)
{
    char digits[64];
    size_t n = u64_to_digits(mag, base, upper, digits);

    /* A precision of 0 with a value of 0 produces NO characters at all -
     * an easy case to get wrong, and one real code relies on for
     * conditional output. */
    if (sp->prec == 0 && mag == 0) n = 0;

    char prefix[4];
    size_t plen = 0;
    if (negative)                   prefix[plen++] = '-';
    else if (sp->flags & FL_PLUS)   prefix[plen++] = '+';
    else if (sp->flags & FL_SPACE)  prefix[plen++] = ' ';

    if ((sp->flags & FL_ALT) && base == 16 && mag != 0) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    } else if ((sp->flags & FL_ALT) && base == 8 && digits[0] != '0') {
        prefix[plen++] = '0';
    }

    long zero_pad = 0;
    cos_spec_t eff = *sp;
    if (sp->prec >= 0) {
        if ((long)n < sp->prec) zero_pad = sp->prec - (long)n;
        eff.flags &= ~FL_ZERO;   /* precision beats the 0 flag */
    }
    emit_padded(o, &eff, prefix, plen, digits, n, zero_pad);
}

/* ---- floating point -----------------------------------------------------
 *
 * Done on the decomposed double with 64-bit integer arithmetic rather
 * than by repeatedly multiplying the fraction, which loses digits fast.
 * The value is split into an integer part (built by repeated division)
 * and a scaled fractional part, with round-half-away-from-zero applied
 * to the last kept digit - and a carry from that rounding propagated
 * back into the integer part, which is the case that turns 9.99 at
 * precision 1 into "10.0" rather than "9.10".
 */

typedef union { double d; uint64_t u; } dbits_t;

static bool d_is_nan(double v) { dbits_t b; b.d = v;
    return ((b.u >> 52) & 0x7FF) == 0x7FF && (b.u & 0xFFFFFFFFFFFFFULL) != 0; }
static bool d_is_inf(double v) { dbits_t b; b.d = v;
    return ((b.u >> 52) & 0x7FF) == 0x7FF && (b.u & 0xFFFFFFFFFFFFFULL) == 0; }
static bool d_is_neg(double v) { dbits_t b; b.d = v; return (b.u >> 63) != 0; }

static bool fmt_special(cos_sink_t *o, const cos_spec_t *sp, double v, bool upper)
{
    const char *body = NULL;
    if (d_is_nan(v)) body = upper ? "NAN" : "nan";
    else if (d_is_inf(v)) body = upper ? "INF" : "inf";
    if (!body) return false;

    char prefix[2]; size_t plen = 0;
    if (d_is_neg(v))               prefix[plen++] = '-';
    else if (sp->flags & FL_PLUS)  prefix[plen++] = '+';
    else if (sp->flags & FL_SPACE) prefix[plen++] = ' ';

    /* Zero-padding a NaN would produce "00nan", which is nonsense. */
    cos_spec_t eff = *sp;
    eff.flags &= ~FL_ZERO;
    size_t blen = 0; while (body[blen]) blen++;
    emit_padded(o, &eff, prefix, plen, body, blen, 0);
    return true;
}

/* ---- exact decimal conversion -------------------------------------------
 *
 * The obvious implementation - peel digits off by repeatedly multiplying
 * the fraction by ten - accumulates error, and the error lands exactly
 * where it matters: on the tie test for the last kept digit. `%.3f` of
 * 0.0005 is the case that caught it here. The true value of that double
 * is 5.0000000000000001e-04, strictly ABOVE the tie, so it must round to
 * "0.001"; after four floating multiplications the residue had drifted
 * to exactly 5.0, the code saw a tie, rounded to even and printed
 * "0.000". No amount of care with the tie rule fixes that, because by
 * then the information needed to decide is gone.
 *
 * So the conversion is done on the exact value instead. A double is
 * m * 2^e with m a 53-bit integer, which means its decimal expansion is
 * finite and computable with integer arithmetic alone - no rounding, no
 * drift, no libm. The integer part needs up to 1024 bits and the
 * fraction denominator up to 2^1074, so a fixed 20-word (1280-bit)
 * big integer covers every finite double with room to spare.
 */
#define BN_WORDS 20
typedef struct { uint64_t w[BN_WORDS]; } bn_t;

static void bn_zero(bn_t *a) { for (int i = 0; i < BN_WORDS; ++i) a->w[i] = 0; }
static void bn_set(bn_t *a, uint64_t v) { bn_zero(a); a->w[0] = v; }
static bool bn_is_zero(const bn_t *a)
{
    for (int i = 0; i < BN_WORDS; ++i) if (a->w[i]) return false;
    return true;
}

static void bn_shl(bn_t *a, unsigned n)
{
    unsigned words = n / 64, bits = n % 64;
    if (words) {
        for (int i = BN_WORDS - 1; i >= 0; --i) {
            a->w[i] = (i >= (int)words) ? a->w[i - words] : 0;
        }
    }
    if (bits) {
        uint64_t carry = 0;
        for (int i = 0; i < BN_WORDS; ++i) {
            uint64_t next = a->w[i] >> (64 - bits);
            a->w[i] = (a->w[i] << bits) | carry;
            carry = next;
        }
    }
}

static void bn_shr(bn_t *a, unsigned n)
{
    unsigned words = n / 64, bits = n % 64;
    if (words) {
        for (int i = 0; i < BN_WORDS; ++i) {
            a->w[i] = (i + (int)words < BN_WORDS) ? a->w[i + words] : 0;
        }
    }
    if (bits) {
        uint64_t carry = 0;
        for (int i = BN_WORDS - 1; i >= 0; --i) {
            uint64_t next = a->w[i] << (64 - bits);
            a->w[i] = (a->w[i] >> bits) | carry;
            carry = next;
        }
    }
}

/* Keeps only the low `n` bits - the fractional remainder after the
 * integer part has been taken out. */
static void bn_mask_low(bn_t *a, unsigned n)
{
    for (int i = 0; i < BN_WORDS; ++i) {
        unsigned base = (unsigned)i * 64;
        if (base >= n) { a->w[i] = 0; continue; }
        unsigned keep = n - base;
        if (keep < 64) a->w[i] &= (keep == 0) ? 0ULL : ((1ULL << keep) - 1ULL);
    }
}

/* a *= m, m small. Returns the overflow out of the top word.
 *
 * Also in 32-bit halves, for the same reason as the division: this one
 * would only need a widening multiply, which gcc does emit inline, but
 * keeping both in the same style means neither can quietly regrow a
 * libgcc dependency the next time someone edits it. */
static uint64_t bn_mul_small(bn_t *a, uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < BN_WORDS; ++i) {
        uint64_t lo = (a->w[i] & 0xFFFFFFFFULL) * m + (carry & 0xFFFFFFFFULL);
        uint64_t hi = (a->w[i] >> 32) * m + (lo >> 32) + (carry >> 32);
        a->w[i] = (hi << 32) | (lo & 0xFFFFFFFFULL);
        carry = hi >> 32;
    }
    return carry;
}

/* a /= d, returning the remainder. `d` must fit in 32 bits.
 *
 * Written in 32-bit halves rather than as a 128-by-64 division. The
 * obvious `(unsigned __int128)rem << 64 | word` form compiles to calls
 * into libgcc (__udivti3 / __umodti3), and .c-os programs link
 * -nostdlib with no libgcc - so the obvious version builds fine here and
 * fails to LINK in the environment this code actually ships to. Splitting
 * each word in half keeps every intermediate inside 64 bits, because the
 * running remainder is always below `d`. */
static uint64_t bn_div_small(bn_t *a, uint32_t d)
{
    uint64_t rem = 0;
    for (int i = BN_WORDS - 1; i >= 0; --i) {
        uint64_t hi = (rem << 32) | (a->w[i] >> 32);
        uint64_t qhi = hi / d;
        rem = hi % d;
        uint64_t lo = (rem << 32) | (a->w[i] & 0xFFFFFFFFULL);
        uint64_t qlo = lo / d;
        rem = lo % d;
        a->w[i] = (qhi << 32) | qlo;
    }
    return rem;
}

/* Compares a against 2^bit. */
static int bn_cmp_pow2(const bn_t *a, unsigned bit)
{
    for (int i = BN_WORDS - 1; i >= 0; --i) {
        uint64_t mine = a->w[i];
        uint64_t theirs = 0;
        if ((unsigned)i == bit / 64) theirs = 1ULL << (bit % 64);
        else if ((unsigned)i > bit / 64) theirs = 0;
        if (mine != theirs) return mine > theirs ? 1 : -1;
    }
    return 0;
}

/* Decomposes a finite non-negative double into m * 2^e with m integral. */
static void decompose(double v, uint64_t *out_m, int *out_e)
{
    dbits_t b; b.d = v;
    int biased = (int)((b.u >> 52) & 0x7FF);
    uint64_t frac = b.u & 0xFFFFFFFFFFFFFULL;
    if (biased == 0) { *out_m = frac; *out_e = -1074; }          /* subnormal */
    else             { *out_m = frac | (1ULL << 52); *out_e = biased - 1075; }
}

/* Produces the FULL exact decimal expansion of |v| - every digit, with
 * no rounding anywhere - into `digits`, and reports how many of them are
 * before the decimal point.
 *
 * This is the primitive every conversion is built on. Doing the rounding
 * once, at the end, on an exact digit string is what makes %e and %g
 * correct: normalising the mantissa by dividing the double by ten
 * reintroduces exactly the drift the exact integer path removed, and
 * fuzzing against the host libc showed it disagreeing on about one value
 * in twelve at high precision.
 *
 * A double's expansion is finite: at most 309 integer digits and 1074
 * fraction digits. */
#define EXPAND_CAP 1500

static size_t exact_expand(double v, char *digits, size_t *intlen)
{
    uint64_t m; int e;
    decompose(v, &m, &e);

    bn_t ip, fr;
    unsigned k = 0;

    if (e >= 0) {
        bn_set(&ip, m);
        if (e > 0) bn_shl(&ip, (unsigned)e);
        bn_zero(&fr);
    } else {
        k = (unsigned)(-e);
        bn_set(&ip, m);
        bn_shr(&ip, k);
        bn_set(&fr, m);
        bn_mask_low(&fr, k);
    }

    size_t n = 0;
    if (bn_is_zero(&ip)) {
        digits[n++] = '0';
    } else {
        char tmp[400];
        size_t t = 0;
        bn_t q = ip;
        while (!bn_is_zero(&q) && t < sizeof(tmp)) {
            tmp[t++] = (char)('0' + bn_div_small(&q, 10));
        }
        while (t > 0) digits[n++] = tmp[--t];
    }
    *intlen = n;

    /* Fraction digits until the remainder is exhausted. Bounded by the
     * cap, which a finite double can never reach. */
    while (!bn_is_zero(&fr) && n < EXPAND_CAP - 1) {
        bn_mul_small(&fr, 10);
        bn_t whole = fr;
        bn_shr(&whole, k);
        digits[n++] = (char)('0' + (whole.w[0] % 10));
        bn_mask_low(&fr, k);
    }
    return n;
}

/* Rounds an exact digit string to `keep` digits, ties to even.
 *
 * Ties to even rather than away from zero: that is the IEEE default
 * rounding mode and what every C library produces, so "%.0f" of 2.5 is
 * "2". The obvious choice is the other one, and it is wrong.
 *
 * Returns the resulting length (always `keep`). Sets *grew when the
 * carry ran off the front, which shifts the decimal point one place -
 * the 9.99 -> 10.0 case. */
static size_t decimal_round(char *digits, size_t n, size_t keep, bool *grew)
{
    *grew = false;
    if (keep >= n) {
        /* Nothing to drop; pad with zeros so the caller always gets
         * exactly `keep` digits to emit. */
        while (n < keep) digits[n++] = '0';
        return keep;
    }

    char first_dropped = digits[keep];
    bool rest_nonzero = false;
    for (size_t i = keep + 1; i < n; ++i) {
        if (digits[i] != '0') { rest_nonzero = true; break; }
    }

    bool up;
    if (first_dropped > '5')      up = true;
    else if (first_dropped < '5') up = false;
    else if (rest_nonzero)        up = true;
    else up = (keep > 0) && (((digits[keep - 1] - '0') & 1) != 0);

    if (up) {
        long i = (long)keep - 1;
        bool carry = true;
        while (carry && i >= 0) {
            if (digits[i] == '9') { digits[i] = '0'; i--; }
            else { digits[i]++; carry = false; }
        }
        if (carry) {
            for (long q = (long)keep; q > 0; --q) digits[q] = digits[q - 1];
            digits[0] = '1';
            *grew = true;
        }
    }
    return keep;
}

/* Renders |v| in fixed notation with exactly `prec` fraction digits.
 * `out` must hold at least EXPAND_CAP + 8 bytes. */
static size_t render_fixed(double v, long prec, char *out)
{
    if (prec < 0) prec = 0;
    if (prec > 400) prec = 400;

    char digits[EXPAND_CAP];
    size_t ilen = 0;
    size_t n = exact_expand(v, digits, &ilen);

    bool grew = false;
    size_t keep = ilen + (size_t)prec;
    n = decimal_round(digits, n, keep, &grew);
    if (grew) { ilen++; n++; }

    size_t out_n = 0;
    for (size_t i = 0; i < ilen; ++i) out[out_n++] = digits[i];
    if (prec > 0) {
        out[out_n++] = '.';
        for (size_t i = ilen; i < ilen + (size_t)prec; ++i) out[out_n++] = digits[i];
    }
    return out_n;
}

/* Renders |v| in scientific notation with `prec` fraction digits, and
 * reports the decimal exponent. Exact, for the same reason as above. */
static size_t render_sci(double v, long prec, char *out, int *out_exp,
                         bool upper)
{
    if (prec < 0) prec = 0;
    if (prec > 400) prec = 400;

    char digits[EXPAND_CAP];
    size_t ilen = 0;
    size_t n = exact_expand(v, digits, &ilen);

    /* Locate the first significant digit. Its position relative to the
     * decimal point IS the exponent. */
    size_t z = 0;
    while (z < n && digits[z] == '0') z++;
    if (z == n) {
        /* Exactly zero. It still needs the exponent tail: "%e" of 0.0 is
         * "0.000000e+00", not "0.000000" - forgetting it produces
         * something that looks like a valid %f and is not a valid %e. */
        *out_exp = 0;
        size_t k = 0;
        out[k++] = '0';
        if (prec > 0) { out[k++] = '.'; for (long i = 0; i < prec; ++i) out[k++] = '0'; }
        out[k++] = upper ? 'E' : 'e';
        out[k++] = '+';
        out[k++] = '0';
        out[k++] = '0';
        return k;
    }

    int exp10 = (int)ilen - 1 - (int)z;

    /* Drop the leading zeros so digit 0 is the first significant one,
     * then round to prec+1 significant digits. */
    for (size_t i = 0; i + z < n; ++i) digits[i] = digits[i + z];
    n -= z;

    bool grew = false;
    n = decimal_round(digits, n, (size_t)prec + 1, &grew);
    if (grew) {
        /* The carry produced an extra leading digit, so the exponent
         * moves and the now-surplus last digit is dropped: 9.99 at
         * precision 1 becomes 1.0e+01, not 10.0e+00. */
        exp10++;
    }

    size_t k = 0;
    out[k++] = digits[0];
    if (prec > 0) {
        out[k++] = '.';
        for (long i = 0; i < prec; ++i) out[k++] = digits[1 + i];
    }
    out[k++] = upper ? 'E' : 'e';
    out[k++] = (exp10 < 0) ? '-' : '+';
    int ae = exp10 < 0 ? -exp10 : exp10;
    if (ae >= 100) out[k++] = (char)('0' + ae / 100);
    out[k++] = (char)('0' + (ae / 10) % 10);
    out[k++] = (char)('0' + ae % 10);
    *out_exp = exp10;
    return k;
}

/* The decimal exponent of |v|, exactly - used by %g to decide between
 * the fixed and scientific forms. Computed from the expansion rather
 * than with log10(), which is both a libm dependency and wrong at the
 * boundaries. */
static int exact_exp10(double v)
{
    char digits[EXPAND_CAP];
    size_t ilen = 0;
    size_t n = exact_expand(v, digits, &ilen);
    size_t z = 0;
    while (z < n && digits[z] == '0') z++;
    if (z == n) return 0;
    return (int)ilen - 1 - (int)z;
}

static void fmt_double(cos_sink_t *o, const cos_spec_t *sp, double v,
                       char conv)
{
    bool upper = (conv == 'F' || conv == 'E' || conv == 'G');
    char lc = (char)(conv | 0x20);

    if (fmt_special(o, sp, v, upper)) return;

    long prec = (sp->prec < 0) ? 6 : sp->prec;
    bool neg = d_is_neg(v);
    double a = neg ? -v : v;

    /* Room for the widest exact rendering: 309 integer digits, a
     * 400-digit fraction, the point and the exponent tail. */
    char body[EXPAND_CAP + 8];
    size_t blen = 0;

    if (lc == 'e') {
        int e10;
        blen = render_sci(a, prec, body, &e10, upper);
        if ((sp->flags & FL_ALT) && prec == 0) {
            /* # keeps the point even with no fraction digits; it has to
             * be inserted before the exponent, not appended. */
            char tmp[EXPAND_CAP + 8];
            size_t t = 0;
            tmp[t++] = body[0];
            tmp[t++] = '.';
            for (size_t i = 1; i < blen; ++i) tmp[t++] = body[i];
            for (size_t i = 0; i < t; ++i) body[i] = tmp[i];
            blen = t;
        }
    } else if (lc == 'g') {
        /* %g picks the shorter-looking form by exponent, then strips
         * trailing zeros unless # was given. */
        if (prec == 0) prec = 1;
        int exp10 = exact_exp10(a);

        if (exp10 < -4 || exp10 >= prec) {
            int e10;
            blen = render_sci(a, prec - 1, body, &e10, upper);
            if (!(sp->flags & FL_ALT)) {
                /* Trailing zeros are stripped from the MANTISSA, which
                 * means splicing out of the middle of the string rather
                 * than truncating it - 1000000.0 must print as "1e+06",
                 * not "1.00000e+06". */
                size_t epos = 0;
                while (epos < blen && body[epos] != 'e' && body[epos] != 'E') epos++;
                size_t end = epos;
                bool has_dot = false;
                for (size_t i = 0; i < epos; ++i) if (body[i] == '.') has_dot = true;
                if (has_dot) {
                    while (end > 0 && body[end - 1] == '0') end--;
                    if (end > 0 && body[end - 1] == '.') end--;
                    for (size_t i = 0; epos + i < blen; ++i) body[end + i] = body[epos + i];
                    blen -= (epos - end);
                }
            }
        } else {
            blen = render_fixed(a, prec - 1 - exp10, body);
            if (!(sp->flags & FL_ALT)) {
                bool has_dot = false;
                for (size_t i = 0; i < blen; ++i) if (body[i] == '.') has_dot = true;
                if (has_dot) {
                    while (blen && body[blen - 1] == '0') blen--;
                    if (blen && body[blen - 1] == '.') blen--;
                }
            }
        }
    } else {
        blen = render_fixed(a, prec, body);
        if ((sp->flags & FL_ALT) && prec == 0) body[blen++] = '.';
    }

    char prefix[2]; size_t plen = 0;
    if (neg)                       prefix[plen++] = '-';
    else if (sp->flags & FL_PLUS)  prefix[plen++] = '+';
    else if (sp->flags & FL_SPACE) prefix[plen++] = ' ';

    emit_padded(o, sp, prefix, plen, body, blen, 0);
}

/* ---- the driver --------------------------------------------------------- */

static size_t str_len_bounded(const char *s, long max)
{
    size_t n = 0;
    while (s[n] && (max < 0 || (long)n < max)) n++;
    return n;
}

int cos_vformat_sink(cos_sink_t *o, const char *fmt, __builtin_va_list ap);

int cos_vformat_sink(cos_sink_t *o, const char *fmt, __builtin_va_list ap)
{
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { sink_put(o, *fmt); continue; }
        ++fmt;

        cos_spec_t sp = { .flags = 0, .width = 0, .prec = -1, .lenmod = 0 };

        for (;; ++fmt) {
            if      (*fmt == '-') sp.flags |= FL_LEFT;
            else if (*fmt == '+') sp.flags |= FL_PLUS;
            else if (*fmt == ' ') sp.flags |= FL_SPACE;
            else if (*fmt == '#') sp.flags |= FL_ALT;
            else if (*fmt == '0') sp.flags |= FL_ZERO;
            else break;
        }

        if (*fmt == '*') {
            int w = __builtin_va_arg(ap, int);
            /* A negative * width means left-justify with |width|, which
             * is easy to drop and produces silently unaligned output. */
            if (w < 0) { sp.flags |= FL_LEFT; sp.width = -w; }
            else sp.width = w;
            ++fmt;
        } else {
            while (*fmt >= '0' && *fmt <= '9') sp.width = sp.width * 10 + (*fmt++ - '0');
        }

        if (*fmt == '.') {
            ++fmt;
            sp.prec = 0;
            if (*fmt == '*') {
                int p = __builtin_va_arg(ap, int);
                sp.prec = (p < 0) ? -1 : p;   /* negative precision == absent */
                ++fmt;
            } else {
                while (*fmt >= '0' && *fmt <= '9') sp.prec = sp.prec * 10 + (*fmt++ - '0');
            }
        }

        switch (*fmt) {
        case 'h': sp.lenmod = (fmt[1] == 'h') ? (++fmt, 2) : 1; ++fmt; break;
        case 'l': sp.lenmod = (fmt[1] == 'l') ? (++fmt, 4) : 3; ++fmt; break;
        case 'z': sp.lenmod = 5; ++fmt; break;
        case 't': sp.lenmod = 6; ++fmt; break;
        case 'j': sp.lenmod = 7; ++fmt; break;
        case 'L': sp.lenmod = 8; ++fmt; break;
        default: break;
        }

        switch (*fmt) {
        case '\0':
            /* A format string ending in '%' - stop rather than running
             * off the end of the buffer. */
            return (int)o->len;

        case '%': sink_put(o, '%'); break;

        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            cos_spec_t eff = sp; eff.flags &= ~FL_ZERO;
            emit_padded(o, &eff, "", 0, &c, 1, 0);
            break;
        }

        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            /* Precision on %s is a MAXIMUM length, and the string need
             * not be NUL-terminated within it - which is exactly why
             * this cannot just call strlen(). */
            size_t n = str_len_bounded(s, sp.prec);
            cos_spec_t eff = sp; eff.flags &= ~FL_ZERO;
            emit_padded(o, &eff, "", 0, s, n, 0);
            break;
        }

        case 'd': case 'i': {
            int64_t v;
            switch (sp.lenmod) {
            case 1: v = (short)__builtin_va_arg(ap, int); break;
            case 2: v = (signed char)__builtin_va_arg(ap, int); break;
            case 3: v = __builtin_va_arg(ap, long); break;
            case 4: v = __builtin_va_arg(ap, long long); break;
            case 5: case 6: case 7: v = __builtin_va_arg(ap, long); break;
            default: v = __builtin_va_arg(ap, int); break;
            }
            bool neg = v < 0;
            /* Negating INT64_MIN overflows; convert through the unsigned
             * domain instead. */
            uint64_t mag = neg ? (~(uint64_t)v + 1ULL) : (uint64_t)v;
            fmt_integer(o, &sp, mag, neg, 10, false);
            break;
        }

        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v;
            switch (sp.lenmod) {
            case 1: v = (unsigned short)__builtin_va_arg(ap, unsigned int); break;
            case 2: v = (unsigned char)__builtin_va_arg(ap, unsigned int); break;
            case 3: v = __builtin_va_arg(ap, unsigned long); break;
            case 4: v = __builtin_va_arg(ap, unsigned long long); break;
            case 5: case 6: case 7: v = __builtin_va_arg(ap, unsigned long); break;
            default: v = __builtin_va_arg(ap, unsigned int); break;
            }
            unsigned base = (*fmt == 'o') ? 8 : (*fmt == 'u') ? 10 : 16;
            fmt_integer(o, &sp, v, false, base, *fmt == 'X');
            break;
        }

        case 'p': {
            void *p = __builtin_va_arg(ap, void *);
            if (!p) {
                cos_spec_t eff = sp; eff.flags &= ~FL_ZERO;
                emit_padded(o, &eff, "", 0, "(nil)", 5, 0);
            } else {
                cos_spec_t eff = sp;
                eff.flags |= FL_ALT;
                eff.prec = -1;
                fmt_integer(o, &eff, (uint64_t)(uintptr_t)p, false, 16, false);
            }
            break;
        }

        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = __builtin_va_arg(ap, double);
            fmt_double(o, &sp, v, *fmt);
            break;
        }

        case 'n':
            /* Refused, not ignored. See the header note: this is an
             * arbitrary-write primitive and a program using it should
             * find out here rather than silently produce nothing. */
            sink_putn(o, "<%n refused>", 12);
            break;

        default:
            /* An unknown conversion: emit it literally so the output
             * shows what was not understood, rather than swallowing it. */
            sink_put(o, '%');
            sink_put(o, *fmt);
            break;
        }
    }
    return (int)o->len;
}

/* ---- public entry points ------------------------------------------------ */

int cos_vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    cos_sink_t o = { .buf = buf, .cap = size, .len = 0 };
    int n = cos_vformat_sink(&o, fmt, ap);
    if (buf && size) buf[(o.len < size) ? o.len : size - 1] = '\0';
    return n;
}

int cos_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = cos_vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

/* printf goes through a fixed staging buffer rather than writing one
 * character per syscall: SYS_WRITE is a ring transition, and a
 * per-character one turns a 60-column line into 60 of them. */
static void printf_flush(const char *s, size_t n, void *ctx)
{
    (void)ctx;
    (void)cos_write(s, n);
}

int cos_vprintf(const char *fmt, __builtin_va_list ap)
{
    char stage[512];
    cos_sink_t o = { .buf = stage, .cap = sizeof(stage), .len = 0 };
    int n = cos_vformat_sink(&o, fmt, ap);
    size_t have = (o.len < sizeof(stage)) ? o.len : sizeof(stage) - 1;
    if (have) printf_flush(stage, have, NULL);
    /* Output longer than the staging buffer is truncated on the console
     * but still COUNTED correctly, matching what snprintf reports. Said
     * out loud because a silently shortened log line is confusing. */
    if (o.len >= sizeof(stage)) {
        printf_flush("...<truncated>", 14, NULL);
    }
    return n;
}

int cos_printf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = cos_vprintf(fmt, ap);
    __builtin_va_end(ap);
    return n;
}
