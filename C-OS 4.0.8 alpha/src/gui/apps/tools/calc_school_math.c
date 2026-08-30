
#include "calc_school_math.h"
#include <stdio.h>
#include <string.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef struct { long long n, d; } rat_t;
typedef struct { double a, b; int ok; } lin_t;

static int sm_parse_int64_prefix(const char** p, long long* out);
static int sm_parse_double_prefix(const char** p, double* out);

static double sm_abs(double x) { return x < 0.0 ? -x : x; }
static int sm_is_space(char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r'; }
static int sm_is_digit(char c) { return c >= '0' && c <= '9'; }
static int sm_is_alpha(char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z'); }

static void sm_copy(char* dst, size_t dst_sz, const char* src);

static int sm_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

static void sm_format_unsigned_base(unsigned long long value, unsigned base, char* out, size_t out_sz) {
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char tmp[70];
    int n = 0;
    if (!out || out_sz == 0) return;
    if (base < 2 || base > 36) { sm_copy(out, out_sz, "Error"); return; }
    if (value == 0) { sm_copy(out, out_sz, "0"); return; }
    while (value > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    size_t pos = 0;
    while (n > 0 && pos + 1 < out_sz) out[pos++] = tmp[--n];
    out[pos] = '\0';
}

static void sm_format_signed_base(long long value, unsigned base, const char* prefix, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (base < 2 || base > 36) { sm_copy(out, out_sz, "Error"); return; }
    if (value < 0) {
        unsigned long long mag = (value == (long long)(-9223372036854775807LL - 1LL)) ? 9223372036854775808ULL : (unsigned long long)(-value);
        char buf[80];
        sm_format_unsigned_base(mag, base, buf, sizeof(buf));
        if (prefix && prefix[0]) snprintf(out, out_sz, "-%s%s", prefix, buf);
        else snprintf(out, out_sz, "-%s", buf);
    } else {
        char buf[80];
        sm_format_unsigned_base((unsigned long long)value, base, buf, sizeof(buf));
        if (prefix && prefix[0]) snprintf(out, out_sz, "%s%s", prefix, buf);
        else sm_copy(out, out_sz, buf);
    }
}

static void sm_copy(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < dst_sz && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

static void sm_append(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0 || !src) return;
    size_t len = strlen(dst);
    size_t i = 0;
    while (len + i + 1 < dst_sz && src[i]) { dst[len+i] = src[i]; i++; }
    dst[len+i] = '\0';
}

static void sm_append_ch(char* dst, size_t dst_sz, char ch) {
    if (!dst || dst_sz < 2) return;
    size_t len = strlen(dst);
    if (len + 1 < dst_sz) {
        dst[len] = ch;
        dst[len+1] = '\0';
    }
}

static int sm_parse_double_token(const char* s, double* out) {
    if (!s || !out) return 0;
    const char* p = s;
    double v = 0.0;
    if (!sm_parse_double_prefix(&p, &v)) return 0;
    *out = v;
    return 1;
}

static int sm_parse_ll_token(const char* s, long long* out) {
    if (!s || !out) return 0;
    const char* p = s;
    return sm_parse_int64_prefix(&p, out);
}

static void sm_trim(char* s) {
    if (!s) return;
    size_t len = strlen(s), start = 0, end = len;
    while (start < len && sm_is_space(s[start])) start++;
    while (end > start && sm_is_space(s[end-1])) end--;
    if (start > 0) memmove(s, s + start, end - start);
    s[end - start] = '\0';
}

static void sm_tolower(char* s) {
    if (!s) return;
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static int sm_starts_with_ci(const char* s, const char* prefix) {
    while (s && prefix && *s && *prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        ++s; ++prefix;
    }
    return prefix && *prefix == '\0';
}

static int sm_parse_int64_prefix(const char** p, long long* out) {
    if (!p || !*p || !out) return 0;
    const char* s = *p;
    while (sm_is_space(*s)) s++;
    int sign = 1;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1;
        s++;
    }

    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O' || s[1] == 'd' || s[1] == 'D')) {
        char b = s[1];
        if (b == 'x' || b == 'X') base = 16;
        else if (b == 'b' || b == 'B') base = 2;
        else if (b == 'o' || b == 'O') base = 8;
        else base = 10;
        s += 2;
    }

    const unsigned long long limit_pos = 9223372036854775807ULL;
    const unsigned long long limit_neg = 9223372036854775808ULL;
    unsigned long long v = 0;
    int seen = 0;
    while (*s) {
        if (*s == '_') { s++; continue; }
        int d = sm_digit_value(*s);
        if (d < 0 || (unsigned)d >= base) break;
        seen = 1;
        unsigned long long limit = (sign > 0) ? limit_pos : limit_neg;
        if (v > (limit - (unsigned long long)d) / (unsigned long long)base) return 0;
        v = v * (unsigned long long)base + (unsigned long long)d;
        s++;
    }
    while (sm_is_space(*s)) s++;
    if (!seen || *s != '\0') return 0;
    if (sign > 0) *out = (long long)v;
    else if (v == limit_neg) *out = (long long)(-9223372036854775807LL - 1LL);
    else *out = -(long long)v;
    *p = s;
    return 1;
}

static int sm_parse_double_prefix(const char** p, double* out) {
    if (!p || !*p || !out) return 0;
    const char* s = *p;
    while (sm_is_space(*s)) s++;
    int sign = 1;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1;
        s++;
    }

    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O' || s[1] == 'd' || s[1] == 'D')) {
        char b = s[1];
        if (b == 'x' || b == 'X') base = 16;
        else if (b == 'b' || b == 'B') base = 2;
        else if (b == 'o' || b == 'O') base = 8;
        else base = 10;
        s += 2;
        unsigned long long iv = 0;
        int seen = 0;
        while (*s) {
            if (*s == '_') { s++; continue; }
            int d = sm_digit_value(*s);
            if (d < 0 || (unsigned)d >= base) break;
            seen = 1;
            iv = iv * (unsigned long long)base + (unsigned long long)d;
            s++;
        }
        while (sm_is_space(*s)) s++;
        if (!seen || *s != '\0') return 0;
        *out = sign < 0 ? -(double)iv : (double)iv;
        *p = s;
        return 1;
    }

    double value = 0.0;
    int saw_digit = 0;
    while (*s) {
        if (*s == '_') { s++; continue; }
        if (!sm_is_digit(*s)) break;
        saw_digit = 1;
        value = value * 10.0 + (double)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s) {
            if (*s == '_') { s++; continue; }
            if (!sm_is_digit(*s)) break;
            saw_digit = 1;
            value += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (!saw_digit) return 0;
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        if (*s == '+' || *s == '-') {
            if (*s == '-') exp_sign = -1;
            s++;
        }
        if (!sm_is_digit(*s)) return 0;
        int expv = 0;
        while (*s) {
            if (*s == '_') { s++; continue; }
            if (!sm_is_digit(*s)) break;
            if (expv < 10000) expv = expv * 10 + (*s - '0');
            s++;
        }
        if (exp_sign < 0) expv = -expv;
        double scale = 1.0;
        int n = expv < 0 ? -expv : expv;
        while (n-- > 0) scale *= 10.0;
        value = (expv < 0) ? (value / scale) : (value * scale);
    }
    while (sm_is_space(*s)) s++;
    if (*s != '\0') return 0;
    *out = sign < 0 ? -value : value;
    *p = s;
    return 1;
}


static long long sm_llabs(long long v) { return v < 0 ? -v : v; }

static long long sm_gcd(long long a, long long b) {
    a = sm_llabs(a); b = sm_llabs(b);
    while (b) { long long t = a % b; a = b; b = t; }
    return a ? a : 1;
}

static rat_t sm_rat_norm(long long n, long long d) {
    if (d == 0) return (rat_t){0,0};
    if (d < 0) { n = -n; d = -d; }
    long long g = sm_gcd(n, d);
    return (rat_t){ n / g, d / g };
}

static rat_t sm_rat(long long n) { return (rat_t){ n, 1 }; }
static rat_t sm_rat_zero(void) { return (rat_t){ 0, 1 }; }
static rat_t sm_rat_add(rat_t a, rat_t b) { return sm_rat_norm(a.n*b.d + b.n*a.d, a.d*b.d); }
static rat_t sm_rat_sub(rat_t a, rat_t b) { return sm_rat_norm(a.n*b.d - b.n*a.d, a.d*b.d); }
static rat_t sm_rat_mul(rat_t a, rat_t b) { return sm_rat_norm(a.n*b.n, a.d*b.d); }
static rat_t sm_rat_div(rat_t a, rat_t b) { return sm_rat_norm(a.n*b.d, a.d*b.n); }

static rat_t sm_rat_pow(rat_t base, long long exp) {
    if (exp == 0) return sm_rat(1);
    if (exp < 0) {
        if (base.n == 0) return sm_rat_zero();
        base = sm_rat_norm(base.d, base.n);
        exp = -exp;
    }
    rat_t out = sm_rat(1);
    while (exp > 0) {
        if (exp & 1LL) out = sm_rat_mul(out, base);
        base = sm_rat_mul(base, base);
        exp >>= 1LL;
    }
    return out;
}

static void sm_rat_to_text(rat_t r, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (r.d == 0) { sm_copy(out, out_sz, "undefined"); return; }
    if (r.d == 1) { snprintf(out, out_sz, "%lld", r.n); return; }
    if (r.n == 0) { sm_copy(out, out_sz, "0"); return; }
    if (sm_llabs(r.n) >= r.d) {
        long long whole = r.n / r.d;
        long long rem = sm_llabs(r.n % r.d);
        if (rem == 0) { snprintf(out, out_sz, "%lld", whole); return; }
        snprintf(out, out_sz, "%lld %lld/%lld", whole, rem, r.d);
        return;
    }
    snprintf(out, out_sz, "%lld/%lld", r.n, r.d);
}

