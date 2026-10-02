type Scalar = int | float
type Pair[T = int] = tuple[T, T]

def identity[T = int](value: T) -> T:
    return value

class Box[T = int]:
    pass

print(identity(42))
