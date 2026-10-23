squares = {x * x for x in range(8) if x % 2 == 0}
print(sorted(squares))
pairs = {a + b for a in [1, 2, 2] for b in [10, 20] if b > 10}
print(sorted(pairs))
print(len({x % 3 for x in range(10)}))
