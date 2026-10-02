log = []

def mark(value):
    log.append(value)
    return value

print("bool", 0 or 7, 3 and 5, [] or "fallback")
print("short", False and mark("bad"), True or mark("bad2"), log)

for_value = []
for x in [1, 2, 3]:
    if x == 2:
        break
    for_value.append(x)
else:
    for_value.append("else")
print("for_break", for_value)

for_done = []
for x in [1, 2]:
    for_done.append(x)
else:
    for_done.append("else")
print("for_done", for_done)

def finally_value():
    try:
        return "try"
    finally:
        return "finally"

print("finally", finally_value())

class CM:
    def __enter__(self):
        log.append("enter")
        return self
    def __exit__(self, typ, value, tb):
        log.append("exit")
        return False

def with_clean():
    with CM() as cm:
        log.append("body")
    return log

print("with_clean", with_clean())
