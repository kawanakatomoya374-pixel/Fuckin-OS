/**
 * gui_apps_calculator.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * 電卓 (HS3代数エンジン・表計算モード含む)
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"

#ifndef KEY_PAGEUP
#define KEY_PAGEUP   0x49
#endif
#ifndef KEY_PAGEDOWN
#define KEY_PAGEDOWN 0x51
#endif
#ifndef KEY_HOME
#define KEY_HOME     0x47
#endif
#ifndef KEY_END
#define KEY_END      0x4F
#endif
#include "memory.h"
#include "memory_physical.h"
#include "cos_version.h"
#include "rtc.h"
#include "scheduler.h"
#include "../../bios/bios.h"
#include "../../kernel/drivers/usb.h"
#include "../../kernel/drivers/pci.h"
#include "fs.h"
#include "keyboard.h"
#include "../../drivers/disk/storage.h"
#include "../../drivers/input/mouse_minimal.h"
#include <shell.h>
extern const char* fs_read_file_at(const char* path, const char* name);
extern const char* config_get_string(const char* key);
extern void gui_snapshot_save_desktop(void);
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_normalize_desktop_icons(void);
#include "gui_apps_common.h"

/* ================================================================
 * Calculator - advanced expression engine and study/hissan modes
 * ================================================================ */
/* ============================================================ */

typedef struct {
    char* steps;
    int   steps_cap;
    int   steps_len;
    bool  deg_mode;
    bool  ok;
    bool  use_x;
    double x_value;
} calc_eval_ctx_t;

static bool calc_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool calc_is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static bool calc_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static double calc_absd(double x) { return x < 0.0 ? -x : x; }

static void calc_copy_text(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void calc_append_text(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0 || !src) return;
    int dl = slen(dst);
    int i = 0;
    while (dl + i < cap - 1 && src[i]) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}

static void calc_append_char(char* dst, int cap, char ch) {
    if (!dst || cap <= 1) return;
    int dl = slen(dst);
    if (dl < cap - 1) { dst[dl] = ch; dst[dl + 1] = 0; }
}

static void calc_step_add(calc_eval_ctx_t* ctx, const char* line) {
    if (!ctx || !ctx->steps || ctx->steps_cap <= 1 || !line || !line[0]) return;
    int len = ctx->steps_len;
    if (len > 0 && len < ctx->steps_cap - 1) ctx->steps[len++] = '\0';
    int i = 0;
    while (len + i < ctx->steps_cap - 1 && line[i]) { ctx->steps[len + i] = line[i]; i++; }
    ctx->steps[len + i] = 0;
    ctx->steps_len = len + i;
}

static void calc_num_to_text(double value, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    if (value != value) { calc_copy_text(out, out_sz, "Error"); return; }
    if (value > 1e18) { calc_copy_text(out, out_sz, "Overflow"); return; }
    if (value < -1e18) { calc_copy_text(out, out_sz, "-Overflow"); return; }
    if (calc_absd(value) < 0.0000000001) value = 0.0;

    int pos = 0;
    if (value < 0.0) { out[pos++] = '-'; value = -value; }

    uint64_t whole = (uint64_t)(value + 0.0000001);
    double frac = value - (double)whole;
    char tmp[32];
    int ti = 0;
    if (whole == 0) tmp[ti++] = '0';
    else {
        while (whole > 0 && ti < 30) { tmp[ti++] = (char)('0' + (whole % 10)); whole /= 10; }
    }
    for (int i = 0; i < ti / 2; i++) {
        char c = tmp[i]; tmp[i] = tmp[ti - 1 - i]; tmp[ti - 1 - i] = c;
    }
    while (pos < out_sz - 1 && ti > 0) out[pos++] = tmp[--ti];

    if (frac > 0.0000005) {
        out[pos++] = '.';
        int digits = 0;
        while (frac > 0.0000005 && digits < 8 && pos < out_sz - 1) {
            frac *= 10.0;
            int d = (int)frac;
            out[pos++] = (char)('0' + d);
            frac -= d;
            digits++;
        }
        while (pos > 0 && out[pos - 1] == '0') pos--;
        if (pos > 0 && out[pos - 1] == '.') pos--;
    }
    out[pos] = 0;
}

static double calc_strtodbl(const char* s) {
    if (!s) return 0.0;
    double result = 0.0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s == ' ' || *s == '	') s++;
    while (*s >= '0' && *s <= '9') { result = result * 10.0 + (double)(*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { result += (double)(*s - '0') * frac; frac *= 0.1; s++; }
    }
    return result * (double)sign;
}

static double calc_round_nearest(double x) {
    return x >= 0.0 ? (double)((int)(x + 0.5)) : -(double)((int)((-x) + 0.5));
}

static double calc_to_radians(double value, bool deg) {
    return deg ? value * 3.14159265358979323846 / 180.0 : value;
}

static double calc_from_radians(double value, bool deg) {
    return deg ? value * 180.0 / 3.14159265358979323846 : value;
}

static double calc_sqrt_approx(double x) {
    if (x <= 0.0) return 0.0;
    double r = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 24; i++) {
        double nr = 0.5 * (r + x / r);
        if (calc_absd(nr - r) < 0.0000001) { r = nr; break; }
        r = nr;
    }
    return r;
}

static double calc_sin_approx(double x) {
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    double term = x;
    double sum = x;
    double xx = x * x;
    for (int n = 1; n < 8; n++) {
        term *= -xx / ((2.0 * n) * (2.0 * n + 1.0));
        sum += term;
    }
    return sum;
}

static double calc_cos_approx(double x) {
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    double term = 1.0;
    double sum = 1.0;
    double xx = x * x;
    for (int n = 1; n < 8; n++) {
        term *= -xx / ((2.0 * n - 1.0) * (2.0 * n));
        sum += term;
    }
    return sum;
}

static double calc_tan_approx(double x) {
    double c = calc_cos_approx(x);
    if (calc_absd(c) < 0.0000001) return 0.0;
    return calc_sin_approx(x) / c;
}

static double calc_atan_approx(double x) {
    const double pi = 3.14159265358979323846;
    if (x > 1.0) return pi / 2.0 - calc_atan_approx(1.0 / x);
    if (x < -1.0) return -pi / 2.0 - calc_atan_approx(1.0 / x);
    double xx = x * x;
    double term = x;
    double sum = x;
    for (int n = 1; n < 12; n++) {
        term *= -xx;
        sum += term / (2.0 * n + 1.0);
    }
    return sum;
}

static double calc_ln_approx(double x) {
    const double ln2 = 0.69314718055994530942;
    if (x <= 0.0) return 0.0;
    int shift = 0;
    while (x > 1.5) { x *= 0.5; shift++; }
    while (x < 0.75) { x *= 2.0; shift--; }
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double term = y;
    double sum = 0.0;
    for (int n = 1; n < 30; n += 2) {
        sum += term / (double)n;
        term *= y2;
    }
    return 2.0 * sum + (double)shift * ln2;
}

static double calc_exp_approx(double x) {
    const double ln2 = 0.69314718055994530942;
    int shift = 0;
    while (x > ln2) { x -= ln2; shift++; }
    while (x < -ln2) { x += ln2; shift--; }
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 18; n++) {
        term *= x / (double)n;
        sum += term;
    }
    while (shift > 0) { sum *= 2.0; shift--; }
    while (shift < 0) { sum *= 0.5; shift++; }
    return sum;
}

static double calc_pow_approx(double base, double exponent) {
    if (calc_absd(exponent - calc_round_nearest(exponent)) < 0.0000001) {
        long long e = (long long)calc_round_nearest(exponent);
        if (e == 0) return 1.0;
        bool neg = FALSE;
        if (e < 0) { neg = TRUE; e = -e; }
        double result = 1.0;
        while (e > 0) {
            if (e & 1LL) result *= base;
            base *= base;
            e >>= 1;
        }
        return neg && result != 0.0 ? 1.0 / result : result;
    }
    if (base <= 0.0) return 0.0;
    return calc_exp_approx(exponent * calc_ln_approx(base));
}

static double calc_factorial(double x) {
    if (x < 0.0) return 0.0;
    double n = calc_round_nearest(x);
    if (calc_absd(n - x) > 0.0000001) return 0.0;
    if (n > 20.0) return 0.0;
    double r = 1.0;
    long long i = (long long)n;
    for (long long v = 2; v <= i; v++) r *= (double)v;
    return r;
}

static long long calc_ll_abs(long long v) {
    return (v < 0) ? -v : v;
}

static long long calc_gcd_ll(long long a, long long b) {
    a = calc_ll_abs(a);
    b = calc_ll_abs(b);
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

static long long calc_lcm_ll(long long a, long long b) {
    if (a == 0 || b == 0) return 0;
    return calc_ll_abs((a / calc_gcd_ll(a, b)) * b);
}

static bool calc_is_prime_ll(long long n) {
    if (n < 2) return FALSE;
    if (n % 2 == 0) return n == 2;
    for (long long i = 3; i * i <= n; i += 2) {
        if (n % i == 0) return FALSE;
    }
    return TRUE;
}

static void calc_ll_to_text(long long v, char* out, int out_sz) {
    calc_num_to_text((double)v, out, out_sz);
}

static void calc_fraction_to_text(long long n, long long d, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (d == 0) { calc_copy_text(out, out_sz, "undefined"); return; }
    if (d < 0) { n = -n; d = -d; }
    long long g = calc_gcd_ll(n, d);
    n /= g;
    d /= g;
    if (d == 1) {
        calc_ll_to_text(n, out, out_sz);
        return;
    }
    char nb[48], db[48];
    calc_ll_to_text(n, nb, (int)sizeof(nb));
    calc_ll_to_text(d, db, (int)sizeof(db));
    calc_append_text(out, out_sz, nb);
    calc_append_text(out, out_sz, "/");
    calc_append_text(out, out_sz, db);
}

static void calc_prime_factors_text(long long n, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (n == 0) { calc_copy_text(out, out_sz, "0"); return; }
    if (n < 0) {
        calc_append_text(out, out_sz, "-1 * ");
        n = -n;
    }
    if (n == 1) { calc_append_text(out, out_sz, "1"); return; }
    bool first = TRUE;
    for (long long p = 2; p * p <= n; ++p) {
        int cnt = 0;
        while (n % p == 0) { n /= p; cnt++; }
        if (cnt > 0) {
            if (!first) calc_append_text(out, out_sz, " * ");
            char pb[32];
            calc_ll_to_text(p, pb, (int)sizeof(pb));
            calc_append_text(out, out_sz, pb);
            if (cnt > 1) {
                calc_append_text(out, out_sz, "^");
                char cb[16];
                calc_ll_to_text(cnt, cb, (int)sizeof(cb));
                calc_append_text(out, out_sz, cb);
            }
            first = FALSE;
        }
    }
    if (n > 1) {
        if (!first) calc_append_text(out, out_sz, " * ");
        char nb[32];
        calc_ll_to_text(n, nb, (int)sizeof(nb));
        calc_append_text(out, out_sz, nb);
    }
}

static double calc_floor_approx(double x) {
    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

static double calc_ceil_approx(double x) {
    long long i = (long long)x;
    if ((double)i < x) i++;
    return (double)i;
}

static double calc_ncr(double n, double r) {
    double nn = calc_round_nearest(n);
    double rr = calc_round_nearest(r);
    if (nn < 0.0 || rr < 0.0 || rr > nn) return 0.0;
    if (calc_absd(nn - n) > 0.0000001 || calc_absd(rr - r) > 0.0000001) return 0.0;
    long long N = (long long)nn;
    long long R = (long long)rr;
    if (R > N - R) R = N - R;
    double result = 1.0;
    for (long long i = 1; i <= R; i++) {
        result *= (double)(N - R + i);
        result /= (double)i;
    }
    return result;
}

static double calc_npr(double n, double r) {
    double nn = calc_round_nearest(n);
    double rr = calc_round_nearest(r);
    if (nn < 0.0 || rr < 0.0 || rr > nn) return 0.0;
    if (calc_absd(nn - n) > 0.0000001 || calc_absd(rr - r) > 0.0000001) return 0.0;
    long long N = (long long)nn;
    long long R = (long long)rr;
    double result = 1.0;
    for (long long i = 0; i < R; i++) {
        result *= (double)(N - i);
    }
    return result;
}

static void calc_skip_ws(const char** p);
static bool calc_read_ident(const char** p, char* out, int out_sz);
static void calc_trim(char* s);
static void calc_append_line(char* dst, int cap, const char* line);
static double calc_parse_expr(calc_eval_ctx_t* ctx, const char** p);

static void calc_sort_doubles(double* values, int n) {
    if (!values || n <= 1) return;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (values[j] < values[i]) {
                double t = values[i];
                values[i] = values[j];
                values[j] = t;
            }
        }
    }
}

static int calc_read_call_args(calc_eval_ctx_t* ctx, const char** p, double* args, int max_args) {
    int count = 0;
    if (!ctx || !p || !args || max_args <= 0) return -1;
    for (;;) {
        calc_skip_ws(p);
        if (**p == ')') {
            (*p)++;
            return count;
        }
        if (count >= max_args) return -1;
        args[count++] = calc_parse_expr(ctx, p);
        calc_skip_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == ')') {
            (*p)++;
            return count;
        }
        return -1;
    }
}

static double calc_list_sum(const double* values, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += values[i];
    return sum;
}

static double calc_list_product(const double* values, int n) {
    double product = 1.0;
    for (int i = 0; i < n; i++) product *= values[i];
    return product;
}

static double calc_list_mean(const double* values, int n) {
    if (n <= 0) return 0.0;
    return calc_list_sum(values, n) / (double)n;
}

static double calc_list_variance(const double* values, int n) {
    if (n <= 0) return 0.0;
    double mean = calc_list_mean(values, n);
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double d = values[i] - mean;
        acc += d * d;
    }
    return acc / (double)n;
}

static double calc_list_median(double* values, int n) {
    if (n <= 0) return 0.0;
    calc_sort_doubles(values, n);
    if (n & 1) return values[n / 2];
    return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

static double calc_list_mode(const double* values, int n) {
    if (n <= 0) return 0.0;
    double tmp[32];
    int m = n;
    if (m > 32) m = 32;
    for (int i = 0; i < m; i++) tmp[i] = values[i];
    calc_sort_doubles(tmp, m);
    double best = tmp[0];
    int best_count = 1;
    for (int i = 0; i < m; ) {
        int j = i + 1;
        while (j < m && calc_absd(tmp[j] - tmp[i]) < 1e-9) j++;
        int count = j - i;
        if (count > best_count) {
            best_count = count;
            best = tmp[i];
        }
        i = j;
    }
    return best;
}

static double calc_list_stddev(const double* values, int n) {
    return calc_sqrt_approx(calc_list_variance(values, n));
}

static long long calc_sum_ll_args(const double* values, int n) {
    long long acc = 0;
    for (int i = 0; i < n; i++) acc += (long long)calc_round_nearest(values[i]);
    return acc;
}

static long long calc_gcd_ll_args(const double* values, int n) {
    if (n <= 0) return 0;
    long long g = (long long)calc_round_nearest(values[0]);
    for (int i = 1; i < n; i++) g = calc_gcd_ll(g, (long long)calc_round_nearest(values[i]));
    return g;
}

static long long calc_lcm_ll_args(const double* values, int n) {
    if (n <= 0) return 0;
    long long l = (long long)calc_round_nearest(values[0]);
    for (int i = 1; i < n; i++) l = calc_lcm_ll(l, (long long)calc_round_nearest(values[i]));
    return l;
}

static void calc_eval_number_text(const char* text, bool deg_mode, char* out, int out_sz);
static double calc_parse_expr(calc_eval_ctx_t* ctx, const char** p);
static bool calc_eval_text_with_x(const char* text, double x_value, bool deg_mode, double* out_value, char* steps, int steps_sz, bool record_steps);
static void calc_open_graph_window(window_t* source);

static bool calc_extract_raw_call(const char** p, char* inside, int inside_sz) {
    if (!p || !*p || !inside || inside_sz <= 0 || **p != '(') return false;
    (*p)++;
    int depth = 1;
    int pos = 0;
    while (**p) {
        char c = **p;
        if (c == '(') {
            depth++;
            if (pos + 1 < inside_sz) inside[pos++] = c;
            (*p)++;
            continue;
        }
        if (c == ')') {
            depth--;
            if (depth == 0) {
                (*p)++;
                break;
            }
            if (pos + 1 < inside_sz) inside[pos++] = c;
            (*p)++;
            continue;
        }
        if (pos + 1 < inside_sz) inside[pos++] = c;
        (*p)++;
    }
    inside[pos] = 0;
    calc_trim(inside);
    return depth == 0;
}

static int calc_split_top_level_args(const char* text, char args[][256], int max_args) {
    if (!text || !args || max_args <= 0) return 0;
    int depth = 0, count = 0, pos = 0;
    args[0][0] = 0;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '(') depth++;
        else if (c == ')') {
            if (depth > 0) depth--;
            if (pos + 1 < 256) args[count][pos++] = c;
            continue;
        }
        if (c == ',' && depth == 0) {
            args[count][pos] = 0;
            calc_trim(args[count]);
            count++;
            if (count >= max_args) return count;
            pos = 0;
            args[count][0] = 0;
            continue;
        }
        if (pos + 1 < 256) args[count][pos++] = c;
    }
    args[count][pos] = 0;
    calc_trim(args[count]);
    return count + 1;
}

static bool calc_eval_expr_at_x(const char* expr, double x_value, bool deg_mode, double* out_value) {
    return calc_eval_text_with_x(expr, x_value, deg_mode, out_value, NULL, 0, FALSE);
}

static bool calc_eval_diff_raw(const char* inside, bool deg_mode, char* display, int display_sz, char* status, int status_sz, char* steps, int steps_sz) {
    char args[2][256];
    int argc = calc_split_top_level_args(inside, args, 2);
    if (argc != 2) return false;
    double x0 = 0.0;
    if (!calc_eval_text_with_x(args[1], 0.0, deg_mode, &x0, NULL, 0, FALSE)) return false;
    double h = 1e-5 * (calc_absd(x0) + 1.0);
    double f1 = 0.0, f2 = 0.0;
    if (!calc_eval_expr_at_x(args[0], x0 + h, deg_mode, &f1)) return false;
    if (!calc_eval_expr_at_x(args[0], x0 - h, deg_mode, &f2)) return false;
    double d = (f1 - f2) / (2.0 * h);
    calc_num_to_text(d, display, display_sz);
    calc_copy_text(status, status_sz, "numerical derivative");
    if (steps && steps_sz > 0) {
        steps[0] = 0;
        calc_append_line(steps, steps_sz, "Central difference derivative");
        calc_append_text(steps, steps_sz, "f(x) = "); calc_append_line(steps, steps_sz, args[0]);
        char xb[64], hb[64], b1[64], b2[64], db[64];
        calc_num_to_text(x0, xb, sizeof(xb));
        calc_num_to_text(h, hb, sizeof(hb));
        calc_num_to_text(f1, b1, sizeof(b1));
        calc_num_to_text(f2, b2, sizeof(b2));
        calc_num_to_text(d, db, sizeof(db));
        calc_append_text(steps, steps_sz, "x0 = "); calc_append_line(steps, steps_sz, xb);
        calc_append_text(steps, steps_sz, "h = "); calc_append_line(steps, steps_sz, hb);
        calc_append_text(steps, steps_sz, "f(x0+h) = "); calc_append_line(steps, steps_sz, b1);
        calc_append_text(steps, steps_sz, "f(x0-h) = "); calc_append_line(steps, steps_sz, b2);
        calc_append_text(steps, steps_sz, "f'(x0) ≈ "); calc_append_line(steps, steps_sz, db);
    }
    return true;
}

