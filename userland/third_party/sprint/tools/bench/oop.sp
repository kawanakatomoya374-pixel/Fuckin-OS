class P:
    let x = 0
    let y = 0
    func init(self, x, y):
        self.x = x
        self.y = y
    func add(self, o):
        return P(self.x + o.x, self.y + o.y)
    func len2(self):
        return self.x * self.x + self.y * self.y
let p = P(0, 0)
let d = P(1, 2)
let s = 0
for i in range(3000000):
    p = p.add(d)
    s += p.len2() % 3
print(s)
