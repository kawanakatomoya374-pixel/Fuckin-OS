events = []


def subject(value):
    events.append(value)
    return value


match subject(2):
    case 1:
        print("one")
    case 2 | 3:
        print("small")
    case _:
        print("other")

match "alpha":
    case value:
        print("capture", value)

match None:
    case None:
        print("none")
    case _:
        print("unexpected")

print(events)

match 8:
    case number if number > 10:
        print("large")
    case number if number > 5:
        print("guard", number)
    case _:
        print("fallback")
