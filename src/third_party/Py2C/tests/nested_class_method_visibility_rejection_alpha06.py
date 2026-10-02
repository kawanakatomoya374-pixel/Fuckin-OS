"""メソッド本体からクラススコープのネストクラス名を参照した場合の診断テスト。

Pythonではクラススコープはメソッド本体から見えない（NameError）。
transpilerも同じ規則で、黙って壊れたCを出さずに診断する。
外側クラス経由の Outer.Inner なら参照できる。
"""


class Outer:
    class Inner:
        pass

    def make(self):
        return Inner()


print(Outer().make())
