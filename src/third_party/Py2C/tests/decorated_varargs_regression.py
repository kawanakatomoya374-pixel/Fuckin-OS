def deco(fn):
    def w(*a, **k):
        print("call", fn.__name__, a, k)
        return fn(*a, **k)
    return w

@deco
def add(a, b=2, *r, **kw):
    return a + b + sum(r) + len(kw)

print(add(1))
print(add(1, 2, 3, 4))
print(add(5, 1, 10))

@deco
def only_star(*xs):
    return xs

print(only_star(), only_star(7, 8))

@deco
def kwonly(a, *, c=9):
    return a * c

print(kwonly(2))

def tag(*labels):
    def d(fn):
        def w(*a):
            return (labels, fn(*a))
        return w
    return d

@tag("x", "y")
def sq(n):
    return n * n

print(sq(4))
