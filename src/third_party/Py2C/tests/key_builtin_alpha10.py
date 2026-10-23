a = ["bb", "a", "ccc"]; a.sort(key=len); print(a)
print(sorted(a, key=len, reverse=True), sorted([-1, 2, -3], key=abs))
print(list(map(str, [1, 2])), sorted(["B", "a"], key=lambda s: s.lower()))
