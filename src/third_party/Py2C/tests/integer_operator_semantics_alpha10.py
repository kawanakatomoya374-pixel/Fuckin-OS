print(-7 // 3, 7 // -3, -7 // -3)
print(-7 % 3, 7 % -3, -7 % -3)
print(2 ** -3, -2 ** 3, 2 ** 10)
print(-5 >> 1, 5 << 3, -5 << 3)

try:
    0 ** -1
    print("FAIL zero negative power")
except ZeroDivisionError:
    print("OK zero negative power")

try:
    1 << -1
    print("FAIL negative shift")
except ValueError:
    print("OK negative shift")
