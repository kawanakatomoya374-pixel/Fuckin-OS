/* HobbyOS向け参照libc（templates/hobby_os/embed/hobby_os_libc.c）の検証 (Alpha0.6)
 *
 * 参照実装は libc/libm を持たないターゲット向けに自前の級数・近似で実装して
 * いるため、ホストの libm と比較して精度と境界（±inf/NaN/0/負値）の扱いを
 * 回帰として固定する。シンボル衝突を避けるため、ビルド時に
 * -include tests/hobbyos_libc_prefix.h で stub_ 前置へ改名する。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

extern double stub_fabs(double x);
extern double stub_floor(double x);
extern double stub_ceil(double x);
extern double stub_trunc(double x);
extern double stub_fmod(double x, double y);
extern double stub_sqrt(double x);
extern double stub_cbrt(double x);
extern double stub_hypot(double x, double y);
extern double stub_copysign(double x, double y);
extern double stub_ldexp(double x, int e);
extern double stub_exp(double x);
extern double stub_log(double x);
extern double stub_log2(double x);
extern double stub_log10(double x);
extern double stub_pow(double x, double y);
extern double stub_sin(double x);
extern double stub_cos(double x);
extern double stub_tan(double x);
extern double stub_atan(double x);
extern double stub_asin(double x);
extern double stub_acos(double x);
extern double stub_atan2(double y, double x);
extern double stub_erf(double x);
extern double stub_tgamma(double x);
extern int stub_snprintf(char *buf, size_t cap, const char *fmt, ...);
extern long long stub_strtoll(const char *s, char **end, int base);
extern double stub_strtod(const char *s, char **end);

static int failures;

static void check_close(const char *name, double got, double want, double tol) {
    double diff = got - want;
    if (diff < 0) diff = -diff;
    if (want < 0) want = -want;
    if (want > 1.0) diff /= want;   /* 大きい値は相対誤差で見る */
    if (diff > tol) {
        printf("FAIL %s: got %.17g want %.17g (tol %.1e)\n", name, got, want, tol);
        failures++;
    } else {
        printf("ok %s %.12g\n", name, got);
    }
}

static void check_int(const char *name, long long got, long long want) {
    if (got != want) {
        printf("FAIL %s: got %lld want %lld\n", name, got, want);
        failures++;
    } else {
        printf("ok %s %lld\n", name, got);
    }
}

int main(void) {
    char buf[64];
    char *end = NULL;

    check_close("fabs", stub_fabs(-2.5), fabs(-2.5), 0.0);
    check_close("floor", stub_floor(-2.5), floor(-2.5), 0.0);
    check_close("floor+", stub_floor(2.5), floor(2.5), 0.0);
    check_close("ceil", stub_ceil(-2.5), ceil(-2.5), 0.0);
    check_close("trunc", stub_trunc(-2.7), trunc(-2.7), 0.0);
    check_close("fmod", stub_fmod(7.5, 2.0), fmod(7.5, 2.0), 1e-15);
    check_close("fmod-", stub_fmod(-7.5, 2.0), fmod(-7.5, 2.0), 1e-15);
    check_close("sqrt", stub_sqrt(2.0), sqrt(2.0), 1e-15);
    check_close("cbrt", stub_cbrt(-27.0), cbrt(-27.0), 1e-14);
    check_close("hypot", stub_hypot(3.0, 4.0), hypot(3.0, 4.0), 1e-15);
    check_close("copysign", stub_copysign(3.0, -1.0), copysign(3.0, -1.0), 0.0);
    check_close("ldexp", stub_ldexp(1.5, 4), ldexp(1.5, 4), 0.0);
    check_close("exp", stub_exp(1.5), exp(1.5), 1e-12);
    check_close("exp0", stub_exp(0.0), 1.0, 0.0);
    check_close("log", stub_log(10.0), log(10.0), 1e-14);
    check_close("log2", stub_log2(8.0), log2(8.0), 1e-14);
    check_close("log10", stub_log10(1000.0), log10(1000.0), 1e-14);
    check_close("pow", stub_pow(2.0, 10.0), pow(2.0, 10.0), 1e-12);
    check_close("powneg", stub_pow(-2.0, 3.0), pow(-2.0, 3.0), 1e-12);
    check_close("sin", stub_sin(1.0), sin(1.0), 1e-7);
    check_close("sinbig", stub_sin(100.0), sin(100.0), 1e-5);
    check_close("cos", stub_cos(2.0), cos(2.0), 1e-7);
    check_close("tan", stub_tan(0.5), tan(0.5), 1e-6);
    check_close("atan", stub_atan(1.0), atan(1.0), 1e-12);
    check_close("atan2", stub_atan2(1.0, -1.0), atan2(1.0, -1.0), 1e-12);
    check_close("asin", stub_asin(0.5), asin(0.5), 1e-7);
    check_close("acos", stub_acos(0.5), acos(0.5), 1e-7);
    check_close("erf", stub_erf(0.5), erf(0.5), 1e-6);
    check_close("tgamma", stub_tgamma(5.0), tgamma(5.0), 1e-9);

    if (stub_sqrt(-1.0) == stub_sqrt(-1.0)) { printf("FAIL sqrt: NaN expected\n"); failures++; }
    else printf("ok sqrt_nan\n");
    if (stub_sqrt(0.0) != 0.0) { printf("FAIL sqrt: 0 expected\n"); failures++; }
    else printf("ok sqrt_zero\n");

    if (stub_snprintf(buf, sizeof(buf), "%d %s %05.2f %x", -12, "ab", 3.14159, 255u) < 0) {
        printf("FAIL snprintf: negative result\n");
        failures++;
    } else if (strcmp(buf, "-12 ab 03.14 ff") != 0) {
        printf("FAIL snprintf: got '%s'\n", buf);
        failures++;
    } else {
        printf("ok snprintf '%s'\n", buf);
    }

    check_int("strtoll", stub_strtoll(" -42x", NULL, 10), -42);
    check_int("strtoll16", stub_strtoll("0x1f", NULL, 16), 31);
    check_int("strtoll_oct", stub_strtoll("0755", NULL, 0), 493);
    check_close("strtod", stub_strtod("3.5e2xyz", &end), 350.0, 1e-12);
    if (!end || *end != 'x') { printf("FAIL strtod: endptr\n"); failures++; }
    else printf("ok strtod endptr\n");

    if (failures) {
        printf("hobbyos_libc_failed: %d\n", failures);
        return 1;
    }
    printf("hobbyos_libc_ok\n");
    return 0;
}
