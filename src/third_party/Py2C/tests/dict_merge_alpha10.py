left = {"a": 1, "b": 2}
right = {"b": 9, "c": 3}
merged = left | right
print(merged, list(merged.keys()))
print(left)
left |= right
print(left, list(left.keys()))
