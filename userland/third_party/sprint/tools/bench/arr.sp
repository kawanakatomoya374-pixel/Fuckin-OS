let a = []
for i in range(3000000):
    a.append(i * 2)
let t = 0
for i in range(len(a)):
    t += a[i]
for x in a:
    t += x
print(t)