static int sm_parse_int(const char** p, long long* out) {
    const char* s = *p;
    while (sm_is_space(*s)) s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if (!sm_is_digit(*s)) return 0;
    long long v = 0;
    while (sm_is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
    *out = v * sign;
    *p = s;
    return 1;
}

static rat_t sm_parse_rat_expr(const char** p, int* ok);

static rat_t sm_parse_rat_primary(const char** p, int* ok) {
    while (sm_is_space(**p)) (*p)++;
    if (**p == '(') {
        (*p)++;
        rat_t v = sm_parse_rat_expr(p, ok);
        while (sm_is_space(**p)) (*p)++;
        if (**p == ')') (*p)++; else *ok = 0;
        return v;
    }
    if (**p == '+' || **p == '-') {
        char s = **p; (*p)++;
        rat_t v = sm_parse_rat_primary(p, ok);
        return (s == '-') ? sm_rat_sub(sm_rat_zero(), v) : v;
    }
    long long v = 0;
    if (sm_parse_int(p, &v)) return sm_rat(v);
    *ok = 0;
    return sm_rat_zero();
}

static rat_t sm_parse_rat_power(const char** p, int* ok) {
    rat_t v = sm_parse_rat_primary(p, ok);
    if (!*ok) return sm_rat_zero();
    while (sm_is_space(**p)) (*p)++;
    if (**p == '^') {
        (*p)++;
        long long expv = 0;
        if (!sm_parse_int(p, &expv)) { *ok = 0; return sm_rat_zero(); }
        if (expv > 32 || expv < -32) { *ok = 0; return sm_rat_zero(); }
        return sm_rat_pow(v, expv);
    }
    return v;
}

static rat_t sm_parse_rat_term(const char** p, int* ok) {
    rat_t v = sm_parse_rat_power(p, ok);
    if (!*ok) return sm_rat_zero();
    for (;;) {
        while (sm_is_space(**p)) (*p)++;
        if (**p == '*' || **p == '/') {
            char op = **p; (*p)++;
            rat_t rhs = sm_parse_rat_power(p, ok);
            if (!*ok) return sm_rat_zero();
            v = (op == '*') ? sm_rat_mul(v, rhs) : sm_rat_div(v, rhs);
        } else break;
    }
    return v;
}

static rat_t sm_parse_rat_expr(const char** p, int* ok) {
    rat_t v = sm_parse_rat_term(p, ok);
    if (!*ok) return sm_rat_zero();
    for (;;) {
        while (sm_is_space(**p)) (*p)++;
        if (**p == '+' || **p == '-') {
            char op = **p; (*p)++;
            rat_t rhs = sm_parse_rat_term(p, ok);
            if (!*ok) return sm_rat_zero();
            v = (op == '+') ? sm_rat_add(v, rhs) : sm_rat_sub(v, rhs);
        } else break;
    }
    return v;
}

static int sm_rat_eval(const char* expr, rat_t* out) {
    const char* p = expr;
    int ok = 1;
    rat_t v = sm_parse_rat_expr(&p, &ok);
    while (sm_is_space(*p)) p++;
    if (!ok || *p) return 0;
    *out = v;
    return 1;
}



#define SM_CAS_MAX_DEG 16

typedef struct {
    rat_t c[SM_CAS_MAX_DEG + 1];
    int deg;
} sm_poly_t;

static rat_t sm_rat_abs(rat_t r) {
    if (r.n < 0) r.n = -r.n;
    return r;
}

static int sm_rat_is_zero(rat_t r) { return r.n == 0; }
static int sm_rat_is_one(rat_t r) { return r.n == 1 && r.d == 1; }
static int sm_rat_is_minus_one(rat_t r) { return r.n == -1 && r.d == 1; }

static void sm_poly_zero(sm_poly_t* p) {
    if (!p) return;
    for (int i = 0; i <= SM_CAS_MAX_DEG; ++i) p->c[i] = sm_rat_zero();
    p->deg = 0;
}

static void sm_poly_normalize(sm_poly_t* p) {
    if (!p) return;
    int d = SM_CAS_MAX_DEG;
    while (d > 0 && sm_rat_is_zero(p->c[d])) d--;
    p->deg = d;
    if (d == 0 && sm_rat_is_zero(p->c[0])) p->c[0] = sm_rat_zero();
}

static sm_poly_t sm_poly_const(rat_t v) {
    sm_poly_t p;
    sm_poly_zero(&p);
    p.c[0] = v;
    sm_poly_normalize(&p);
    return p;
}

static sm_poly_t sm_poly_x(void) {
    sm_poly_t p;
    sm_poly_zero(&p);
    p.c[1] = sm_rat(1);
    p.deg = 1;
    return p;
}

static int sm_poly_is_zero(const sm_poly_t* p) {
    return p && p->deg == 0 && sm_rat_is_zero(p->c[0]);
}

static sm_poly_t sm_poly_add(sm_poly_t a, sm_poly_t b, int* ok) {
    sm_poly_t r;
    sm_poly_zero(&r);
    int md = a.deg > b.deg ? a.deg : b.deg;
    if (md > SM_CAS_MAX_DEG) { if (ok) *ok = 0; return sm_poly_const(sm_rat_zero()); }
    for (int i = 0; i <= md; ++i) r.c[i] = sm_rat_add(a.c[i], b.c[i]);
    r.deg = md;
    sm_poly_normalize(&r);
    return r;
}

static sm_poly_t sm_poly_sub(sm_poly_t a, sm_poly_t b, int* ok) {
    sm_poly_t r;
    sm_poly_zero(&r);
    int md = a.deg > b.deg ? a.deg : b.deg;
    if (md > SM_CAS_MAX_DEG) { if (ok) *ok = 0; return sm_poly_const(sm_rat_zero()); }
    for (int i = 0; i <= md; ++i) r.c[i] = sm_rat_sub(a.c[i], b.c[i]);
    r.deg = md;
    sm_poly_normalize(&r);
    return r;
}

static sm_poly_t sm_poly_mul(sm_poly_t a, sm_poly_t b, int* ok) {
    sm_poly_t r;
    sm_poly_zero(&r);
    if (a.deg == 0 && sm_rat_is_zero(a.c[0])) return r;
    if (b.deg == 0 && sm_rat_is_zero(b.c[0])) return r;
    int md = a.deg + b.deg;
    if (md > SM_CAS_MAX_DEG) { if (ok) *ok = 0; return sm_poly_const(sm_rat_zero()); }
    for (int i = 0; i <= a.deg; ++i) {
        for (int j = 0; j <= b.deg; ++j) {
            r.c[i + j] = sm_rat_add(r.c[i + j], sm_rat_mul(a.c[i], b.c[j]));
        }
    }
    r.deg = md;
    sm_poly_normalize(&r);
    return r;
}

static sm_poly_t sm_poly_scale(sm_poly_t a, rat_t s) {
    sm_poly_t r;
    sm_poly_zero(&r);
    if (sm_rat_is_zero(s)) return r;
    for (int i = 0; i <= a.deg; ++i) r.c[i] = sm_rat_mul(a.c[i], s);
    r.deg = a.deg;
    sm_poly_normalize(&r);
    return r;
}

static sm_poly_t sm_poly_pow_int(sm_poly_t base, long long exp, int* ok) {
    if (exp < 0) { if (ok) *ok = 0; return sm_poly_const(sm_rat_zero()); }
    sm_poly_t r = sm_poly_const(sm_rat(1));
    while (exp > 0) {
        if (exp & 1LL) r = sm_poly_mul(r, base, ok);
        if (ok && !*ok) return r;
        exp >>= 1LL;
        if (exp) base = sm_poly_mul(base, base, ok);
        if (ok && !*ok) return r;
    }
    return r;
}

static sm_poly_t sm_poly_derivative(sm_poly_t p) {
    sm_poly_t r;
    sm_poly_zero(&r);
    if (p.deg == 0) return r;
    for (int i = 1; i <= p.deg && i <= SM_CAS_MAX_DEG; ++i) {
        r.c[i - 1] = sm_rat_mul(p.c[i], sm_rat(i));
    }
    r.deg = p.deg - 1;
    sm_poly_normalize(&r);
    return r;
}

static int sm_poly_all_integer(const sm_poly_t* p) {
    if (!p) return 0;
    for (int i = 0; i <= p->deg; ++i) if (p->c[i].d != 1) return 0;
    return 1;
}

static long long sm_poly_int_gcd(const sm_poly_t* p) {
    long long g = 0;
    if (!p) return 1;
    for (int i = 0; i <= p->deg; ++i) {
        long long v = p->c[i].n;
        if (v == 0) continue;
        g = (g == 0) ? sm_llabs(v) : sm_gcd(g, v);
    }
    return g == 0 ? 1 : g;
}


static void sm_rat_append_text(rat_t r, char* out, size_t out_sz) {
    char b[64];
    sm_rat_to_text(r, b, sizeof(b));
    sm_append(out, out_sz, b);
}

static void sm_poly_term_to_text(rat_t coeff, int degree, char* out, size_t out_sz, int first) {
    if (!out || out_sz == 0) return;
    char buf[64];
    rat_t abscoeff = coeff;
    if (abscoeff.n < 0) abscoeff.n = -abscoeff.n;

    if (degree == 0) {
        sm_rat_to_text(first ? coeff : abscoeff, buf, sizeof(buf));
        if (!first) sm_append(out, out_sz, coeff.n < 0 ? " - " : " + ");
        sm_append(out, out_sz, first ? buf : (coeff.n < 0 ? buf : buf));
        return;
    }

    if (!first) sm_append(out, out_sz, coeff.n < 0 ? " - " : " + ");
    else if (coeff.n < 0) sm_append(out, out_sz, "-");

    if (!sm_rat_is_one(abscoeff)) {
        sm_rat_to_text(abscoeff, buf, sizeof(buf));
        sm_append(out, out_sz, buf);
    }

    sm_append(out, out_sz, "x");
    if (degree > 1) {
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "^%d", degree);
        sm_append(out, out_sz, dbuf);
    }
}

