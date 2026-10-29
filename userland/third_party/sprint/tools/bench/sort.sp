import random
random.seed(7)
let a = []
for i in range(300000):
    a.append(random.int(0, 1000000))
a.sort()
let b = a.map(func(x): x * 2).filter(func(x): x % 3 == 0)
let s = b.reduce(func(acc, x): acc + x, 0)
a.sort(func(x, y): y - x)
print(a[0], len(b), s)
