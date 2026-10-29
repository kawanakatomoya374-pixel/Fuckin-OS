let s = ""
for i in range(200000):
    s = s .. "x"
let n = 0
for i in range(len(s)):
    if s[i] == 120:
        n += 1
let parts = []
for i in range(300000):
    parts.append(f"{i}:{i * 2}")
print(n, len(parts.join(";")))
