def outer():
    value = 10
    def inner():
        nonlocal value
        value += 2
        return value
    print(inner(), inner(), value)

outer()
