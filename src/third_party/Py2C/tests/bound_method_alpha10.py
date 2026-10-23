# メソッドを値として取り出す（束縛メソッド）回帰。
# 以前は obj.method がAttributeErrorになり、コールバックとして渡せなかった。

class Counter:
    def __init__(self):
        self.count = 0

    def bump(self):
        self.count = self.count + 1
        return self.count


class Scale:
    def __init__(self, factor):
        self.factor = factor

    def apply(self, x):
        return x * self.factor


c = Counter()
bump = c.bump
print(bump(), bump(), c.count)
print(hasattr(c, "bump"), hasattr(c, "missing"))

s = Scale(3)
apply = s.apply
print(apply(5))
print(list(map(s.apply, [1, 2, 3])))
print(list(filter(lambda v: v > 6, map(s.apply, [1, 2, 3]))))

class Node:
    def __init__(self, name):
        self.name = name

    def describe(self, prefix):
        return prefix + self.name


n = Node("node")
d = n.describe
print(d("a-"))
