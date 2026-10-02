def identity(fn):
    return fn

def outer():
    @identity
    def inner():
        return 1
    return inner()

print(outer())
