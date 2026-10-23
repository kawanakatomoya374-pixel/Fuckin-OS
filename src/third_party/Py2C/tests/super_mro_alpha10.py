# super() のMRO解決の回帰。
# 以前は基底チェーンをコード生成時に静的に辿っていたため、ダイヤモンド継承では
# インスタンスの型ではなく静的な基底が選ばれ、CPythonと異なる結果になっていた。

class A:
    def who(self):
        return "A"

    def __init__(self, value=0):
        self.value = value


class C(A):
    def who(self):
        return "C"


class B(A):
    def who(self):
        return "B>" + super().who()


class D(B, C):
    pass


class E(B, C):
    def __init__(self, value=0):
        super().__init__(value * 2)


print(D().who())
print(B().who())
print(E(21).value)
