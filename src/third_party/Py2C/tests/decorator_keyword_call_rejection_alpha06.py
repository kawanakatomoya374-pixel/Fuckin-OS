def identity(fn):
    return fn

@identity
def value(number=1):
    return number

print(value(number=3))
