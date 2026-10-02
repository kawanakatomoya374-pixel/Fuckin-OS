def passthrough(*items):
    return items[0]

@passthrough
def value():
    return 1

print(value())