static bool calc_eval_integral_raw(const char* inside, bool deg_mode, char* display, int display_sz, char* status, int status_sz, char* steps, int steps_sz) {
    char args[3][256];
    int argc = calc_split_top_level_args(inside, args, 3);
    if (argc != 3) return false;
    double a = 0.0, b = 0.0;
    if (!calc_eval_text_with_x(args[1], 0.0, deg_mode, &a, NULL, 0, FALSE)) return false;
    if (!calc_eval_text_with_x(args[2], 0.0, deg_mode, &b, NULL, 0, FALSE)) return false;
    int n = 128;
    if (n & 1) n++;
    double h = (b - a) / (double)n;
    double sum = 0.0;
    for (int i = 0; i <= n; ++i) {
        double x = a + h * (double)i;
        double y = 0.0;
        if (!calc_eval_expr_at_x(args[0], x, deg_mode, &y)) return false;
        double coeff = (i == 0 || i == n) ? 1.0 : ((i & 1) ? 4.0 : 2.0);
        sum += coeff * y;
    }
    double res = sum * h / 3.0;
    calc_num_to_text(res, display, display_sz);
    calc_copy_text(status, status_sz, "numerical integral");
    if (steps && steps_sz > 0) {
        steps[0] = 0;
        calc_append_line(steps, steps_sz, "Simpson integration");
        calc_append_text(steps, steps_sz, "f(x) = "); calc_append_line(steps, steps_sz, args[0]);
        char ab[64], bb[64], rb[64];
        calc_num_to_text(a, ab, sizeof(ab)); calc_num_to_text(b, bb, sizeof(bb)); calc_num_to_text(res, rb, sizeof(rb));
        calc_append_text(steps, steps_sz, "a = "); calc_append_line(steps, steps_sz, ab);
        calc_append_text(steps, steps_sz, "b = "); calc_append_line(steps, steps_sz, bb);
        calc_append_text(steps, steps_sz, "∫ f(x) dx ≈ "); calc_append_line(steps, steps_sz, rb);
    }
    return true;
}

static bool calc_eval_solve_raw(const char* inside, bool deg_mode, char* display, int display_sz, char* status, int status_sz, char* steps, int steps_sz) {
    char args[3][256];
    int argc = calc_split_top_level_args(inside, args, 3);
    if (argc != 3) return false;
    double lo = 0.0, hi = 0.0;
    if (!calc_eval_text_with_x(args[1], 0.0, deg_mode, &lo, NULL, 0, FALSE)) return false;
    if (!calc_eval_text_with_x(args[2], 0.0, deg_mode, &hi, NULL, 0, FALSE)) return false;
    double flo = 0.0, fhi = 0.0;
    if (!calc_eval_expr_at_x(args[0], lo, deg_mode, &flo)) return false;
    if (!calc_eval_expr_at_x(args[0], hi, deg_mode, &fhi)) return false;
    if (calc_absd(flo) < 1e-12) {
        calc_num_to_text(lo, display, display_sz);
    } else if (calc_absd(fhi) < 1e-12) {
        calc_num_to_text(hi, display, display_sz);
    } else if (flo * fhi > 0.0) {
        return false;
    } else {
        for (int i = 0; i < 64; ++i) {
            double mid = 0.5 * (lo + hi);
            double fmid = 0.0;
            if (!calc_eval_expr_at_x(args[0], mid, deg_mode, &fmid)) return false;
            if (calc_absd(fmid) < 1e-10) { lo = hi = mid; break; }
            if (flo * fmid <= 0.0) { hi = mid; fhi = fmid; }
            else { lo = mid; flo = fmid; }
        }
        calc_num_to_text(0.5 * (lo + hi), display, display_sz);
    }
    calc_copy_text(status, status_sz, "root solve");
    if (steps && steps_sz > 0) {
        steps[0] = 0;
        calc_append_line(steps, steps_sz, "Bisection root search");
        calc_append_text(steps, steps_sz, "f(x) = "); calc_append_line(steps, steps_sz, args[0]);
        calc_append_text(steps, steps_sz, "interval = [");
        char lb[64], hb[64]; calc_num_to_text(lo, lb, sizeof(lb)); calc_num_to_text(hi, hb, sizeof(hb));
        calc_append_text(steps, steps_sz, lb); calc_append_text(steps, steps_sz, ", "); calc_append_text(steps, steps_sz, hb); calc_append_line(steps, steps_sz, "]");
        calc_append_text(steps, steps_sz, "root ≈ "); calc_append_line(steps, steps_sz, display);
    }
    return true;
}

static bool calc_eval_table_raw(const char* inside, bool deg_mode, char* display, int display_sz, char* status, int status_sz, char* steps, int steps_sz) {
    char args[4][256];
    int argc = calc_split_top_level_args(inside, args, 4);
    if (argc < 3) return false;
    double a = 0.0, b = 0.0, step = 0.0;
    if (!calc_eval_text_with_x(args[1], 0.0, deg_mode, &a, NULL, 0, FALSE)) return false;
    if (!calc_eval_text_with_x(args[2], 0.0, deg_mode, &b, NULL, 0, FALSE)) return false;
    if (argc >= 4) {
        if (!calc_eval_text_with_x(args[3], 0.0, deg_mode, &step, NULL, 0, FALSE)) return false;
    } else {
        step = (b >= a) ? 1.0 : -1.0;
    }
    if (calc_absd(step) < 1e-12) return false;
    calc_copy_text(status, status_sz, "value table");
    if (steps && steps_sz > 0) {
        steps[0] = 0;
        calc_append_line(steps, steps_sz, "Table of f(x)");
        calc_append_text(steps, steps_sz, "f(x) = "); calc_append_line(steps, steps_sz, args[0]);
        int rows = 0;
        for (double x = a; (step > 0.0) ? (x <= b + 1e-12) : (x >= b - 1e-12); x += step) {
            double y = 0.0;
            if (!calc_eval_expr_at_x(args[0], x, deg_mode, &y)) return false;
            char xb[64], yb[64], line[160];
            calc_num_to_text(x, xb, sizeof(xb));
            calc_num_to_text(y, yb, sizeof(yb));
            line[0] = 0;
            calc_append_text(line, sizeof(line), "x="); calc_append_text(line, sizeof(line), xb);
            calc_append_text(line, sizeof(line), " -> y="); calc_append_text(line, sizeof(line), yb);
            calc_append_line(steps, steps_sz, line);
            rows++;
            if (rows >= 48) { calc_append_line(steps, steps_sz, "(truncated)"); break; }
        }
        calc_copy_text(display, display_sz, "table");
    } else {
        calc_copy_text(display, display_sz, "table");
    }
    return true;
}

static bool calc_eval_special_raw_function(calc_eval_ctx_t* ctx, const char* ident, const char** p, double* out) {
    char inside[512];
    if (!calc_extract_raw_call(p, inside, (int)sizeof(inside))) return false;
    char display[256], steps[2048], status[128];
    bool ok = false;
    if (smatch(ident, "diff") || smatch(ident, "deriv") || smatch(ident, "d")) {
        ok = calc_eval_diff_raw(inside, ctx->deg_mode, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "int") || smatch(ident, "integral") || smatch(ident, "integrate")) {
        ok = calc_eval_integral_raw(inside, ctx->deg_mode, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "solve") || smatch(ident, "root") || smatch(ident, "nsolve")) {
        ok = calc_eval_solve_raw(inside, ctx->deg_mode, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "table") || smatch(ident, "tabulate")) {
        ok = calc_eval_table_raw(inside, ctx->deg_mode, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    }
    if (!ok) return false;
    if (ctx->steps && steps[0]) calc_step_add(ctx, steps);
    if (out) *out = calc_strtodbl(display);
    return true;
}

static void calc_skip_ws(const char** p) {
    while (*p && **p && calc_is_space(**p)) (*p)++;
}

static bool calc_read_ident(const char** p, char* out, int out_sz) {
    int i = 0;
    calc_skip_ws(p);
    while (**p && calc_is_alpha(**p) && i < out_sz - 1) {
        out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    return i > 0;
}

static int calc_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

static bool calc_read_number(const char** p, char* out, int out_sz, double* value) {
    calc_skip_ws(p);
    if (!p || !*p || !out || out_sz <= 0 || !value) return FALSE;
    const char* s = *p;
    int i = 0;
    int sign = 1;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1;
        if (i + 1 < out_sz) out[i++] = *s;
        s++;
    }
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
        char b = s[1];
        if (b == 'x' || b == 'X') base = 16;
        else if (b == 'b' || b == 'B') base = 2;
        else if (b == 'o' || b == 'O') base = 8;
        if (i + 2 < out_sz) { out[i++] = '0'; out[i++] = b; }
        s += 2;
        unsigned long long iv = 0;
        int seen = 0;
        while (*s) {
            if (*s == '_') { if (i + 1 < out_sz) out[i++] = *s; s++; continue; }
            int d = calc_digit_value(*s);
            if (d < 0 || (unsigned)d >= base) break;
            seen = 1;
            iv = iv * (unsigned long long)base + (unsigned long long)d;
            if (i + 1 < out_sz) out[i++] = *s;
            s++;
        }
        out[i] = 0;
        if (!seen) return FALSE;
        while (calc_is_space(*s)) s++;
        if (*s != '\0') return FALSE;
        *value = sign < 0 ? -(double)iv : (double)iv;
        *p = s;
        return TRUE;
    }

    bool dot = FALSE;
    bool any = FALSE;
    while (*s && (calc_is_digit(*s) || *s == '.' || *s == '_')) {
        if (*s == '_') { if (i + 1 < out_sz) out[i++] = *s; s++; continue; }
        if (*s == '.') {
            if (dot) break;
            dot = TRUE;
        }
        if (i < out_sz - 1) out[i++] = *s;
        if (calc_is_digit(*s)) any = TRUE;
        s++;
    }
    out[i] = 0;
    if (!any) return FALSE;
    double whole = 0.0, frac = 0.1;
    bool seen_dot = FALSE;
    const char* q = out;
    if (*q == '+' || *q == '-') q++;
    while (*q) {
        if (*q == '_') { q++; continue; }
        if (*q == '.') { seen_dot = TRUE; q++; continue; }
        if (!seen_dot) whole = whole * 10.0 + (double)(*q - '0');
        else { whole += (double)(*q - '0') * frac; frac *= 0.1; }
        q++;
    }
    while (calc_is_space(*s)) s++;
    if (*s != '\0') return FALSE;
    *value = sign < 0 ? -whole : whole;
    *p = s;
    return TRUE;
}
static uint32_t calc_noise_hash_u32(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u + 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return h;
}

static double calc_noise_hash01(int x, int y, int z) {
    uint32_t h = calc_noise_hash_u32((uint32_t)x, (uint32_t)y, (uint32_t)z);
    return (double)(h & 0x00FFFFFFu) / 16777215.0;
}

static double calc_noise_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

static double calc_noise_fade(double t) {
    return t * t * (3.0 - 2.0 * t);
}

static double calc_noise_value1d(double x) {
    int x0 = (int)calc_floor_approx(x);
    int x1 = x0 + 1;
    double t = x - (double)x0;
    double a = calc_noise_hash01(x0, 0, 0) * 2.0 - 1.0;
    double b = calc_noise_hash01(x1, 0, 0) * 2.0 - 1.0;
    return calc_noise_lerp(a, b, calc_noise_fade(t));
}

static double calc_noise_value2d(double x, double y) {
    int x0 = (int)calc_floor_approx(x);
    int y0 = (int)calc_floor_approx(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    double tx = x - (double)x0;
    double ty = y - (double)y0;
    double n00 = calc_noise_hash01(x0, y0, 0) * 2.0 - 1.0;
    double n10 = calc_noise_hash01(x1, y0, 0) * 2.0 - 1.0;
    double n01 = calc_noise_hash01(x0, y1, 0) * 2.0 - 1.0;
    double n11 = calc_noise_hash01(x1, y1, 0) * 2.0 - 1.0;
    double u = calc_noise_fade(tx);
    double v = calc_noise_fade(ty);
    double nx0 = calc_noise_lerp(n00, n10, u);
    double nx1 = calc_noise_lerp(n01, n11, u);
    return calc_noise_lerp(nx0, nx1, v);
}

static double calc_noise_fbm1d(double x, int octaves, double lacunarity, double gain) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    double sum = 0.0;
    double amp = 1.0;
    double freq = 1.0;
    double norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += calc_noise_value1d(x * freq) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0) ? (sum / norm) : 0.0;
}

static double calc_noise_fbm2d(double x, double y, int octaves, double lacunarity, double gain) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    double sum = 0.0;
    double amp = 1.0;
    double freq = 1.0;
    double norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += calc_noise_value2d(x * freq, y * freq) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0) ? (sum / norm) : 0.0;
}

static double calc_parse_primary(calc_eval_ctx_t* ctx, const char** p) {
    calc_skip_ws(p);
    if (**p == '(') {
        (*p)++;
        double v = calc_parse_expr(ctx, p);
        calc_skip_ws(p);
        if (**p == ')') (*p)++;
        return v;
    }

    if (**p == '+' || **p == '-') {
        char sign = **p;
        (*p)++;
        double v = calc_parse_primary(ctx, p);
        return sign == '-' ? -v : v;
    }

    if (calc_is_alpha(**p)) {
        char ident[24];
        if (!calc_read_ident(p, ident, 24)) return 0.0;
        if (smatch(ident, "pi")) return 3.14159265358979323846;
        if (smatch(ident, "e")) return 2.71828182845904523536;
        if (smatch(ident, "tau")) return 6.28318530717958647692;
        if (smatch(ident, "x")) return ctx->use_x ? ctx->x_value : 0.0;

        calc_skip_ws(p);
        if (**p == '(') {
            (*p)++;

            double args[32];
            int argc = calc_read_call_args(ctx, p, args, 32);
            if (argc < 0) return 0.0;

            double arg1 = (argc > 0) ? args[0] : 0.0;
            double arg2 = (argc > 1) ? args[1] : 0.0;
            bool has_second_arg = (argc > 1);

            double result = arg1;
            if (smatch(ident, "sin")) {
                result = calc_sin_approx(calc_to_radians(arg1, ctx->deg_mode));
            } else if (smatch(ident, "cos")) {
                result = calc_cos_approx(calc_to_radians(arg1, ctx->deg_mode));
            } else if (smatch(ident, "tan")) {
                result = calc_tan_approx(calc_to_radians(arg1, ctx->deg_mode));
            } else if (smatch(ident, "asin")) {
                result = calc_from_radians(calc_atan_approx(arg1 / calc_sqrt_approx(1.0 - arg1 * arg1)), ctx->deg_mode);
            } else if (smatch(ident, "acos")) {
                result = calc_from_radians(3.14159265358979323846 / 2.0 - calc_atan_approx(arg1 / calc_sqrt_approx(1.0 - arg1 * arg1)), ctx->deg_mode);
            } else if (smatch(ident, "atan")) {
                result = calc_from_radians(calc_atan_approx(arg1), ctx->deg_mode);
            } else if (smatch(ident, "sqrt")) {
                result = calc_sqrt_approx(arg1);
            } else if (smatch(ident, "cbrt")) {
                result = (arg1 < 0.0) ? -calc_pow_approx(-arg1, 1.0 / 3.0) : calc_pow_approx(arg1, 1.0 / 3.0);
            } else if (smatch(ident, "abs")) {
                result = calc_absd(arg1);
            } else if (smatch(ident, "floor")) {
                result = calc_floor_approx(arg1);
            } else if (smatch(ident, "ceil")) {
                result = calc_ceil_approx(arg1);
            } else if (smatch(ident, "round")) {
                result = calc_round_nearest(arg1);
            } else if (smatch(ident, "sign") || smatch(ident, "sgn")) {
                result = (arg1 > 0.0) ? 1.0 : (arg1 < 0.0 ? -1.0 : 0.0);
            } else if (smatch(ident, "log") || smatch(ident, "lg")) {
                result = calc_ln_approx(arg1) / 2.302585092994046;
            } else if (smatch(ident, "ln")) {
                result = calc_ln_approx(arg1);
            } else if (smatch(ident, "exp")) {
                result = calc_exp_approx(arg1);
            } else if (smatch(ident, "noise") || smatch(ident, "perlin")) {
                if (argc >= 2) {
                    result = calc_noise_value2d(arg1, arg2);
                    if (argc >= 3) {
                        char line[192];
                        char xbuf[48], ybuf[48], rbuf[48];
                        calc_num_to_text(arg1, xbuf, (int)sizeof(xbuf));
                        calc_num_to_text(arg2, ybuf, (int)sizeof(ybuf));
                        calc_num_to_text(result, rbuf, (int)sizeof(rbuf));
                        line[0] = 0;
                        scat(line, "noise2(", (int)sizeof(line));
                        scat(line, xbuf, (int)sizeof(line));
                        scat(line, ", ", (int)sizeof(line));
                        scat(line, ybuf, (int)sizeof(line));
                        scat(line, ") = ", (int)sizeof(line));
                        scat(line, rbuf, (int)sizeof(line));
                        calc_step_add(ctx, line);
                    }
                } else {
                    result = calc_noise_value1d(arg1);
                    char line[160], xbuf[48], rbuf[48];
                    calc_num_to_text(arg1, xbuf, (int)sizeof(xbuf));
                    calc_num_to_text(result, rbuf, (int)sizeof(rbuf));
                    line[0] = 0;
                    scat(line, "noise(", (int)sizeof(line));
                    scat(line, xbuf, (int)sizeof(line));
                    scat(line, ") = ", (int)sizeof(line));
                    scat(line, rbuf, (int)sizeof(line));
                    calc_step_add(ctx, line);
                }
            } else if (smatch(ident, "fbm") || smatch(ident, "fbm2")) {
                int octaves = (argc >= 3) ? (int)calc_round_nearest(args[2]) : 4;
                double lacunarity = (argc >= 4) ? args[3] : 2.0;
                double gain = (argc >= 5) ? args[4] : 0.5;
                if (smatch(ident, "fbm2") && argc >= 2) {
                    result = calc_noise_fbm2d(arg1, arg2, octaves, lacunarity, gain);
                } else if (argc >= 1) {
                    result = calc_noise_fbm1d(arg1, octaves, lacunarity, gain);
                }
                {
                    char line[224], xbuf[48], ybuf[48], rbuf[48], obuf[32], lbuf[32], gbuf[32];
                    calc_num_to_text(arg1, xbuf, (int)sizeof(xbuf));
                    calc_num_to_text(arg2, ybuf, (int)sizeof(ybuf));
                    calc_num_to_text(result, rbuf, (int)sizeof(rbuf));
                    calc_num_to_text((double)octaves, obuf, (int)sizeof(obuf));
                    calc_num_to_text(lacunarity, lbuf, (int)sizeof(lbuf));
                    calc_num_to_text(gain, gbuf, (int)sizeof(gbuf));
                    line[0] = 0;
                    scat(line, smatch(ident, "fbm2") ? "fbm2(" : "fbm(", (int)sizeof(line));
                    scat(line, xbuf, (int)sizeof(line));
                    if (smatch(ident, "fbm2")) {
                        scat(line, ", ", (int)sizeof(line));
                        scat(line, ybuf, (int)sizeof(line));
                    }
                    scat(line, ", oct=", (int)sizeof(line));
                    scat(line, obuf, (int)sizeof(line));
                    scat(line, ", lac=", (int)sizeof(line));
                    scat(line, lbuf, (int)sizeof(line));
                    scat(line, ", gain=", (int)sizeof(line));
                    scat(line, gbuf, (int)sizeof(line));
                    scat(line, ") = ", (int)sizeof(line));
                    scat(line, rbuf, (int)sizeof(line));
                    calc_step_add(ctx, line);
                }
            } else if (smatch(ident, "deg")) {
                result = calc_from_radians(arg1, TRUE);
            } else if (smatch(ident, "rad")) {
                result = calc_to_radians(arg1, TRUE);
            } else if (smatch(ident, "fact") || smatch(ident, "factorial")) {
                result = calc_factorial(arg1);
            } else if (smatch(ident, "pow")) {
                if (has_second_arg) result = calc_pow_approx(arg1, arg2);
            } else if (smatch(ident, "root")) {
                if (has_second_arg && calc_absd(arg2) > 0.0000001) result = calc_pow_approx(arg1, 1.0 / arg2);
            } else if (smatch(ident, "mod")) {
                if (has_second_arg) {
                    long long a = (long long)calc_round_nearest(arg1);
                    long long b = (long long)calc_round_nearest(arg2);
                    result = (b != 0) ? (double)(a % b) : 0.0;
                }
            } else if (smatch(ident, "gcd")) {
                if (argc >= 2) result = (double)calc_gcd_ll_args(args, argc);
            } else if (smatch(ident, "lcm")) {
                if (argc >= 2) result = (double)calc_lcm_ll_args(args, argc);
            } else if (smatch(ident, "sum")) {
                result = calc_list_sum(args, argc);
            } else if (smatch(ident, "avg") || smatch(ident, "mean")) {
                result = calc_list_mean(args, argc);
            } else if (smatch(ident, "prod") || smatch(ident, "product")) {
                result = calc_list_product(args, argc);
            } else if (smatch(ident, "var") || smatch(ident, "variance")) {
                result = calc_list_variance(args, argc);
            } else if (smatch(ident, "std") || smatch(ident, "stdev") || smatch(ident, "sd")) {
                result = calc_list_stddev(args, argc);
            } else if (smatch(ident, "median") || smatch(ident, "med")) {
                double tmp[32];
                int n = argc < 32 ? argc : 32;
                for (int i = 0; i < n; i++) tmp[i] = args[i];
                result = calc_list_median(tmp, n);
            } else if (smatch(ident, "mode")) {
                result = calc_list_mode(args, argc);
            } else if (smatch(ident, "min")) {
                if (argc > 0) {
                    result = args[0];
                    for (int i = 1; i < argc; i++) if (args[i] < result) result = args[i];
                }
            } else if (smatch(ident, "max")) {
                if (argc > 0) {
                    result = args[0];
                    for (int i = 1; i < argc; i++) if (args[i] > result) result = args[i];
                }
            } else if (smatch(ident, "clamp")) {
                if (argc >= 3) {
                    result = arg1;
                    if (result < arg2) result = arg2;
                    if (result > args[2]) result = args[2];
                }
            } else if (smatch(ident, "frac") || smatch(ident, "simplify")) {
                if (has_second_arg && calc_absd(arg2) > 0.0000001) {
                    long long n = (long long)calc_round_nearest(arg1);
                    long long d = (long long)calc_round_nearest(arg2);
                    long long g = calc_gcd_ll(n, d);
                    long long sn = n / g;
                    long long sd = d / g;
                    result = (double)sn / (double)sd;
                    char inbuf[64], outbuf[64], line[192];
                    calc_fraction_to_text(n, d, inbuf, (int)sizeof(inbuf));
                    calc_fraction_to_text(sn, sd, outbuf, (int)sizeof(outbuf));
                    line[0] = 0;
                    scat(line, "simplify ", 192); scat(line, inbuf, 192); scat(line, " = ", 192); scat(line, outbuf, 192);
                    calc_step_add(ctx, line);
                }
            } else if (smatch(ident, "prime") || smatch(ident, "isprime")) {
                result = calc_is_prime_ll((long long)calc_round_nearest(arg1)) ? 1.0 : 0.0;
            } else if (smatch(ident, "factor") || smatch(ident, "factors") || smatch(ident, "primefactors")) {
                long long n = (long long)calc_round_nearest(arg1);
                char line[192], nbuf[48], fbuf[160];
                calc_ll_to_text(n, nbuf, (int)sizeof(nbuf));
                calc_prime_factors_text(n, fbuf, (int)sizeof(fbuf));
                line[0] = 0;
                scat(line, nbuf, 192); scat(line, " = ", 192); scat(line, fbuf, 192);
                calc_step_add(ctx, line);
                result = arg1;
            } else if (smatch(ident, "ncr") || smatch(ident, "comb") || smatch(ident, "choose")) {
                if (has_second_arg) result = calc_ncr(arg1, arg2);
            } else if (smatch(ident, "npr") || smatch(ident, "perm")) {
                if (has_second_arg) result = calc_npr(arg1, arg2);
            }

            char line[160], abuf[48], rbuf[48];
            calc_num_to_text(arg1, abuf, 48);
            calc_num_to_text(result, rbuf, 48);
            line[0] = 0;
            scat(line, ident, 160);
            scat(line, "(", 160);
            scat(line, abuf, 160);
            if (has_second_arg) {
                char bbuf[48];
                calc_num_to_text(arg2, bbuf, 48);
                scat(line, ", ", 160);
                scat(line, bbuf, 160);
            }
            if (argc > 2 && !smatch(ident, "gcd") && !smatch(ident, "lcm") && !smatch(ident, "sum") && !smatch(ident, "avg") && !smatch(ident, "mean") && !smatch(ident, "prod") && !smatch(ident, "product") && !smatch(ident, "var") && !smatch(ident, "variance") && !smatch(ident, "std") && !smatch(ident, "stdev") && !smatch(ident, "sd") && !smatch(ident, "median") && !smatch(ident, "med") && !smatch(ident, "mode") && !smatch(ident, "min") && !smatch(ident, "max") && !smatch(ident, "clamp")) {
                for (int i = 2; i < argc && i < 32; i++) {
                    char tbuf[48];
                    calc_num_to_text(args[i], tbuf, 48);
                    scat(line, ", ", 160);
                    scat(line, tbuf, 160);
                }
            } else if (argc > 2 && (smatch(ident, "gcd") || smatch(ident, "lcm") || smatch(ident, "sum") || smatch(ident, "avg") || smatch(ident, "mean") || smatch(ident, "prod") || smatch(ident, "product") || smatch(ident, "var") || smatch(ident, "variance") || smatch(ident, "std") || smatch(ident, "stdev") || smatch(ident, "sd") || smatch(ident, "median") || smatch(ident, "med") || smatch(ident, "mode") || smatch(ident, "min") || smatch(ident, "max"))) {
                for (int i = 2; i < argc && i < 32; i++) {
                    char tbuf[48];
                    calc_num_to_text(args[i], tbuf, 48);
                    scat(line, ", ", 160);
                    scat(line, tbuf, 160);
                }
            }
            scat(line, ") = ", 160);
            scat(line, rbuf, 160);
            calc_step_add(ctx, line);
            return result;
        }
        return 0.0;
    }


    char numbuf[32];
    double value = 0.0;
    if (calc_read_number(p, numbuf, 32, &value)) return value;
    return 0.0;
}