static void sm_poly_to_text(const sm_poly_t* p, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!p) { sm_copy(out, out_sz, "Error"); return; }
    if (sm_poly_is_zero(p)) { sm_copy(out, out_sz, "0"); return; }
    int first = 1;
    for (int d = p->deg; d >= 0; --d) {
        if (sm_rat_is_zero(p->c[d])) continue;
        sm_poly_term_to_text(p->c[d], d, out, out_sz, first);
        first = 0;
    }
    if (out[0] == '\0') sm_copy(out, out_sz, "0");
}

static int sm_cas_is_factor_start(char c) {
    return sm_is_digit(c) || c == '.' || c == '(' || c == 'x' || c == 'X';
}

static int sm_cas_parse_signed_int(const char** p, long long* out) {
    if (!p || !*p || !out) return 0;
    const char* s = *p;
    while (sm_is_space(*s)) s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if (!sm_is_digit(*s)) return 0;
    long long v = 0;
    while (sm_is_digit(*s)) {
        if (v > 922337203685477580LL) return 0;
        v = v * 10 + (*s - '0');
        s++;
    }
    while (sm_is_space(*s)) s++;
    *out = sign < 0 ? -v : v;
    *p = s;
    return 1;
}

static int sm_cas_parse_number_rat(const char** p, rat_t* out) {
    if (!p || !*p || !out) return 0;
    const char* s = *p;
    while (sm_is_space(*s)) s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    char digits[64];
    int nd = 0, frac = 0, saw = 0, seen_dot = 0;
    while (*s) {
        if (*s == '_') { s++; continue; }
        if (*s == '.') {
            if (seen_dot) return 0;
            seen_dot = 1;
            s++;
            continue;
        }
        if (!sm_is_digit(*s)) break;
        if (nd + 1 >= (int)sizeof(digits)) return 0;
        digits[nd++] = *s;
        if (seen_dot) frac++;
        saw = 1;
        s++;
    }
    while (sm_is_space(*s)) s++;
    if (!saw) return 0;
    digits[nd] = '\0';
    long long n = 0;
    for (int i = 0; i < nd; ++i) {
        if (n > 922337203685477580LL) return 0;
        n = n * 10 + (digits[i] - '0');
    }
    long long d = 1;
    while (frac-- > 0) {
        if (d > 922337203685477580LL / 10LL) return 0;
        d *= 10LL;
    }
    if (sign < 0) n = -n;
    *out = sm_rat_norm(n, d);
    *p = s;
    return 1;
}

static sm_poly_t sm_cas_parse_expr(const char** p, int* ok);

static sm_poly_t sm_cas_parse_primary(const char** p, int* ok) {
    while (sm_is_space(**p)) (*p)++;
    if (**p == '(') {
        (*p)++;
        sm_poly_t v = sm_cas_parse_expr(p, ok);
        while (sm_is_space(**p)) (*p)++;
        if (**p == ')') (*p)++; else *ok = 0;
        return v;
    }
    if (**p == '+' || **p == '-') {
        char sign = **p;
        (*p)++;
        sm_poly_t v = sm_cas_parse_primary(p, ok);
        if (sign == '-') v = sm_poly_scale(v, sm_rat(-1));
        return v;
    }
    if (**p == 'x' || **p == 'X') {
        (*p)++;
        return sm_poly_x();
    }
    if (sm_is_alpha(**p)) {
        char ident[16];
        int i = 0;
        while (sm_is_alpha(**p) && i < (int)sizeof(ident) - 1) ident[i++] = (char)*((*p)++);
        ident[i] = '\0';
        while (sm_is_space(**p)) (*p)++;
        if (!strcmp(ident, "pi") || !strcmp(ident, "e") || !strcmp(ident, "tau")) {
            *ok = 0;
            return sm_poly_const(sm_rat_zero());
        }
        *ok = 0;
        return sm_poly_const(sm_rat_zero());
    }
    rat_t n;
    if (sm_cas_parse_number_rat(p, &n)) return sm_poly_const(n);
    *ok = 0;
    return sm_poly_const(sm_rat_zero());
}

static sm_poly_t sm_cas_parse_power(const char** p, int* ok) {
    sm_poly_t base = sm_cas_parse_primary(p, ok);
    if (!*ok) return base;
    while (sm_is_space(**p)) (*p)++;
    if (**p == '^') {
        (*p)++;
        long long expv = 0;
        if (!sm_cas_parse_signed_int(p, &expv)) { *ok = 0; return sm_poly_const(sm_rat_zero()); }
        return sm_poly_pow_int(base, expv, ok);
    }
    return base;
}

static sm_poly_t sm_cas_parse_term(const char** p, int* ok) {
    sm_poly_t v = sm_cas_parse_power(p, ok);
    if (!*ok) return v;
    while (1) {
        const char* save = *p;
        while (sm_is_space(**p)) (*p)++;
        if (**p == '*' || **p == '/') {
            char op = **p;
            (*p)++;
            sm_poly_t rhs = sm_cas_parse_power(p, ok);
            if (!*ok) return v;
            if (op == '*') v = sm_poly_mul(v, rhs, ok);
            else {
                if (rhs.deg != 0 || sm_rat_is_zero(rhs.c[0])) { *ok = 0; return sm_poly_const(sm_rat_zero()); }
                v = sm_poly_scale(v, sm_rat_norm(rhs.c[0].d, rhs.c[0].n));
            }
            continue;
        }
        if (sm_cas_is_factor_start(**p)) {
            sm_poly_t rhs = sm_cas_parse_power(p, ok);
            if (!*ok) return v;
            v = sm_poly_mul(v, rhs, ok);
            continue;
        }
        *p = save;
        break;
    }
    return v;
}

static sm_poly_t sm_cas_parse_expr(const char** p, int* ok) {
    sm_poly_t v = sm_cas_parse_term(p, ok);
    if (!*ok) return v;
    while (1) {
        while (sm_is_space(**p)) (*p)++;
        if (**p == '+' || **p == '-') {
            char op = **p;
            (*p)++;
            sm_poly_t rhs = sm_cas_parse_term(p, ok);
            if (!*ok) return v;
            v = (op == '+') ? sm_poly_add(v, rhs, ok) : sm_poly_sub(v, rhs, ok);
        } else break;
    }
    return v;
}

static int sm_cas_parse_poly(const char* expr, sm_poly_t* out) {
    if (!expr || !out) return 0;
    const char* p = expr;
    int ok = 1;
    sm_poly_t v = sm_cas_parse_expr(&p, &ok);
    while (sm_is_space(*p)) p++;
    if (!ok || *p) return 0;
    sm_poly_normalize(&v);
    *out = v;
    return 1;
}

static void sm_cas_format_factor_linear(rat_t root, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    sm_append(out, out_sz, "(x ");
    if (root.n < 0) {
        sm_append(out, out_sz, "+ ");
        root.n = -root.n;
    } else {
        sm_append(out, out_sz, "- ");
    }
    char rb[64];
    sm_rat_to_text(root, rb, sizeof(rb));
    sm_append(out, out_sz, rb);
    sm_append(out, out_sz, ")");
}

static int sm_cas_try_factor_quadratic(const sm_poly_t* p, char* out, size_t out_sz) {
    if (!p || p->deg != 2) return 0;
    if (!sm_poly_all_integer(p)) return 0;

    long long a = p->c[2].n, b = p->c[1].n, c = p->c[0].n;
    if (a == 0) return 0;

    out[0] = '\0';

    long long g = sm_poly_int_gcd(p);
    if (g > 1) {
        char gb[32];
        snprintf(gb, sizeof(gb), "%lld", g);
        sm_append(out, out_sz, gb);
        sm_append(out, out_sz, " * ");
        a /= g; b /= g; c /= g;
    }

    if (c == 0) {
        // ax^2 + bx = x(ax + b)
        char inner[96];
        sm_poly_t lin;
        sm_poly_zero(&lin);
        lin.c[1] = sm_rat(a);
        lin.c[0] = sm_rat(b);
        lin.deg = 1;
        sm_poly_normalize(&lin);
        sm_poly_to_text(&lin, inner, sizeof(inner));
        sm_append(out, out_sz, "x * ");
        sm_append(out, out_sz, inner);
        return 1;
    }

    long long d = b * b - 4LL * a * c;
    if (d < 0) return 0;
    long long s = 0;
    while (s * s < d) {
        if (s > 3037000499LL) return 0;
        s++;
    }
    if (s * s != d) return 0;
    long long den = 2LL * a;
    rat_t r1 = sm_rat_norm(-b + s, den);
    rat_t r2 = sm_rat_norm(-b - s, den);
    if (a != 1) {
        char ab[32];
        snprintf(ab, sizeof(ab), "%lld", a);
        sm_append(out, out_sz, ab);
        sm_append(out, out_sz, " * ");
    }
    sm_cas_format_factor_linear(r1, out + strlen(out), out_sz - strlen(out));
    sm_append(out, out_sz, " * ");
    sm_cas_format_factor_linear(r2, out + strlen(out), out_sz - strlen(out));
    return 1;
}

