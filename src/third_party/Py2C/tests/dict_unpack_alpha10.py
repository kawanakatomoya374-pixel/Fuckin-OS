def mark(label, value):
    print(label, value)
    return value

first = {"a": 1, "shared": 10}
second = {"b": 2, "shared": 20}
merged = {"start": mark("start", 0), **first, "middle": mark("middle", 1), **second, "shared": mark("end", 30)}
print(merged)
print(merged["a"], merged["b"], merged["shared"], merged["start"], merged["middle"])

try:
    invalid = {**[1, 2]}
except TypeError:
    print("typeerror")
