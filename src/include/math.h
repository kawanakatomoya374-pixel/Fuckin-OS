#ifndef COS_MATH_H
#define COS_MATH_H

#define INFINITY (__builtin_inff())
#define NAN      (__builtin_nanf(""))
#define HUGE_VAL (__builtin_huge_val())

double fabs(double x);
double floor(double x);
double ceil(double x);
float ceilf(float x);
float fabsf(float x);
long lroundf(float x);
double atof(const char *text);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double copysign(double x, double y);
double sqrt(double x);
double cbrt(double x);
double pow(double x, double y);
double exp(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double hypot(double x, double y);
double scalbn(double x, int n);
double frexp(double x, int* exp);
double modf(double x, double* iptr);
long lrint(double x);

int isnan(double x);
int isfinite(double x);
int isinf(double x);
int signbit(double x);

#endif
