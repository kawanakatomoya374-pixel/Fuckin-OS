def f(a, b=2, *r):
    return (a, b, r)
def deco(fn):
    def w(*a, **k):
        print("call", a, k)
        return fn(*a, **k)
    return w
g = deco(lambda x, y: x + y)
print(g(1, 2), g(*[3, 4]))
h = lambda *xs: sum(xs)
args = [1, 2, 3]
print(h(*args), h(*(4, 5)), h(*"ab" if False else []))
fns = [str.upper, str.lower]
print([fn(*("ab",)) for fn in fns])
import math
print(math.hypot(*[3, 4]))
