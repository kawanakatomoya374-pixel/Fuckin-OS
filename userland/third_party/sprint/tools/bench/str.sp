let parts = []
for i in range(500000):
    parts.append("item" .. i)
let s = parts.join(",")
let c = 0
for w in s.split(","):
    if w.starts_with("item1"):
        c += 1
print(len(s), c)
