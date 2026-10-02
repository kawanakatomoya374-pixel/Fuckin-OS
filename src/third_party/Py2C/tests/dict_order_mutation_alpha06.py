d = {"z": 0, "a": 1, "m": 2}
print(list(d.keys()))
print(list(d.values()))
print(list(d.items()))
d.update({"b": 3, "a": 10})
print(list(d.items()))
print(d.pop("a"))
print(list(d.keys()))
d.clear()
print(len(d))
d["reused"] = 9
print(list(d.items()))
