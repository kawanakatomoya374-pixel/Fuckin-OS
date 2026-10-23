d = {}
for i in range(20000):
    d[i] = i
found = 0
for i in range(20000):
    if i in d:
        found += 1
print(found)
s = set()
for i in range(20000):
    s.add(i)
c = 0
for i in range(0, 40000, 2):
    if i in s:
        c += 1
print(c)
