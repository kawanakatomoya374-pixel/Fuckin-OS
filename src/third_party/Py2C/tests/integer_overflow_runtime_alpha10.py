maximum = 9223372036854775807
minimum = -9223372036854775807 - 1

try:
    maximum + 1
    print("FAIL add overflow")
except OverflowError:
    print("OK add overflow")

try:
    minimum - 1
    print("FAIL sub overflow")
except OverflowError:
    print("OK sub overflow")

try:
    maximum * 2
    print("FAIL mul positive overflow")
except OverflowError:
    print("OK mul positive overflow")

try:
    minimum * -1
    print("FAIL mul negative overflow")
except OverflowError:
    print("OK mul negative overflow")

print(maximum - 1)
print(minimum + 1)
print(3037000499 * 3037000499)
