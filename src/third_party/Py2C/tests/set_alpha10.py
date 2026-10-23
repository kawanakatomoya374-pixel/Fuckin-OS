values = {3, 1, 3, 2}
print(sorted(values))
print(len(values), 1 in values, 4 not in values, bool(values), bool(set()))
print(type(values))
print(isinstance(values, set), isinstance(values, (list, set)))
print(sorted(set([2, 1, 2, 3])))
for value in sorted(values):
    print("item", value)
