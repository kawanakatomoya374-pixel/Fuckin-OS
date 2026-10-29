func make(n):
    let m = []
    for i in range(n):
        let row = []
        for j in range(n):
            row.append((i * j) % 7 + 1)
        m.append(row)
    return m
func mul(a, b, n):
    let c = []
    for i in range(n):
        let row = []
        for j in range(n):
            let s = 0
            for k in range(n):
                s += a[i][k] * b[k][j]
            row.append(s)
        c.append(row)
    return c
let n = 200
let c = mul(make(n), make(n), n)
print(c[10][10], c[199][199])