static double calc_parse_postfix(calc_eval_ctx_t* ctx, const char** p) {
    double v = calc_parse_primary(ctx, p);
    while (1) {
        calc_skip_ws(p);
        if (**p == '!') {
            (*p)++;
            double r = calc_factorial(v);
            char abuf[48], rbuf[48], line[160];
            calc_num_to_text(v, abuf, 48);
            calc_num_to_text(r, rbuf, 48);
            line[0] = 0; scat(line, abuf, 160); scat(line, "! = ", 160); scat(line, rbuf, 160);
            calc_step_add(ctx, line);
            v = r;
        } else if (**p == '%') {
            (*p)++;
            double r = v / 100.0;
            char abuf[48], rbuf[48], line[160];
            calc_num_to_text(v, abuf, 48);
            calc_num_to_text(r, rbuf, 48);
            line[0] = 0; scat(line, abuf, 160); scat(line, "% = ", 160); scat(line, rbuf, 160);
            calc_step_add(ctx, line);
            v = r;
        } else break;
    }
    return v;
}

static double calc_parse_power(calc_eval_ctx_t* ctx, const char** p) {
    double left = calc_parse_postfix(ctx, p);
    calc_skip_ws(p);
    if (**p == '^') {
        (*p)++;
        double right = calc_parse_power(ctx, p);
        double result = calc_pow_approx(left, right);
        char la[48], ra[48], rr[48], line[160];
        calc_num_to_text(left, la, 48);
        calc_num_to_text(right, ra, 48);
        calc_num_to_text(result, rr, 48);
        line[0] = 0;
        scat(line, la, 160); scat(line, " ^ ", 160); scat(line, ra, 160); scat(line, " = ", 160); scat(line, rr, 160);
        calc_step_add(ctx, line);
        return result;
    }
    return left;
}

static double calc_parse_term(calc_eval_ctx_t* ctx, const char** p) {
    double left = calc_parse_power(ctx, p);
    while (1) {
        calc_skip_ws(p);
        char op = **p;
        if (op != '*' && op != '/') break;
        (*p)++;
        double right = calc_parse_power(ctx, p);
        double result = (op == '*') ? left * right : (right != 0.0 ? left / right : 0.0);
        char la[48], ra[48], rr[48], line[160];
        calc_num_to_text(left, la, 48);
        calc_num_to_text(right, ra, 48);
        calc_num_to_text(result, rr, 48);
        line[0] = 0;
        scat(line, la, 160); scat(line, op == '*' ? " * " : " / ", 160); scat(line, ra, 160); scat(line, " = ", 160); scat(line, rr, 160);
        calc_step_add(ctx, line);
        left = result;
    }
    return left;
}

static double calc_parse_expr(calc_eval_ctx_t* ctx, const char** p) {
    double left = calc_parse_term(ctx, p);
    while (1) {
        calc_skip_ws(p);
        char op = **p;
        if (op != '+' && op != '-') break;
        (*p)++;
        double right = calc_parse_term(ctx, p);
        double result = (op == '+') ? left + right : left - right;
        char la[48], ra[48], rr[48], line[160];
        calc_num_to_text(left, la, 48);
        calc_num_to_text(right, ra, 48);
        calc_num_to_text(result, rr, 48);
        line[0] = 0;
        scat(line, la, 160); scat(line, op == '+' ? " + " : " - ", 160); scat(line, ra, 160); scat(line, " = ", 160); scat(line, rr, 160);
        calc_step_add(ctx, line);
        left = result;
    }
    return left;
}

static bool calc_has_top_level_operator(const char* expr, int* index, char* op) {
    int depth = 0;
    int last_sig = 0;
    for (int i = 0; expr && expr[i]; i++) {
        char c = expr[i];
        if (calc_is_space(c)) continue;
        if (c == '(') { depth++; last_sig = '('; continue; }
        if (c == ')') { if (depth > 0) depth--; last_sig = ')'; continue; }
        if (depth == 0 && (c == '+' || c == '-' || c == '*' || c == '/')) {
            bool unary = FALSE;
            if (c == '-' || c == '+') {
                if (i == 0) unary = TRUE;
                else if (last_sig == 0 || last_sig == '(' || last_sig == '+' || last_sig == '-' || last_sig == '*' || last_sig == '/' || last_sig == '^') unary = TRUE;
            }
            if (!unary) { if (index) *index = i; if (op) *op = c; return TRUE; }
        }
        last_sig = c;
    }
    return FALSE;
}

static void calc_trim(char* s) {
    if (!s) return;
    int n = slen(s);
    while (n > 0 && calc_is_space(s[n - 1])) s[--n] = 0;
}

static void calc_expr_clear(window_t* w) {
    w->calc_expr[0] = 0;
    w->calc_steps[0] = 0;
    w->calc_status[0] = 0;
    w->calc_display[0] = '0'; w->calc_display[1] = 0;
    w->calc_acc = 0.0;
    w->calc_op = 0;
    w->calc_clear_next = TRUE;
}

static void calc_expr_backspace(window_t* w) {
    int len = slen(w->calc_expr);
    if (len <= 0) return;
    while (len > 0 && calc_is_space(w->calc_expr[len - 1])) w->calc_expr[--len] = 0;
    if (len <= 0) return;
    if (calc_is_alpha(w->calc_expr[len - 1])) {
        while (len > 0 && calc_is_alpha(w->calc_expr[len - 1])) w->calc_expr[--len] = 0;
    } else {
        w->calc_expr[--len] = 0;
    }
    calc_trim(w->calc_expr);
}

static void calc_expr_append(window_t* w, const char* token) {
    if (!token || !token[0]) return;
    int len = slen(w->calc_expr);
    if (len > 0 && !calc_is_space(w->calc_expr[len - 1])) {
        bool need_space = TRUE;
        if (token[0] == ')' || token[0] == '.' || token[0] == '!') need_space = FALSE;
        if (token[0] == '(') need_space = FALSE;
        if (token[0] == '+' || token[0] == '-' || token[0] == '*' || token[0] == '/' || token[0] == '^' || token[0] == '%') need_space = TRUE;
        if (need_space && len < (int)sizeof(w->calc_expr) - 2) { w->calc_expr[len++] = ' '; w->calc_expr[len] = 0; }
    }
    calc_append_text(w->calc_expr, (int)sizeof(w->calc_expr), token);
}


static void calc_append_line(char* dst, int cap, const char* line) {
    if (!dst || cap <= 0 || !line || !line[0]) return;
    calc_append_text(dst, cap, line);
    calc_append_char(dst, cap, '\n');
}

static bool calc_text_to_i64(const char* text, long long* out) {
    if (!text || !out) return FALSE;
    while (calc_is_space(*text)) text++;
    int sign = 1;
    if (*text == '+' || *text == '-') {
        if (*text == '-') sign = -1;
        text++;
    }
    if (!calc_is_digit(*text)) return FALSE;
    long long value = 0;
    while (*text) {
        if (calc_is_space(*text)) { text++; continue; }
        if (*text == '.') return FALSE;
        if (!calc_is_digit(*text)) return FALSE;
        value = value * 10 + (long long)(*text - '0');
        text++;
    }
    *out = sign < 0 ? -value : value;
    return TRUE;
}

