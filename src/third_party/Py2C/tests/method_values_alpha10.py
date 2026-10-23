c = {"a": 1, "b": 3, "c": 2}
print(max(c, key=c.get), c.get("z", 9), sorted(["b", "A", "c"], key=str.lower))
f = c.get
l = []
ap = l.append
ap(4); ap(5); print(l, [x for x in map(str.upper, ["x", "y"])])
