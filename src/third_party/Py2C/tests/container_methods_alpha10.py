items = [3, 1, 2]
items_copy = items.copy()
items_copy.append(4)
print(items, items_copy)

mapping = {"a": 1, "b": 2}
mapping_copy = mapping.copy()
mapping_copy["a"] = 9
print(mapping, mapping_copy)
print(mapping.popitem(), mapping)

first = {1, 2, 3}
second = {3, 4}
print(sorted(first.union(second)))
print(sorted(first.intersection(second)))
print(sorted(first.difference(second)))
print(sorted(first.symmetric_difference(second)))
first.update([4, 5], {6})
print(sorted(first))
first.intersection_update({2, 4, 6, 8})
print(sorted(first))
first.difference_update([4])
print(sorted(first))
first.symmetric_difference_update({2, 7})
print(sorted(first), first.isdisjoint({1, 3}), first.isdisjoint({2, 9}))
