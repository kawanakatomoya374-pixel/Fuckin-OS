"""Cのライブラリ名（libc/libmと衝突しうる名前）をパラメータ・ローカル・
モジュール関数名に使う回帰テスト。

transpilerは衝突回避のため mangle_ident() で ``p2c_user_<name>`` へ名前を変える。
パラメータ宣言・``(void)`` キャスト・本体参照・前方宣言がすべて同じ名前を使わないと、
生成Cが「undeclared identifier」でコンパイルできなくなる（以前はクラスメソッドの
``(void)`` キャストだけ生名で、``def __init__(self, index, ...)`` が壊れていた）。
"""


def mix(index, round, abs):
    total = index + round + abs
    log = total * 2
    return total, log


def shadow_sqrt(sqrt, pow):
    return sqrt * pow


def index(values, target):
    for position in range(len(values)):
        if values[position] == target:
            return position
    return -1


class Holder:
    def __init__(self, index, round):
        self.index = index
        self.round = round

    def total(self, abs):
        return self.index + self.round + abs


print(mix(1, 2, 3))
print(shadow_sqrt(4, 5))
print(index([5, 6, 7], 6))
holder = Holder(6, 7)
print(holder.total(8))
print(holder.index, holder.round)
print(mix(abs=1, index=2, round=3))
main = 10
print(main)