static int calc_ll_abs_digits(long long v, char* out, int out_sz) {
    if (!out || out_sz <= 0) return 0;
    if (v < 0) v = -v;
    char tmp[32];
    int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v > 0 && ti < (int)sizeof(tmp)) {
        tmp[ti++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    int len = 0;
    for (int i = ti - 1; i >= 0 && len < out_sz - 1; i--) out[len++] = tmp[i];
    out[len] = 0;
    return len;
}

static void calc_append_right_aligned(char* dst, int cap, const char* prefix, const char* value, int width) {
    if (!dst || cap <= 0) return;
    if (!prefix) prefix = "";
    if (!value) value = "";
    char line[256];
    line[0] = 0;
    calc_append_text(line, 256, prefix);
    int used = slen(prefix);
    int pad = width - slen(value);
    if (pad < 1) pad = 1;
    for (int i = 0; i < pad && used < 255; i++) line[used++] = ' ';
    line[used] = 0;
    calc_append_text(line, 256, value);
    calc_append_line(dst, cap, line);
}



typedef struct {
    long long scaled;
    int decimals;
    bool valid;
} calc_hissan_decimal_t;

typedef struct {
    char digits[48];
    int len;
    bool negative;
} calc_bigint_t;

static long long calc_pow10_ll_safe(int n) {
    long long v = 1;
    while (n-- > 0) {
        if (v > 922337203685477580LL) return 0;
        v *= 10LL;
    }
    return v;
}

static void calc_bigint_trim(calc_bigint_t* v) {
    if (!v) return;
    int i = 0;
    while (i + 1 < v->len && v->digits[i] == '0') i++;
    if (i > 0) {
        memmove(v->digits, v->digits + i, (size_t)(v->len - i));
        v->len -= i;
        v->digits[v->len] = 0;
    }
    if (v->len <= 0) {
        v->len = 1;
        v->digits[0] = '0';
        v->digits[1] = 0;
        v->negative = false;
    }
    if (v->len == 1 && v->digits[0] == '0') v->negative = false;
}

static bool calc_bigint_from_text(const char* text, calc_bigint_t* out) {
    if (!text || !out) return FALSE;
    while (calc_is_space(*text)) text++;
    out->negative = FALSE;
    if (*text == '+' || *text == '-') {
        out->negative = (*text == '-');
        text++;
    }
    int n = 0;
    bool saw = FALSE;
    for (; *text; ++text) {
        if (calc_is_space(*text) || *text == '_') continue;
        if (!calc_is_digit(*text)) return FALSE;
        if (n + 1 >= (int)sizeof(out->digits)) return FALSE;
        out->digits[n++] = *text;
        saw = TRUE;
    }
    if (!saw) return FALSE;
    out->digits[n] = 0;
    out->len = n;
    calc_bigint_trim(out);
    return TRUE;
}

static int calc_bigint_cmp_abs(const calc_bigint_t* a, const calc_bigint_t* b) {
    if (a->len != b->len) return (a->len > b->len) ? 1 : -1;
    for (int i = 0; i < a->len; ++i) {
        if (a->digits[i] != b->digits[i]) return (a->digits[i] > b->digits[i]) ? 1 : -1;
    }
    return 0;
}

static void calc_bigint_copy(calc_bigint_t* dst, const calc_bigint_t* src) {
    if (!dst || !src) return;
    memcpy(dst, src, sizeof(*dst));
}

static void calc_bigint_sub_abs(calc_bigint_t* a, const calc_bigint_t* b) {
    // assumes |a| >= |b|, both positive
    int ia = a->len - 1;
    int ib = b->len - 1;
    int borrow = 0;
    while (ia >= 0) {
        int da = a->digits[ia] - '0' - borrow;
        int db = (ib >= 0) ? (b->digits[ib] - '0') : 0;
        if (da < db) { da += 10; borrow = 1; } else borrow = 0;
        a->digits[ia] = (char)('0' + (da - db));
        ia--; ib--;
    }
    calc_bigint_trim(a);
}

static void calc_bigint_mul10_add(calc_bigint_t* v, int digit) {
    if (!v || digit < 0 || digit > 9) return;
    if (v->len == 1 && v->digits[0] == '0') {
        v->digits[0] = (char)('0' + digit);
        v->digits[1] = 0;
        v->len = 1;
        calc_bigint_trim(v);
        return;
    }
    if (v->len + 1 >= (int)sizeof(v->digits)) return;
    v->digits[v->len++] = (char)('0' + digit);
    v->digits[v->len] = 0;
    calc_bigint_trim(v);
}

static int calc_bigint_mul_digit(const calc_bigint_t* a, int digit, calc_bigint_t* out) {
    if (!a || !out || digit < 0 || digit > 9) return FALSE;
    if (digit == 0) {
        out->digits[0] = '0'; out->digits[1] = 0; out->len = 1; out->negative = false;
        return TRUE;
    }
    int carry = 0;
    int n = a->len;
    if (n + 1 >= (int)sizeof(out->digits)) return FALSE;
    out->digits[n] = 0;
    for (int i = n - 1; i >= 0; --i) {
        int p = (a->digits[i] - '0') * digit + carry;
        out->digits[i + 1] = (char)('0' + (p % 10));
        carry = p / 10;
    }
    out->digits[0] = (char)('0' + carry);
    out->len = n + (carry ? 1 : 0);
    if (!carry) memmove(out->digits, out->digits + 1, (size_t)n);
    out->digits[out->len] = 0;
    out->negative = false;
    calc_bigint_trim(out);
    return TRUE;
}

static bool calc_bigint_long_divide(const calc_bigint_t* dividend, const calc_bigint_t* divisor, calc_bigint_t* quotient, calc_bigint_t* remainder, char* trace, size_t trace_sz) {
    if (!dividend || !divisor || !quotient || !remainder) return FALSE;
    calc_bigint_t cur = {0};
    calc_bigint_t q = {0};
    calc_bigint_t prod = {0};
    calc_bigint_t tmp = {0};

    q.len = 1; q.digits[0] = '0'; q.digits[1] = 0; q.negative = false;
    cur.len = 1; cur.digits[0] = '0'; cur.digits[1] = 0; cur.negative = false;

    if (trace && trace_sz) trace[0] = 0;

    for (int i = 0; i < dividend->len; ++i) {
        calc_bigint_mul10_add(&cur, dividend->digits[i] - '0');

        int qdigit = 0;
        for (int d = 9; d >= 0; --d) {
            if (!calc_bigint_mul_digit(divisor, d, &prod)) return FALSE;
            if (calc_bigint_cmp_abs(&cur, &prod) >= 0) { qdigit = d; break; }
        }

        if (!calc_bigint_mul_digit(divisor, qdigit, &tmp)) return FALSE;
        calc_bigint_copy(&prod, &tmp);
        if (qdigit > 0) calc_bigint_sub_abs(&cur, &prod);

        if (!(q.len == 1 && q.digits[0] == '0')) {
            if (q.len + 1 >= (int)sizeof(q.digits)) return FALSE;
            q.digits[q.len++] = (char)('0' + qdigit);
            q.digits[q.len] = 0;
        } else if (qdigit != 0) {
            q.digits[0] = (char)('0' + qdigit);
            q.digits[1] = 0;
            q.len = 1;
        }
        if (trace && trace_sz) {
            char line[256];
            line[0] = 0;
            calc_append_text(line, sizeof(line), "bring down ");
            char dc[2] = { dividend->digits[i], 0 };
            calc_append_text(line, sizeof(line), dc);
            calc_append_text(line, sizeof(line), " -> ");
            calc_append_text(line, sizeof(line), cur.digits);
            calc_append_text(line, sizeof(line), " | q=");
            char qbuf[8] = { (char)('0' + qdigit), 0 };
            calc_append_text(line, sizeof(line), qbuf);
            calc_append_line(trace, (int)trace_sz, line);
        }
    }

    calc_bigint_trim(&q);
    calc_bigint_copy(quotient, &q);
    calc_bigint_copy(remainder, &cur);
    return TRUE;
}

static bool calc_parse_hissan_decimal(const char* text, long long* scaled_out, int* decimals_out) {
    if (!text || !scaled_out || !decimals_out) return FALSE;
    while (calc_is_space(*text)) text++;

    int sign = 1;
    if (*text == '+' || *text == '-') {
        if (*text == '-') sign = -1;
        text++;
    }

    long long int_part = 0;
    long long frac_part = 0;
    int frac_digits = 0;
    bool saw_digit = FALSE;
    bool saw_dot = FALSE;

    while (*text) {
        if (calc_is_space(*text)) {
            text++;
            continue;
        }
        if (*text == '.') {
            if (saw_dot) return FALSE;
            saw_dot = TRUE;
            text++;
            continue;
        }
        if (!calc_is_digit(*text)) return FALSE;
        saw_digit = TRUE;
        if (!saw_dot) {
            if (int_part > 922337203685477580LL) return FALSE;
            int_part = int_part * 10LL + (long long)(*text - '0');
        } else {
            if (frac_digits >= 12) return FALSE;
            frac_part = frac_part * 10LL + (long long)(*text - '0');
            frac_digits++;
        }
        text++;
    }

    if (!saw_digit) return FALSE;
    long long scale = calc_pow10_ll_safe(frac_digits);
    if (scale == 0) return FALSE;
    long long scaled = int_part * scale + frac_part;
    if (sign < 0) scaled = -scaled;
    *scaled_out = scaled;
    *decimals_out = frac_digits;
    return TRUE;
}

static bool calc_align_hissan_scaled(long long left_scaled, int left_decimals, long long right_scaled, int right_decimals, long long* out_left, long long* out_right, int* out_scale) {
    if (!out_left || !out_right || !out_scale) return FALSE;
    int scale = left_decimals > right_decimals ? left_decimals : right_decimals;
    long long left_mul = calc_pow10_ll_safe(scale - left_decimals);
    long long right_mul = calc_pow10_ll_safe(scale - right_decimals);
    if (left_mul == 0 || right_mul == 0) return FALSE;
    *out_left = left_scaled * left_mul;
    *out_right = right_scaled * right_mul;
    *out_scale = scale;
    return TRUE;
}

static bool calc_eval_simple_vertical(window_t* w, const char* expr) {
    int idx = -1;
    char op = 0;
    if (!calc_has_top_level_operator(expr, &idx, &op)) {
        calc_eval_ctx_t ctx = {0};
        char trace[1024];
        trace[0] = 0;
        ctx.steps = trace;
        ctx.steps_cap = (int)sizeof(trace);
        ctx.steps_len = 0;
        ctx.deg_mode = w->calc_angle_deg;
        ctx.ok = TRUE;
        const char* p = expr;
        double value = calc_parse_expr(&ctx, &p);
        calc_skip_ws(&p);
        if (!ctx.ok || (p && *p)) return FALSE;
        char out[1024];
        out[0] = 0;
        calc_append_line(out, 1024, "Expression trace:");
        if (trace[0]) {
            calc_append_text(out, 1024, trace);
            if (out[slen(out) - 1] != '\n') calc_append_char(out, 1024, '\n');
        }
        char rbuf[48];
        calc_num_to_text(value, rbuf, 48);
        calc_append_text(out, 1024, "Result: ");
        calc_append_text(out, 1024, rbuf);
        calc_append_char(out, 1024, '\n');
        calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), out);
        calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), rbuf);
        w->calc_acc = value;
        w->calc_clear_next = TRUE;
        w->calc_op = 0;
        return TRUE;
    }

    char left_txt[128], right_txt[128];
    int li = 0, ri = 0;
    for (int i = 0; expr[i] && i < idx && li < 127; i++) if (expr[i] != ' ') left_txt[li++] = expr[i];
    left_txt[li] = 0;
    for (int i = idx + 1; expr[i] && ri < 127; i++) if (expr[i] != ' ') right_txt[ri++] = expr[i];
    right_txt[ri] = 0;

    calc_eval_ctx_t ctx = {0};
    char left_steps[256], right_steps[256];
    left_steps[0] = 0; right_steps[0] = 0;
    ctx.steps = left_steps; ctx.steps_cap = 256; ctx.steps_len = 0; ctx.deg_mode = w->calc_angle_deg; ctx.ok = TRUE;
    const char* lp = left_txt;
    double left = calc_parse_expr(&ctx, &lp);
    calc_skip_ws(&lp);
    if (!ctx.ok || (lp && *lp)) return FALSE;

    ctx.steps = right_steps; ctx.steps_cap = 256; ctx.steps_len = 0; ctx.deg_mode = w->calc_angle_deg; ctx.ok = TRUE;
    const char* rp = right_txt;
    double right = calc_parse_expr(&ctx, &rp);
    calc_skip_ws(&rp);
    if (!ctx.ok || (rp && *rp)) return FALSE;

    char out[1024]; out[0] = 0;
    char abuf[48], bbuf[48], rbuf[48];
    calc_num_to_text(left, abuf, 48);
    calc_num_to_text(right, bbuf, 48);

    bool left_int = FALSE, right_int = FALSE;
    long long a_ll = 0, b_ll = 0;
    left_int = calc_text_to_i64(left_txt, &a_ll);
    right_int = calc_text_to_i64(right_txt, &b_ll);

    long long left_scaled = 0, right_scaled = 0;
    int left_dec = 0, right_dec = 0;
    bool left_dec_ok = calc_parse_hissan_decimal(left_txt, &left_scaled, &left_dec);
    bool right_dec_ok = calc_parse_hissan_decimal(right_txt, &right_scaled, &right_dec);

    double result = (op == '/' && right != 0.0) ? left / right : (op == '+' ? left + right : op == '-' ? left - right : op == '*' ? left * right : left);
    calc_num_to_text(result, rbuf, 48);

    calc_append_line(out, 1024, "Hissan:");
    {
        char expr_line[256];
        expr_line[0] = 0;
        calc_append_text(expr_line, 256, left_txt);
        char opbuf[4] = { op, 0, 0, 0 };
        calc_append_text(expr_line, 256, " ");
        calc_append_text(expr_line, 256, opbuf);
        calc_append_text(expr_line, 256, " ");
        calc_append_text(expr_line, 256, right_txt);
        calc_append_text(expr_line, 256, " = ");
        calc_append_text(expr_line, 256, rbuf);
        calc_append_line(out, 1024, expr_line);
    }
    if (left_steps[0]) {
        calc_append_line(out, 1024, "Left side steps:");
        calc_append_text(out, 1024, left_steps);
        if (out[slen(out) - 1] != '\n') calc_append_char(out, 1024, '\n');
    }
    if (right_steps[0]) {
        calc_append_line(out, 1024, "Right side steps:");
        calc_append_text(out, 1024, right_steps);
        if (out[slen(out) - 1] != '\n') calc_append_char(out, 1024, '\n');
    }

    long long aa = a_ll;
    long long bb = b_ll;
    int decimal_scale = 0;
    bool decimal_mode = FALSE;
    if ((op == '+' || op == '-' || op == '*') && left_dec_ok && right_dec_ok) {
        decimal_mode = (left_dec > 0 || right_dec > 0);
        if (decimal_mode) {
            long long aligned_a = 0, aligned_b = 0;
            if (calc_align_hissan_scaled(left_scaled, left_dec, right_scaled, right_dec, &aligned_a, &aligned_b, &decimal_scale)) {
                aa = aligned_a;
                bb = aligned_b;
                calc_append_line(out, 1024, "Decimal alignment:");
                {
                    char line[192];
                    line[0] = 0;
                    calc_append_text(line, 192, left_txt);
                    calc_append_text(line, 192, " -> ");
                    char atext[64];
                    calc_num_to_text((double)aa, atext, 64);
                    calc_append_text(line, 192, atext);
                    calc_append_text(line, 192, " , ");
                    calc_append_text(line, 192, right_txt);
                    calc_append_text(line, 192, " -> ");
                    char btext[64];
                    calc_num_to_text((double)bb, btext, 64);
                    calc_append_text(line, 192, btext);
                    calc_append_text(line, 192, " (×10^");
                    char stext[16];
                    calc_num_to_text((double)decimal_scale, stext, 16);
                    calc_append_text(line, 192, stext);
                    calc_append_text(line, 192, ")");
                    calc_append_line(out, 1024, line);
                }
            }
        }
    }

    if ((op == '+' || op == '-' || op == '*') && ((left_int && right_int) || decimal_mode)) {
        bool negative_result = FALSE;
        if (op == '-' && aa < bb) {
            negative_result = TRUE;
            long long tmp = aa; aa = bb; bb = tmp;
            calc_append_line(out, 1024, "Left < right, so compute the absolute difference and apply the minus sign.");
        }

        char aabs[64], babs[64], rabs[64];
        calc_ll_abs_digits(aa, aabs, 64);
        calc_ll_abs_digits(bb, babs, 64);
        long long rr_ll = (long long)calc_round_nearest(result);
        calc_ll_abs_digits(rr_ll < 0 ? -rr_ll : rr_ll, rabs, 64);

        calc_append_line(out, 1024, "------------");
        if (op == '+') {
            long long carry = 0;
            int place = 0;
            long long x = aa, y = bb;
            while (x > 0 || y > 0 || carry > 0) {
                int da = (int)(x % 10);
                int db = (int)(y % 10);
                int sum = da + db + (int)carry;
                int digit = sum % 10;
                int next_carry = sum / 10;
                char line[192];
                line[0] = 0;
                char placebuf[32]={0};
                if (decimal_mode && decimal_scale > 0 && place == decimal_scale) {
                    calc_append_text(placebuf, 32, "decimal point");
                } else if (place == 0) calc_append_text(placebuf, 32, "ones");
                else if (place == 1) calc_append_text(placebuf, 32, "tens");
                else if (place == 2) calc_append_text(placebuf, 32, "hundreds");
                else {
                    calc_append_text(placebuf, 32, "10^");
                    char pbuf[16]; calc_num_to_text((double)place, pbuf, 16);
                    calc_append_text(placebuf, 32, pbuf);
                }
                calc_append_text(line, 192, placebuf);
                calc_append_text(line, 192, ": ");
                char dabuf2[16], dbbuf2[16], sumbuf[16], digbuf[16], cbuf[16];
                calc_num_to_text((double)da, dabuf2, 16);
                calc_num_to_text((double)db, dbbuf2, 16);
                calc_num_to_text((double)sum, sumbuf, 16);
                calc_num_to_text((double)digit, digbuf, 16);
                calc_num_to_text((double)next_carry, cbuf, 16);
                calc_append_text(line, 192, dabuf2);
                calc_append_text(line, 192, " + ");
                calc_append_text(line, 192, dbbuf2);
                if (carry) calc_append_text(line, 192, " + carry 1");
                calc_append_text(line, 192, " = ");
                calc_append_text(line, 192, sumbuf);
                calc_append_text(line, 192, " -> write ");
                calc_append_text(line, 192, digbuf);
                if (next_carry) {
                    calc_append_text(line, 192, ", carry ");
                    calc_append_text(line, 192, cbuf);
                }
                calc_append_line(out, 1024, line);
                carry = (long long)next_carry;
                x /= 10; y /= 10; place++;
            }
            if (decimal_mode && decimal_scale > 0) {
                calc_append_line(out, 1024, "Decimal point stays aligned after the scaled addition.");
            }
            calc_append_text(out, 1024, "Result: ");
            calc_append_text(out, 1024, rbuf);
            calc_append_char(out, 1024, '\n');
        } else if (op == '-') {
            long long borrow = 0;
            int place = 0;
            long long x = aa, y = bb;
            while (x > 0 || y > 0 || borrow > 0) {
                int da = (int)(x % 10);
                int db = (int)(y % 10);
                int minuend = da - (int)borrow;
                int next_borrow = 0;
                if (minuend < db) {
                    minuend += 10;
                    next_borrow = 1;
                }
                int diff = minuend - db;
                char line[192];
                line[0] = 0;
                char placebuf[32]={0};
                if (decimal_mode && decimal_scale > 0 && place == decimal_scale) {
                    calc_append_text(placebuf, 32, "decimal point");
                } else if (place == 0) calc_append_text(placebuf, 32, "ones");
                else if (place == 1) calc_append_text(placebuf, 32, "tens");
                else if (place == 2) calc_append_text(placebuf, 32, "hundreds");
                else {
                    calc_append_text(placebuf, 32, "10^");
                    char pbuf[16]; calc_num_to_text((double)place, pbuf, 16);
                    calc_append_text(placebuf, 32, pbuf);
                }
                calc_append_text(line, 192, placebuf);
                calc_append_text(line, 192, ": ");
                char dabuf2[16], dbbuf2[16], difbuf[16];
                calc_num_to_text((double)da, dabuf2, 16);
                calc_num_to_text((double)db, dbbuf2, 16);
                calc_num_to_text((double)diff, difbuf, 16);
                calc_append_text(line, 192, dabuf2);
                if (borrow) calc_append_text(line, 192, " - borrow 1");
                calc_append_text(line, 192, " - ");
                calc_append_text(line, 192, dbbuf2);
                calc_append_text(line, 192, " = ");
                calc_append_text(line, 192, difbuf);
                if (next_borrow) calc_append_text(line, 192, " (borrow next column)");
                calc_append_line(out, 1024, line);
                borrow = (long long)next_borrow;
                x /= 10; y /= 10; place++;
            }
            if (negative_result) calc_append_line(out, 1024, "Apply negative sign to the absolute difference.");
            if (decimal_mode && decimal_scale > 0) {
                calc_append_line(out, 1024, "Decimal point stays aligned after the scaled subtraction.");
            }
            calc_append_text(out, 1024, "Result: ");
            if (negative_result) calc_append_text(out, 1024, "-");
            calc_append_text(out, 1024, rbuf);
            calc_append_char(out, 1024, '\n');
        } else if (op == '*') {
            long long abs_a = aa < 0 ? -aa : aa;
            long long abs_b = bb < 0 ? -bb : bb;
            long long sign = ((aa < 0) ^ (bb < 0)) ? -1 : 1;
            calc_append_line(out, 1024, "Partial products:");
            long long place = 0;
            long long temp_b = abs_b;
            while (temp_b > 0) {
                int digit = (int)(temp_b % 10);
                long long partial = abs_a * (long long)digit;
                long long shifted = partial;
                for (long long s = 0; s < place; s++) shifted *= 10;
                char line[192];
                line[0] = 0;
                char dtext[16], ptext[48], stext[48];
                calc_num_to_text((double)digit, dtext, 16);
                calc_num_to_text((double)partial, ptext, 48);
                calc_num_to_text((double)shifted, stext, 48);
                calc_append_text(line, 192, "digit ");
                calc_append_text(line, 192, dtext);
                calc_append_text(line, 192, " -> ");
                calc_append_text(line, 192, ptext);
                if (place > 0) {
                    calc_append_text(line, 192, " (shifted ");
                    calc_append_text(line, 192, stext);
                    calc_append_text(line, 192, ")");
                }
                calc_append_line(out, 1024, line);
                temp_b /= 10;
                place++;
            }
            if (abs_b > 9) calc_append_line(out, 1024, "Combine partials to get the final result.");
            if (decimal_mode && decimal_scale > 0) {
                calc_append_text(out, 1024, "Decimal places total: ");
                char dbuf[16];
                calc_num_to_text((double)decimal_scale, dbuf, 16);
                calc_append_text(out, 1024, dbuf);
                calc_append_line(out, 1024, " (remove that many digits from the end of the product).");
            }
            if (sign < 0) calc_append_line(out, 1024, "Final result is negative because the signs differ.");
            calc_append_text(out, 1024, "Result: ");
            if (sign < 0 && rbuf[0] != '-') calc_append_text(out, 1024, "-");
            calc_append_text(out, 1024, rbuf);
            calc_append_char(out, 1024, '\n');
        }
    } else if (op == '/') {
        if (calc_absd(right) < 0.0000001) {
            calc_append_line(out, 1024, "Division by zero.");
            calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), out);
            calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "Error");
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Division by zero");
            w->calc_clear_next = TRUE;
            w->calc_op = 0;
            return FALSE;
        }
        if (left_int && right_int && b_ll != 0) {
            calc_bigint_t dividend, divisor, quotient, remainder;
            if (calc_bigint_from_text(left_txt, &dividend) && calc_bigint_from_text(right_txt, &divisor) && divisor.len > 0 && !(divisor.len == 1 && divisor.digits[0] == '0')) {
                bool neg = dividend.negative ^ divisor.negative;
                dividend.negative = false;
                divisor.negative = false;
                char trace[1024];
                trace[0] = 0;
                calc_append_line(out, 1024, "Long division (up to 40 digits):");
                if (calc_bigint_long_divide(&dividend, &divisor, &quotient, &remainder, trace, sizeof(trace))) {
                    char qtxt[64], rtxt[64];
                    qtxt[0] = 0; rtxt[0] = 0;
                    calc_append_text(qtxt, sizeof(qtxt), quotient.digits);
                    calc_append_text(rtxt, sizeof(rtxt), remainder.digits);
                    if (neg && !(quotient.len == 1 && quotient.digits[0] == '0')) {
                        char qneg[70];
                        qneg[0] = 0;
                        calc_append_text(qneg, sizeof(qneg), "-");
                        calc_append_text(qneg, sizeof(qneg), qtxt);
                        calc_copy_text(qtxt, sizeof(qtxt), qneg);
                    }
                    calc_append_line(out, 1024, "Layout:");
                    calc_append_text(out, 1024, "  dividend  : ");
                    calc_append_line(out, 1024, dividend.digits);
                    calc_append_text(out, 1024, "  divisor   : ");
                    calc_append_line(out, 1024, divisor.digits);
                    calc_append_text(out, 1024, "  quotient  : ");
                    calc_append_line(out, 1024, qtxt);
                    calc_append_text(out, 1024, "  remainder : ");
                    calc_append_line(out, 1024, rtxt);
                    if (trace[0]) {
                        calc_append_line(out, 1024, "Steps:");
                        calc_append_text(out, 1024, trace);
                    }
                    calc_append_text(out, 1024, "Check: ");
                    if (neg && !(quotient.len == 1 && quotient.digits[0] == '0')) calc_append_text(out, 1024, "-");
                    calc_append_text(out, 1024, qtxt);
                    calc_append_text(out, 1024, " * ");
                    calc_append_text(out, 1024, divisor.digits);
                    calc_append_text(out, 1024, " + ");
                    calc_append_text(out, 1024, rtxt);
                    calc_append_text(out, 1024, " = ");
                    calc_append_text(out, 1024, dividend.digits);
                    calc_append_char(out, 1024, '\n');
                    if (neg) calc_append_line(out, 1024, "Result sign is negative because the signs differ.");
                } else {
                    calc_append_line(out, 1024, "Long division failed.");
                }
            } else {
                calc_append_line(out, 1024, "Long division supports integer input up to 40 digits.");
            }
        } else {
            calc_append_line(out, 1024, "Decimal division:");
            calc_append_text(out, 1024, abuf);
            calc_append_text(out, 1024, " / ");
            calc_append_text(out, 1024, bbuf);
            calc_append_text(out, 1024, " = ");
            calc_append_text(out, 1024, rbuf);
            calc_append_char(out, 1024, '\n');
        }
    } else {
        calc_append_line(out, 1024, "Unsupported hissan expression");
    }

    calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), out);
    calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), rbuf);
    w->calc_acc = result;
    w->calc_clear_next = TRUE;
    w->calc_op = 0;
    return TRUE;
}



// ============================================================
// HS3 symbolic algebra helpers (degree <= 2)
// ============================================================

typedef struct {
    double c; /* constant */
    double b; /* x */
    double a; /* x^2 */
    bool valid;
} calc_poly_t;

static bool calc_near_zero(double v) {
    return calc_absd(v) < 0.0000001;
}

static bool calc_near_int(double v) {
    return calc_absd(v - calc_round_nearest(v)) < 0.0000001;
}

static calc_poly_t calc_poly_invalid(void) {
    calc_poly_t p;
    p.c = 0.0; p.b = 0.0; p.a = 0.0; p.valid = FALSE;
    return p;
}

static calc_poly_t calc_poly_make(double c, double b, double a) {
    calc_poly_t p;
    p.c = c; p.b = b; p.a = a; p.valid = TRUE;
    return p;
}

