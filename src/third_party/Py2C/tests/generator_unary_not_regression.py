def g(c):
    if not c:
        print("neg")
    yield 1
print(list(g(False)))
print(list(g(True)))
