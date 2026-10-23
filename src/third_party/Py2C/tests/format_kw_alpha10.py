# str.format のキーワード引数（{name}）と位置引数の混在
print("{x}".format(x=7), "{x} {y}".format(x=1, y=2), "{a}{b}{a}".format(a=1, b=2))
print("{} {x}".format("a", x=1), "{x:>5}|".format(x="a"), "{n:.2f}".format(n=3.14159))
print("{x}{}".format("z", x="y"), "{empty}".format(empty=""))
print("{name} is {age}".format(name="bob", age=42))
try:
    "{z}".format(x=1)
except KeyError as e:
    print("KeyError", str(e))
print("{a}{c}".format(**{"a": 1, "c": 3}) if False else "{a}{c}".format(a=1, c=3))
