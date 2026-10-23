# 複合的な「ちゃんとしたPython」が通るかを試す回帰。
from math import sqrt


class Shape:
    def __init__(self, name):
        self.name = name

    def area(self):
        return 0.0

    def describe(self):
        return "%s(%.2f)" % (self.name, self.area())


class Rect(Shape):
    def __init__(self, w, h):
        super().__init__("rect")
        self.w = w
        self.h = h

    def area(self):
        return float(self.w * self.h)


class Square(Rect):
    def __init__(self, side):
        super().__init__(side, side)
        self.name = "square"


shapes = [Rect(2, 3), Square(4)]
for s in shapes:
    print(s.describe(), round(s.area(), 2))


def fib(n, memo={}):
    if n in memo:
        return memo[n]
    if n < 2:
        return n
    memo[n] = fib(n - 1) + fib(n - 2)
    return memo[n]


print([fib(i) for i in range(10)])
print({k: v for k, v in zip("abc", [1, 2, 3])})
print(sorted({3, 1, 2}, reverse=True), sorted(["bb", "a", "ccc"], key=len))

words = "the quick brown fox jumps over the lazy dog".split()
counts = {}
for w in words:
    counts[w] = counts.get(w, 0) + 1
print(counts["the"], len(counts))

squares = {x: x * x for x in range(1, 6) if x % 2 == 1}
print(squares, sum(squares.values()))


def counter(start):
    n = start

    def step():
        return n

    return step


print(counter(7)())

try:
    data = [1, 2, 3]
    print(data[1], len(data))
    raise ValueError("boom")
except ValueError as exc:
    print("caught", str(exc))
finally:
    print("cleanup")

text = "".join(ch.upper() if i % 2 == 0 else ch for i, ch in enumerate("abcdef"))
print(text, sqrt(144.0), format(3.14159, ".3f"), f"{len(words)}:{max(counts.values())}")
