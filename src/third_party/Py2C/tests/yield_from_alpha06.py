def delegated():
    yield from [1, 2]
    yield from (3,)
    yield 4

print(list(delegated()))
