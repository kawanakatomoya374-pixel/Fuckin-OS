word = "banana"
print(word.index("na"))
print(word.rindex("na"))
try:
    print(word.index("xy"))
except ValueError:
    print("index-value-error")
try:
    print(word.rindex("xy"))
except ValueError:
    print("rindex-value-error")
