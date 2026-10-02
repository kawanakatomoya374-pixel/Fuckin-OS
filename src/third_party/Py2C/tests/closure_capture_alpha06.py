def make_adder(base):
    offset = 3
    def add(value):
        return base + offset + value
    return add

first = make_adder(10)
second = make_adder(20)
print(first(5), second(1), first(0))
