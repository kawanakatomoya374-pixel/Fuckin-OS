records = [(1, 2, 3, 4), (5, 6, 7)]
for first, *middle, last in records:
    print(first, middle, last)

nested_rows = []
for left, right in [(10, 20), (30, 40)]:
    row = []
    for value in [left, right]:
        row.append(value)
    nested_rows.append(row)
print(nested_rows)

try:
    for only, pair in [(1, 2, 3)]:
        print(only, pair)
except ValueError:
    print("arity-error")
