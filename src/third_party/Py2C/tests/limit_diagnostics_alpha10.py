# 固定上限の診断と -D 上書きを検証するための小さなプログラム。
# （tests/limit_diagnostics_alpha10.sh から、上限を絞った/広げたビルドで使う）
#
#   * 直鎖 A->B->C は深さ方向の上限（P2C_MRO_MAX_DEPTH）を刺激する。
#   * ダイヤモンド P<-Q,R<-S は C3 線形化が必要になり、名前数の上限
#     （P2C_MRO_MAX_NAMES）を刺激する。
#   * クラスレジストリは -DP2C_MAX_CLASS_REGISTRY=2 で通知（と例外）が出る。


class A:
    def who(self):
        return "A"


class B(A):
    def who(self):
        return "B"


class C(B):
    def who(self):
        return "C"


class P:
    def tag(self):
        return "P"


class Q(P):
    def tag(self):
        return "Q"


class R(P):
    def tag(self):
        return "R"


class S(Q, R):
    pass


a = A()
b = B()
c = C()
print(a.who())
print(b.who())
print(c.who())
print(c.who() + b.who() + a.who())
print(Q().tag(), R().tag())
print(S().tag())
