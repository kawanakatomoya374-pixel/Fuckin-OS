text = "bananana"
print(text.find("na", 3))
print(text.find("na", 0, 4))
print(text.rfind("na", 0, 6))
print(text.index("na", -4))
print(text.rindex("na", 0, -1))
print(text.count("na", 2, -1))
print(text.count("", 2, -1))
print(text.find("", 3, 2))
try:
    print(text.index("ba", 1, 4))
except ValueError:
    print("index-range-value-error")
try:
    print(text.rindex("ba", 1, 4))
except ValueError:
    print("rindex-range-value-error")
