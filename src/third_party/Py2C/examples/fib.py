def fib(n):
    a = 0
    b = 1
    i = 0
    while i < n:
        print(a)
        tmp = a + b
        a = b
        b = tmp
        i = i + 1
    return a

print(fib(7))
