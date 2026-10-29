let n = 0
let parts = []
for i in range(200000):
    let s = f"item-{i}-{i * 2}"
    n += len(s)
    if s.contains("99"):
        parts.append(s.upper())
    let u = s.replace("item", "x").slice(0, 3)
    n += len(u)
print(n, len(parts))
