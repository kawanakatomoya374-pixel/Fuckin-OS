# Alpha0.6 で強化した変換精度・対応幅のデモ

def summarize(*nums, **opts):
    total = sum(nums)
    label = opts.get("label", "sum")
    return f"{label}: {total}"

print(summarize(1, 2, 3, label="total"))

def add3(a, b, c):
    return a + b + c

print(add3(*[10, 20, 30]))

values = [3, 1, 4, 1, 5, 9, 2, 6]
print(sorted(values, reverse=True))
print(any([v > 8 for v in values]))
print(all([v > 0 for v in values]))

x = 42
print(f"{x:05d} {x:x} {x:b}")
print("{} squared is {}".format(x, x * x))

data = {"a": 1, "b": 2}
if "a" in data and "z" not in data:
    print("lookup ok")

a = b = c = 0
a, b, c = 1, 2, 3
print(a + b + c)
