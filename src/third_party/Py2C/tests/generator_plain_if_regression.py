def g1(c):
    total = 0
    if c:
        total = 10
    else:
        total = 20
    yield total
    if total > 15:
        flag = "big"
    else:
        flag = "small"
    yield flag
    if c:
        if total > 5:
            k = 1
        else:
            k = 2
    yield total + 1

def g2(c):
    if c:
        pass
    print("body")
    if not c:
        print("neg")
    yield 7

print(list(g1(True)))
print(list(g1(False)))
print(list(g2(False)))