static calc_poly_t calc_poly_const(double c) { return calc_poly_make(c, 0.0, 0.0); }
static calc_poly_t calc_poly_var(void) { return calc_poly_make(0.0, 1.0, 0.0); }

static int calc_poly_degree(calc_poly_t p) {
    if (!p.valid) return -1;
    if (!calc_near_zero(p.a)) return 2;
    if (!calc_near_zero(p.b)) return 1;
    if (!calc_near_zero(p.c)) return 0;
    return 0;
}

static calc_poly_t calc_poly_add(calc_poly_t x, calc_poly_t y) {
    if (!x.valid || !y.valid) return calc_poly_invalid();
    return calc_poly_make(x.c + y.c, x.b + y.b, x.a + y.a);
}
static calc_poly_t calc_poly_sub(calc_poly_t x, calc_poly_t y) {
    if (!x.valid || !y.valid) return calc_poly_invalid();
    return calc_poly_make(x.c - y.c, x.b - y.b, x.a - y.a);
}
static calc_poly_t calc_poly_scale(calc_poly_t x, double k) {
    if (!x.valid) return calc_poly_invalid();
    return calc_poly_make(x.c * k, x.b * k, x.a * k);
}

static calc_poly_t calc_poly_mul(calc_poly_t x, calc_poly_t y) {
    if (!x.valid || !y.valid) return calc_poly_invalid();
    double deg3 = x.a * y.b + x.b * y.a;
    double deg4 = x.a * y.a;
    if (!calc_near_zero(deg3) || !calc_near_zero(deg4)) return calc_poly_invalid();
    double c = x.c * y.c;
    double b = x.c * y.b + x.b * y.c;
    double a = x.c * y.a + x.b * y.b + x.a * y.c;
    return calc_poly_make(c, b, a);
}

static calc_poly_t calc_poly_div_scalar(calc_poly_t x, double d) {
    if (!x.valid || calc_near_zero(d)) return calc_poly_invalid();
    return calc_poly_make(x.c / d, x.b / d, x.a / d);
}

static calc_poly_t calc_poly_pow_int(calc_poly_t x, int exp) {
    if (!x.valid || exp < 0) return calc_poly_invalid();
    if (exp == 0) return calc_poly_const(1.0);
    if (exp == 1) return x;
    if (exp == 2) return calc_poly_mul(x, x);
    return calc_poly_invalid();
}

static void calc_poly_trim_text(char* s) {
    if (!s) return;
    int len = slen(s);
    while (len > 0 && calc_is_space(s[len - 1])) s[--len] = 0;
}

static void calc_poly_append_coeff(char* dst, int cap, double coeff, const char* suffix, bool first_term) {
    if (!dst || cap <= 0 || calc_near_zero(coeff)) return;
    if (!suffix) suffix = "";
    bool negative = coeff < 0.0;
    double abs_coeff = negative ? -coeff : coeff;

    if (!first_term) {
        calc_append_text(dst, cap, negative ? " - " : " + ");
    } else if (negative) {
        calc_append_text(dst, cap, "-");
    }

    bool omit_coeff = (abs_coeff > 0.9999999 && abs_coeff < 1.0000001 && suffix[0] != 0);
    if (!omit_coeff) {
        char num[48];
        calc_num_to_text(abs_coeff, num, (int)sizeof(num));
        calc_append_text(dst, cap, num);
    }
    calc_append_text(dst, cap, suffix);
}

static void calc_poly_to_text(calc_poly_t p, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (!p.valid) { calc_copy_text(out, out_sz, "Invalid"); return; }
    bool first = TRUE;
    calc_poly_append_coeff(out, out_sz, p.a, "x^2", first); if (!calc_near_zero(p.a)) first = FALSE;
    calc_poly_append_coeff(out, out_sz, p.b, "x", first); if (!calc_near_zero(p.b)) first = FALSE;
    if (!calc_near_zero(p.c) || first) {
        char num[48];
        calc_num_to_text(p.c, num, (int)sizeof(num));
        if (!first) {
            if (p.c < 0.0) {
                calc_append_text(out, out_sz, " - ");
                calc_num_to_text(-p.c, num, (int)sizeof(num));
            } else {
                calc_append_text(out, out_sz, " + ");
            }
        }
        calc_append_text(out, out_sz, num);
    }
}

static void calc_poly_format_equation(calc_poly_t p, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    calc_poly_to_text(p, out, out_sz);
    calc_append_text(out, out_sz, " = 0");
}

static void calc_poly_format_linear_step(double a, double b, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    char abuf[48], bbuf[48];
    calc_num_to_text(a, abuf, (int)sizeof(abuf));
    calc_num_to_text(calc_absd(b), bbuf, (int)sizeof(bbuf));
    calc_append_text(out, out_sz, abuf);
    calc_append_text(out, out_sz, "x");
    calc_append_text(out, out_sz, (b < 0.0) ? " - " : " + ");
    calc_append_text(out, out_sz, bbuf);
    calc_append_text(out, out_sz, " = 0");
}

static bool calc_poly_starts_primary(const char** p) {
    if (!p || !*p) return FALSE;
    const char* q = *p;
    while (*q && calc_is_space(*q)) q++;
    if (*q == '(' || *q == 'x' || *q == 'X' || calc_is_digit(*q) || *q == '.') return TRUE;
    return FALSE;
}

static calc_poly_t calc_poly_parse_expr(const char** p, bool* ok);

static calc_poly_t calc_poly_parse_primary(const char** p, bool* ok) {
    calc_skip_ws(p);
    if (**p == '(') {
        (*p)++;
        calc_poly_t inner = calc_poly_parse_expr(p, ok);
        calc_skip_ws(p);
        if (**p == ')') (*p)++; else *ok = FALSE;
        return inner;
    }
    if (**p == 'x' || **p == 'X') {
        (*p)++;
        return calc_poly_var();
    }
    char numbuf[32];
    double value = 0.0;
    if (calc_read_number(p, numbuf, (int)sizeof(numbuf), &value)) {
        return calc_poly_const(value);
    }
    *ok = FALSE;
    return calc_poly_invalid();
}

static calc_poly_t calc_poly_parse_power(const char** p, bool* ok) {
    calc_poly_t base = calc_poly_parse_primary(p, ok);
    if (!*ok) return calc_poly_invalid();
    calc_skip_ws(p);
    if (**p == '^') {
        (*p)++;
        calc_skip_ws(p);
        double expv = 0.0;
        char expbuf[16];
        if (!calc_read_number(p, expbuf, (int)sizeof(expbuf), &expv)) { *ok = FALSE; return calc_poly_invalid(); }
        if (!calc_near_int(expv)) { *ok = FALSE; return calc_poly_invalid(); }
        int expi = (int)calc_round_nearest(expv);
        return calc_poly_pow_int(base, expi);
    }
    return base;
}

static calc_poly_t calc_poly_parse_unary(const char** p, bool* ok) {
    calc_skip_ws(p);
    if (**p == '+') { (*p)++; return calc_poly_parse_unary(p, ok); }
    if (**p == '-') { (*p)++; return calc_poly_scale(calc_poly_parse_unary(p, ok), -1.0); }
    return calc_poly_parse_power(p, ok);
}

static calc_poly_t calc_poly_parse_term(const char** p, bool* ok) {
    calc_poly_t left = calc_poly_parse_unary(p, ok);
    if (!*ok) return calc_poly_invalid();
    for (;;) {
        calc_skip_ws(p);
        if (**p == '*' ) {
            (*p)++;
            calc_poly_t right = calc_poly_parse_unary(p, ok);
            left = calc_poly_mul(left, right);
        } else if (**p == '/') {
            (*p)++;
            calc_poly_t right = calc_poly_parse_unary(p, ok);
            if (!right.valid) { *ok = FALSE; return calc_poly_invalid(); }
            if (!calc_near_zero(right.a) || !calc_near_zero(right.b) || calc_near_zero(right.c)) { *ok = FALSE; return calc_poly_invalid(); }
            left = calc_poly_div_scalar(left, right.c);
        } else if (calc_poly_starts_primary(p)) {
             /* implicit multiplication: 2x, 3(x+1), (x+1)(x-1) */
            calc_poly_t right = calc_poly_parse_unary(p, ok);
            left = calc_poly_mul(left, right);
        } else {
            break;
        }
        if (!*ok || !left.valid) { *ok = FALSE; return calc_poly_invalid(); }
    }
    return left;
}

static calc_poly_t calc_poly_parse_expr(const char** p, bool* ok) {
    calc_poly_t left = calc_poly_parse_term(p, ok);
    if (!*ok) return calc_poly_invalid();
    for (;;) {
        calc_skip_ws(p);
        if (**p == '+') {
            (*p)++;
            calc_poly_t right = calc_poly_parse_term(p, ok);
            left = calc_poly_add(left, right);
        } else if (**p == '-') {
            (*p)++;
            calc_poly_t right = calc_poly_parse_term(p, ok);
            left = calc_poly_sub(left, right);
        } else {
            break;
        }
        if (!*ok || !left.valid) { *ok = FALSE; return calc_poly_invalid(); }
    }
    return left;
}

static bool calc_find_top_level_equal(const char* expr, int* eq_pos) {
    if (!expr || !eq_pos) return FALSE;
    int depth = 0;
    for (int i = 0; expr[i]; ++i) {
        char c = expr[i];
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if (c == '=' && depth == 0) { *eq_pos = i; return TRUE; }
    }
    return FALSE;
}

static bool calc_contains_x(const char* expr) {
    if (!expr) return FALSE;
    for (int i = 0; expr[i]; ++i) if (expr[i] == 'x' || expr[i] == 'X') return TRUE;
    return FALSE;
}

static bool calc_eval_hs3_symbolic(window_t* w, const char* expr) {
    if (!w || !expr || !expr[0] || !calc_contains_x(expr)) return FALSE;

    int eq_pos = -1;
    char left_txt[256], right_txt[256];
    if (calc_find_top_level_equal(expr, &eq_pos)) {
        int li = 0, ri = 0;
        for (int i = 0; i < eq_pos && li < (int)sizeof(left_txt) - 1; ++i) left_txt[li++] = expr[i];
        left_txt[li] = 0;
        for (int i = eq_pos + 1; expr[i] && ri < (int)sizeof(right_txt) - 1; ++i) right_txt[ri++] = expr[i];
        right_txt[ri] = 0;
        calc_poly_trim_text(left_txt);
        calc_poly_trim_text(right_txt);

        bool ok = TRUE;
        const char* lp = left_txt;
        const char* rp = right_txt;
        calc_poly_t lhs = calc_poly_parse_expr(&lp, &ok);
        calc_poly_t rhs = calc_poly_parse_expr(&rp, &ok);
        calc_skip_ws(&lp); calc_skip_ws(&rp);
        if (!ok || !lhs.valid || !rhs.valid || (lp && *lp) || (rp && *rp)) return FALSE;

        calc_poly_t eq = calc_poly_sub(lhs, rhs);
        int deg = calc_poly_degree(eq);
        char steps[1024];
        steps[0] = 0;
        calc_append_line(steps, (int)sizeof(steps), "HS3 equation solver:");
        {
            char line[256];
            line[0] = 0;
            calc_append_text(line, 256, left_txt);
            calc_append_text(line, 256, " = ");
            calc_append_text(line, 256, right_txt);
            calc_append_line(steps, (int)sizeof(steps), line);
        }

        char eqbuf[256];
        calc_poly_format_equation(eq, eqbuf, (int)sizeof(eqbuf));
        calc_append_line(steps, (int)sizeof(steps), "Move all terms to one side:");
        calc_append_line(steps, (int)sizeof(steps), eqbuf);

        if (deg < 0) return FALSE;
        if (deg == 0) {
            if (calc_near_zero(eq.c)) {
                calc_append_line(steps, (int)sizeof(steps), "Identity: infinitely many solutions.");
                calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "All real numbers");
                calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
                calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Identity");
            } else {
                calc_append_line(steps, (int)sizeof(steps), "Contradiction: no solution.");
                calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "No solution");
                calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
                calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Contradiction");
            }
            w->calc_clear_next = TRUE;
            return TRUE;
        }

        if (deg == 1) {
            double a = eq.b;
            double b = eq.c;
            if (calc_near_zero(a)) return FALSE;
            char line1[256]={0}, line2[256]={0}, line3[256]={0}, xbuf[64]={0};
            calc_append_line(steps, (int)sizeof(steps), "Linear equation:");
            calc_poly_format_linear_step(a, b, line1, (int)sizeof(line1));
            calc_append_line(steps, (int)sizeof(steps), line1);
            calc_append_line(steps, (int)sizeof(steps), "x = -b / a");
            double x = -b / a;
            calc_num_to_text(x, xbuf, (int)sizeof(xbuf));
            calc_append_text(line2, (int)sizeof(line2), "x = -(");
            char btxt[48], atxt[48];
            calc_num_to_text(b, btxt, (int)sizeof(btxt));
            calc_num_to_text(a, atxt, (int)sizeof(atxt));
            calc_append_text(line2, (int)sizeof(line2), btxt);
            calc_append_text(line2, (int)sizeof(line2), ") / ");
            calc_append_text(line2, (int)sizeof(line2), atxt);
            calc_append_line(steps, (int)sizeof(steps), line2);
            calc_append_text(line3, (int)sizeof(line3), "x = ");
            calc_append_text(line3, (int)sizeof(line3), xbuf);
            calc_append_line(steps, (int)sizeof(steps), line3);
            calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), xbuf);
            calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Solved");
            w->calc_clear_next = TRUE;
            return TRUE;
        }

        if (deg == 2) {
            double a = eq.a, b = eq.b, c = eq.c;
            if (calc_near_zero(a)) return FALSE;
            double D = b * b - 4.0 * a * c;
            char abuf[48], bbuf[48], cbuf[48], dbuf[64];
            calc_num_to_text(a, abuf, (int)sizeof(abuf));
            calc_num_to_text(b, bbuf, (int)sizeof(bbuf));
            calc_num_to_text(c, cbuf, (int)sizeof(cbuf));
            calc_num_to_text(D, dbuf, (int)sizeof(dbuf));
            calc_append_line(steps, (int)sizeof(steps), "Quadratic equation:");
            calc_append_line(steps, (int)sizeof(steps), "ax^2 + bx + c = 0");
            {
                char line[256];
                line[0] = 0;
                calc_append_text(line, 256, "a = "); calc_append_text(line, 256, abuf);
                calc_append_text(line, 256, ", b = "); calc_append_text(line, 256, bbuf);
                calc_append_text(line, 256, ", c = "); calc_append_text(line, 256, cbuf);
                calc_append_line(steps, (int)sizeof(steps), line);
            }
            {
                char line[256];
                line[0] = 0;
                calc_append_text(line, 256, "D = b^2 - 4ac = ");
                calc_append_text(line, 256, dbuf);
                calc_append_line(steps, (int)sizeof(steps), line);
            }
            if (D >= 0.0) {
                double sD = calc_sqrt_approx(D);
                double x1 = (-b + sD) / (2.0 * a);
                double x2 = (-b - sD) / (2.0 * a);
                char sdbuf[64], x1buf[64], x2buf[64];
                calc_num_to_text(sD, sdbuf, (int)sizeof(sdbuf));
                calc_num_to_text(x1, x1buf, (int)sizeof(x1buf));
                calc_num_to_text(x2, x2buf, (int)sizeof(x2buf));
                calc_append_line(steps, (int)sizeof(steps), "x = (-b ± sqrt(D)) / (2a)");
                {
                    char line[256];
                    line[0] = 0;
                    calc_append_text(line, 256, "sqrt(D) = ");
                    calc_append_text(line, 256, sdbuf);
                    calc_append_line(steps, (int)sizeof(steps), line);
                }
                {
                    char line[256];
                    line[0] = 0;
                    calc_append_text(line, 256, "x1 = "); calc_append_text(line, 256, x1buf);
                    calc_append_text(line, 256, ", x2 = "); calc_append_text(line, 256, x2buf);
                    calc_append_line(steps, (int)sizeof(steps), line);
                }
                {
                    char disp[128];
                    disp[0] = 0;
                    calc_append_text(disp, 128, "x1="); calc_append_text(disp, 128, x1buf);
                    calc_append_text(disp, 128, " x2="); calc_append_text(disp, 128, x2buf);
                    calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), disp);
                }
                calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
                calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), (D == 0.0) ? "One real root" : "Two real roots");
                w->calc_clear_next = TRUE;
                return TRUE;
            } else {
                double sD = calc_sqrt_approx(-D);
                double real = -b / (2.0 * a);
                double imag = sD / (2.0 * a);
                char realbuf[64], imagbuf[64];
                calc_num_to_text(real, realbuf, (int)sizeof(realbuf));
                calc_num_to_text(calc_absd(imag), imagbuf, (int)sizeof(imagbuf));
                calc_append_line(steps, (int)sizeof(steps), "D < 0, so the roots are complex.");
                calc_append_line(steps, (int)sizeof(steps), "x = (-b ± i*sqrt(-D)) / (2a)");
                {
                    char line[256];
                    line[0] = 0;
                    calc_append_text(line, 256, "x = ");
                    calc_append_text(line, 256, realbuf);
                    calc_append_text(line, 256, " ± ");
                    calc_append_text(line, 256, imagbuf);
                    calc_append_text(line, 256, "i");
                    calc_append_line(steps, (int)sizeof(steps), line);
                }
                calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "Complex roots");
                calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
                calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Complex");
                w->calc_clear_next = TRUE;
                return TRUE;
            }
        }
    }

    bool ok = TRUE;
    const char* p = expr;
    calc_poly_t poly = calc_poly_parse_expr(&p, &ok);
    calc_skip_ws(&p);
    if (!ok || !poly.valid || (p && *p)) return FALSE;

    char steps[1024];
    steps[0] = 0;
    calc_append_line(steps, (int)sizeof(steps), "HS3 symbolic simplification:");
    {
        char line[256];
        calc_poly_to_text(poly, line, (int)sizeof(line));
        calc_append_line(steps, (int)sizeof(steps), expr);
        calc_append_line(steps, (int)sizeof(steps), "→");
        calc_append_line(steps, (int)sizeof(steps), line);
        calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), line);
        calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Simplified");
        w->calc_clear_next = TRUE;
        return TRUE;
    }
}

static int calc_study_find_topic(const char* token);
static void calc_study_apply_topic(window_t* w, int topic_idx, int load_mode);
static void calc_eval_number_text(const char* text, bool deg_mode, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (!text) return;
    calc_eval_ctx_t ctx = {0};
    char steps[256];
    steps[0] = 0;
    ctx.steps = steps;
    ctx.steps_cap = (int)sizeof(steps);
    ctx.steps_len = 0;
    ctx.deg_mode = deg_mode;
    ctx.ok = TRUE;
    const char* p = text;
    double value = calc_parse_expr(&ctx, &p);
    calc_skip_ws(&p);
    if (!ctx.ok || (p && *p)) {
        calc_copy_text(out, out_sz, "Error");
        return;
    }
    calc_num_to_text(value, out, out_sz);
}

static bool calc_try_eval_special_ui(window_t* w) {
    if (!w || !w->calc_expr[0]) return false;
    char expr[512];
    calc_copy_text(expr, (int)sizeof(expr), w->calc_expr);
    calc_trim(expr);
    if (!expr[0]) return false;

    const char* p = expr;
    char ident[24];
    if (!calc_read_ident(&p, ident, 24)) return false;
    calc_skip_ws(&p);
    if (*p != '(') return false;

    char display[160], status[96], steps[4096];
    display[0] = 0; status[0] = 0; steps[0] = 0;
    bool ok = false;
    if (smatch(ident, "diff") || smatch(ident, "deriv") || smatch(ident, "d")) {
        char inside[512];
        const char* q = p;
        if (calc_extract_raw_call(&q, inside, (int)sizeof(inside))) ok = calc_eval_diff_raw(inside, w->calc_angle_deg, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "int") || smatch(ident, "integral") || smatch(ident, "integrate")) {
        char inside[512];
        const char* q = p;
        if (calc_extract_raw_call(&q, inside, (int)sizeof(inside))) ok = calc_eval_integral_raw(inside, w->calc_angle_deg, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "solve") || smatch(ident, "root") || smatch(ident, "nsolve")) {
        char inside[512];
        const char* q = p;
        if (calc_extract_raw_call(&q, inside, (int)sizeof(inside))) ok = calc_eval_solve_raw(inside, w->calc_angle_deg, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    } else if (smatch(ident, "table") || smatch(ident, "tabulate")) {
        char inside[512];
        const char* q = p;
        if (calc_extract_raw_call(&q, inside, (int)sizeof(inside))) ok = calc_eval_table_raw(inside, w->calc_angle_deg, display, (int)sizeof(display), status, (int)sizeof(status), steps, (int)sizeof(steps));
    }

    if (!ok) return false;
    calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), display);
    calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), status);
    calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), steps);
    calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), display);
    w->calc_clear_next = TRUE;
    w->calc_op = 0;
    return true;
}

