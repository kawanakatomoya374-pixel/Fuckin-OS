s = frozenset([1, 2])
try:
    s.add(3)
    print("added")
except AttributeError:
    print("AttributeError add")
try:
    s.discard(1)
    print("discarded")
except AttributeError:
    print("AttributeError discard")
print(type(s).__name__, repr(s), len(s))
print(s == {1, 2}, {1, 2} == s, s == frozenset([2, 1]))
d = {frozenset([1]): "a"}
print(d[frozenset([1])])
print(hash(frozenset([1, 2])) == hash(frozenset([2, 1])))
print(isinstance(s, frozenset), isinstance({1}, frozenset), isinstance(s, set))
u = s | {3}
print(type(u).__name__, sorted(u))
x = frozenset([1])
x |= {2}
print(type(x).__name__, sorted(x))
print(repr(frozenset()), type(frozenset()).__name__)
print(sorted(s.union([9])), type(s.union([9])).__name__)
print(sorted(s.intersection([2, 5])), type(s.intersection([2, 5])).__name__)
print(sorted(s.copy()), type(s.copy()).__name__)
m = {1, 2}
m |= {3}
print(type(m).__name__, sorted(m))
print(type(m | s).__name__, type(s | m).__name__)

# 等値・ハッシュ・in（同じ要素の frozenset）
k = frozenset([1])
d2 = {k: "y"}
k2 = frozenset([1])
print(k == k2, k2 in d2)
print(1 in k2, 1 in k)
print(d2.get(k2))
t = frozenset([1, 2, 3])
d3 = {t: 1}
print(frozenset([1, 2, 3]) in d3)
print(frozenset(["a"]) in {frozenset(["a"]): 2})
