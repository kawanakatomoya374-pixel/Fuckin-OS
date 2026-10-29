func counter():
    let n = 0
    func inc():
        n += 1
        return n
    return inc
let f = counter()
let r = 0
for i in range(20000000):
    r = f()
print(r)
