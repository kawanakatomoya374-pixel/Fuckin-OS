# 多重継承（C3線形化MRO）と、基底クラス属性・束縛メソッドの回帰。
# 以前は基底クラスを左から深さ優先で辿る近似だったため、ダイヤモンド継承で
# CPythonと異なるメソッドが選ばれていた。

class A:
    kind = "A"

    def who(self):
        return "A"


class B(A):
    pass


class C(A):
    def who(self):
        return "C"


class D(B, C):
    pass


class Base:
    label = "base"

    def __init__(self, v=0):
        self.v = v

    def tag(self):
        return "base"


class Left(Base):
    def tag(self):
        return "left"


class Right(Base):
    def tag(self):
        return "right"


class Mix(Left, Right):
    def tag(self):
        return "mix:" + Left.tag(self) + "/" + Right.tag(self)


print(D().who())
print(C().who(), B().who(), A().who())
print(isinstance(D(), A), isinstance(D(), B), isinstance(D(), C))
print(Mix(0).tag())
print(D().kind, Mix().label)
print(Mix(7).v)
print(D.kind)
