"""Alpha1.0 で修正したバグの CPython 差分回帰（tests/fixes_alpha10.py）。

網羅する修正:
  * C11予約語と衝突する識別子（double/float/int/struct）
  * 同一スコープに複数ある連鎖代入の一時変数名衝突
  * モジュール関数を値として使う（g = f / sorted(key=f) / map(f, ...)）
  * 拡張代入の in-place 意味論（list += / set |= &= ^= / dict |=）
  * tuple + tuple
  * __bool__ / __len__ によるインスタンスの真偽
  * isinstance(True, int)
  * round() の負の桁数
  * min([]) / max([]) の ValueError
  * repr() の引用符選択
  * print(range(...)) と range.start/stop/step
  * ユーザー例外の str()（args の反映）
  * str.format の % 指定
  * starred 代入ターゲットが list になる
  * lambda の既定引数
  * 捕捉した callable の呼び出し（高階関数）
  * str.istitle()
"""

# C11予約語と衝突する識別子（Pythonの組込み名は避ける）
double = 2
char = "c"
struct = [1, 2]
register = 3
print(double, char, struct, register)

# 連鎖代入（同一スコープに2つ）
a = b = 1
c = d = 2
print(a, b, c, d)

# 関数を値として使う
def dbl(x):
    return x * 2

g = dbl
print(g(3), sorted([1, 2, 3], key=dbl), list(map(dbl, [1, 2])))

# 拡張代入の in-place
xs = [1, 2]
ys = xs
ys += [3]
print(xs, ys, xs is ys)

ss = {1, 2}
ts = ss
ts |= {3}
print(sorted(ss), sorted(ts), ss is ts)

ss2 = {1, 2, 3}
ss2 &= {2, 3}
print(sorted(ss2))

ss3 = {1, 2}
ss3 ^= {2, 3}
print(sorted(ss3))

ds = {"x": 1}
es = ds
es |= {"y": 2}
print(sorted(ds.items()), ds is es)

# tuple 連結
print((1, 2) + (3,), () + (1,))

# __bool__ / __len__
class E:
    def __bool__(self):
        return False

class L:
    def __len__(self):
        return 0

print(bool(E()), bool(L()))

# isinstance(bool, int)
print(isinstance(True, int), isinstance(1, bool))

# round の負の桁数
print(round(1234.5678, -2), round(15, -1), round(25, -1))

# min/max 空
try:
    print(min([]))
except ValueError:
    print("empty")

# repr の引用符選択
print(repr("it's"), repr('say "hi"'))

# range の print と属性
r = range(2, 10, 3)
print(r, r.start, r.stop, r.step)

# ユーザー例外の str()
class MyError(Exception):
    pass

try:
    raise MyError("custom")
except MyError as e:
    print(e)

# 高階関数（捕捉した callable の呼び出し）
def apply_fn(fn, v):
    return fn(v)

print(apply_fn(dbl, 5))

# str.format の % 指定
print("{:%}".format(0.25), "{:.1%}".format(0.256))

# starred 代入ターゲット
*x, y = (1, 2, 3)
print(x, y)

# lambda の既定引数
addf = lambda p, q=10: p + q
print(addf(1), addf(1, 2))

# str.istitle
print("Hello World".istitle(), "Hello world".istitle())

# --- 追加修正分（堅牢性・意味論）---

# len() はサイズを持たない型で TypeError、hash() は int、abs(-0.0) は +0.0
def probe(f):
    try:
        f()
        return "no-exc"
    except TypeError:
        return "TypeError"
print(probe(lambda: len(1)), probe(lambda: len(None)), probe(lambda: "a" + 1),
      probe(lambda: [1] + (2,)), abs(-0.0), hash(1) == hash(1.0))

# 複合代入の **=
n = 3
n **= 3
print(n)

# 既定値は def 時に一度だけ評価される（可変既定値の共有）
def acc(v, box=[]):
    box.append(v)
    return box
print(acc(1), acc(2))

# クロージャの i=i イディオム
def make_fns():
    fns = []
    for i in range(3):
        def f(x, i=i):
            return x + i
        fns.append(f)
    return fns
print([f(10) for f in make_fns()])

# クラス属性の更新とクラス生成のキーワード引数
class Counter:
    total = 0
    def __init__(self, step=1):
        Counter.total = Counter.total + step
Counter()
Counter(step=5)
print(Counter.total)

# ユーザー定義 __hash__/__eq__ が set のキーとして機能する
class Key:
    def __init__(self, v):
        self.v = v
    def __eq__(self, other):
        return self.v == other.v
    def __hash__(self):
        return self.v
print(len({Key(1), Key(1), Key(2)}), Key(1) in {Key(1)})

# カスタム __iter__/__next__ の list() と内包表記
class Count:
    def __init__(self, n):
        self.n = n
        self.i = 0
    def __iter__(self):
        return self
    def __next__(self):
        if self.i >= self.n:
            raise StopIteration
        self.i += 1
        return self.i
print(list(Count(3)), [x * 2 for x in Count(3)])

# 桁区切りと代替表記
print(format(1234567, ","), format(255, "#o"), format(255, "#b"), format(4294967295, "_x"))

# isinstance(x, object)
print(isinstance(1, object), isinstance([], object))

# 式位置の組み込み例外と raise ... from
exc = ValueError("v")
print(str(exc))
try:
    raise TypeError("t") from ValueError("c")
except TypeError as caught:
    print(str(caught))

# 多段 capture（デコレータファクトリ）
def make_adder(n):
    def deco(fn):
        def wrapper(x):
            return fn(x) + n
        return wrapper
    return deco
@make_adder(100)
def ident(x):
    return x
print(ident(1))

