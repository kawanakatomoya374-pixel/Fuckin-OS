events = []

def take(value):
    events.append(value)
    return value

print(f"{take(3) + 1}:{take(4) * 2}")
print(events)
print(f"{take(5):04d}")
print(events)
print(f"{take(6) + take(7)}")
print(events)
