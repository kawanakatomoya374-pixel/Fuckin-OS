def f1():
    try:
        print(x)
    except UnboundLocalError:
        return "ube"
    x = 1
    return x

def f2(c):
    if c:
        y = 5
    return y

def f3():
    z = 0
    def inner():
        return z
    z = 7
    return inner()

def f4():
    def g():
        return w
    try:
        g()
    except NameError:
        return "caught"
    w = 3
    return g()

print(f1())
try:
    print(f2(False))
except UnboundLocalError:
    print("ube2")
print(f2(True))
print(f3())
print(f4())
def h():
    del_me = 4
    return del_me
print(h())
try:
    {}["k"]
except LookupError:
    print("lookup ok")
try:
    1 / 0
except ArithmeticError:
    print("arith ok")
