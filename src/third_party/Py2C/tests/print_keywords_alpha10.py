print("a", "b", "c", sep="|", end="!")
print("next")
print(1, 2, sep=None, end=None)
try:
    print("bad", sep=3)
except TypeError:
    print("sep-type-error")
try:
    print("bad", end=3)
except TypeError:
    print("end-type-error")
