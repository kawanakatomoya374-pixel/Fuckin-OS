#include <stdint.h>

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif

static double tgl_absd(double x) {
    return x < 0.0 ? -x : x;
}

double fabs(double x) {
    return tgl_absd(x);
}

double floor(double x) {
    long long i = (long long)x;
    if ((double)i > x) {
        i -= 1;
    }
    return (double)i;
}

static double tgl_wrap_pi(double x) {
    const double two_pi = 2.0 * M_PI;
    while (x > M_PI) x -= two_pi;
    while (x < -M_PI) x += two_pi;
    return x;
}

double sin(double x) {
    x = tgl_wrap_pi(x);
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    double x7 = x5 * x2;
    return x - (x3 / 6.0) + (x5 / 120.0) - (x7 / 5040.0);
}

double cos(double x) {
    x = tgl_wrap_pi(x);
    double x2 = x * x;
    double x4 = x2 * x2;
    double x6 = x4 * x2;
    return 1.0 - (x2 / 2.0) + (x4 / 24.0) - (x6 / 720.0);
}

double sqrt(double x) {
    if (x <= 0.0) {
        return 0.0;
    }
    double r = (x > 1.0) ? x : 1.0;
    for (int i = 0; i < 10; ++i) {
        r = 0.5 * (r + x / r);
    }
    return r;
}

double pow(double base, double expv) {
    if (base == 0.0) {
        return 0.0;
    }
    if (expv == 0.0) {
        return 1.0;
    }
    if (base < 0.0) {
        return 0.0;
    }

    int n = (int)(expv >= 0.0 ? (expv + 0.5) : (expv - 0.5));
    if (n < 0) {
        n = -n;
        base = 1.0 / base;
    }

    double result = 1.0;
    while (n > 0) {
        if (n & 1) {
            result *= base;
        }
        base *= base;
        n >>= 1;
    }
    return result;
}
