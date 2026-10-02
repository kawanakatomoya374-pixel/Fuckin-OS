def make(scale):
    return list(scale * item + 1 for item in range(4) if item >= 1)


print(make(5))
