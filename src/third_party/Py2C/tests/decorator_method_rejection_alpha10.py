def identity(fn):
    return fn

class Sample:
    @identity
    def value(self):
        return 1

print(Sample().value())
