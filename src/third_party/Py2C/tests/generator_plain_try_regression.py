def g(items):
    total = 0
    for s in items:
        try:
            total = total + int(s)
        except ValueError:
            total = total + 100
    yield total
    try:
        q = 1 // 0
    except (ZeroDivisionError, KeyError) as err:
        q = -5
    yield q
    try:
        pass
    except ValueError:
        pass
    yield "end"

def h():
    try:
        r = 3 // 0
    except ValueError:
        r = 0
    yield r

print(list(g(["1", "x", "2"])))
try:
    print(list(h()))
except ZeroDivisionError:
    print("propagated")
