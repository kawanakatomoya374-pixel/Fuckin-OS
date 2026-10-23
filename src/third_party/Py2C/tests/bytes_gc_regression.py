total = 0
for i in range(200000):
    b = b"abc\x01"
    total = total + len(b) + b[3]
print(total)
