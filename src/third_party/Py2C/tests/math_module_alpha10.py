# math モジュールの拡充（floor/ceil/trunc/fabs/fmod/hypot/三角/対数/指数/整数論）の回帰。
import math

print(math.floor(3.7), math.ceil(3.2), math.trunc(-3.7), math.floor(-3.7))
print(math.fabs(-2.5), math.fmod(7, 3), math.hypot(3, 4), math.copysign(2, -1))
print(math.degrees(math.pi), math.radians(180), math.ldexp(1.0, 3))
print(math.log(math.e), math.log2(8), math.log10(1000), math.log(8, 2))
print(math.exp(0), math.expm1(0), math.log1p(0), round(math.tan(0)))
print(math.isnan(math.nan), math.isinf(math.inf), math.isfinite(1.0), math.isfinite(math.inf))
print(math.fsum([0.1, 0.2]), math.prod([2, 3, 4]), math.factorial(5))
print(math.gcd(12, 18), math.gcd(-12, 18), math.isqrt(17), math.isqrt(16))
print(math.comb(5, 2), math.perm(5, 2), round(math.cbrt(27)))
print(round(math.erf(0), 6), round(math.gamma(5), 6))
print(math.tau > 6.28, math.pi > 3.14, math.e > 2.71)