static void calc_eval_expression(window_t* w) {
    if (!w) return;
    calc_trim(w->calc_expr);
    if (!w->calc_expr[0]) return;

    if (calc_try_eval_special_ui(w)) return;

    calc_school_result_t school = {0};
    if (calc_engine_evaluate(w->calc_expr, w->calc_angle_deg, &school)) {
        calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), school.display);
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), school.status);
        calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), school.steps);
        calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), school.display);
        w->calc_clear_next = TRUE;
        w->calc_op = 0;
        return;
    }

    if (w->calc_mode == 3 && calc_eval_simple_vertical(w, w->calc_expr)) {
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Hissan");
        return;
    }

    if (w->calc_mode == 2 && calc_eval_hs3_symbolic(w, w->calc_expr)) {
        return;
    }

    calc_eval_ctx_t ctx = {0};
    char steps[1024];
    steps[0] = 0;
    ctx.steps = steps;
    ctx.steps_cap = (int)sizeof(steps);
    ctx.steps_len = 0;
    ctx.deg_mode = w->calc_angle_deg;
    ctx.ok = TRUE;

    const char* p = w->calc_expr;
    double result = calc_parse_expr(&ctx, &p);
    calc_skip_ws(&p);
    if (!ctx.ok || (p && *p)) {
        calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "Error");
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Parse error");
        calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), ctx.steps[0] ? ctx.steps : "Parse error");
        w->calc_clear_next = TRUE;
        return;
    }

    calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), ctx.steps);
    calc_num_to_text(result, w->calc_display, (int)sizeof(w->calc_display));
    if (w->calc_mode == 1) {
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), w->calc_angle_deg ? "Deg" : "Rad");
    } else if (w->calc_mode == 2) {
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study");
    } else {
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Ready");
    }
    calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), w->calc_display);
    w->calc_clear_next = TRUE;
    w->calc_op = 0;
    w->calc_acc = result;
}

static void calc_apply_token(window_t* w, const char* token) {
    if (!w || !token || !token[0]) return;

    if (smatch(token, "C")) {
        calc_expr_clear(w);
        if (w->calc_mode == 3) {
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Hissan");
        } else if (w->calc_mode == 2) {
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study");
        } else if (w->calc_mode == 1) {
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), w->calc_angle_deg ? "Deg" : "Rad");
        }
        return;
    }

    if (smatch(token, "CE") || smatch(token, "<-")) { calc_expr_backspace(w); return; }
    if (smatch(token, "=")) { calc_eval_expression(w); return; }

    if (smatch(token, "ANS")) {
        char ans_buf[64];
        calc_num_to_text(w->calc_acc, ans_buf, (int)sizeof(ans_buf) - 1);
        calc_expr_append(w, ans_buf);
        w->calc_clear_next = FALSE;
        return;
    }
    if (smatch(token, "HISSAN")) {
        w->calc_mode = 3;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Hissan mode");
        return;
    }
    if (smatch(token, "REPL") || smatch(token, "SIMPLIFY")) {
        w->calc_mode = 2;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study");
        calc_study_apply_topic(w, w->calc_topic_idx, 1);
        return;
    }
    if (smatch(token, "EXAM") || smatch(token, "SOLVE")) {
        w->calc_mode = 2;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study");
        calc_study_apply_topic(w, w->calc_topic_idx, 2);
        return;
    }
    if (smatch(token, "CLR")) {
        calc_expr_clear(w);
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study cleared");
        return;
    }
    if (smatch(token, "GRAPH")) {
        calc_open_graph_window(w);
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Graph window");
        return;
    }

    int topic_idx = calc_study_find_topic(token);
    if (topic_idx >= 0) {
        w->calc_mode = 2;
        calc_study_apply_topic(w, topic_idx, 0);
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Study");
        return;
    }

    if (smatch(token, "DEG")) { w->calc_angle_deg = TRUE; calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Degrees"); return; }
    if (smatch(token, "RAD")) { w->calc_angle_deg = FALSE; calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Radians"); return; }
    if (smatch(token, "MC")) { w->calc_mem = 0; calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Memory cleared"); return; }
    if (smatch(token, "MR")) { calc_num_to_text(w->calc_mem, w->calc_display, (int)sizeof(w->calc_display)); calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), w->calc_display); w->calc_clear_next = TRUE; return; }
    if (smatch(token, "MS")) { w->calc_mem = calc_strtodbl(w->calc_display); calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Memory stored"); return; }
    if (smatch(token, "M+")) { w->calc_mem += calc_strtodbl(w->calc_display); calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Memory +"); return; }
    if (smatch(token, "M-")) { w->calc_mem -= calc_strtodbl(w->calc_display); calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Memory -"); return; }

    if (smatch(token, "+/-")) {
        double v = calc_strtodbl(w->calc_display[0] ? w->calc_display : w->calc_expr);
        if (calc_absd(v) > 0.0000001 || (w->calc_display[0] == '0' && w->calc_display[1] == 0)) {
            v = -v;
            calc_num_to_text(v, w->calc_display, (int)sizeof(w->calc_display));
            calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), w->calc_display);
            w->calc_clear_next = TRUE;
        }
        return;
    }

    if (smatch(token, "1/x")) {
        double v = calc_strtodbl(w->calc_display);
        if (calc_absd(v) > 0.0000001 || w->calc_display[0] == '0') {
            calc_num_to_text(1.0 / v, w->calc_display, (int)sizeof(w->calc_display));
            calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), w->calc_display);
        } else {
            calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), "Error");
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Division by zero");
        }
        w->calc_clear_next = TRUE;
        return;
    }

    if (smatch(token, "sin") || smatch(token, "cos") || smatch(token, "tan") ||
        smatch(token, "asin") || smatch(token, "acos") || smatch(token, "atan") ||
        smatch(token, "sqrt") || smatch(token, "cbrt") || smatch(token, "floor") ||
        smatch(token, "ceil") || smatch(token, "round") || smatch(token, "sign") ||
        smatch(token, "sgn") || smatch(token, "log") || smatch(token, "ln") ||
        smatch(token, "abs") || smatch(token, "exp") || smatch(token, "nCr") ||
        smatch(token, "nPr") || smatch(token, "deg") || smatch(token, "rad") ||
        smatch(token, "fact") || smatch(token, "factorial") || smatch(token, "pow") ||
        smatch(token, "root") || smatch(token, "max") || smatch(token, "min")) {
        calc_expr_append(w, token);
        calc_expr_append(w, "(");
        return;
    }

    if (smatch(token, "pi") || smatch(token, "e") || smatch(token, "!") || smatch(token, "%") || smatch(token, "(") || smatch(token, ")") || smatch(token, ",")) {
        calc_expr_append(w, token);
        return;
    }
    if (smatch(token, "x^2")) { calc_expr_append(w, "^2"); return; }
    if (smatch(token, "x^y")) { calc_expr_append(w, "^"); return; }

    if (token[0] >= '0' && token[0] <= '9' && token[1] == 0) {
        if (w->calc_clear_next && w->calc_expr[0] == 0) w->calc_expr[0] = 0;
        calc_expr_append(w, token);
        w->calc_display[0] = 0;
        w->calc_clear_next = FALSE;
        return;
    }
    if (token[0] == '.' && token[1] == 0) { calc_expr_append(w, token); w->calc_clear_next = FALSE; return; }
    if (token[0] == '+' || token[0] == '-' || token[0] == '*' || token[0] == '/' || token[0] == '^') { calc_expr_append(w, token); return; }
}

static bool calc_eval_text_with_x(const char* text, double x_value, bool deg_mode, double* out_value, char* steps, int steps_sz, bool record_steps) {
    if (!text || !out_value) return FALSE;
    calc_eval_ctx_t ctx = {0};
    ctx.deg_mode = deg_mode;
    ctx.ok = TRUE;
    ctx.use_x = TRUE;
    ctx.x_value = x_value;
    if (record_steps && steps && steps_sz > 0) {
        steps[0] = 0;
        ctx.steps = steps;
        ctx.steps_cap = steps_sz;
        ctx.steps_len = 0;
    }
    const char* p = text;
    double value = calc_parse_expr(&ctx, &p);
    calc_skip_ws(&p);
    if (!ctx.ok || (p && *p)) return FALSE;
    *out_value = value;
    return TRUE;
}

static void calc_graph_build_label(const char* expr, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (!expr) return;
    int eq_pos = -1;
    if (calc_find_top_level_equal(expr, &eq_pos)) {
        char left[256], right[256];
        int li = 0, ri = 0;
        for (int i = 0; i < eq_pos && li < (int)sizeof(left) - 1; ++i) left[li++] = expr[i];
        left[li] = 0;
        for (int i = eq_pos + 1; expr[i] && ri < (int)sizeof(right) - 1; ++i) right[ri++] = expr[i];
        right[ri] = 0;
        calc_poly_trim_text(left);
        calc_poly_trim_text(right);
        calc_copy_text(out, out_sz, "Graph: ");
        calc_append_text(out, out_sz, left);
        calc_append_text(out, out_sz, " = ");
        calc_append_text(out, out_sz, right);
    } else {
        calc_copy_text(out, out_sz, "Graph: ");
        calc_append_text(out, out_sz, expr);
    }
}

static bool calc_graph_eval_y(const char* expr, double x, bool deg_mode, double* y_out, char* steps, int steps_sz, bool record_steps) {
    if (!expr || !expr[0] || !y_out) return FALSE;
    int eq_pos = -1;
    if (calc_find_top_level_equal(expr, &eq_pos)) {
        char left[256], right[256];
        int li = 0, ri = 0;
        for (int i = 0; i < eq_pos && li < (int)sizeof(left) - 1; ++i) left[li++] = expr[i];
        left[li] = 0;
        for (int i = eq_pos + 1; expr[i] && ri < (int)sizeof(right) - 1; ++i) right[ri++] = expr[i];
        right[ri] = 0;
        calc_poly_trim_text(left);
        calc_poly_trim_text(right);
        double lv = 0.0, rv = 0.0;
        if (!calc_eval_text_with_x(left, x, deg_mode, &lv, steps, steps_sz, record_steps)) return FALSE;
        if (!calc_eval_text_with_x(right, x, deg_mode, &rv, steps, steps_sz, FALSE)) return FALSE;
        *y_out = lv - rv;
        return TRUE;
    }
    return calc_eval_text_with_x(expr, x, deg_mode, y_out, steps, steps_sz, record_steps);
}

static void calc_open_graph_window(window_t* source) {
    if (!source) return;
    char expr[256];
    expr[0] = 0;
    if (source->calc_expr[0]) calc_copy_text(expr, (int)sizeof(expr), source->calc_expr);
    else if (source->calc_display[0]) calc_copy_text(expr, (int)sizeof(expr), source->calc_display);
    else calc_copy_text(expr, (int)sizeof(expr), "x^2");
    calc_trim(expr);
    if (!expr[0]) calc_copy_text(expr, (int)sizeof(expr), "x^2");

    int existing = gui_find_window(WIN_CALC_GRAPH);
    window_t* gw = NULL;
    if (existing >= 0) {
        gw = &windows[existing];
    } else {
        gw = gui_open_window(WIN_CALC_GRAPH, gui_text("Graph", "グラフ"), 180, 90, 760, 520);
        if (!gw) return;
    }
    calc_copy_text(gw->calc_expr, (int)sizeof(gw->calc_expr), expr);
    calc_copy_text(gw->calc_status, (int)sizeof(gw->calc_status), "Plot");
    calc_copy_text(gw->calc_display, (int)sizeof(gw->calc_display), "Graph");
    calc_copy_text(gw->calc_steps, (int)sizeof(gw->calc_steps), "Separate graph window");
    gw->calc_angle_deg = source->calc_angle_deg;
    gw->calc_mode = 2;
    gw->calc_initialized = TRUE;
    gw->calc_clear_next = TRUE;
    if (existing >= 0) gui_bring_to_front(existing);
}

void draw_calc_graph(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    vga_fill_rect(cx, cy, cw, ch, rgb(245, 247, 250));

    const char* expr = w->calc_expr[0] ? w->calc_expr : "x^2";
    char label[320];
    calc_graph_build_label(expr, label, (int)sizeof(label));
    vga_draw_string(cx + 12, cy + 8, label, C_ACCENT, 0xFFFFFFFF);
    vga_draw_string(cx + 12, cy + 24, w->calc_angle_deg ? "Angle: DEG" : "Angle: RAD", C_TEXT, 0xFFFFFFFF);

    int plot_x = cx + 42;
    int plot_y = cy + 52;
    int plot_w = cw - 62;
    int plot_h = ch - 74;
    if (plot_w < 80 || plot_h < 80) {
        vga_draw_string(cx + 12, cy + 52, "Window too small for graph.", C_TEXT, 0xFFFFFFFF);
        return;
    }

    vga_fill_rect(plot_x, plot_y, plot_w, plot_h, rgb(255, 255, 255));
    vga_draw_rect(plot_x, plot_y, plot_w, plot_h, rgb(160, 170, 182));

    const int samples = 160;
    double xs[samples];
    double ys[samples];
    bool ok[samples];
    double x_min = -10.0, x_max = 10.0;
    double y_min = 0.0, y_max = 0.0;
    bool any = FALSE;

    for (int i = 0; i < samples; ++i) {
        double t = (samples == 1) ? 0.0 : (double)i / (double)(samples - 1);
        double x = x_min + (x_max - x_min) * t;
        double y = 0.0;
        char tmp_steps[16];
        if (calc_graph_eval_y(expr, x, w->calc_angle_deg, &y, tmp_steps, (int)sizeof(tmp_steps), FALSE) &&
            y == y && y > -1e9 && y < 1e9) {
            xs[i] = x;
            ys[i] = y;
            ok[i] = TRUE;
            if (!any) {
                y_min = y_max = y;
                any = TRUE;
            } else {
                if (y < y_min) y_min = y;
                if (y > y_max) y_max = y;
            }
        } else {
            ok[i] = FALSE;
        }
    }

    if (!any) {
        vga_draw_string(cx + 12, cy + 72, "Cannot plot this expression.", C_TEXT, 0xFFFFFFFF);
        return;
    }

    if (calc_absd(y_max - y_min) < 0.0001) {
        double pad = (calc_absd(y_max) < 1.0) ? 1.0 : calc_absd(y_max) * 0.25;
        y_min -= pad;
        y_max += pad;
    } else {
        double pad = (y_max - y_min) * 0.1;
        y_min -= pad;
        y_max += pad;
    }

    // grid
    for (int i = 0; i <= 10; ++i) {
        int gx = plot_x + (plot_w * i) / 10;
        vga_draw_line(gx, plot_y, gx, plot_y + plot_h - 1, rgb(235, 239, 244));
    }
    for (int i = 0; i <= 8; ++i) {
        int gy = plot_y + (plot_h * i) / 8;
        vga_draw_line(plot_x, gy, plot_x + plot_w - 1, gy, rgb(235, 239, 244));
    }

    // axes
    if (x_min <= 0.0 && x_max >= 0.0) {
        int zx = plot_x + (int)((0.0 - x_min) * (double)(plot_w - 1) / (x_max - x_min));
        vga_draw_line(zx, plot_y, zx, plot_y + plot_h - 1, rgb(150, 150, 160));
    }
    if (y_min <= 0.0 && y_max >= 0.0) {
        int zy = plot_y + plot_h - 1 - (int)((0.0 - y_min) * (double)(plot_h - 1) / (y_max - y_min));
        vga_draw_line(plot_x, zy, plot_x + plot_w - 1, zy, rgb(150, 150, 160));
    }

    // curve
    int prev_x = 0, prev_y = 0;
    bool prev_ok = FALSE;
    for (int i = 0; i < samples; ++i) {
        if (!ok[i]) { prev_ok = FALSE; continue; }
        int sx = plot_x + (int)((xs[i] - x_min) * (double)(plot_w - 1) / (x_max - x_min));
        int sy = plot_y + plot_h - 1 - (int)((ys[i] - y_min) * (double)(plot_h - 1) / (y_max - y_min));
        if (prev_ok) {
            int dy = sy - prev_y;
            if (dy < 80 && dy > -80) {
                vga_draw_line(prev_x, prev_y, sx, sy, C_ACCENT);
            }
        }
        prev_x = sx;
        prev_y = sy;
        prev_ok = TRUE;
    }

    // Labels
    char xmin[32], xmax[32], ymin[32], ymax[32];
    calc_num_to_text(x_min, xmin, (int)sizeof(xmin));
    calc_num_to_text(x_max, xmax, (int)sizeof(xmax));
    calc_num_to_text(y_min, ymin, (int)sizeof(ymin));
    calc_num_to_text(y_max, ymax, (int)sizeof(ymax));
    vga_draw_string(plot_x, plot_y + plot_h + 2, xmin, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(plot_x + plot_w - slen(xmax) * FONT_W, plot_y + plot_h + 2, xmax, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(plot_x - (int)(slen(ymax) * FONT_W) - 6, plot_y, ymax, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(plot_x - (int)(slen(ymin) * FONT_W) - 6, plot_y + plot_h - FONT_H, ymin, C_TEXT, 0xFFFFFFFF);
}


// ============================================================
// Spreadsheet app - lightweight Excel-like grid
// ============================================================

#define SHEET_ROWS 24
#define SHEET_COLS 10
#define SHEET_CELL_W 84
#define SHEET_CELL_H 24
#define SHEET_HEADER_W 44
#define SHEET_HEADER_H 24

static void sheet_col_label(int col, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    if (col < 0) { calc_copy_text(out, out_sz, "?"); return; }
    char tmp[8];
    int pos = 0;
    int v = col;
    do {
        tmp[pos++] = (char)('A' + (v % 26));
        v = (v / 26) - 1;
    } while (v >= 0 && pos < (int)sizeof(tmp));
    int i = 0;
    while (pos > 0 && i < out_sz - 1) out[i++] = tmp[--pos];
    out[i] = 0;
}

static bool sheet_parse_ref(const char* text, int* row, int* col, int* consumed) {
    if (!text || !row || !col || !consumed) return FALSE;
    int c = 0, r = 0, i = 0;
    bool saw_col = FALSE, saw_row = FALSE;
    while (text[i] && calc_is_alpha(text[i])) {
        saw_col = TRUE;
        c = c * 26 + (int)((((unsigned char)text[i] >= 'a' && (unsigned char)text[i] <= 'z') ? (int)((unsigned char)text[i] - 'a' + 'A') : (int)(unsigned char)text[i]) - 'A' + 1);
        i++;
    }
    while (text[i] && calc_is_digit(text[i])) {
        saw_row = TRUE;
        r = r * 10 + (int)(text[i] - '0');
        i++;
    }
    if (!saw_col || !saw_row) return FALSE;
    c -= 1;
    r -= 1;
    if (c < 0 || c >= SHEET_COLS || r < 0 || r >= SHEET_ROWS) return FALSE;
    *row = r;
    *col = c;
    *consumed = i;
    return TRUE;
}

static double sheet_eval_cell(window_t* w, int row, int col, bool visiting[SHEET_ROWS][SHEET_COLS], bool* ok);

static void sheet_expand_formula(window_t* w, const char* expr, char* out, int out_sz, bool visiting[SHEET_ROWS][SHEET_COLS], bool* ok) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (!expr) return;
    int oi = 0;
    for (int i = 0; expr[i] && oi < out_sz - 1; ) {
        int row = -1, col = -1, consumed = 0;
        if (calc_is_alpha(expr[i]) && sheet_parse_ref(expr + i, &row, &col, &consumed)) {
            double v = sheet_eval_cell(w, row, col, visiting, ok);
            char nbuf[48];
            calc_num_to_text(v, nbuf, (int)sizeof(nbuf));
            for (int k = 0; nbuf[k] && oi < out_sz - 1; k++) out[oi++] = nbuf[k];
            i += consumed;
            continue;
        }
        out[oi++] = expr[i++];
    }
    out[oi] = 0;
}

static bool sheet_cell_is_numeric_text(const char* text) {
    if (!text || !text[0]) return FALSE;
    const char* p = text;
    if (*p == '+' || *p == '-') p++;
    bool saw_digit = FALSE;
    bool saw_dot = FALSE;
    while (*p) {
        if (calc_is_digit(*p)) saw_digit = TRUE;
        else if (*p == '.' && !saw_dot) saw_dot = TRUE;
        else return FALSE;
        p++;
    }
    return saw_digit;
}

static double sheet_eval_cell(window_t* w, int row, int col, bool visiting[SHEET_ROWS][SHEET_COLS], bool* ok) {
    if (!w || row < 0 || row >= SHEET_ROWS || col < 0 || col >= SHEET_COLS) {
        if (ok) *ok = FALSE;
        return 0.0;
    }
    if (visiting[row][col]) {
        if (ok) *ok = FALSE;
        return 0.0;
    }
    visiting[row][col] = TRUE;
    const char* raw = w->sheet_cells[row][col];
    double result = 0.0;
    bool local_ok = TRUE;

    if (!raw || !raw[0]) {
        result = 0.0;
    } else if (raw[0] == '=') {
        char expr[512];
        sheet_expand_formula(w, raw + 1, expr, (int)sizeof(expr), visiting, &local_ok);
        if (local_ok) {
            calc_eval_ctx_t ctx = {0};
            ctx.deg_mode = TRUE;
            ctx.ok = TRUE;
            const char* p = expr;
            result = calc_parse_expr(&ctx, &p);
            calc_skip_ws(&p);
            if (p && *p) local_ok = FALSE;
        }
    } else if (sheet_cell_is_numeric_text(raw)) {
        calc_eval_ctx_t ctx = {0};
        ctx.deg_mode = TRUE;
        ctx.ok = TRUE;
        const char* p = raw;
        result = calc_parse_expr(&ctx, &p);
        calc_skip_ws(&p);
        if (p && *p) local_ok = FALSE;
    } else {
        local_ok = FALSE;
    }

    visiting[row][col] = FALSE;
    if (ok && !local_ok) *ok = FALSE;
    return result;
}

static void sheet_format_cell_value(window_t* w, int row, int col, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = 0;
    if (!w || row < 0 || row >= SHEET_ROWS || col < 0 || col >= SHEET_COLS) return;
    const char* raw = w->sheet_cells[row][col];
    if (!raw || !raw[0]) return;
    if (raw[0] != '=') {
        calc_copy_text(out, out_sz, raw);
        return;
    }
    bool visiting[SHEET_ROWS][SHEET_COLS] = {{0}};
    bool ok = TRUE;
    double value = sheet_eval_cell(w, row, col, visiting, &ok);
    if (!ok) {
        calc_copy_text(out, out_sz, "ERR");
    } else {
        calc_num_to_text(value, out, out_sz);
    }
}

static void sheet_set_status(window_t* w, const char* text) {
    if (!w) return;
    calc_copy_text(w->sheet_status, (int)sizeof(w->sheet_status), text ? text : "");
}

static void sheet_begin_edit(window_t* w) {
    if (!w) return;
    const char* raw = w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col];
    calc_copy_text(w->sheet_edit_buf, (int)sizeof(w->sheet_edit_buf), raw ? raw : "");
    w->sheet_editing = TRUE;
    sheet_set_status(w, "Editing cell");
}

static void sheet_commit_edit(window_t* w) {
    if (!w) return;
    calc_copy_text(w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col], 64, w->sheet_edit_buf);
    w->sheet_editing = FALSE;
    w->sheet_edit_buf[0] = 0;
    sheet_set_status(w, "Saved");
}

