import typed
func make(n):
    let m = typed.f64(n * n)
    for i in range(n):
        for j in range(n):
            m[i * n + j] = (i * j) % 7 + 1
    return m
func mul(a, b, n):
    let c = typed.f64(n * n)
    for i in range(n):
        for j in range(n):
            let s = 0.0
            for k in range(n):
                s += a[i * n + k] * b[k * n + j]
            c[i * n + j] = s
    return c
let n = 200
let c = mul(make(n), make(n), n)
print(c[10 * n + 10], c[199 * n + 199])