static int sm_eval_cas_poly(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz, int mode) {
    sm_poly_t p;
    if (!sm_cas_parse_poly(inside, &p)) return 0;
    char txt[256];
    char line[384];
    txt[0] = '\0';
    line[0] = '\0';

    if (mode == 1) {
        sm_poly_t d = sm_poly_derivative(p);
        sm_poly_to_text(&d, txt, sizeof(txt));
        sm_copy(status, status_sz, "CAS derivative");
        sm_append(line, sizeof(line), "d/dx(");
        sm_append(line, sizeof(line), inside);
        sm_append(line, sizeof(line), ") = ");
        sm_append(line, sizeof(line), txt);
        sm_append(line, sizeof(line), "\n");
    } else if (mode == 2) {
        if (!sm_cas_try_factor_quadratic(&p, txt, sizeof(txt))) {
            sm_poly_to_text(&p, txt, sizeof(txt));
            sm_copy(status, status_sz, "CAS factor (partial)");
            sm_append(line, sizeof(line), "factor(");
            sm_append(line, sizeof(line), inside);
            sm_append(line, sizeof(line), ") = ");
            sm_append(line, sizeof(line), txt);
            sm_append(line, sizeof(line), "\n");
            sm_append(line, sizeof(line), "No exact quadratic split found.\n");
        } else {
            sm_copy(status, status_sz, "CAS factor");
            sm_append(line, sizeof(line), "factor(");
            sm_append(line, sizeof(line), inside);
            sm_append(line, sizeof(line), ") = ");
            sm_append(line, sizeof(line), txt);
            sm_append(line, sizeof(line), "\n");
        }
    } else {
        sm_poly_to_text(&p, txt, sizeof(txt));
        sm_copy(status, status_sz, (mode == 3) ? "CAS expand" : "CAS simplify");
        sm_append(line, sizeof(line), (mode == 3) ? "expand(" : "simplify(");
        sm_append(line, sizeof(line), inside);
        sm_append(line, sizeof(line), ") = ");
        sm_append(line, sizeof(line), txt);
        sm_append(line, sizeof(line), "\n");
    }
    sm_copy(display, display_sz, txt);
    if (steps && steps_sz) sm_copy(steps, steps_sz, line);
    return 1;
}

static double sm_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 16; ++i) g = 0.5 * (g + x / g);
    return g;
}

static double sm_atan(double x) {
    double a = sm_abs(x);
    return (3.14159265358979323846 / 4.0) * x - x * (a - 1.0) * (0.2447 + 0.0663 * a);
}

static double sm_atan2(double y, double x) {
    const double pi = 3.14159265358979323846;
    if (x > 0.0) return sm_atan(y / x);
    if (x < 0.0 && y >= 0.0) return sm_atan(y / x) + pi;
    if (x < 0.0 && y < 0.0) return sm_atan(y / x) - pi;
    if (x == 0.0 && y > 0.0) return pi / 2.0;
    if (x == 0.0 && y < 0.0) return -pi / 2.0;
    return 0.0;
}

static double sm_deg(double rad) { return rad * 180.0 / 3.14159265358979323846; }

static void sm_fmt_double(double v, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (v != v) { sm_copy(out, out_sz, "Error"); return; }
    if (sm_abs(v) < 1e-10) v = 0.0;
    snprintf(out, out_sz, "%.8f", v);
    size_t len = strlen(out);
    while (len > 0 && out[len-1] == '0') out[--len] = '\0';
    if (len > 0 && out[len-1] == '.') out[--len] = '\0';
    if (len == 0) sm_copy(out, out_sz, "0");
}


static double sm_floor_approx(double x) {
    long long i = (long long)x;
    if ((double)i > x) i -= 1LL;
    return (double)i;
}

static uint32_t sm_noise_hash_u32(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u + 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return h;
}

static double sm_noise_hash01(int x, int y, int z) {
    uint32_t h = sm_noise_hash_u32((uint32_t)x, (uint32_t)y, (uint32_t)z);
    return (double)(h & 0x00FFFFFFu) / 16777215.0;
}

static double sm_noise_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

static double sm_noise_fade(double t) {
    return t * t * (3.0 - 2.0 * t);
}

static double sm_noise_value1d(double x) {
    int x0 = (int)sm_floor_approx(x);
    int x1 = x0 + 1;
    double t = x - (double)x0;
    double a = sm_noise_hash01(x0, 0, 0) * 2.0 - 1.0;
    double b = sm_noise_hash01(x1, 0, 0) * 2.0 - 1.0;
    return sm_noise_lerp(a, b, sm_noise_fade(t));
}

static double sm_noise_value2d(double x, double y) {
    int x0 = (int)sm_floor_approx(x);
    int y0 = (int)sm_floor_approx(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    double tx = x - (double)x0;
    double ty = y - (double)y0;
    double n00 = sm_noise_hash01(x0, y0, 0) * 2.0 - 1.0;
    double n10 = sm_noise_hash01(x1, y0, 0) * 2.0 - 1.0;
    double n01 = sm_noise_hash01(x0, y1, 0) * 2.0 - 1.0;
    double n11 = sm_noise_hash01(x1, y1, 0) * 2.0 - 1.0;
    double u = sm_noise_fade(tx);
    double v = sm_noise_fade(ty);
    double nx0 = sm_noise_lerp(n00, n10, u);
    double nx1 = sm_noise_lerp(n01, n11, u);
    return sm_noise_lerp(nx0, nx1, v);
}

static double sm_noise_fbm1d(double x, int octaves, double lacunarity, double gain) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    double sum = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += sm_noise_value1d(x * freq) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0) ? (sum / norm) : 0.0;
}

static double sm_noise_fbm2d(double x, double y, int octaves, double lacunarity, double gain) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    double sum = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += sm_noise_value2d(x * freq, y * freq) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0) ? (sum / norm) : 0.0;
}

static int sm_split_args(char* s, char* parts[], int max_parts) {
    int count = 0, depth = 0;
    char* start = s;
    for (char* p = s; ; ++p) {
        char c = *p;
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if ((c == ',' && depth == 0) || c == '\0') {
            if (count < max_parts) {
                while (start < p && sm_is_space(*start)) start++;
                char* end = p;
                while (end > start && sm_is_space(end[-1])) end--;
                *end = '\0';
                parts[count++] = start;
            }
            if (c == '\0') break;
            start = p + 1;
        }
    }
    return count;
}

static int sm_parse_args_copy(const char* inside, char parts[][128], int max_parts) {
    char buf[512];
    sm_copy(buf, sizeof(buf), inside ? inside : "");
    char* ptrs[16];
    int n = sm_split_args(buf, ptrs, max_parts);
    for (int i = 0; i < n; ++i) sm_copy(parts[i], 128, ptrs[i]);
    return n;
}

static int sm_find_func(const char* expr, char* name, size_t name_sz, char* inside, size_t inside_sz) {
    if (!expr) return 0;
    const char* p = expr;
    while (sm_is_space(*p)) p++;
    size_t ni = 0;
    while (sm_is_alpha(*p) && ni + 1 < name_sz) name[ni++] = *p++;
    name[ni] = '\0';
    while (sm_is_space(*p)) p++;
    if (*p != '(') return 0;
    p++;
    int depth = 1;
    size_t ii = 0;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) break;
        }
        if (ii + 1 < inside_sz) inside[ii++] = *p;
        p++;
    }
    inside[ii] = '\0';
    return depth == 0;
}

static int sm_pure_fraction_expr(const char* expr) {
    int has_slash = 0;
    for (const char* p = expr; p && *p; ++p) {
        if (sm_is_alpha(*p)) return 0;
        if (*p == '.') return 0;
        if (*p == '/') has_slash = 1;
    }
    return has_slash;
}

static int sm_eval_fraction_expr(const char* expr, char* display, size_t display_sz, char* steps, size_t steps_sz) {
    rat_t r;
    if (!sm_rat_eval(expr, &r)) return 0;
    char buf[128];
    sm_rat_to_text(r, buf, sizeof(buf));
    sm_copy(display, display_sz, buf);
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, "Exact fraction mode\n");
        sm_append(steps, steps_sz, expr);
        sm_append(steps, steps_sz, " = ");
        sm_append(steps, steps_sz, buf);
        sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

static void sm_sort_double(double* a, int n) {
    for (int i = 0; i < n - 1; ++i)
        for (int j = i + 1; j < n; ++j)
            if (a[j] < a[i]) { double t = a[i]; a[i] = a[j]; a[j] = t; }
}

static double sm_median_sorted(const double* a, int n) {
    if (n <= 0) return 0.0;
    return (n & 1) ? a[n/2] : 0.5 * (a[n/2 - 1] + a[n/2]);
}

static int sm_eval_stats(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[64][128];
    int n = sm_parse_args_copy(inside, parts, 64);
    if (n <= 0) return 0;
    double v[64];
    for (int i = 0; i < n; ++i) {
        if (!sm_parse_double_token(parts[i], &v[i])) return 0;
    }
    double s[64];
    for (int i = 0; i < n; ++i) s[i] = v[i];
    sm_sort_double(s, n);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += v[i];
    double mean = sum / (double)n;
    double var = 0.0;
    for (int i = 0; i < n; ++i) { double d = v[i] - mean; var += d * d; }
    var /= (double)n;
    double std = sm_sqrt(var);
    double med = sm_median_sorted(s, n);
    double q1 = n >= 4 ? sm_median_sorted(s, n/2) : s[0];
    double q3 = n >= 4 ? sm_median_sorted(s + (n + 1)/2, n/2) : s[n-1];
    sm_copy(display, display_sz, "Statistics");
    sm_copy(status, status_sz, "mean/median/std");
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, "Values: ");
        for (int i = 0; i < n; ++i) {
            char b[64]; sm_fmt_double(v[i], b, sizeof(b)); sm_append(steps, steps_sz, b);
            if (i + 1 < n) sm_append(steps, steps_sz, ", ");
        }
        sm_append(steps, steps_sz, "\nSorted: ");
        for (int i = 0; i < n; ++i) {
            char b[64]; sm_fmt_double(s[i], b, sizeof(b)); sm_append(steps, steps_sz, b);
            if (i + 1 < n) sm_append(steps, steps_sz, ", ");
        }
        char b[64];
        sm_append(steps, steps_sz, "\nmean = "); sm_fmt_double(mean, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, "\nmedian = "); sm_fmt_double(med, b, sizeof(b)); sm_append(steps, steps_sz, b);
        int mode_idx = 0, mode_count = 1;
        for (int i = 0; i < n; ++i) {
            int c = 1;
            for (int j = i + 1; j < n; ++j) if (sm_abs(v[i] - v[j]) < 1e-12) c++;
            if (c > mode_count) { mode_count = c; mode_idx = i; }
        }
        sm_append(steps, steps_sz, "\nmode = "); sm_fmt_double(v[mode_idx], b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, "\nvariance = "); sm_fmt_double(var, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, "\nstd = "); sm_fmt_double(std, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, "\n5-number summary: min=");
        sm_fmt_double(s[0], b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, " Q1="); sm_fmt_double(q1, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, " median="); sm_fmt_double(med, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, " Q3="); sm_fmt_double(q3, b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, " max="); sm_fmt_double(s[n-1], b, sizeof(b)); sm_append(steps, steps_sz, b);
        sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