static void sheet_clear_cell(window_t* w) {
    if (!w) return;
    w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col][0] = 0;
    sheet_set_status(w, "Cell cleared");
}

static void sheet_move_selection(window_t* w, int dr, int dc) {
    if (!w) return;
    int nr = w->sheet_sel_row + dr;
    int nc = w->sheet_sel_col + dc;
    if (nr < 0) nr = 0;
    if (nr >= SHEET_ROWS) nr = SHEET_ROWS - 1;
    if (nc < 0) nc = 0;
    if (nc >= SHEET_COLS) nc = SHEET_COLS - 1;
    w->sheet_sel_row = nr;
    w->sheet_sel_col = nc;
}

void handle_sheet_key(int idx, int key, char ascii, bool ctrl) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (w->kind != WIN_SHEET) return;

    if (w->sheet_editing) {
        if (key == KEY_ESC) {
            w->sheet_editing = FALSE;
            w->sheet_edit_buf[0] = 0;
            sheet_set_status(w, "Edit canceled");
            return;
        }
        if (key == KEY_ENTER) {
            sheet_commit_edit(w);
            sheet_move_selection(w, 1, 0);
            return;
        }
        if (key == KEY_BACKSPACE) {
            int len = slen(w->sheet_edit_buf);
            if (len > 0) w->sheet_edit_buf[len - 1] = 0;
            return;
        }
        if (!ctrl && ascii >= 32 && ascii < 127) {
            int len = slen(w->sheet_edit_buf);
            if (len < (int)sizeof(w->sheet_edit_buf) - 2) {
                w->sheet_edit_buf[len] = ascii;
                w->sheet_edit_buf[len + 1] = 0;
            }
            return;
        }
        return;
    }

    if (ctrl && (ascii == 'c' || ascii == 'C')) {
        gui_clipboard_set_text(w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col]);
        sheet_set_status(w, "Cell copied");
        return;
    }
    if (ctrl && (ascii == 'v' || ascii == 'V')) {
        const char* clip = gui_clipboard_get_text();
        if (clip && clip[0]) {
            calc_copy_text(w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col], 64, clip);
            sheet_set_status(w, "Pasted");
        }
        return;
    }

    switch (key) {
        case KEY_F2:
        case KEY_ENTER:
            sheet_begin_edit(w);
            return;
        case KEY_BACKSPACE:
        case KEY_DELETE:
            sheet_clear_cell(w);
            return;
        case KEY_LEFT:
            sheet_move_selection(w, 0, -1);
            return;
        case KEY_RIGHT:
            sheet_move_selection(w, 0, 1);
            return;
        case KEY_UP:
            sheet_move_selection(w, -1, 0);
            return;
        case KEY_DOWN:
            sheet_move_selection(w, 1, 0);
            return;
        case KEY_TAB:
            sheet_move_selection(w, 0, 1);
            return;
        default:
            break;
    }

    if (ascii >= 32 && ascii < 127) {
        w->sheet_edit_buf[0] = ascii;
        w->sheet_edit_buf[1] = 0;
        w->sheet_editing = TRUE;
        sheet_set_status(w, "Editing cell");
    }
}

void handle_sheet_click(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (w->kind != WIN_SHEET) return;
    int cx = w->x + 10;
    int cy = w->y + TITLEBAR_H + 10;
    int topbar_y = cy;
    if (my >= topbar_y && my < topbar_y + 24) {
        if (mx >= cx && mx < cx + 52) { sheet_begin_edit(w); return; }
        if (mx >= cx + 58 && mx < cx + 118) { sheet_clear_cell(w); return; }
        if (mx >= cx + 124 && mx < cx + 186) {
            gui_clipboard_set_text(w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col]);
            sheet_set_status(w, "Cell copied");
            return;
        }
        if (mx >= cx + 192 && mx < cx + 248) {
            const char* clip = gui_clipboard_get_text();
            if (clip && clip[0]) {
                calc_copy_text(w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col], 64, clip);
                sheet_set_status(w, "Pasted");
            }
            return;
        }
    }
    int grid_x = cx;
    int grid_y = cy + 34;
    int header_w = SHEET_HEADER_W;
    int header_h = SHEET_HEADER_H;
    int visible_cols = (w->w - 20 - 10 - header_w) / SHEET_CELL_W;
    int visible_rows = (w->h - TITLEBAR_H - 20 - 12 - header_h) / SHEET_CELL_H;
    if (visible_cols < 1) visible_cols = 1;
    if (visible_rows < 1) visible_rows = 1;
    if (visible_cols > SHEET_COLS) visible_cols = SHEET_COLS;
    if (visible_rows > SHEET_ROWS) visible_rows = SHEET_ROWS;
    if (mx >= grid_x + header_w && my >= grid_y + header_h) {
        int col = (mx - (grid_x + header_w)) / SHEET_CELL_W + w->sheet_scroll_col;
        int row = (my - (grid_y + header_h)) / SHEET_CELL_H + w->sheet_scroll_row;
        if (row >= 0 && row < visible_rows + w->sheet_scroll_row && col >= 0 && col < visible_cols + w->sheet_scroll_col && row < SHEET_ROWS && col < SHEET_COLS) {
            w->sheet_sel_row = row;
            w->sheet_sel_col = col;
            sheet_set_status(w, "Cell selected");
        }
    }
}

void draw_sheet_app(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (w->kind != WIN_SHEET) return;

    int cx = w->x + 10;
    int cy = w->y + TITLEBAR_H + 10;
    int cw = w->w - 20;
    int ch = w->h - TITLEBAR_H - 20;
    if (cw < 100 || ch < 100) return;

    vga_fill_rect(cx, cy, cw, ch, rgb(245, 248, 252));
    vga_draw_rect(cx, cy, cw, ch, rgb(190, 202, 220));

    char cell_name[16];
    sheet_col_label(w->sheet_sel_col, cell_name, (int)sizeof(cell_name));
    char rowbuf[16];
    char namebuf[24];
    calc_num_to_text((double)(w->sheet_sel_row + 1), rowbuf, (int)sizeof(rowbuf));
    namebuf[0] = 0;
    calc_append_text(namebuf, (int)sizeof(namebuf), cell_name);
    calc_append_text(namebuf, (int)sizeof(namebuf), rowbuf);

    char raw[128];
    calc_copy_text(raw, (int)sizeof(raw), w->sheet_cells[w->sheet_sel_row][w->sheet_sel_col]);
    char disp[128];
    sheet_format_cell_value(w, w->sheet_sel_row, w->sheet_sel_col, disp, (int)sizeof(disp));

    vga_fill_rect(cx + 8, cy + 4, cw - 16, 24, rgb(236, 240, 245));
    vga_draw_rect(cx + 8, cy + 4, cw - 16, 24, rgb(200, 210, 224));
    vga_draw_string(cx + 16, cy + 10, "Edit", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 58, cy + 10, "Clear", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 118, cy + 10, "Copy", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 176, cy + 10, "Paste", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 236, cy + 10, w->sheet_editing ? "Editing" : "Ready", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + cw - 130, cy + 10, namebuf, C_TEXT, 0xFFFFFFFF);

    vga_fill_rect(cx + 8, cy + 32, cw - 16, 24, rgb(255, 255, 255));
    vga_draw_rect(cx + 8, cy + 32, cw - 16, 24, rgb(200, 210, 224));
    vga_draw_string(cx + 16, cy + 38, "fx", C_TEXT_GRAY, 0xFFFFFFFF);
    vga_draw_string(cx + 42, cy + 38, w->sheet_editing ? w->sheet_edit_buf : raw, C_TEXT, 0xFFFFFFFF);
    if (!w->sheet_editing) {
        vga_draw_string(cx + cw - 180, cy + 38, "=", C_TEXT_GRAY, 0xFFFFFFFF);
        vga_draw_string(cx + cw - 168, cy + 38, disp[0] ? disp : "", C_TEXT, 0xFFFFFFFF);
    }

    int grid_x = cx;
    int grid_y = cy + 64;
    int header_w = SHEET_HEADER_W;
    int header_h = SHEET_HEADER_H;
    int visible_cols = (w->w - 20 - 10 - header_w) / SHEET_CELL_W;
    int visible_rows = (w->h - TITLEBAR_H - 20 - 12 - header_h - 22) / SHEET_CELL_H;
    if (visible_cols < 1) visible_cols = 1;
    if (visible_rows < 1) visible_rows = 1;
    if (visible_cols > SHEET_COLS) visible_cols = SHEET_COLS;
    if (visible_rows > SHEET_ROWS) visible_rows = SHEET_ROWS;
    int grid_w = header_w + visible_cols * SHEET_CELL_W;
    int grid_h = header_h + visible_rows * SHEET_CELL_H;

    vga_fill_rect(grid_x, grid_y, grid_w, grid_h, rgb(255, 255, 255));
    vga_draw_rect(grid_x, grid_y, grid_w, grid_h, rgb(184, 197, 214));

    for (int c = 0; c < visible_cols; c++) {
        int x = grid_x + header_w + c * SHEET_CELL_W;
        char lab[8];
        sheet_col_label(c + w->sheet_scroll_col, lab, (int)sizeof(lab));
        vga_fill_rect(x, grid_y, SHEET_CELL_W, header_h, rgb(230, 236, 244));
        vga_draw_rect(x, grid_y, SHEET_CELL_W, header_h, rgb(188, 199, 214));
        vga_draw_string(x + 4, grid_y + 4, lab, C_TEXT, 0xFFFFFFFF);
    }
    for (int r = 0; r < visible_rows; r++) {
        int y = grid_y + header_h + r * SHEET_CELL_H;
        char lab[8];
        calc_num_to_text((double)(r + 1 + w->sheet_scroll_row), lab, (int)sizeof(lab));
        vga_fill_rect(grid_x, y, header_w, SHEET_CELL_H, rgb(230, 236, 244));
        vga_draw_rect(grid_x, y, header_w, SHEET_CELL_H, rgb(188, 199, 214));
        vga_draw_string(grid_x + 6, y + 4, lab, C_TEXT, 0xFFFFFFFF);
        for (int c = 0; c < visible_cols; c++) {
            int col = c + w->sheet_scroll_col;
            int row = r + w->sheet_scroll_row;
            int x = grid_x + header_w + c * SHEET_CELL_W;
            uint64_t bg = rgb(255, 255, 255);
            if (row == w->sheet_sel_row && col == w->sheet_sel_col) bg = rgb(231, 242, 255);
            vga_fill_rect(x, y, SHEET_CELL_W, SHEET_CELL_H, bg);
            vga_draw_rect(x, y, SHEET_CELL_W, SHEET_CELL_H, rgb(225, 230, 236));
            char val[96];
            sheet_format_cell_value(w, row, col, val, (int)sizeof(val));
            if (!val[0] && row == w->sheet_sel_row && col == w->sheet_sel_col && w->sheet_editing) {
                calc_copy_text(val, (int)sizeof(val), w->sheet_edit_buf);
            }
            vga_draw_string(x + 4, y + 4, val, C_TEXT, 0xFFFFFFFF);
        }
    }

    int status_y = cy + ch - 22;
    if (w->sheet_editing) {
        vga_fill_rect(cx + 6, status_y, cw - 12, 18, rgb(40, 44, 54));
        vga_draw_string(cx + 10, status_y + 2, "Enter=save  Esc=cancel  Type to edit", C_TEXT_LIGHT, 0xFFFFFFFF);
    } else if (w->sheet_status[0]) {
        vga_draw_string(cx + 10, status_y + 2, w->sheet_status, C_TEXT, 0xFFFFFFFF);
    } else {
        vga_draw_string(cx + 10, status_y + 2, "F2 edit, arrows move, Ctrl+C/V copy-paste", C_TEXT, 0xFFFFFFFF);
    }
    vga_draw_string(cx + cw - 180, status_y + 2, "Excel-like sheet", C_TEXT_GRAY, 0xFFFFFFFF);
}

static const char* calc_mode_names[] = {"Basic", "Sci", "HS3", "Hissan"};
static const char* calc_tab_labels[] = {"Basic", "Sci", "HS3", "Hissan", NULL};
static const char* calc_mem_labels[] = {"MC", "MR", "MS", "M+", "M-", NULL};

typedef struct {
    const char* token;
    const char* title;
    const char* formula;
    const char* notes;
    const char* template_expr;
    const char* example_expr;
} calc_study_topic_t;

static const calc_study_topic_t calc_study_topics[] = {
    { "FRAC",   "Fractions",      "Exact rationals / mixed numbers",         "Use exact fraction mode for addition, subtraction, multiplication, division, and simplification.", "1/2 + 1/3",                                  "8/12" },
    { "RATIO",  "Ratio",          "a:b / proportion / rate",                "Ratio, scale, and unit rate.",                                                  "3:5",                                       "12/20" },
    { "EQN",    "Equation",       "ax + b = c",                             "One-step and two-step linear equations with step tracing.",                     "2x + 3 = 11",                               "2*4 + 3" },
    { "INEQ",   "Inequality",     "ax + b < c / ax + b > c",                "Number line, solution set, and sign reversal when dividing by a negative.",      "2x - 5 < 9",                               "2*7 - 5" },
    { "SYS2",   "2x linear sys",  "solve2(a1,b1,c1,a2,b2,c2)",               "Elimination / substitution with full steps.",                                   "solve2(1,1,12,1,-1,1)",                     "1,1,12,1,-1,1" },
    { "SYS3",   "3x linear sys",  "solve3(12 coeffs)",                     "3-variable linear systems via Gaussian elimination.",                           "solve3(1,1,1,6,2,-1,3,1,-1,2,3,4)",        "1,1,1,6,2,-1,3,1,-1,2,3,4" },
    { "QUAD",   "Quadratic",      "vertex / roots / axis",                  "Quadratic graph analysis, discriminant, max/min, and intercepts.",               "quad(1,-3,-4)",                            "1,-3,-4" },
    { "TRIG",   "Trigonometry",   "sin / cos / tan / unit circle",          "Unit circle, addition formulas, half-angle / double-angle ideas.",               "sin(30)",                                  "cos(60)" },
    { "EXP",    "Exponent",       "a^n / roots / laws",                     "Exponent rules, powers, and roots.",                                              "2^5",                                      "3^4" },
    { "LOG",    "Logarithm",      "log / ln / base change",                 "Logarithm rules, base conversion, and inverse relation with exponentials.",      "log(100)",                                 "ln(e)" },
    { "UNIT",   "Unit convert",   "length / area / volume / mass / time",   "Fast unit conversion for math and science.",                                     "unit(1500,mm,m)",                          "12,cm,m" },
    { "STAT",   "Statistics",     "mean / median / mode / sd",              "Mean, variance, standard deviation, five-number summary, and list functions like sum/prod/median.",                   "stat(1,2,2,5,9)",                          "1,2,2,5,9" },
    { "VECT",   "Vector",         "dot / parallel / perpendicular",         "2D and 3D vector dot products and orthogonality checks.",                        "vec2(1,2,3,4)",                            "1,2,3,4" },
    { "MAT2",   "Matrix 2x2",     "det / inverse",                          "2x2 determinant and inverse.",                                                   "mat2(1,2,3,4)",                            "1,2,3,4" },
    { "MAT3",   "Matrix 3x3",     "det / inverse",                          "3x3 determinant and inverse.",                                                   "mat3(1,2,3,0,1,4,5,6,0)",                   "1,2,3,0,1,4,5,6,0" },
    { "COMP",   "Complex",        "a + bi / modulus / arg",                 "Rectangular and polar forms, absolute value, and argument.",                    "complex(3,4)",                             "3,4" },
    { "SEQ",    "Sequence",       "arithmetic / geometric / recurrence",    "Arithmetic and geometric sequences, sums, and a simple Fibonacci recurrence.",   "seq(3,2,5)",                               "3,2,5" },
    { "VERIFY", "Verify",         "answer checking mode",                   "Check answers exactly with fractions when possible.",                           "verify(1/2+1/3,5/6)",                      "1/2+1/3,5/6" },
    { "GRAPH",  "Graph",          "y=f(x) / graph window",                  "Open the graph window for functions and equations.",                             "x^2-3x+2",                                 "x^2-3x+2" },
    { "POLAR",  "Polar",          "r / theta",                              "Convert complex numbers to polar form.",                                          "polar(3,4)",                               "3,4" },
    { "MIX",    "Mixed number",   "whole + remainder / fraction",           "Show improper fractions as mixed numbers.",                                       "7/3",                                      "7/3" },
    { "SIMPLIFY","Simplify",      "fraction / expression cleanup",           "Re-run the current topic or simplify the exact-fraction display.",               "1/2 + 1/4",                                "2/4" },
    { "CAS",    "CAS",           "symbolic algebra / derivative / factor",  "Use simplify(expr), expand(expr), diff(expr), or factor(expr) for one-variable polynomial CAS.", "simplify((x+1)^2)",                        "diff(x^3 - 2x + 1)" },
    { "SOLVE",  "Solve",          "study mode solver",                      "Load the current study template and solve it.",                                  "solve2(1,1,12,1,-1,1)",                     "solve2(1,1,12,1,-1,1)" },
    { "HISSAN", "Hissan",         "vertical calculation",                   "Column calculation / working steps for arithmetic. Use hissan(expr) for any arithmetic formula.",                             "hissan(12345/67)",                        "hissan(12345/67)" },
    { "NOISE",  "Noise",          "noise / perlin / fbm",                   "Value noise and fractal noise. Try noise(x), noise(x,y), fbm(x), or fbm(x,y,octaves,lacunarity,gain).",                    "noise(12.5,3.25)",                        "fbm(1.5,2.25,4,2.0,0.5)" },
    { "CLR",    "Clear",          "reset study panel",                      "Clear study text and return to the current mode defaults.",                      "",                                         "" }
};

