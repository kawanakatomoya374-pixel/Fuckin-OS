# メソッドデコレータ（@staticmethod / @classmethod / @property）の回帰。
# これらはメソッド種別として扱い、selfを渡すか・クラスを渡すか・
# 属性読み出しでゲッターを呼ぶかをランタイムが種別で切り替える。

class Temp:
    unit = "C"

    def __init__(self, celsius):
        self._celsius = celsius

    @property
    def celsius(self):
        return self._celsius

    @property
    def fahrenheit(self):
        return self._celsius * 9 / 5 + 32

    @staticmethod
    def to_celsius(f):
        return (f - 32) * 5 / 9

    @classmethod
    def from_fahrenheit(cls, f):
        return cls(cls.to_celsius(f))

    @classmethod
    def describe(cls):
        return "unit=" + cls.unit


t = Temp(100)
print(t.celsius, t.fahrenheit)
print(Temp.to_celsius(212), t.to_celsius(32))
print(Temp.from_fahrenheit(32).celsius)
print(t.describe(), Temp.describe())
print(hasattr(t, "fahrenheit"), hasattr(t, "missing"))

fetcher = t.fahrenheit
print(fetcher)
try:
    t.fahrenheit = 1
    print("assigned")
except AttributeError:
    print("AttributeError")


class Base:
    def __init__(self):
        self._v = 1

    @property
    def v(self):
        return self._v


class Sub(Base):
    @property
    def v(self):
        return 100


print(Sub().v, Base().v)


class Counter:
    total = 0

    @classmethod
    def bump(cls):
        cls.total = cls.total + 1
        return cls.total


print(Counter.bump(), Counter.bump(), Counter.total)


class Shape:
    def __init__(self, name):
        self.name = name

    @staticmethod
    def kind():
        return "shape"

    @classmethod
    def make(cls, name):
        return cls(name)

    def label(self):
        return self.name + "(" + self.kind() + ")"


print(Shape.make("box").label(), Shape.kind())
