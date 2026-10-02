"""Regression: the `...` (Ellipsis) value and `Ellipsis` builtin.

Python 3 allows `...` as an expression (commonly used as a stub body and as a
placeholder), and exposes the same object as the builtin name `Ellipsis`.
Both forms are accepted, produce the singleton, and keep `is` identity,
`repr`, `type()` and container usage consistent with CPython.
"""

stub = ...

print(stub)
print(repr(stub))
print(stub is ...)
print(stub is Ellipsis)
print(stub is None)
print(stub is not None)
print(stub == Ellipsis)
print(type(stub))
print(bool(stub))
print([...], (...,), {"k": ...})
print(len([..., ...]))

values = {"placeholder": Ellipsis, "number": 1}
print(values["placeholder"] is ...)
print(values)


def unfinished():
    ...


def finished():
    ...
    return "done"


print(unfinished(), finished())


def typed() -> ...:
    return Ellipsis


print(typed())
