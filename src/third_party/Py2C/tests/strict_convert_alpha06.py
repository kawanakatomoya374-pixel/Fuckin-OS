def probe(tag, fn):
    try:
        print(tag, fn())
    except ValueError:
        print(tag, "<ValueError>")
    except OverflowError:
        print(tag, "<OverflowError>")


probe("S01", lambda: int("123"))
probe("S02", lambda: int("123x"))
probe("S03", lambda: int(" 42 "))
probe("S04", lambda: int("1_000"))
probe("S05", lambda: int("_1"))
probe("S06", lambda: int("1__0"))
probe("S07", lambda: int("-9223372036854775808"))
probe("S08", lambda: int("9223372036854775807"))
probe("S09", lambda: int("-0"))
probe("S10", lambda: int(" \t7\n"))
probe("S11", lambda: float("1.5"))
probe("S12", lambda: float("1.2x"))
probe("S13", lambda: float(" 2.5 "))
probe("S14", lambda: float("nan") == float("nan"))
probe("S15", lambda: int(""))
probe("S16", lambda: int("  "))
probe("S17", lambda: int("+7"))
probe("S18", lambda: len({"a": 1, "b": 2}))
probe("S19", lambda: "a" in {"a": 1})
probe("S20", lambda: "z" in {"a": 1})
probe("S21", lambda: 3 in {1, 2, 3})
probe("S22", lambda: 9 in {1, 2, 3})
