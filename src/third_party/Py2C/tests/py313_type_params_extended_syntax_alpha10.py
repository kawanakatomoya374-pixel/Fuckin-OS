type CallableAlias[**P, T = int] = object
type Packed[*Ts] = tuple

def passthrough[*Ts](value: int) -> int:
    return value

def callable_passthrough[**P](value: int) -> int:
    return value

class GenericBox[T = int, *Ts, **P]:
    pass

print(passthrough(7) + callable_passthrough(8))
