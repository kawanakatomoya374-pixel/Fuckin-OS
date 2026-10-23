"""ネストしたクラス定義（class本体の直下に置いたclass）の適合テスト。

C名は外側クラス名を前置した "Outer__Inner" になり、外側クラスの
__classobj() が属性として登録する。クラス本体内ではその名前を
参照でき、メソッド本体からは見えない（PythonではNameError）ため
Outer.Inner のように外側クラス経由で参照する。
"""


class Shape:
    kind = "shape"

    def __init__(self, name):
        self.name = name


class Outer:
    label = "outer"

    class Inner:
        size = 3

        def __init__(self, tag):
            self.tag = tag

        def describe(self):
            return "Inner(" + self.tag + ")"

    class Point:
        def __init__(self, x, y):
            self.x = x
            self.y = y

        def total(self):
            return self.x + self.y

    class Derived(Shape):
        def __init__(self, name):
            Shape.__init__(self, name)
            self.extra = 1

        def describe(self):
            return "Derived(" + self.name + ")"

    alias = Inner

    def make_inner(self, tag):
        return Outer.Inner(tag)

    def make_via_alias(self, tag):
        return Outer.alias(tag)


print(Outer.label)
print(Outer.Inner.size)
inner = Outer.Inner("a")
print(inner.describe())
print(Outer.alias is Outer.Inner)
print(Outer().make_inner("b").describe())
print(Outer().make_via_alias("c").describe())
point = Outer.Point(2, 5)
print(point.total())
print(point.x, point.y)
print(isinstance(inner, Outer.Inner))
derived = Outer.Derived("d")
print(derived.describe(), derived.extra)
print(derived.name)


class Left:
    class Node:
        tag = "left"


class Right:
    class Node:
        tag = "right"


print(Left.Node.tag, Right.Node.tag)
print(Left.Node is Right.Node)


class Deep:
    class Mid:
        class Core:
            value = 42

        core = Core


print(Deep.Mid.Core.value)
print(Deep.Mid.core is Deep.Mid.Core)


class Ordered:
    class First:
        pass

    seen_first = First

    class Second:
        pass

    seen_both = [First, Second]


print(Ordered.seen_first is Ordered.First)
print(Ordered.seen_both[0] is Ordered.First, Ordered.seen_both[1] is Ordered.Second)


class WithStr:
    class Item:
        def __init__(self, value):
            self.value = value

        def __str__(self):
            return "Item:" + str(self.value)


print(WithStr.Item(7))
print(WithStr().Item(8))
