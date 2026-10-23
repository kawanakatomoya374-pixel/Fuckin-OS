def sub():
    yield 1
    yield 2
    return "rv"
def outer():
    v = yield from sub()
    print("got", v)
    yield 3
print(list(outer()))
def g():
    global Z
    Z = 10
    return Z
print(g(), Z)
