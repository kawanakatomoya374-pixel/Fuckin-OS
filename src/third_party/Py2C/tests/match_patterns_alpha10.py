def classify(value):
    match value:
        case [1, middle, *tail, 9] as original if middle > 0:
            return ("sequence", middle, tail, original)
        case {"kind": "ok", "value": captured} as record:
            return ("mapping", captured, record)
        case 7 | 8:
            return ("or", value)
        case _:
            return ("other", value)


def mapping_rest(value):
    match value:
        case {"x": x, **rest}:
            return ("rest", x, rest)
        case _:
            return ("no-rest", value)


def mapping_rest_only(value):
    match value:
        case {**rest}:
            return ("all", rest)
        case _:
            return ("not-a-mapping", value)


print(classify([1, 2, 3, 4, 9]))
print(classify({"kind": "ok", "value": 11, "extra": 5}))
print(classify(8))
print(classify("abc"))
print(classify([1, -2, 9]))
print(mapping_rest({"x": 1, "y": 2, "z": 3}))
print(mapping_rest({"x": 1}))
print(mapping_rest({"y": 2}))
print(mapping_rest_only({"a": 1, "b": 2}))
print(mapping_rest_only(12))
