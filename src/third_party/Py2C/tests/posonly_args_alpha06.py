def join(left, right=2, /, scale=1):
    return (left + right) * scale

print(join(3))
print(join(3, 4, scale=2))
try:
    join(left=3)
except TypeError:
    print("TypeError")
