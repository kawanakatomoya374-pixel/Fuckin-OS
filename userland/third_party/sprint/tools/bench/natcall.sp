import math
let s = 0.0
for i in range(5000000):
    s += math.sqrt(i) + math.abs(-i) + len("abc")
print(s)
