# 多数のキーを入れて dict/set の動的拡張（rehash）後も
# 挿入順・参照・削除が CPython と同じであることを確認する。
d = {}
for i in range(2000):
    d[i] = i * i
print(len(d), d[0], d[1000], d[1999])

keys = list(d.keys())
print(len(keys), keys[0], keys[1], keys[1999])
vals = list(d.values())
print(vals[0], vals[10], vals[1999])
print(sum(d.values()) % 1000003)

found = 0
for i in range(0, 4000):
    if i in d:
        found += 1
print(found)

s = set()
for i in range(1500):
    s.add(i % 700)
print(len(s))
ordered = list(s)
print(len(ordered), ordered[0], ordered[699])
print(sorted(s)[0], sorted(s)[-1])

for i in range(200):
    d.pop(i, None)
print(len(d), 0 in d, 199 in d, 200 in d)

print(len(d), sum(d.values()) % 1000)
