print(sum([1, 2, 3]), sum((4, 5)), sum([], 10), sum(range(4)))
try:
    sum(5)
except TypeError:
    print("type error")
try:
    max(5)
except TypeError:
    print("type error")
