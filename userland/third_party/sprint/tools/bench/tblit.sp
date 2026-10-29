let t = {}
for i in range(200000):
    t["key" .. i] = i
let s = 0
for r in range(10):
    for k, v in t:
        s += v
    for k in t:
        s += len(k)
print(s)
