def f1():
    fs = []
    for i in range(3):
        fs.append(lambda i=i: i)
    return [g() for g in fs]
print(f1())

def f2(x):
    g = lambda: x
    x = x + 10
    return g()
print(f2(1))

def f3():
    counter = 0
    def inc():
        nonlocal counter
        counter += 1
        return counter
    inc(); inc()
    def show():
        return counter
    return show()
print(f3())

def f4():
    gens = [lambda: k for k in range(2)]
    return [h() for h in gens]
print(f4())

def f5():
    total = 0
    add = lambda v: v + total
    total = 5
    return add(1)
print(f5())

def f6():
    fns = []
    for n in range(3):
        def mk():
            return n * 10
        fns.append(mk)
    return [q() for q in fns]
print(f6())

def f7_removed():
    try:
        raise ValueError("boom")
    except ValueError as err:
        msg = lambda: str(err)
    return msg()
# f7 removed: CPython deletes except-as names at block end

def f8():
    data = [1, 2, 3]
    it = (v * 2 for v in data)
    data = [9]
    return list(it)
print(f8())

def f9():
    if (w := 4) > 3:
        return (lambda: w)()
print(f9())

class K:
    def m(self):
        acc = []
        for t in range(2):
            acc.append(lambda: t)
        return [a() for a in acc]
print(K().m())

def f10(a, b):
    return [lambda: a + b, lambda: a * b]
print([h() for h in f10(2, 3)])

# 外側の変数の遅延束縛（モジュール直下のループ）
fns = []
for i in range(3):
    fns.append(lambda: i)
print([f() for f in fns])
def outer():
    x = 1
    def get():
        return x
    x = 2
    return get
print(outer()())
