# finally内のreturn/break/continueの回帰。
# Pythonではfinally本体を実行したうえで、進行中の制御フローを上書きする。

log = []


def f():
    try:
        return 1
    finally:
        log.append("fin")


def g():
    try:
        return 2
    finally:
        return 3


def h():
    out = []
    for i in range(5):
        try:
            if i == 1:
                continue
            if i == 3:
                break
            out.append(i)
        finally:
            out.append(-i)
    return out


def k():
    try:
        raise ValueError("boom")
    finally:
        return "suppressed"


def m():
    try:
        return "inner"
    finally:
        try:
            return "outer"
        finally:
            log.append("nested")


def nested_loop():
    out = []
    while True:
        try:
            out.append("body")
            break
        finally:
            out.append("cleanup")
    return out


print(f(), log)
print(g())
print(h())
print(k())
print(m(), log)
print(nested_loop())
