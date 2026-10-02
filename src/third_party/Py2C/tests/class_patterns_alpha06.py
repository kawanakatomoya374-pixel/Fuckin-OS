class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def classify(value):
    match value:
        case Point(x=0, y=y):
            return ("axis", y)
        case Point(x=x, y=[first, *rest]) as point if x > 0:
            return ("point", x, first, rest, point.x)
        case Point():
            return ("point-other",)
        case _:
            return ("other",)


def classify_builtin(value):
    match value:
        case int():
            return ("int",)
        case str():
            return ("str",)
        case object():
            return ("object",)


print(classify(Point(0, 9)))
print(classify(Point(3, [4, 5, 6])))
print(classify(Point(-1, [2])))
print(classify({"x": 0, "y": 9}))
broken = Point(1, [2])
del broken.y
print(classify(broken))
print(classify_builtin(True))
print(classify_builtin("text"))
print(classify_builtin([]))
