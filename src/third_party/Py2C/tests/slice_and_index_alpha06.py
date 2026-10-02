items = [0, 1, 2, 3, 4]
items[-1] = 9
items[-3] = 7
print(items, items[-1], items[-5])

text = "abcde"
print(text[-1], text[-10:10], text[-1:-10:-2], text[::-1], text[4:0:-2])

tuple_value = (0, 1, 2, 3, 4)
print(tuple_value[-1], tuple_value[-10:10], tuple_value[-1:-10:-2], tuple_value[::-1])

print(items[-10:10], items[10:20], items[-20:-10])

try:
    items[-6]
    print("FAIL list get bounds")
except IndexError:
    print("OK list get bounds")

try:
    items[-6] = 1
    print("FAIL list set bounds")
except IndexError:
    print("OK list set bounds")

try:
    text[5]
    print("FAIL str get bounds")
except IndexError:
    print("OK str get bounds")

try:
    items[1.0]
    print("FAIL list float index")
except TypeError:
    print("OK list float index")

try:
    tuple_value["1"]
    print("FAIL tuple str index")
except TypeError:
    print("OK tuple str index")

try:
    text[1.0]
    print("FAIL str float index")
except TypeError:
    print("OK str float index")

try:
    items[1.0] = 1
    print("FAIL list float set")
except TypeError:
    print("OK list float set")

try:
    del items["1"]
    print("FAIL list str delete")
except TypeError:
    print("OK list str delete")

try:
    items[::1.0]
    print("FAIL slice float step")
except TypeError:
    print("OK slice float step")
