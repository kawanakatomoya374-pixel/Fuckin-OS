class Shape:
    def __init__(self, name):
        print("Shape.init", name)
        self.name = name

    def describe(self):
        return "shape:" + self.name


class Rect(Shape):
    def __init__(self, w, h):
        print("Rect.init", w, h)
        super().__init__("rect")
        self.w = w
        self.h = h


class Square(Rect):
    def __init__(self, side):
        print("Square.init", side)
        super().__init__(side, side)
        self.name = "square"


s = Square(4)
print("final", s.name, s.w, s.h)
print(s.describe())
