xs = [3, 9, 4]
print(max(*xs), min(*xs), sum(*[[1, 2, 3]]))
print(max(*xs, 7), min(1, *xs))
print(list(zip(*[[1, 2], [3, 4]])))
rows = [[1, 2, 3], [4, 5, 6]]
print([list(c) for c in zip(*rows)])
print(max(*[[5, 2]]))
try:
    max(*[5])
except TypeError:
    print("type error")
