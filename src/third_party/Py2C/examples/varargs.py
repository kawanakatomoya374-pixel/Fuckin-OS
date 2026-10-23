def total(*nums):
    s = 0
    for n in nums:
        s = s + n
    return s

print(total(1, 2, 3, 4))
print(total())

def describe(name, *tags, **info):
    print(name, tags, len(info))
    return info.get("age", 0)

print(describe("Tama", "cat", "brown", age=3, weight=4))

def greet(name, *, greeting="Hello"):
    return greeting + ", " + name

print(greet("World"))
print(greet("Claude", greeting="Hi"))
