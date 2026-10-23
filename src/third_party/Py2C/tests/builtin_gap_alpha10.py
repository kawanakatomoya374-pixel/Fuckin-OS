"""Regression: builtins that previously produced non-compilable C.

`divmod`, 3-argument `pow`, `format`, `callable` and the string predicates
`isascii`/`isprintable` were listed as supported but were emitted as raw C
identifiers, so the generated C failed to compile (implicit declaration of
`divmod`, undefined `p2c_user_pow`, ...). They are now lowered to runtime
helpers with CPython semantics and covered here.
"""

# divmod: floor division semantics for int and float, both signs
print(divmod(7, 2))
print(divmod(-7, 2))
print(divmod(7, -2))
print(divmod(-7, -2))
print(divmod(7.5, 2))
print(divmod(-7.5, 2))
print(divmod(7, 2.0))
print(divmod(0, 5))
q, r = divmod(17, 5)
print(q, r, q * 5 + r)

# pow with modulus: positive, zero, negative exponents, negative base/modulus
print(pow(2, 10, 1000))
print(pow(3, 0, 7))
print(pow(2, -1, 5))
print(pow(-2, 3, 5))
print(pow(-2, -1, 5))
print(pow(2, 10, -7))
print(pow(123456789, 3, 1000000007))

# format(value, spec)
print(format(3.14159, ".2f") + "|" + format(42, "05d") + "|" + format(255, "#x"))
print(format("text", ">8") + "|")
print(format(1234567, ","))
print(format(0.5, ".0%"))

# callable: functions, lambdas, classes, instances with __call__, values
def top_level():
    return 1


lam = lambda: 2


class Plain:
    pass


class Callable:
    def __call__(self):
        return 3


print(callable(top_level), callable(lam), callable(Plain), callable(Callable))
print(callable(Callable()), callable(Plain()), callable(3), callable("x"), callable(None))
print(callable(len), callable(print))

# string predicates
print("abc".isascii(), "".isascii(), "caf\u00e9".isascii())
print("abc".isprintable(), "".isprintable(), "a\tb".isprintable(), "a b".isprintable())
print("abc".isalpha(), "abc1".isalnum(), "12".isdigit(), " ".isspace(), "".isalpha())
