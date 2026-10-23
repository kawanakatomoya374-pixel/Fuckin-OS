"""CPU バウンドなベンチマーク（Round-7 の性能調査用）。

各関数は「Python の機能のうち、どの部分に時間がかかっているか」を切り分けるために
分けてある。実行時間は prints でなく経過時間だけを表示する。
"""


def bench_loop(n):
    total = 0
    for i in range(n):
        total += i
    return total


def bench_while(n):
    total = 0
    i = 0
    while i < n:
        total += i
        i += 1
    return total


def bench_fib(n):
    if n < 2:
        return n
    return bench_fib(n - 1) + bench_fib(n - 2)


def bench_list_comp(n):
    xs = [i * 2 for i in range(n) if i % 3 == 0]
    return len(xs)


def bench_str_join(n):
    parts = []
    for i in range(n):
        parts.append(str(i))
    return len(",".join(parts))


def bench_dict(n):
    d = {}
    for i in range(n):
        d[i % 1000] = i
    total = 0
    for i in range(1000):
        total += d.get(i, 0)
    return total


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def move(self, dx, dy):
        self.x += dx
        self.y += dy
        return self.x + self.y


def bench_class(n):
    p = Point(0, 0)
    total = 0
    for i in range(n):
        total += p.move(1, 2)
    return total


def gen_squares(n):
    for i in range(n):
        yield i * i


def bench_generator(n):
    total = 0
    for v in gen_squares(n):
        total += v
    return total


def bench_str_format(n):
    total = 0
    for i in range(n):
        s = "{}:{}".format(i, i * 2)
        total += len(s)
    return total


BENCHES = [
    ("loop", bench_loop, 3000000),
    ("while", bench_while, 3000000),
    ("fib", bench_fib, 26),
    ("list_comp", bench_list_comp, 300000),
    ("str_join", bench_str_join, 60000),
    ("dict", bench_dict, 300000),
    ("class", bench_class, 600000),
    ("generator", bench_generator, 300000),
    ("str_format", bench_str_format, 200000),
]


def main():
    for name, fn, arg in BENCHES:
        result = fn(arg)
        print(name, result)


main()