static int calc_study_topic_count(void) {
    return (int)(sizeof(calc_study_topics) / sizeof(calc_study_topics[0]));
}

static int calc_study_find_topic(const char* token) {
    if (!token) return -1;
    for (int i = 0; i < calc_study_topic_count(); i++) {
        if (smatch(token, calc_study_topics[i].token)) return i;
    }
    return -1;
}

static const calc_study_topic_t* calc_study_get_topic(int idx) {
    if (idx < 0 || idx >= calc_study_topic_count()) return NULL;
    return &calc_study_topics[idx];
}

static void calc_study_apply_topic(window_t* w, int topic_idx, int load_mode) {
    if (!w) return;
    const calc_study_topic_t* t = calc_study_get_topic(topic_idx);
    if (!t) return;

    w->calc_topic_idx = topic_idx;
    calc_copy_text(w->calc_display, (int)sizeof(w->calc_display), t->title);
    calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), t->formula);
    calc_copy_text(w->calc_steps, (int)sizeof(w->calc_steps), t->notes);
    if (load_mode == 1) {
        calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), t->template_expr);
        w->calc_clear_next = TRUE;
    } else if (load_mode == 2) {
        calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), t->example_expr);
        w->calc_clear_next = TRUE;
    }
}

static const char* calc_btns_basic[] = {
    "C", "CE", "<-", "(", ")",
    "7", "8", "9", "/", "%",
    "4", "5", "6", "*", "^",
    "1", "2", "3", "-", "sqrt",
    "0", ".", "+", "=", "+/-",
    NULL
};

static const char* calc_btns_sci[] = {
    "sin", "cos", "tan", "log", "ln",
    "sqrt", "x^2", "x^y", "abs", "pi",
    "e", "!", "1/x", "(", ")",
    "7", "8", "9", "/", "%",
    "4", "5", "6", "*", "DEG",
    "1", "2", "3", "-", "RAD",
    "0", ".", "+", "=", "C",
    "nCr", "nPr", "cbrt", "floor", "ceil",
    "sum", "avg", "prod", "median", "std",
    "ANS",
    NULL
};

static const char* calc_btns_study[] = {
    "FRAC", "RATIO", "EQN", "INEQ", "SYS2",
    "SYS3", "QUAD", "TRIG", "EXP", "LOG",
    "UNIT", "STAT", "VECT", "MAT2", "MAT3",
    "COMP", "SEQ", "VERIFY", "GRAPH", "POLAR",
    "MIX", "SIMPLIFY", "SOLVE", "HISSAN", "NOISE",
    "CLR",
    NULL
};

static const char* calc_btns_hissan[] = {
    "C", "CE", "<-", "(", ")",
    "7", "8", "9", "/", "%",
    "4", "5", "6", "*", "^",
    "1", "2", "3", "-", "sqrt",
    "0", ".", "+", "=", "+/-",
    "ANS",
    NULL
};

static const char** calc_mode_buttons(int mode) {
    switch (mode) {
        case 1: return calc_btns_sci;
        case 2: return calc_btns_study;
        case 3: return calc_btns_hissan;
        default: return calc_btns_basic;
    }
}

static int calc_mode_button_rows(int mode) {
    switch (mode) {
        case 1: return 10;
        case 2: return 6;
        case 3: return 6;
        default: return 5;
    }
}

static void calc_draw_line_block(int x, int y, int w, const char* text, uint64_t color) {
    if (!text) return;
    int max_chars = w / FONT_W;
    if (max_chars <= 0) return;
    char line[256];
    int start = 0;
    int len = slen(text);
    if (len > max_chars) start = len - max_chars;
    int i = 0;
    for (; text[start + i] && i < max_chars && i < 255; i++) line[i] = text[start + i];
    line[i] = 0;
    vga_draw_string(x, y, line, color, 0xFFFFFFFF);
}

static void calc_draw_multiline(int x, int y, int w, int h, const char* text, uint64_t color) {
    if (!text || !text[0]) return;
    int line_h = FONT_H + 2;
    int max_lines = h / line_h;
    if (max_lines <= 0) return;
    int shown = 0;
    const char* p = text;
    while (*p && shown < max_lines) {
        char line[256];
        int li = 0;
        while (*p && *p != '\n' && li < 255) line[li++] = *p++;
        line[li] = 0;
        if (*p == '\n') p++;
        calc_draw_line_block(x, y + shown * line_h, w, line, color);
        shown++;
    }
}

void draw_calculator(int idx) {
    window_t* w = &windows[idx];
    if (w->calc_mode < 0 || w->calc_mode > 3) w->calc_mode = 0;
    if (!w->calc_initialized) {
        calc_expr_clear(w);
        w->calc_angle_deg = TRUE;
        w->calc_topic_idx = 0;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Degrees");
        if (w->calc_mode == 2) {
            calc_study_apply_topic(w, 0, 0);
        }
        w->calc_initialized = TRUE;
    }

    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;
    bool hissan_ui = (w->calc_mode == 3);

    vga_fill_rect(cx, cy, cw, ch, hissan_ui ? rgb(12, 16, 26) : rgb(242, 244, 248));

     /* Mode tabs */
    int tab_y = cy + 6;
    int tab_w = (cw - 24) / 4;
    for (int i = 0; i < 4; i++) {
        bool hov = (_get_mouse()->x >= cx + 6 + i * tab_w && _get_mouse()->x < cx + 6 + (i + 1) * tab_w && _get_mouse()->y >= tab_y && _get_mouse()->y < tab_y + 24);
        uint64_t col = (w->calc_mode == i) ? C_ACCENT : (hov ? (hissan_ui ? rgb(50, 70, 100) : rgb(210, 220, 235)) : (hissan_ui ? rgb(28, 32, 46) : rgb(225, 228, 235)));
        vga_fill_rounded_rect(cx + 6 + i * tab_w, tab_y, tab_w - 4, 24, 6, col);
        vga_draw_rounded_rect(cx + 6 + i * tab_w, tab_y, tab_w - 4, 24, 6, hissan_ui ? rgb(70, 78, 92) : rgb(180, 185, 195));
        int tw = slen(calc_tab_labels[i]) * FONT_W;
        vga_draw_string(cx + 6 + i * tab_w + (tab_w - 4) / 2 - tw / 2, tab_y + 4, calc_tab_labels[i], (w->calc_mode == i) ? C_TEXT_LIGHT : (hissan_ui ? rgb(210, 218, 235) : C_TEXT), 0xFFFFFFFF);
    }

     /* Angle toggle */
    bool hov_deg = (_get_mouse()->x >= cx + cw - 88 && _get_mouse()->x < cx + cw - 10 && _get_mouse()->y >= tab_y && _get_mouse()->y < tab_y + 24);
    draw_tbtn(cx + cw - 88, tab_y, 78, 24, w->calc_angle_deg ? "DEG" : "RAD", w->calc_angle_deg ? (hissan_ui ? rgb(60, 120, 88) : rgb(150, 220, 150)) : (hissan_ui ? rgb(60, 64, 78) : rgb(220, 220, 220)), hov_deg);

     /* Memory row */
    int mem_y = tab_y + 30;
    int mem_w = 52;
    for (int i = 0; i < 5; i++) {
        bool hov = (_get_mouse()->x >= cx + 6 + i * (mem_w + 4) && _get_mouse()->x < cx + 6 + i * (mem_w + 4) + mem_w && _get_mouse()->y >= mem_y && _get_mouse()->y < mem_y + 20);
        draw_tbtn(cx + 6 + i * (mem_w + 4), mem_y, mem_w, 20, calc_mem_labels[i], hov, FALSE);
    }

     /* Display area */
    int disp_y = mem_y + 28;
    int disp_h = hissan_ui ? 106 : 88;
    vga_fill_rect(cx + 6, disp_y, cw - 12, disp_h, hissan_ui ? rgb(20, 24, 36) : rgb(22, 26, 33));
    vga_draw_rect(cx + 6, disp_y, cw - 12, disp_h, hissan_ui ? rgb(80, 90, 110) : rgb(60, 68, 78));
    calc_draw_line_block(cx + 14, disp_y + 8, cw - 28, w->calc_expr[0] ? w->calc_expr : "0", hissan_ui ? rgb(120, 190, 255) : rgb(180, 210, 255));
    calc_draw_line_block(cx + 14, disp_y + 28, cw - 28, w->calc_display[0] ? w->calc_display : "0", rgb(255, 255, 255));
    if (w->calc_status[0]) calc_draw_line_block(cx + 14, disp_y + 56, cw - 28, w->calc_status, hissan_ui ? rgb(150, 220, 180) : rgb(140, 180, 140));
    calc_draw_line_block(cx + 14, disp_y + 72, cw - 28, gui_text("Ctrl+G/J: Hissan mode", "Ctrl+G/J: ひっ算モード"), hissan_ui ? rgb(130, 190, 255) : rgb(110, 130, 170));
    if (hissan_ui) calc_draw_line_block(cx + 14, disp_y + 88, cw - 28, gui_text("Up to 40 digits supported", "最大40桁まで対応"), hissan_ui ? rgb(170, 210, 255) : rgb(110, 130, 170));

     /* Steps / history */
    int steps_y = disp_y + disp_h + 8;
    int steps_h = (w->calc_mode == 2) ? 96 : (w->calc_mode == 3 ? 124 : 88);
    vga_fill_rect(cx + 6, steps_y, cw - 12, steps_h, hissan_ui ? rgb(16, 20, 30) : rgb(250, 250, 252));
    vga_draw_rect(cx + 6, steps_y, cw - 12, steps_h, hissan_ui ? rgb(72, 80, 94) : rgb(210, 214, 220));
    calc_draw_multiline(cx + 10, steps_y + 6, cw - 20, steps_h - 12, w->calc_steps, hissan_ui ? rgb(215, 220, 230) : rgb(70, 70, 80));

     /* Buttons */
    const char** btns = calc_mode_buttons(w->calc_mode);
    int count = 0;
    while (btns[count]) count++;
    int rows = calc_mode_button_rows(w->calc_mode);
    int cols = 5;
    int btn_area_y = steps_y + steps_h + 8;
    int btn_area_h = ch - (btn_area_y - cy) - 8;
    int btn_w = (cw - 16 - (cols - 1) * 4) / cols;
    int btn_h = (btn_area_h - (rows - 1) * 4) / rows;
    if (btn_h > 34) btn_h = 34;
    if (btn_h < 22) btn_h = 22;

    int bi = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (bi >= count) break;
            const char* b = btns[bi++];
            int bx = cx + 8 + c * (btn_w + 4);
            int by = btn_area_y + r * (btn_h + 4);
            bool hov = (_get_mouse()->x >= bx && _get_mouse()->x < bx + btn_w && _get_mouse()->y >= by && _get_mouse()->y < by + btn_h);
            uint64_t bg = hissan_ui ? (hov ? rgb(44, 50, 66) : rgb(30, 34, 46)) : (hov ? rgb(228, 230, 235) : rgb(255, 255, 255));
            uint64_t tc = hissan_ui ? rgb(232, 238, 246) : rgb(40, 40, 48);
            if (w->calc_mode == 2) {
                if (smatch(b, "REPL") || smatch(b, "EXAM") || smatch(b, "SIMPLIFY") || smatch(b, "SOLVE") || smatch(b, "GRAPH") || smatch(b, "HISSAN")) {
                    bg = hov ? (hissan_ui ? rgb(54, 72, 102) : rgb(215, 230, 250)) : (hissan_ui ? rgb(36, 44, 60) : rgb(225, 238, 252));
                    tc = hissan_ui ? rgb(156, 198, 255) : rgb(40, 85, 160);
                } else if (smatch(b, "CLR")) {
                    bg = hov ? (hissan_ui ? rgb(82, 44, 52) : rgb(240, 214, 214)) : (hissan_ui ? rgb(56, 32, 40) : rgb(247, 228, 228));
                    tc = hissan_ui ? rgb(255, 150, 150) : rgb(170, 40, 40);
                } else {
                    bg = hov ? (hissan_ui ? rgb(44, 54, 74) : rgb(222, 232, 244)) : (hissan_ui ? rgb(28, 34, 46) : rgb(233, 240, 250));
                    tc = hissan_ui ? rgb(180, 195, 220) : rgb(55, 80, 120);
                }
            } else {
                if (smatch(b, "=") || smatch(b, " =")) { bg = hov ? (hissan_ui ? rgb(70, 120, 200) : rgb(90, 150, 220)) : (hissan_ui ? rgb(48, 88, 160) : rgb(80, 140, 210)); tc = C_TEXT_LIGHT; }
                else if (smatch(b, "C") || smatch(b, "CE") || smatch(b, "<-") ) { bg = hov ? (hissan_ui ? rgb(82, 44, 52) : rgb(235, 210, 210)) : (hissan_ui ? rgb(54, 34, 42) : rgb(245, 225, 225)); tc = hissan_ui ? rgb(255, 150, 150) : rgb(170, 40, 40); }
                else if (smatch(b, "MC") || smatch(b, "MR") || smatch(b, "MS") || smatch(b, "M+") || smatch(b, "M-")) { bg = hov ? (hissan_ui ? rgb(54, 62, 82) : rgb(210, 220, 240)) : (hissan_ui ? rgb(34, 40, 52) : rgb(220, 228, 245)); tc = hissan_ui ? rgb(160, 190, 255) : rgb(60, 90, 170); }
                else if (smatch(b, "+") || smatch(b, "-") || smatch(b, "*") || smatch(b, "/") || smatch(b, "^") || smatch(b, "%")) { tc = hissan_ui ? rgb(130, 200, 255) : C_ACCENT; }
                else if (smatch(b, "sin") || smatch(b, "cos") || smatch(b, "tan") || smatch(b, "log") || smatch(b, "ln") || smatch(b, "sqrt") || smatch(b, "x^2") || smatch(b, "x^y") || smatch(b, "abs") || smatch(b, "pi") || smatch(b, "e") || smatch(b, "!")) {
                    bg = hov ? (hissan_ui ? rgb(46, 56, 74) : rgb(220, 225, 240)) : (hissan_ui ? rgb(30, 36, 48) : rgb(230, 235, 248));
                    tc = hissan_ui ? rgb(200, 210, 255) : rgb(70, 80, 140);
                }
            }
            vga_fill_rounded_rect(bx, by, btn_w, btn_h, 5, bg);
            vga_draw_rounded_rect(bx, by, btn_w, btn_h, 5, hissan_ui ? rgb(70, 78, 92) : rgb(195, 198, 206));
            int tw = slen(b) * FONT_W;
            vga_draw_string(bx + btn_w / 2 - tw / 2, by + btn_h / 2 - FONT_H / 2, b, tc, 0xFFFFFFFF);
        }
    }
}

void handle_calculator_key(int idx, char ascii, int scancode, bool ctrl) {
    window_t* w = &windows[idx];
    (void)scancode;
    if (!w->calc_initialized) {
        calc_expr_clear(w);
        w->calc_angle_deg = TRUE;
        w->calc_topic_idx = 0;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Degrees");
        if (w->calc_mode == 2) {
            calc_study_apply_topic(w, 0, 0);
        }
        w->calc_initialized = TRUE;
    }

    if (ctrl && (ascii == 'g' || ascii == 'G' || ascii == 'j' || ascii == 'J' || scancode == 0x22 || scancode == 0x24)) {
        w->calc_mode = 3;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), "Hissan mode");
        if (!w->calc_expr[0]) {
            calc_copy_text(w->calc_expr, (int)sizeof(w->calc_expr), "hissan(");
            w->calc_clear_next = FALSE;
        }
        return;
    }
    if (!ctrl && (ascii == 'g' || ascii == 'G')) { calc_open_graph_window(w); return; }
    if (ascii == '\t') {
        w->calc_mode = (w->calc_mode + 1) & 3;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), calc_mode_names[w->calc_mode]);
        return;
    }
    if (ascii == '\b') { calc_apply_token(w, "<-"); return; }
    if (ascii == '\n' || ascii == '\r' || ascii == '=') { calc_apply_token(w, "="); return; }
    if (ascii == 'c' || ascii == 'C') { calc_apply_token(w, "C"); return; }
    if (ascii == '(' || ascii == ')' || ascii == '%' || ascii == '^' || ascii == '.' || ascii == '+' || ascii == '-' || ascii == '*' || ascii == '/') {
        char t[2] = { ascii, 0 };
        calc_apply_token(w, t);
        return;
    }
    if (ascii >= '0' && ascii <= '9') {
        char t[2] = { ascii, 0 };
        calc_apply_token(w, t);
        return;
    }
}

void handle_calculator_click(int idx, int mx, int my2) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w;
    int tab_y = cy + 6;
    int tab_w = (cw - 24) / 4;

    for (int i = 0; i < 4; i++) {
        int tx = cx + 6 + i * tab_w;
        if (mx >= tx && mx < tx + tab_w - 4 && my2 >= tab_y && my2 < tab_y + 24) {
            w->calc_mode = i;
            calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), calc_mode_names[w->calc_mode]);
            return;
        }
    }

    if (mx >= cx + cw - 88 && mx < cx + cw - 10 && my2 >= tab_y && my2 < tab_y + 24) {
        w->calc_angle_deg = !w->calc_angle_deg;
        calc_copy_text(w->calc_status, (int)sizeof(w->calc_status), w->calc_angle_deg ? "Degrees" : "Radians");
        return;
    }

    int mem_y = tab_y + 30;
    int mem_w = 52;
    for (int i = 0; i < 5; i++) {
        int bx = cx + 6 + i * (mem_w + 4);
        if (mx >= bx && mx < bx + mem_w && my2 >= mem_y && my2 < mem_y + 20) {
            switch (i) {
                case 0: calc_apply_token(w, "MC"); break;
                case 1: calc_apply_token(w, "MR"); break;
                case 2: calc_apply_token(w, "MS"); break;
                case 3: calc_apply_token(w, "M+"); break;
                case 4: calc_apply_token(w, "M-"); break;
            }
            return;
        }
    }

    const char** btns = calc_mode_buttons(w->calc_mode);
    int count = 0; while (btns[count]) count++;
    int rows = calc_mode_button_rows(w->calc_mode);
    int cols = 5;
    int disp_y = mem_y + 28;
    int steps_y = disp_y + 88 + 8;
    int steps_h = (w->calc_mode == 2) ? 96 : (w->calc_mode == 3 ? 124 : 88);
    int btn_area_y = steps_y + steps_h + 8;
    int btn_area_h = (w->h - C_TITLEBAR_H) - (btn_area_y - cy) - 8;
    int btn_w = (cw - 16 - (cols - 1) * 4) / cols;
    int btn_h = (btn_area_h - (rows - 1) * 4) / rows;
    if (btn_h > 34) btn_h = 34;
    if (btn_h < 22) btn_h = 22;

    int bi = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (bi >= count) break;
            const char* b = btns[bi++];
            int bx = cx + 8 + c * (btn_w + 4);
            int by = btn_area_y + r * (btn_h + 4);
            if (mx >= bx && mx < bx + btn_w && my2 >= by && my2 < by + btn_h) {
                calc_apply_token(w, b);
                return;
            }
        }
    }
}

 /* Wallpaper presets are sourced from gui.c via gui_get_wallpaper_* helpers. */


