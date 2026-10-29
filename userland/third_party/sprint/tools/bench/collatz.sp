func steps(n):
    let c = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        c += 1
    return c
let best = 0
for i in range(1, 600000):
    let s = steps(i)
    if s > best:
        best = s
print(best)
