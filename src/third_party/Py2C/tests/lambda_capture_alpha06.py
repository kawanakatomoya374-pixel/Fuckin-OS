def make_adder(base):
    return lambda value: base + value

add_eight = make_adder(8)
print(add_eight(3))