typedef struct { const char* name; double factor; int kind; } unit_def_t;
enum { UNIT_LEN=1, UNIT_AREA, UNIT_VOL, UNIT_MASS, UNIT_TIME, UNIT_ANGLE };

static const unit_def_t g_units[] = {
    {"mm",0.001,UNIT_LEN},{"cm",0.01,UNIT_LEN},{"m",1.0,UNIT_LEN},{"km",1000.0,UNIT_LEN},
    {"in",0.0254,UNIT_LEN},{"ft",0.3048,UNIT_LEN},{"yd",0.9144,UNIT_LEN},{"mi",1609.344,UNIT_LEN},
    {"mm2",1e-6,UNIT_AREA},{"cm2",1e-4,UNIT_AREA},{"m2",1.0,UNIT_AREA},{"km2",1e6,UNIT_AREA},
    {"ha",10000.0,UNIT_AREA},{"acre",4046.8564224,UNIT_AREA},
    {"mm3",1e-9,UNIT_VOL},{"cm3",1e-6,UNIT_VOL},{"m3",1.0,UNIT_VOL},{"l",0.001,UNIT_VOL},{"ml",1e-6,UNIT_VOL},
    {"g",0.001,UNIT_MASS},{"kg",1.0,UNIT_MASS},{"t",1000.0,UNIT_MASS},
    {"ms",0.001,UNIT_TIME},{"s",1.0,UNIT_TIME},{"min",60.0,UNIT_TIME},{"h",3600.0,UNIT_TIME},
    {"deg",3.14159265358979323846/180.0,UNIT_ANGLE},{"rad",1.0,UNIT_ANGLE},{"grad",3.14159265358979323846/200.0,UNIT_ANGLE}
};

static int sm_find_unit(const char* s, double* factor, int* kind) {
    char token[32];
    sm_copy(token, sizeof(token), s ? s : "");
    sm_trim(token);
    sm_tolower(token);
    for (size_t i = 0; i < sizeof(g_units)/sizeof(g_units[0]); ++i) {
        if (strcmp(token, g_units[i].name) == 0) {
            *factor = g_units[i].factor;
            *kind = g_units[i].kind;
            return 1;
        }
    }
    return 0;
}

static int sm_eval_unit(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[3][128];
    if (sm_parse_args_copy(inside, parts, 3) != 3) return 0;
    double value = 0.0;
    if (!sm_parse_double_token(parts[0], &value)) return 0;
    double f1 = 0.0, f2 = 0.0;
    int k1 = 0, k2 = 0;
    if (!sm_find_unit(parts[1], &f1, &k1) || !sm_find_unit(parts[2], &f2, &k2) || k1 != k2) return 0;
    double res = value * f1 / f2;
    char b1[64], b2[64];
    sm_fmt_double(value, b1, sizeof(b1));
    sm_fmt_double(res, b2, sizeof(b2));
    sm_copy(display, display_sz, b2);
    sm_copy(status, status_sz, "unit conversion");
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, b1); sm_append(steps, steps_sz, " "); sm_append(steps, steps_sz, parts[1]);
        sm_append(steps, steps_sz, " = "); sm_append(steps, steps_sz, b2); sm_append(steps, steps_sz, " "); sm_append(steps, steps_sz, parts[2]); sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

static void sm_matrix2_inverse(double a, double b, double c, double d, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    double det = a*d - b*c;
    char buf[128], tmp[64];
    sm_copy(status, status_sz, "2x2 determinant/inverse");
    if (sm_abs(det) < 1e-12) {
        sm_copy(display, display_sz, "Singular");
        sm_copy(steps, steps_sz, "det = 0; inverse does not exist.\n");
        return;
    }
    sm_fmt_double(det, tmp, sizeof(tmp));
    sm_copy(display, display_sz, "Matrix 2x2");
    sm_copy(steps, steps_sz, "det = "); sm_append(steps, steps_sz, tmp); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "inverse = 1/det * [[d,-b],[-c,a]]\n");
    snprintf(buf, sizeof(buf), "[[%g,%g],[%g,%g]]^-1 = [[%g,%g],[%g,%g]]",
             a, b, c, d, d/det, -b/det, -c/det, a/det);
    sm_append(steps, steps_sz, buf); sm_append(steps, steps_sz, "\n");
}

static double sm_det3(double m[3][3]) {
    return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
         - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
         + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
}

static int sm_eval_matrix(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[16][128];
    int n = sm_parse_args_copy(inside, parts, 16);
    if (sm_starts_with_ci(fn, "mat2")) {
        if (n != 4) return 0;
        double a, b, c, d;
        if (!sm_parse_double_token(parts[0], &a) || !sm_parse_double_token(parts[1], &b) || !sm_parse_double_token(parts[2], &c) || !sm_parse_double_token(parts[3], &d)) return 0;
        sm_matrix2_inverse(a, b, c, d, display, display_sz, status, status_sz, steps, steps_sz);
        return 1;
    }
    if (sm_starts_with_ci(fn, "mat3")) {
        if (n != 9) return 0;
        double m[3][3];
        for (int i = 0; i < 9; ++i) { if (!sm_parse_double_token(parts[i], &m[i/3][i%3])) return 0; }
        double det = sm_det3(m);
        char b[64];
        sm_fmt_double(det, b, sizeof(b));
        sm_copy(display, display_sz, "Matrix 3x3");
        sm_copy(status, status_sz, "3x3 determinant/inverse");
        sm_copy(steps, steps_sz, "det = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        if (sm_abs(det) < 1e-12) { sm_append(steps, steps_sz, "Matrix is singular.\n"); return 1; }
        double inv[3][3];
        inv[0][0] =  (m[1][1]*m[2][2]-m[1][2]*m[2][1]) / det;
        inv[0][1] = -(m[0][1]*m[2][2]-m[0][2]*m[2][1]) / det;
        inv[0][2] =  (m[0][1]*m[1][2]-m[0][2]*m[1][1]) / det;
        inv[1][0] = -(m[1][0]*m[2][2]-m[1][2]*m[2][0]) / det;
        inv[1][1] =  (m[0][0]*m[2][2]-m[0][2]*m[2][0]) / det;
        inv[1][2] = -(m[0][0]*m[1][2]-m[0][2]*m[1][0]) / det;
        inv[2][0] =  (m[1][0]*m[2][1]-m[1][1]*m[2][0]) / det;
        inv[2][1] = -(m[0][0]*m[2][1]-m[0][1]*m[2][0]) / det;
        inv[2][2] =  (m[0][0]*m[1][1]-m[0][1]*m[1][0]) / det;
        sm_append(steps, steps_sz, "inverse =\n");
        for (int r = 0; r < 3; ++r) {
            sm_append(steps, steps_sz, "[");
            for (int c = 0; c < 3; ++c) {
                sm_fmt_double(inv[r][c], b, sizeof(b));
                sm_append(steps, steps_sz, b);
                if (c < 2) sm_append(steps, steps_sz, ", ");
            }
            sm_append(steps, steps_sz, "]\n");
        }
        return 1;
    }
    return 0;
}

static int sm_eval_solve2(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[6][128];
    if (sm_parse_args_copy(inside, parts, 6) != 6) return 0;
    double a1,b1,c1,a2,b2,c2;
    if (!sm_parse_double_token(parts[0], &a1) || !sm_parse_double_token(parts[1], &b1) || !sm_parse_double_token(parts[2], &c1) || !sm_parse_double_token(parts[3], &a2) || !sm_parse_double_token(parts[4], &b2) || !sm_parse_double_token(parts[5], &c2)) return 0;
    double det = a1*b2 - a2*b1;
    sm_copy(status, status_sz, "2-variable linear system");
    if (sm_abs(det) < 1e-12) {
        sm_copy(display, display_sz, "No unique solution");
        sm_copy(steps, steps_sz, "Determinant is zero; equations are dependent or inconsistent.\n");
        return 1;
    }
    double x = (c1*b2 - c2*b1) / det;
    double y = (a1*c2 - a2*c1) / det;
    char bx[64], by[64], bd[64];
    sm_fmt_double(x, bx, sizeof(bx));
    sm_fmt_double(y, by, sizeof(by));
    sm_fmt_double(det, bd, sizeof(bd));
    snprintf(display, display_sz, "x=%s,y=%s", bx, by);
    sm_copy(steps, steps_sz, "Eq1: "); sm_append(steps, steps_sz, parts[0]); sm_append(steps, steps_sz, "x + "); sm_append(steps, steps_sz, parts[1]); sm_append(steps, steps_sz, "y = "); sm_append(steps, steps_sz, parts[2]); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "Eq2: "); sm_append(steps, steps_sz, parts[3]); sm_append(steps, steps_sz, "x + "); sm_append(steps, steps_sz, parts[4]); sm_append(steps, steps_sz, "y = "); sm_append(steps, steps_sz, parts[5]); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "det = "); sm_append(steps, steps_sz, bd); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "x = "); sm_append(steps, steps_sz, bx); sm_append(steps, steps_sz, ", y = "); sm_append(steps, steps_sz, by); sm_append(steps, steps_sz, "\n");
    return 1;
}

static int sm_gauss3(double a[3][4], double x[3]) {
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r) if (sm_abs(a[r][col]) > sm_abs(a[piv][col])) piv = r;
        if (sm_abs(a[piv][col]) < 1e-12) return 0;
        if (piv != col) {
            for (int c = col; c < 4; ++c) { double t = a[col][c]; a[col][c] = a[piv][c]; a[piv][c] = t; }
        }
        double div = a[col][col];
        for (int c = col; c < 4; ++c) a[col][c] /= div;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            for (int c = col; c < 4; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int i = 0; i < 3; ++i) x[i] = a[i][3];
    return 1;
}

