def deco(fn):
    def w(*a, **k):
        return fn(*a, **k)
    return w

@deco
def area(w, h=2, scale=1):
    return w * h * scale

print(area(3), area(3, h=4), area(3, scale=10), area(2, 5, scale=2))
print(area(w=4, h=5), area(4, scale=3, h=1))

@deco
def greet(name, greeting="hi", punct="!"):
    return greeting + " " + name + punct

print(greet("bob"), greet("ann", punct="?"), greet("cy", greeting="yo"))

@deco
def collect(first, *rest):
    return first + ":" + ",".join([str(x) for x in rest])

print(collect("a", 1, 2), collect("c", 3))

def identity(fn):
    return fn

@identity
def value(number=1):
    return number

print(value(number=3), value())
