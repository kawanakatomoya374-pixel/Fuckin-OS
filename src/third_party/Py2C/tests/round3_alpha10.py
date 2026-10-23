"""Alpha1.0 Round-3 で追加した構文の CPython 差分回帰（tests/round3_alpha10.py）。

網羅する追加機能:
  * str.format のフィールド: 位置/名前/アクセサ（[i] [key] .attr）/変換 !r !s !a
  * str.format / f-string の入れ子書式（{:>{}} / {:.{}f}）
  * クラスからのメソッド取り出し（A.staticmethod / A.classmethod / A.instance）
  * isinstance(x, type(None)) などの型オブジェクト判定
  * ジェネレータ関数のループ内 yield（for/while/if/break/continue/else/yield from）
  * ジェネレータメソッド（class の内側）、x = yield v
"""

# --- str.format: アクセサと変換 ---
row = [1, 2, 3]
d = {"k": 9, "a b": 7}
print("{0[0]}-{0[2]}".format(row))
print("{d[k]}".format(d=d))
print("{d[a b]}".format(d=d))
print("{!r} {!s}".format("x", "y"))
print("{v!r:>5}".format(v="x"))

class Point:
    pass

p = Point()
p.x = 5
print("{p.x}".format(p=p))

# --- 入れ子書式 ---
print("[{:>{}}]".format("ab", 4))
print("{:.{}f}".format(3.14159, 2))
print("{:0{}d}".format(42, 6))

# --- f-string ---
name = "ab"
width = 4
print(f"{name:>{width}}|")
print(f"{3.14159:.2f}")
print(f"{42:#x} {42:08b}")
print(f"{name!r}")

# --- クラスからのメソッド取り出し ---
class Api:
    def m(self, x):
        return x + 1
    @staticmethod
    def s(x):
        return x * 2
    @classmethod
    def c(cls, x):
        return x - 1

fs = Api.s
fc = Api.c
print(fs(3), fc(3))
inst = Api()
fm = inst.m
print(fm(5))
print(Api.m(inst, 5))

# --- 型オブジェクト判定 ---
print(isinstance(None, type(None)), isinstance(1, type(None)))
print(isinstance([], type([])), isinstance({}, type({})))

# --- ジェネレータ: for ---
def counter(n):
    for i in range(n):
        yield i * i

print(list(counter(4)))

# --- ジェネレータ: while ---
def countdown(n):
    while n > 0:
        yield n
        n -= 1

print(list(countdown(3)))

# --- ジェネレータ: continue / break ---
def evens(xs):
    for x in xs:
        if x % 2:
            continue
        if x > 8:
            break
        yield x

print(list(evens([1, 2, 3, 4, 9, 10, 12])))

# --- ジェネレータ: for-else（break しない）---
def all_positive(xs):
    for x in xs:
        if x < 0:
            break
        yield x
    else:
        yield 0

print(list(all_positive([1, 2])))

# --- ジェネレータ: 入れ子ループ ---
def pairs(a, b):
    for x in a:
        for y in b:
            yield (x, y)

print(list(pairs([1, 2], [3, 4])))

# --- ジェネレータ: ループ内でローカルを更新 ---
def running():
    total = 0
    for i in range(4):
        total += i
        yield total
    yield total * 100

print(list(running()))

# --- ジェネレータ: yield from ---
def inner(n):
    for i in range(n):
        yield i + 1

def outer():
    yield 0
    yield from inner(3)
    yield 9

print(list(outer()))

# --- ジェネレータ: x = yield v（send() 非対応なので再開時は None）---
def echo():
    got = yield 1
    yield got

it = echo()
print(next(it))
print(next(it))

# --- ジェネレータメソッド ---
class Counter:
    def __init__(self, limit):
        self.limit = limit
    def each(self):
        for i in range(self.limit):
            yield i * 10

c = Counter(3)
print(list(c.each()))

# --- ジェネレータ: while True + break ---
def first_over(xs, limit):
    i = 0
    while True:
        if i >= len(xs):
            break
        if xs[i] > limit:
            yield xs[i]
        i += 1

print(list(first_over([1, 5, 9, 2], 3)))
