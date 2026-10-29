let t = {}
for i in range(1000000):
    t["k" .. (i % 5000)] = i
let n = 0
for i in range(2000000):
    n += t["k" .. (i % 5000)]
print(n, len(t))
