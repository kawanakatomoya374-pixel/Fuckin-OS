def g(items):
    total = 0
    for x in items:
        if x < 0:
            continue
        if x > 100:
            break
        total = total + x
    yield total
    n = 0
    while n < 10:
        n = n + 3
        if n == 6:
            continue
        if n > 7:
            break
    yield n
    last = None
    for a in [1, 2]:
        for b in [10, 20]:
            last = a * b
    yield last
    for q in []:
        pass
    yield "done"

print(list(g([1, -2, 3, 200, 5])))
print(list(g([])))
