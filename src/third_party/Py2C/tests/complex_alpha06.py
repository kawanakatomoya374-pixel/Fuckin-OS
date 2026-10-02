# Python Code to C Alpha0.6: 60-case complex regression corpus

def case01(): return 1 + 2 * 3

def case02(): return (10 - 3) // 2

def case03(): return 2 ** 8

def case04(): return 17 % 5

def case05(): return -7 + 12

def case06(): return 3 < 4 and 5 >= 5

def case07(): return not (2 == 3)

def case08(): return "Py" + "thon"

def case09(): return "alpha".upper()

def case10(): return "alpha beta".split()[1]

def case11(): return "abc"[1]

def case12(): return "abcdef"[1:5:2]

def case13(): return len([1, 2, 3])

def case14(): return [1, 2] + [3, 4]

def case15(): return [1, 2, 3][-1]

def case16(): return [x * x for x in [1, 2, 3]]

def case17(): return [x for x in range(6) if x % 2 == 0]

def case18(): return tuple([1, 2, 3])

def case19(): return len(("a", "b", "c"))

def case20(): return {"a": 1, "b": 2}["b"]

def case21():
    d = {"a": 1}
    d["b"] = 2
    return len(d)

def case22(): return "x" in {"x": 1}

def case23(): return 4 in [1, 2, 3, 4]

def case24(): return list(range(3))

def case25(): return sum([1, 2, 3, 4])

def case26(): return max([2, 9, 4])

def case27(): return min([2, 9, 4])

def case28(): return sorted([3, 1, 2])

def case29(): return round(2.5)

def case30(): return abs(-42)

def case31():
    total = 0
    for value in [1, 2, 3]: total += value
    return total

def case32():
    value = 1
    while value < 16: value *= 2
    return value

def case33():
    for value in range(10):
        if value == 4: break
    return value

def case34():
    values = []
    for value in range(5):
        if value == 2: continue
        values.append(value)
    return values

def case35(a=3, b=4): return a * b

def case36(*values): return sum(values)

def case37(**values): return values.get("x", 0) + values.get("y", 0)

def case38(): return case36(*[1, 2, 3])

def case39(): return case37(x=4, y=5)

def case40():
    try:
        return 1 // 0
    except Exception:
        return "caught"

def case41():
    try:
        raise ValueError("x")
    except ValueError as exc:
        return str(exc)

def case42():
    result = []
    try:
        result.append("try")
    finally:
        result.append("finally")
    return result

class Counter:
    def __init__(self, value=0): self.value = value
    def inc(self, amount=1):
        self.value += amount
        return self.value
    def __str__(self): return "Counter(" + str(self.value) + ")"

def case43(): return Counter(3).inc(2)

def case44(): return str(Counter(5))

def case45():
    c = Counter()
    c.inc()
    c.inc(4)
    return c.value

def case46(): return f"value={case01():02d}"

def case47(): return "{}:{}".format("a", 3)

def case48():
    first, second = [10, 20]
    return first + second

def case49():
    values = [1, 2, 3]
    return values[1:]

def case50():
    value = {"x": 1}
    return value.get("missing", 9)

def case51(): return bool(1)

def case52(): return int(3.9)

def case53(): return float(4)

def case54(): return len("portable")

def case55(): return "alpha".startswith("al")

def case56(): return "alpha".endswith("ha")

def case57(): return "x".replace("x", "y")

def case58(): return [1, 2, 3].pop()

def case59(): return list({"a": 1}.keys())

def case60(): return all([True, True, 1]) and any([False, 0, 2])

cases = []
cases.append(case01())
cases.append(case02())
cases.append(case03())
cases.append(case04())
cases.append(case05())
cases.append(case06())
cases.append(case07())
cases.append(case08())
cases.append(case09())
cases.append(case10())
cases.append(case11())
cases.append(case12())
cases.append(case13())
cases.append(case14())
cases.append(case15())
cases.append(case16())
cases.append(case17())
cases.append(case18())
cases.append(case19())
cases.append(case20())
cases.append(case21())
cases.append(case22())
cases.append(case23())
cases.append(case24())
cases.append(case25())
cases.append(case26())
cases.append(case27())
cases.append(case28())
cases.append(case29())
cases.append(case30())
cases.append(case31())
cases.append(case32())
cases.append(case33())
cases.append(case34())
cases.append(case35())
cases.append(case36(1, 2, 3))
cases.append(case37(x=1, y=2))
cases.append(case38())
cases.append(case39())
cases.append(case40())
cases.append(case41())
cases.append(case42())
cases.append(case43())
cases.append(case44())
cases.append(case45())
cases.append(case46())
cases.append(case47())
cases.append(case48())
cases.append(case49())
cases.append(case50())
cases.append(case51())
cases.append(case52())
cases.append(case53())
cases.append(case54())
cases.append(case55())
cases.append(case56())
cases.append(case57())
cases.append(case58())
cases.append(case59())
cases.append(case60())
for case_result in cases:
    print(case_result)
