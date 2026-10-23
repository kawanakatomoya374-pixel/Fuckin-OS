class Point:
    __match_args__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x
        self.y = y

class Names:
    PointType = Point

class Registry:
    names = Names

def classify(value):
    match value:
        case Registry.names.PointType(0, y=tail):
            return ("axis", tail)
        case Registry.names.PointType(x, y):
            return ("point", x, y)
        case _:
            return ("other",)

print(classify(Point(0, 4)))
print(classify(Point(2, 5)))
print(classify(3))
