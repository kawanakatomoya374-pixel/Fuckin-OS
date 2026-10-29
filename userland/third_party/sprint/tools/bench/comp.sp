let total = 0
for k in range(40):
    let a = [i * 2 for i in range(50000) if i % 3 != 0]
    let b = [x + 1 for x in a]
    let t = {i: i for i in range(2000)}
    total += len(b) + len(t)
print(total)
