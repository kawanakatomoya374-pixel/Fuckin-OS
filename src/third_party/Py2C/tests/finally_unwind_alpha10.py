# try/finallyを横断するreturn/break/continueの脱出順序を固定するCPython差分ケース。
# Pythonはreturn式を評価してからfinallyを実行するため、生成Cも
# 「戻り値の確定 → finally本体 → 例外フレームの復元 → return」の順に実行する。
# break/continueは脱出後も生存するtry（ループの外側のtry）を再実行しない。


def ret_in_try():
    trace = []
    try:
        trace.append("try")
        return "returned"
    finally:
        trace.append("finally")
        print("ret_in_try finally", trace)


print("ret_in_try", ret_in_try())


def ret_value_order():
    order_trace = []

    def make_value():
        order_trace.append("value")
        return 1

    try:
        return make_value()
    finally:
        order_trace.append("finally")
        print("ret_value_order finally", order_trace)


print("ret_value_order", ret_value_order())


def loop_control():
    trace = []
    for x in [1, 2, 3, 4]:
        try:
            if x == 2:
                continue
            if x == 4:
                break
            trace.append(x)
        finally:
            trace.append("f")
    print("loop_control", trace)


loop_control()


def loop_else():
    trace = []
    for x in [1, 2]:
        try:
            if x == 2:
                break
        finally:
            trace.append("f")
    else:
        trace.append("else")
    print("loop_else", trace)


loop_else()


def while_continue():
    trace = []
    n = 0
    while n < 3:
        n += 1
        try:
            if n == 2:
                continue
            trace.append(n)
        finally:
            trace.append("f")
    print("while_continue", trace)


while_continue()


def handler_return():
    trace = []
    try:
        trace.append("try")
        raise ValueError("boom")
    except ValueError as exc:
        trace.append("handler")
        return "handled:" + str(exc)
    finally:
        trace.append("finally")
        print("handler_return finally", trace)


print("handler_return", handler_return())


def propagate():
    trace = []
    try:
        try:
            raise ValueError("boom")
        finally:
            trace.append("inner")
            print("propagate inner", trace)
    except ValueError as exc:
        print("propagate caught", exc)


propagate()


def nested_def():
    trace = []

    def inner_value():
        return "inner"

    try:
        trace.append(inner_value())
        return "outer"
    finally:
        trace.append("f")
        print("nested_def finally", trace)


print("nested_def", nested_def())


def call_inner():
    inner_trace = []

    def inner_captured():
        try:
            return "i"
        finally:
            inner_trace.append("fi")

    try:
        inner_trace.append(inner_captured())
        return "o"
    finally:
        inner_trace.append("fo")
        print("call_inner finally", inner_trace)


print("call_inner", call_inner())


class Runner:
    def run(self):
        try:
            return "method"
        finally:
            print("Runner.run finally")

    def run_loop(self):
        trace = []
        for x in [1, 2]:
            try:
                if x == 2:
                    return "method-loop:" + str(trace)
            finally:
                trace.append(x)
        return "not-reached"


print("Runner.run", Runner().run())
print("Runner.run_loop", Runner().run_loop())


module_trace = []
for x in [1, 2, 3]:
    try:
        if x == 2:
            break
        module_trace.append(x)
    finally:
        module_trace.append("f")
print("module", module_trace)
