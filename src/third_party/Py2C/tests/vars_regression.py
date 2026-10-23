class P:
    def __init__(self):
        self.a = 1
        self.b = "x"
p = P()
print(sorted(vars(p).items()))
print(vars(p)["a"])
def f(x, y=2):
    z = x + y
    return sorted(vars().items())
print(f(1))
def g():
    k = 5
    if False:
        j = 1
    return vars()
print(g())
try:
    vars(5)
except TypeError:
    print("type error")
