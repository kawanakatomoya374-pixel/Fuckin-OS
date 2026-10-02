base = dict.fromkeys(["beta", "alpha", "beta"], 0)
print(base, list(base.keys()), list(base.values()))
print(dict.fromkeys(("none",)))

shared = dict.fromkeys(("left", "right"), [])
shared["left"].append(7)
print(shared)

pairs = [("b", 2), ("a", 1), ("b", 3)]
updated = {"root": 0}
updated.update(pairs)
print(updated, list(updated.items()))

try:
    {}.update([("bad", 1, 2)])
except ValueError:
    print("update-value-error")

small = {1, 2}
print(small.issubset([1, 2, 3]), small.issuperset((1,)), small.issuperset([1, 4]), small.isdisjoint([7, 8]))

popped = {1, 2, 3}
item = popped.pop()
print(item in {1, 2, 3}, len(popped), len(popped | {item}), popped.isdisjoint({item}))

empty = set()
try:
    empty.pop()
except KeyError:
    print("set-pop-key-error")
