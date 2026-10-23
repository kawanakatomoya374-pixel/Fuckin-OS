f = lambda *a, **k: (a, k)
print(f(1, 2))
g = lambda a, *r, b=2: (a, r, b)
print(g(1, 2, 3), g(1))
h = lambda x, *, y=5: x + y
print(h(1))
