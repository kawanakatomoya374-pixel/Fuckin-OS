values = [1, 2, 3, 2, 4]
print(values.index(2, 2))
print(values.index(2, -3))
print(values.index(2, 0, 3))
try:
    print(values.index(2, 4))
except ValueError:
    print("list-index-value-error")