static int sm_eval_solve3(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[12][128];
    if (sm_parse_args_copy(inside, parts, 12) != 12) return 0;
    double a[3][4];
    for (int i = 0; i < 12; ++i) { if (!sm_parse_double_token(parts[i], &a[i/4][i%4])) return 0; }
    double x[3] = {0,0,0};
    sm_copy(status, status_sz, "3-variable linear system");
    if (!sm_gauss3(a, x)) {
        sm_copy(display, display_sz, "No unique solution");
        sm_copy(steps, steps_sz, "Gaussian elimination failed.\n");
        return 1;
    }
    char bx[64], by[64], bz[64];
    sm_fmt_double(x[0], bx, sizeof(bx));
    sm_fmt_double(x[1], by, sizeof(by));
    sm_fmt_double(x[2], bz, sizeof(bz));
    snprintf(display, display_sz, "x=%s,y=%s,z=%s", bx, by, bz);
    sm_copy(steps, steps_sz, "Gaussian elimination:\n");
    sm_append(steps, steps_sz, "Solution: x="); sm_append(steps, steps_sz, bx);
    sm_append(steps, steps_sz, ", y="); sm_append(steps, steps_sz, by);
    sm_append(steps, steps_sz, ", z="); sm_append(steps, steps_sz, bz); sm_append(steps, steps_sz, "\n");
    return 1;
}

static int sm_eval_quad(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[3][128];
    if (sm_parse_args_copy(inside, parts, 3) != 3) return 0;
    double a,b,c; if (!sm_parse_double_token(parts[0], &a) || !sm_parse_double_token(parts[1], &b) || !sm_parse_double_token(parts[2], &c)) return 0;
    if (sm_abs(a) < 1e-12) return 0;
    double d = b*b - 4.0*a*c;
    double xv = -b / (2.0*a);
    double yv = a*xv*xv + b*xv + c;
    char buf[64];
    sm_copy(status, status_sz, "quadratic analysis");
    if (d < 0.0) {
        sm_copy(display, display_sz, "Complex roots");
        sm_append(steps, steps_sz, "Discriminant < 0, so the roots are complex.\n");
    } else {
        double sd = sm_sqrt(d);
        char r1[64], r2[64];
        sm_fmt_double((-b + sd)/(2.0*a), r1, sizeof(r1));
        sm_fmt_double((-b - sd)/(2.0*a), r2, sizeof(r2));
        snprintf(display, display_sz, "roots: %s, %s", r1, r2);
        sm_append(steps, steps_sz, "Roots: "); sm_append(steps, steps_sz, r1); sm_append(steps, steps_sz, ", "); sm_append(steps, steps_sz, r2); sm_append(steps, steps_sz, "\n");
    }
    sm_fmt_double(xv, buf, sizeof(buf));
    sm_append(steps, steps_sz, "Axis: x = "); sm_append(steps, steps_sz, buf); sm_append(steps, steps_sz, "\n");
    sm_fmt_double(yv, buf, sizeof(buf));
    sm_append(steps, steps_sz, "Vertex: ("); sm_append(steps, steps_sz, buf); sm_append(steps, steps_sz, ")\n");
    sm_append(steps, steps_sz, (a > 0.0) ? "Opens upward / minimum.\n" : "Opens downward / maximum.\n");
    return 1;
}

static int sm_eval_complex(const char* inside, int deg_mode, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz, int polar_only) {
    char parts[2][128];
    if (sm_parse_args_copy(inside, parts, 2) != 2) return 0;
    double re, im; if (!sm_parse_double_token(parts[0], &re) || !sm_parse_double_token(parts[1], &im)) return 0;
    double r = sm_sqrt(re*re + im*im);
    double th = sm_atan2(im, re);
    double im_abs = sm_abs(im);
    char rb[64], ib[64], mb[64], tb[64];
    sm_fmt_double(re, rb, sizeof(rb));
    sm_fmt_double(im_abs, ib, sizeof(ib));
    sm_fmt_double(r, mb, sizeof(mb));
    sm_fmt_double(deg_mode ? sm_deg(th) : th, tb, sizeof(tb));
    if (polar_only) snprintf(display, display_sz, "%s@%s", mb, tb);
    else snprintf(display, display_sz, "%s%s%si", rb, (im >= 0.0 ? "+" : "-"), ib);
    sm_copy(status, status_sz, "complex number");
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, "z = "); sm_append(steps, steps_sz, rb);
        sm_append(steps, steps_sz, (im >= 0.0) ? " + " : " - ");
        sm_append(steps, steps_sz, ib); sm_append(steps, steps_sz, "i\n");
        sm_append(steps, steps_sz, "|z| = "); sm_append(steps, steps_sz, mb); sm_append(steps, steps_sz, "\n");
        sm_append(steps, steps_sz, "arg = "); sm_append(steps, steps_sz, tb); sm_append(steps, steps_sz, deg_mode ? " deg\n" : " rad\n");
        sm_append(steps, steps_sz, "polar = "); sm_append(steps, steps_sz, mb); sm_append(steps, steps_sz, "@"); sm_append(steps, steps_sz, tb); sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

