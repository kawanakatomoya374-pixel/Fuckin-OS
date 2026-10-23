class Palette:
    RED = 3
    BLUE = 7

class Holder:
    palette = Palette

def classify(value):
    match value:
        case Palette.RED:
            return "red"
        case Holder.palette.BLUE:
            return "blue"
        case _:
            return "other"

print(classify(3))
print(classify(7))
print(classify(9))
