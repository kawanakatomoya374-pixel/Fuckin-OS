"""Newline-handling probe for tests/crlf_source_regression.sh.

Deliberately mixes the constructs that a CR at end-of-line used to break:
a module docstring, a class with blank lines between methods, a tuple for
target, an except binding and a comprehension.
"""

STATE = {"count": 0}


class Probe:
    """docstring inside the class body"""

    def __init__(self, name):
        self.name = name

    def method(self, value):
        return self.name + ":" + str(value)

    def __str__(self):
        return "Probe(" + self.name + ")"


def run(items):
    total = 0
    try:
        for index, value in enumerate(items):
            total += value
            if index > 100:
                raise ValueError("too many")
    except ValueError as exc:
        print("caught", str(exc))
    squares = [x * x for x in items if x % 2 == 1]
    probe = Probe("kernel")
    print(probe, probe.method(total), squares)
    STATE["count"] = total
    return total


print("total", run([1, 2, 3, 4]))
print("state", STATE["count"])