static int sm_eval_vector(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[8][128];
    int n = sm_parse_args_copy(inside, parts, 8);
    if (sm_starts_with_ci(fn, "vec2")) {
        if (n != 4) return 0;
        double ax, ay, bx, by; if (!sm_parse_double_token(parts[0], &ax) || !sm_parse_double_token(parts[1], &ay) || !sm_parse_double_token(parts[2], &bx) || !sm_parse_double_token(parts[3], &by)) return 0;
        double dot = ax*bx + ay*by;
        double la = sm_sqrt(ax*ax + ay*ay), lb = sm_sqrt(bx*bx + by*by);
        char b[64];
        sm_fmt_double(dot, b, sizeof(b));
        sm_copy(display, display_sz, "vector2");
        sm_copy(status, status_sz, "dot product / orthogonality");
        sm_copy(steps, steps_sz, "a·b = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        sm_append(steps, steps_sz, "parallel if a×b = 0 (or if directions match)\n");
        sm_append(steps, steps_sz, "perpendicular if a·b = 0\n");
        sm_append(steps, steps_sz, "|a| = "); sm_fmt_double(la, b, sizeof(b)); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        sm_append(steps, steps_sz, "|b| = "); sm_fmt_double(lb, b, sizeof(b)); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        return 1;
    }
    if (sm_starts_with_ci(fn, "vec3")) {
        if (n != 6) return 0;
        double ax, ay, az, bx, by, bz; if (!sm_parse_double_token(parts[0], &ax) || !sm_parse_double_token(parts[1], &ay) || !sm_parse_double_token(parts[2], &az) || !sm_parse_double_token(parts[3], &bx) || !sm_parse_double_token(parts[4], &by) || !sm_parse_double_token(parts[5], &bz)) return 0;
        double dot = ax*bx + ay*by + az*bz;
        char b[64];
        sm_fmt_double(dot, b, sizeof(b));
        sm_copy(display, display_sz, "vector3");
        sm_copy(status, status_sz, "dot product");
        sm_copy(steps, steps_sz, "a·b = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        sm_append(steps, steps_sz, "perpendicular if a·b = 0\n");
        return 1;
    }
    return 0;
}

static int sm_eval_sequence(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[4][128];
    int n = sm_parse_args_copy(inside, parts, 4);
    if (sm_starts_with_ci(fn, "seq")) {
        if (n != 3) return 0;
        double a1, d; long long k_i;
        if (!sm_parse_double_token(parts[0], &a1) || !sm_parse_double_token(parts[1], &d) || !sm_parse_ll_token(parts[2], &k_i) || k_i <= 0) return 0;
        double k = (double)k_i;
        double an = a1 + (k - 1.0) * d;
        double sn = k * (2.0 * a1 + (k - 1.0) * d) / 2.0;
        char b[64];
        sm_fmt_double(an, b, sizeof(b));
        sm_copy(display, display_sz, b);
        sm_copy(status, status_sz, "arithmetic sequence");
        sm_copy(steps, steps_sz, "a_n = a_1 + (n-1)d\nS_n = n(2a_1+(n-1)d)/2\n");
        sm_append(steps, steps_sz, "a_n = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        sm_fmt_double(sn, b, sizeof(b));
        sm_append(steps, steps_sz, "S_n = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        return 1;
    }
    if (sm_starts_with_ci(fn, "geo")) {
        if (n != 3) return 0;
        double a1, r; long long k_i;
        if (!sm_parse_double_token(parts[0], &a1) || !sm_parse_double_token(parts[1], &r) || !sm_parse_ll_token(parts[2], &k_i) || k_i <= 0) return 0;
        double k = (double)k_i;
        double an = a1;
        for (long long i = 1; i < k_i; ++i) an *= r;
        double rn = 1.0;
        for (long long i = 0; i < k_i; ++i) rn *= r;
        double sn = (sm_abs(r - 1.0) < 1e-12) ? a1 * k : a1 * (1.0 - rn) / (1.0 - r);
        char b[64];
        sm_fmt_double(an, b, sizeof(b));
        sm_copy(display, display_sz, b);
        sm_copy(status, status_sz, "geometric sequence");
        sm_copy(steps, steps_sz, "a_n = a_1 r^(n-1)\nS_n = a_1(1-r^n)/(1-r)\n");
        sm_append(steps, steps_sz, "a_n = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        sm_fmt_double(sn, b, sizeof(b));
        sm_append(steps, steps_sz, "S_n = "); sm_append(steps, steps_sz, b); sm_append(steps, steps_sz, "\n");
        return 1;
    }
    if (sm_starts_with_ci(fn, "fib")) {
        if (n != 1) return 0;
        long long k; if (!sm_parse_ll_token(parts[0], &k)) return 0;
        long long a = 0, b = 1;
        for (long long i = 0; i < k; ++i) { long long t = a + b; a = b; b = t; }
        snprintf(display, display_sz, "%lld", a);
        sm_copy(status, status_sz, "Fibonacci");
        sm_copy(steps, steps_sz, "Simple recurrence: F(n)=F(n-1)+F(n-2)\n");
        return 1;
    }
    return 0;
}

static lin_t sm_lin_add(lin_t a, lin_t b) { return (lin_t){a.a + b.a, a.b + b.b, a.ok && b.ok}; }
static lin_t sm_lin_sub(lin_t a, lin_t b) { return (lin_t){a.a - b.a, a.b - b.b, a.ok && b.ok}; }
static lin_t sm_lin_scale(lin_t a, double k) { return (lin_t){a.a * k, a.b * k, a.ok}; }

static lin_t sm_parse_linear_term(const char** p);

static lin_t sm_parse_linear_primary(const char** p) {
    while (sm_is_space(**p)) (*p)++;
    if (**p == '(') {
        (*p)++;
        lin_t v = sm_parse_linear_term(p);
        while (sm_is_space(**p)) (*p)++;
        if (**p == ')') (*p)++; else return (lin_t){0,0,0};
        return v;
    }
    if (**p == '+' || **p == '-') {
        char s = **p; (*p)++;
        lin_t v = sm_parse_linear_primary(p);
        if (s == '-') v = sm_lin_scale(v, -1.0);
        return v;
    }
    if (**p == 'x' || **p == 'X') { (*p)++; return (lin_t){1.0, 0.0, 1}; }
    const char* start = *p;
    double v = 0.0;
    if (sm_parse_double_prefix(&start, &v)) {
        *p = start;
        return (lin_t){0.0, v, 1};
    }
    return (lin_t){0.0, 0.0, 0};
}

static lin_t sm_parse_linear_factor(const char** p) {
    lin_t v = sm_parse_linear_primary(p);
    while (sm_is_space(**p)) (*p)++;
    if (**p == '^') {
        (*p)++;
        long long exp = 0;
        if (!sm_parse_int(p, &exp)) return (lin_t){0,0,0};
        if (exp == 1) return v;
        if (exp == 2 && sm_abs(v.a) < 1e-12) return (lin_t){0.0, v.b * v.b, 1};
        return (lin_t){0,0,0};
    }
    return v;
}

static lin_t sm_parse_linear_term(const char** p) {
    lin_t v = sm_parse_linear_factor(p);
    if (!v.ok) return v;
    for (;;) {
        while (sm_is_space(**p)) (*p)++;
        if (**p == 'x' || **p == 'X' || **p == '(') {
            lin_t rhs = sm_parse_linear_factor(p);
            if (!rhs.ok) return (lin_t){0,0,0};
            if (sm_abs(v.a) > 1e-12 && sm_abs(rhs.a) > 1e-12) return (lin_t){0,0,0};
            if (sm_abs(v.a) > 1e-12) v = sm_lin_scale(v, rhs.b);
            else if (sm_abs(rhs.a) > 1e-12) v = sm_lin_scale(rhs, v.b);
            else v = (lin_t){0.0, v.b * rhs.b, 1};
        } else if (**p == '*' || **p == '/') {
            char op = **p; (*p)++;
            lin_t rhs = sm_parse_linear_factor(p);
            if (!rhs.ok) return (lin_t){0,0,0};
            if (op == '*') {
                if (sm_abs(v.a) > 1e-12 && sm_abs(rhs.a) > 1e-12) return (lin_t){0,0,0};
                if (sm_abs(v.a) > 1e-12) v = sm_lin_scale(v, rhs.b);
                else if (sm_abs(rhs.a) > 1e-12) v = sm_lin_scale(rhs, v.b);
                else v = (lin_t){0.0, v.b * rhs.b, 1};
            } else {
                if (sm_abs(rhs.a) > 1e-12 || sm_abs(rhs.b) < 1e-12) return (lin_t){0,0,0};
                v = (lin_t){v.a / rhs.b, v.b / rhs.b, 1};
            }
        } else break;
    }
    return v;
}

static lin_t sm_parse_linear_expr(const char** p) {
    lin_t v = sm_parse_linear_term(p);
    if (!v.ok) return v;
    for (;;) {
        while (sm_is_space(**p)) (*p)++;
        if (**p == '+' || **p == '-') {
            char op = **p; (*p)++;
            lin_t rhs = sm_parse_linear_term(p);
            if (!rhs.ok) return (lin_t){0,0,0};
            v = (op == '+') ? sm_lin_add(v, rhs) : sm_lin_sub(v, rhs);
        } else break;
    }
    return v;
}

static int sm_find_ineq_op(const char* s, int* pos, char* op1, char* eq) {
    int depth = 0;
    for (int i = 0; s && s[i]; ++i) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (depth == 0) {
            if ((s[i] == '<' || s[i] == '>') && s[i+1] == '=') { *pos = i; *op1 = s[i]; *eq = '='; return 1; }
            if (s[i] == '<' || s[i] == '>') { *pos = i; *op1 = s[i]; *eq = 0; return 1; }
        }
    }
    return 0;
}

static int sm_eval_inequality(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char expr[256];
    sm_copy(expr, sizeof(expr), inside);
    sm_trim(expr);
    int pos = -1; char op = 0, eq = 0;
    if (!sm_find_ineq_op(expr, &pos, &op, &eq)) return 0;
    char lhs_txt[160], rhs_txt[160];
    sm_copy(lhs_txt, sizeof(lhs_txt), expr);
    lhs_txt[pos] = '\0';
    sm_copy(rhs_txt, sizeof(rhs_txt), expr + pos + (eq ? 2 : 1));
    const char* pl = lhs_txt;
    const char* pr = rhs_txt;
    lin_t lhs = sm_parse_linear_expr(&pl), rhs = sm_parse_linear_expr(&pr);
    if (!lhs.ok || !rhs.ok) return 0;
    lin_t d = sm_lin_sub(lhs, rhs);
    double a = d.a, b = d.b;
    sm_copy(status, status_sz, "linear inequality");
    if (sm_abs(a) < 1e-12) {
        sm_copy(display, display_sz, "all real numbers");
        sm_copy(steps, steps_sz, "Variable cancels out.\n");
        return 1;
    }
    double x0 = -b / a;
    char xb[64];
    sm_fmt_double(x0, xb, sizeof(xb));
    char out[160];
    char sol_op = (a < 0.0 ? (op == '<' ? '>' : '<') : op);
    snprintf(out, sizeof(out), "x %c%s %s", sol_op, (eq ? "=" : ""), xb);
    sm_copy(display, display_sz, out);
    sm_copy(steps, steps_sz, "Move terms to one side.\n");
    if (a < 0.0) sm_append(steps, steps_sz, "Dividing by a negative number flips the sign.\n");
    sm_append(steps, steps_sz, "Solution set: "); sm_append(steps, steps_sz, out); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "Interval: ");
    if (sol_op == '<') {
        sm_append(steps, steps_sz, "(-inf, ");
        sm_append(steps, steps_sz, xb);
        sm_append(steps, steps_sz, eq ? "]" : ")");
    } else {
        sm_append(steps, steps_sz, eq ? "[" : "(");
        sm_append(steps, steps_sz, xb);
        sm_append(steps, steps_sz, ", inf)");
    }
    sm_append(steps, steps_sz, "\n");
    return 1;
}



static int sm_eval_noise(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[6][128];
    int n = sm_parse_args_copy(inside, parts, 6);
    if (n <= 0) return 0;

    if (sm_starts_with_ci(fn, "noise") || sm_starts_with_ci(fn, "perlin")) {
        double x, y = 0.0;
        if (!sm_parse_double_token(parts[0], &x)) return 0;
        double result = x;
        if (n >= 2) {
            if (!sm_parse_double_token(parts[1], &y)) return 0;
            result = sm_noise_value2d(x, y);
        } else {
            result = sm_noise_value1d(x);
        }
        char buf[64];
        sm_fmt_double(result, buf, sizeof(buf));
        sm_copy(display, display_sz, buf);
        sm_copy(status, status_sz, "noise");
        if (steps && steps_sz) {
            steps[0] = '\0';
            sm_append(steps, steps_sz, "Value noise");
            sm_append(steps, steps_sz, n >= 2 ? " 2D\n" : " 1D\n");
            sm_append(steps, steps_sz, "result = ");
            sm_append(steps, steps_sz, buf);
            sm_append(steps, steps_sz, "\n");
        }
        return 1;
    }

    if (sm_starts_with_ci(fn, "fbm")) {
        double x, y = 0.0, oct = 4.0, lac = 2.0, gain = 0.5;
        if (!sm_parse_double_token(parts[0], &x)) return 0;
        if (n >= 2 && !sm_parse_double_token(parts[1], &y)) return 0;
        if (n >= 3 && !sm_parse_double_token(parts[(n >= 2) ? 2 : 1], &oct)) return 0;
        if (n >= 4 && !sm_parse_double_token(parts[(n >= 2) ? 3 : 2], &lac)) return 0;
        if (n >= 5 && !sm_parse_double_token(parts[(n >= 2) ? 4 : 3], &gain)) return 0;
        int octaves = (int)(oct + (oct >= 0.0 ? 0.5 : -0.5));
        double result = (n >= 2) ? sm_noise_fbm2d(x, y, octaves, lac, gain) : sm_noise_fbm1d(x, octaves, lac, gain);
        char buf[64];
        sm_fmt_double(result, buf, sizeof(buf));
        sm_copy(display, display_sz, buf);
        sm_copy(status, status_sz, "fractal noise");
        if (steps && steps_sz) {
            steps[0] = '\0';
            sm_append(steps, steps_sz, "fBm / layered noise\n");
            sm_append(steps, steps_sz, "result = ");
            sm_append(steps, steps_sz, buf);
            sm_append(steps, steps_sz, "\n");
        }
        return 1;
    }

    return 0;
}

static int sm_eval_hissan(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    rat_t r;
    if (!sm_rat_eval(inside, &r)) return 0;
    char buf[128];
    sm_rat_to_text(r, buf, sizeof(buf));
    sm_copy(display, display_sz, buf);
    sm_copy(status, status_sz, "hissan arithmetic");
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, "Hissan / column calculation\n");
        sm_append(steps, steps_sz, inside);
        sm_append(steps, steps_sz, " = ");
        sm_append(steps, steps_sz, buf);
        sm_append(steps, steps_sz, "\n");
        if (strchr(inside, '/') != NULL) {
            sm_append(steps, steps_sz, "Tip: division is shown in long-form in the Hissan window.\n");
        }
    }
    return 1;
}

static int sm_base_convert(long long value, int base, const char* prefix, char* display, size_t display_sz) {
    if (base < 2 || base > 36) return 0;
    sm_format_signed_base(value, (unsigned)base, prefix, display, display_sz);
    return 1;
}

static int sm_eval_base_format(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[2][128];
    int n = sm_parse_args_copy(inside, parts, 2);
    if (n < 1) return 0;
    long long v = 0;
    if (!sm_parse_ll_token(parts[0], &v)) return 0;

    sm_copy(status, status_sz, "base conversion");
    if (!strcmp(fn, "bin")) {
        sm_base_convert(v, 2, "0b", display, display_sz);
    } else if (!strcmp(fn, "oct")) {
        sm_base_convert(v, 8, "0o", display, display_sz);
    } else if (!strcmp(fn, "hex")) {
        sm_base_convert(v, 16, "0x", display, display_sz);
    } else if (!strcmp(fn, "dec")) {
        sm_base_convert(v, 10, "", display, display_sz);
    } else if (!strcmp(fn, "base") || !strcmp(fn, "radix")) {
        if (n < 2) return 0;
        long long b = 0;
        if (!sm_parse_ll_token(parts[1], &b)) return 0;
        if (!sm_base_convert(v, (int)b, (b == 2) ? "0b" : (b == 8) ? "0o" : (b == 16) ? "0x" : "", display, display_sz)) return 0;
    } else {
        return 0;
    }

    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, "Base conversion\n");
        sm_append(steps, steps_sz, fn);
        sm_append(steps, steps_sz, "(");
        sm_append(steps, steps_sz, parts[0]);
        if (n >= 2) {
            sm_append(steps, steps_sz, ", ");
            sm_append(steps, steps_sz, parts[1]);
        }
        sm_append(steps, steps_sz, ") = ");
        sm_append(steps, steps_sz, display);
        sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

static int sm_eval_bitwise(const char* fn, const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[16][128];
    int n = sm_parse_args_copy(inside, parts, 16);
    if (n <= 0) return 0;

    long long vals[16];
    for (int i = 0; i < n; ++i) if (!sm_parse_ll_token(parts[i], &vals[i])) return 0;

    long long r = 0;
    if (!strcmp(fn, "bnot") || !strcmp(fn, "not")) {
        r = ~vals[0];
    } else if (!strcmp(fn, "shl") || !strcmp(fn, "shr")) {
        if (n < 2) return 0;
        long long amt = vals[1];
        if (amt < 0) return 0;
        r = (!strcmp(fn, "shl")) ? (vals[0] << amt) : (vals[0] >> amt);
    } else if (!strcmp(fn, "band") || !strcmp(fn, "and")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r &= vals[i];
    } else if (!strcmp(fn, "bor") || !strcmp(fn, "or")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r |= vals[i];
    } else if (!strcmp(fn, "bxor") || !strcmp(fn, "xor")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r ^= vals[i];
    } else if (!strcmp(fn, "nand")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r &= vals[i];
        r = ~r;
    } else if (!strcmp(fn, "nor")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r |= vals[i];
        r = ~r;
    } else if (!strcmp(fn, "xnor")) {
        r = vals[0];
        for (int i = 1; i < n; ++i) r ^= vals[i];
        r = ~r;
    } else {
        return 0;
    }

    sm_format_signed_base(r, 10, "", display, display_sz);
    sm_copy(status, status_sz, "bitwise");
    if (steps && steps_sz) {
        steps[0] = '\0';
        sm_append(steps, steps_sz, fn);
        sm_append(steps, steps_sz, "(");
        for (int i = 0; i < n; ++i) {
            sm_append(steps, steps_sz, parts[i]);
            if (i + 1 < n) sm_append(steps, steps_sz, ", ");
        }
        sm_append(steps, steps_sz, ") = ");
        sm_append(steps, steps_sz, display);
        sm_append(steps, steps_sz, "\n");
    }
    return 1;
}

static int sm_eval_verify(const char* inside, char* display, size_t display_sz, char* status, size_t status_sz, char* steps, size_t steps_sz) {
    char parts[2][128];
    if (sm_parse_args_copy(inside, parts, 2) != 2) return 0;
    rat_t a, b;
    sm_copy(status, status_sz, "verification");
    if (sm_rat_eval(parts[0], &a) && sm_rat_eval(parts[1], &b)) {
        char la[64], lb[64];
        sm_rat_to_text(a, la, sizeof(la));
        sm_rat_to_text(b, lb, sizeof(lb));
        sm_copy(display, display_sz, (a.n == b.n && a.d == b.d) ? "OK" : "Mismatch");
        sm_copy(steps, steps_sz, "Left = "); sm_append(steps, steps_sz, la); sm_append(steps, steps_sz, "\n");
        sm_append(steps, steps_sz, "Right = "); sm_append(steps, steps_sz, lb); sm_append(steps, steps_sz, "\n");
        return 1;
    }
    double left, right; if (!sm_parse_double_token(parts[0], &left) || !sm_parse_double_token(parts[1], &right)) return 0;
    char la[64], lb[64];
    sm_fmt_double(left, la, sizeof(la));
    sm_fmt_double(right, lb, sizeof(lb));
    sm_copy(display, display_sz, (sm_abs(left - right) < 1e-9) ? "OK" : "Mismatch");
    sm_copy(steps, steps_sz, "Left = "); sm_append(steps, steps_sz, la); sm_append(steps, steps_sz, "\n");
    sm_append(steps, steps_sz, "Right = "); sm_append(steps, steps_sz, lb); sm_append(steps, steps_sz, "\n");
    return 1;
}

bool calc_school_math_evaluate(const char* expr, bool deg_mode, calc_school_result_t* out) {
    if (!expr || !out) return false;
    out->handled = false;
    out->display[0] = '\0';
    out->status[0] = '\0';
    out->steps[0] = '\0';

    char work[512];
    sm_copy(work, sizeof(work), expr);
    sm_trim(work);
    if (!work[0]) return false;

    char fn[32];
    char inside[384];
    if (sm_find_func(work, fn, sizeof(fn), inside, sizeof(inside))) {
        sm_tolower(fn);
        if (!strcmp(fn, "frac") || !strcmp(fn, "exact") || !strcmp(fn, "rational") || !strcmp(fn, "mix")) {
            if (sm_eval_fraction_expr(inside, out->display, sizeof(out->display), out->steps, sizeof(out->steps))) {
                sm_copy(out->status, sizeof(out->status), "exact fraction");
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "stat") || !strcmp(fn, "stats")) {
            if (sm_eval_stats(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "unit") || !strcmp(fn, "conv") || !strcmp(fn, "convert")) {
            if (sm_eval_unit(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "solve2")) {
            if (sm_eval_solve2(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "solve3")) {
            if (sm_eval_solve3(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "quad") || !strcmp(fn, "quadratic") || !strcmp(fn, "graphquad")) {
            if (sm_eval_quad(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "comp") || !strcmp(fn, "complex")) {
            if (sm_eval_complex(inside, deg_mode, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 0)) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "polar")) {
            if (sm_eval_complex(inside, deg_mode, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 1)) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "vec2") || !strcmp(fn, "vec3")) {
            if (sm_eval_vector(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "mat2") || !strcmp(fn, "mat3")) {
            if (sm_eval_matrix(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "seq") || !strcmp(fn, "geo") || !strcmp(fn, "fib")) {
            if (sm_eval_sequence(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "ineq")) {
            if (sm_eval_inequality(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "noise") || !strcmp(fn, "perlin") || !strcmp(fn, "fbm") || !strcmp(fn, "fbm2")) {
            if (sm_eval_noise(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "hex") || !strcmp(fn, "bin") || !strcmp(fn, "oct") || !strcmp(fn, "dec") || !strcmp(fn, "base") || !strcmp(fn, "radix")) {
            if (sm_eval_base_format(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "band") || !strcmp(fn, "bor") || !strcmp(fn, "bxor") || !strcmp(fn, "bnot") || !strcmp(fn, "not") || !strcmp(fn, "nand") || !strcmp(fn, "nor") || !strcmp(fn, "xnor") || !strcmp(fn, "shl") || !strcmp(fn, "shr")) {
            if (sm_eval_bitwise(fn, inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "hissan") || !strcmp(fn, "longdiv") || !strcmp(fn, "column")) {
            if (sm_eval_hissan(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "verify") || !strcmp(fn, "check")) {
            if (sm_eval_verify(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps))) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "cas") || !strcmp(fn, "simplify")) {
            if (sm_eval_cas_poly(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 0)) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "expand")) {
            if (sm_eval_cas_poly(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 3)) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "diff") || !strcmp(fn, "derivative")) {
            if (sm_eval_cas_poly(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 1)) {
                out->handled = true;
                return true;
            }
        } else if (!strcmp(fn, "factor")) {
            if (sm_eval_cas_poly(inside, out->display, sizeof(out->display), out->status, sizeof(out->status), out->steps, sizeof(out->steps), 2)) {
                out->handled = true;
                return true;
            }
        }
    }

    if (sm_pure_fraction_expr(work)) {
        if (sm_eval_fraction_expr(work, out->display, sizeof(out->display), out->steps, sizeof(out->steps))) {
            sm_copy(out->status, sizeof(out->status), "exact fraction");
            out->handled = true;
            return true;
        }
    }

    return false;
}
