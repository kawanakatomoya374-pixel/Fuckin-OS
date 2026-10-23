import math
from math import sqrt
print(math.pi)
print(sqrt(9))
try:
    x = 1 / 0
except ZeroDivisionError as e:
    print("caught")
