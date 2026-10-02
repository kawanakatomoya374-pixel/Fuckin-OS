def make_counter(start):
    value = start

    def bump():
        nonlocal value
        value = value + 1
        return value

    def read():
        return value

    return bump, read

bump, read = make_counter(10)
print(read())
print(bump())
print(bump())
print(read())
