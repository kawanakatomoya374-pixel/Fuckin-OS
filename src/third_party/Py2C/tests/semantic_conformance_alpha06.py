# C61-C76: 高リスク意味論のCPython差分コーパス

# C61: andは左の偽オペランドを返し、右を評価しない。
trace61 = []
def side61(value):
    trace61.append(value)
    return value
print("C61", 0 and side61("bad"), "left" and side61("right"), trace61)

# C62: orは左の真オペランドを返し、右を評価しない。
trace62 = []
def side62(value):
    trace62.append(value)
    return value
print("C62", "left" or side62("bad"), 0 or side62("right"), trace62)

# C63: for-elseはbreak時にelseを実行しない。
def for_else_break():
    record = []
    for value in [1, 2, 3]:
        record.append(value)
        if value == 2:
            break
    else:
        record.append("else")
    return record
print("C63", for_else_break())

# C64: while-elseは自然終了時だけelseを実行する。
def while_else_complete():
    record = []
    value = 0
    while value < 3:
        record.append(value)
        value += 1
    else:
        record.append("else")
    return record
print("C64", while_else_complete())

# C65: 属性複合代入の受け手を一度だけ評価する。
class Box65:
    def __init__(self):
        self.value = 1
box65 = Box65()
trace65 = []
def get_box65():
    trace65.append("box")
    return box65
get_box65().value += 4
print("C65", box65.value, trace65)

# C66: 添字複合代入のコンテナ・キーを一度だけ評価する。
values66 = [10, 20]
trace66 = []
def get_values66():
    trace66.append("values")
    return values66
def get_index66():
    trace66.append("index")
    return 1
get_values66()[get_index66()] += 3
print("C66", values66, trace66)

# C67: starred unpackは可変長中間を正しく分配する。
head67, *middle67, tail67 = [1, 2, 3, 4, 5]
print("C67", head67, middle67, tail67)

# C68: dict **unpackは左から右へ上書きする。
base68 = {"a": 1, "shared": "left"}
next68 = {"b": 2, "shared": "right"}
merged68 = {"start": 0, **base68, "middle": 9, **next68}
print("C68", merged68)

# C69: delは対象属性・対象キー・対象添字だけを削除する。
class Record69:
    def __init__(self):
        self.keep = "keep"
        self.drop = "drop"
record69 = Record69()
del record69.drop
items69 = [10, 20, 30]
del items69[1]
dict69 = {"keep": 1, "drop": 2}
del dict69["drop"]
print("C69", hasattr(record69, "drop"), items69, dict69)

# C70: 整数bit演算・シフト・反転の値を保持する。
print("C70", 5 & 3, 5 | 2, 5 ^ 3, ~5, 3 << 4, 128 >> 3, -9 >> 1)

# C71: setリテラルは重複を除外し、包含・長さを提供する。
set71 = {3, 1, 3, 2}
print("C71", sorted(set71), len(set71), 1 in set71, 4 not in set71)

# C72: set(iterable)はシーケンスを重複排除する。
print("C72", sorted(set([2, 1, 2, 3])))

# C73: set内包表記は複数for・if・重複排除を組み合わせられる。
set73 = {left + right for left in [1, 2, 2] for right in [10, 20] if right > 10}
print("C73", sorted(set73))

# C74: frozensetは読み取り専用用途でsetへフォールバックする。
frozen74 = frozenset([3, 1, 3, 2])
print("C74", sorted(frozen74), len(frozen74), 2 in frozen74)

# C75: 正常なwithはenter/body/exitを順に一度ずつ実行する。
trace75 = []
class Context75:
    def __enter__(self):
        trace75.append("enter")
        return "resource"
    def __exit__(self, exc_type, exc, tb):
        trace75.append("exit")
        return False
with Context75() as resource75:
    trace75.append(resource75)
print("C75", trace75)

# C76: truthyな__exit__はwith本体の例外を抑止する。
trace76 = []
class Context76:
    def __enter__(self):
        trace76.append("enter")
        return self
    def __exit__(self, exc_type, exc, tb):
        trace76.append("exit")
        return True
with Context76():
    trace76.append("body")
    raise ValueError("suppressed")
trace76.append("after")
print("C76", trace76)
