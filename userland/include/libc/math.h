#ifndef _COS_MATH_H
#define _COS_MATH_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_E        2.7182818284590452354
#define M_SQRT2    1.41421356237309504880
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define HUGE_VAL   (__builtin_huge_val())
#define HUGE_VALF  (__builtin_huge_valf())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
double fabs(double); double floor(double); double ceil(double); double trunc(double); double round(double);
double fmod(double, double); double modf(double, double *); double frexp(double, int *); double ldexp(double, int);
double sqrt(double); double cbrt(double); double hypot(double, double);
double exp(double); double exp2(double); double log(double); double log2(double); double log10(double); double pow(double, double);
double sin(double); double cos(double); double tan(double); double asin(double); double acos(double);
double atan(double); double atan2(double, double); double sinh(double); double cosh(double); double tanh(double);
double copysign(double, double); double fmin(double, double); double fmax(double, double); double fma(double, double, double);
long lround(double); long lroundf(float); double rint(double); double nearbyint(double);
float fabsf(float); float floorf(float); float ceilf(float); float truncf(float); float roundf(float);
float fmodf(float, float); float sqrtf(float); float powf(float, float); float expf(float); float logf(float); float log10f(float); float log2f(float);
float sinf(float); float cosf(float); float tanf(float); float atan2f(float, float); float atanf(float);
float hypotf(float, float); float fminf(float, float); float fmaxf(float, float); float copysignf(float, float);
_COS_END
#endif
