class Point:
    __match_args__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x
        self.y = y

def classify(value):
    match value:
        case Point(0, y=tail):
            return ("axis", tail)
        case Point(x, [a, *rest]):
            return ("pair", x, a, rest)
        case Point(x, y):
            return ("point", x, y)
        case int(n):
            return ("int", n)
        case str(text):
            return ("str", text)
        case _:
            return ("other",)

print(classify(Point(0, 7)))
print(classify(Point(3, [4, 5, 6])))
print(classify(Point(2, 8)))
print(classify(9))
print(classify("ok"))
print(classify([1]))
