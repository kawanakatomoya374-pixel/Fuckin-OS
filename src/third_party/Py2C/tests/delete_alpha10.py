class Box:
    def __init__(self):
        self.value = 9

box = Box()
items = [10, 20, 30]
record = {"keep": 1, "drop": 2}
del box.value
del items[1]
del record["drop"]

try:
    print(box.value)
except AttributeError:
    print("attr_deleted")

print(items, record.get("drop", "missing"), record["keep"])
