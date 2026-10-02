def show(label, value):
    print(label, value)

show("is", 3 is 3)
show("notin", 3 not in [1, 2])
show("slice", [0, 1, 2, 3][1:3])
show("nested_unpack", (lambda: (1, 2))())

try:
    raise ValueError("v")
except ValueError as err:
    show("except", str(err))

for x in zip([1, 2], [3, 4]):
    show("zip", x)

show("enumerate", list(enumerate(["a", "b"])))
show("any", any([False, True]))
show("all", all([True, True]))
show("set", {1, 2, 1})
show("dict_unpack", {"a": 1, **{"b": 2}})
