for a, (b, c) in [(1, (2, 3)), (4, (5, 6))]:
    print(a, b, c)
for x, [y, z] in [(7, [8, 9])]:
    print(x + y + z)
for k, (v, *rest) in [("q", (1, 2, 3))]:
    print(k, v, rest)
pairs = {"s": (1, 2)}
for key, (p1, p2) in pairs.items():
    print(key, p1 + p2)
