#!/usr/bin/env python3
"""Differential fuzzer for the JIT: random numeric loop programs must print the same with and without the JIT.
usage: jitfuzz.py SPRINT [COUNT] [SEED] [loops|calls|oop]   (a development tool; not part of the build)"""
import random, subprocess, sys, os, tempfile

def gen(rng):
    ints = ["a", "b", "c"]
    flts = ["x", "y"]
    consts_i = ["0", "1", "2", "3", "7", "-1", "-5", "16", "100", "123457", "9223372036854775807", "4611686018427387904"]
    consts_f = ["0.5", "1.5", "2.0", "-0.25", "3.25", "0.0"]
    def iexpr(d=0):
        r = rng.random()
        if d > 2 or r < 0.3:
            return rng.choice(ints + [rng.choice(consts_i)] + ["i", "j", "k0"])
        if r < 0.4:
            return ("arr[%s %% 8]" % iexpr(d + 1)) if rng.random() < 0.95 else ("arr[%s]" % iexpr(d + 1))
        op = rng.choice(["+", "-", "*", "//", "%", "+", "-", "*"])
        if op in ("//", "%"):
            if rng.random() < 0.8:
                return "(%s %s %s)" % (iexpr(d + 1), op, rng.choice(["2", "3", "7", "8", "-3", "16", "5", "641", "1000"]))
            return "(%s %s (1 + (%s %% 5)))" % (iexpr(d + 1), op, iexpr(d + 1))
        return "(%s %s %s)" % (iexpr(d + 1), op, iexpr(d + 1))
    def fexpr(d=0):
        r = rng.random()
        if d > 2 or r < 0.3:
            return rng.choice(flts + [rng.choice(consts_f)])
        if r < 0.4:
            return "(%s)" % iexpr(d + 1) if False else rng.choice(flts)
        if r < 0.5:
            e = fexpr(d + 1)
            return "math.sqrt(%s * %s)" % (e, e)
        op = rng.choice(["+", "-", "*", "/"])
        rhs = fexpr(d + 1) if op != "/" else rng.choice(["2.0", "3.5", "(2.0 + x * x)", "(1.5 + y * y)"])
        mix = rng.random() < 0.25
        lhs = fexpr(d + 1)
        if mix:
            return "(%s %s %s)" % (lhs, op, rng.choice(["i", "a", "2", "3"]))
        return "(%s %s %s)" % (lhs, op, rhs)
    def cond():
        r = rng.random()
        if r < 0.15:
            return "%s and %s" % (cond0(), cond0())
        if r < 0.3:
            return "%s or %s" % (cond0(), cond0())
        if r < 0.38:
            return "not (%s)" % cond0()
        if r < 0.43:
            return "(%s or %s) and %s" % (cond0(), cond0(), cond0())
        return cond0()
    def cond0():
        if rng.random() < 0.5:
            return "%s %s %s" % (iexpr(1), rng.choice(["<", "<=", ">", ">=", "==", "!="]), iexpr(1))
        return "%s %s %s" % (fexpr(1), rng.choice(["<", "<=", ">", ">=", "==", "!="]), fexpr(1))
    def stmt(ind, d):
        p = "    " * ind
        r = rng.random()
        if r < 0.3:
            return [p + "%s = (%s) %% 1000003" % (rng.choice(ints), iexpr())] if rng.random() < 0.9 else [p + "%s = %s" % (rng.choice(ints), iexpr())]
        if r < 0.45:
            return [p + "%s = %s" % (rng.choice(flts), fexpr())]
        if r < 0.55:
            return [p + "%s = (%s + %s) %% 1000003" % ((v := rng.choice(ints)), v, iexpr(1))]
        if r < 0.62:
            return [p + "arr[%s %% 8] = %s" % (iexpr(1), iexpr(1))]
        if r < 0.67:
            return [p + "g = (g + %s) %% 1000003" % iexpr(1)]
        if r < 0.72:
            return [p + "h = h * 0.5 + %s" % fexpr(1)]
        if r < 0.9 and d < 2:
            out = [p + "if %s:" % cond()]
            for _ in range(rng.randint(1, 3)): out += stmt(ind + 1, d + 1)
            if rng.random() < 0.4:
                out.append(p + "else:")
                for _ in range(rng.randint(1, 2)): out += stmt(ind + 1, d + 1)
            return out
        if d < 2 and rng.random() < 0.2:
            out = [p + "for v in arr:"]
            out.append(p + "    a = (a + v) % 1000003")
            for _ in range(rng.randint(0, 2)): out += stmt(ind + 1, d + 1)
            return out
        if d < 2 and rng.random() < 0.2:
            kv = "k%d" % d
            out = [p + kv + " = 0", p + "while %s < %d:" % (kv, rng.randint(1, 5)), p + "    %s += 1" % kv]
            if rng.random() < 0.5: out += [p + "    if %s:" % cond(), p + "        continue"]
            if rng.random() < 0.3: out += [p + "    if %s:" % cond(), p + "        break"]
            for _ in range(rng.randint(1, 2)): out += stmt(ind + 1, d + 1)
            return out
        if d < 1:
            out = [p + "for j in range(%d):" % rng.randint(1, 4)]
            for _ in range(rng.randint(1, 3)): out += stmt(ind + 1, d + 1)
            return out
        return [p + "%s = %s" % (rng.choice(ints), iexpr())]
    lines = ["import math", "let g = 3", "let h = 1.5", "func main(n):",
             "    let a = %s" % rng.choice(consts_i[:7]), "    let b = %s" % rng.choice(consts_i[:7]), "    let c = 5",
             "    let x = 1.5", "    let y = -2.25", "    let j = 0", "    let k0 = 0", "    let k1 = 0", "    let k2 = 0", "    let v = 0", "    let arr = [1, 2, 3, 4, 5, 6, 7, 8]",
             "    for i in range(n):"]
    for _ in range(rng.randint(3, 8)): lines += stmt(2, 0)
    lines += ["    print(a, b, c, x, y, g, h, arr)", "main(%d)" % rng.choice([5, 20, 100, 300]), "print(g, h)"]
    return "\n".join(lines) + "\n"


def gen2(rng):
    """programs built from small functions that call each other, recurse, mutate module variables and upvalues, mix
    ints and floats and sometimes fail"""
    nf = rng.randint(2, 5)
    out = ["import math", "let g = 1", "let h = 0.5", "let cnt = 0"]
    names = ["f%d" % i for i in range(nf)]
    def ex(depth, vars_, callable_):
        r = rng.random()
        if depth > 2 or r < 0.3:
            return rng.choice(vars_ + [rng.choice(["0", "1", "2", "3", "7", "-4", "100", "0.5", "2.5", "g", "h", "cnt"])])
        if r < 0.5 and callable_:
            return "%s(%s)" % (rng.choice(callable_), ex(depth + 1, vars_, callable_))
        op = rng.choice(["+", "-", "*", "+", "-", "%", "//"])
        if op in ("%", "//"):
            return "(%s %s %s)" % (ex(depth + 1, vars_, callable_), op, rng.choice(["3", "7", "2", "(1 + (cnt % 3))"]))
        return "(%s %s %s)" % (ex(depth + 1, vars_, callable_), op, ex(depth + 1, vars_, callable_))
    for i, nm in enumerate(names):
        callable_ = names[:i]          # only earlier functions: no unbounded recursion
        selfrec = rng.random() < 0.4
        out.append("func %s(n):" % nm)
        out.append("    cnt = (cnt + 1) % 1000")
        if selfrec:
            out.append("    n = n % 12")
            out.append("    if n < 2:")
            out.append("        return n")
            out.append("    return (%s(n - 1) + %s(n - 2)) %% 1000003" % (nm, nm))
        else:
            for _ in range(rng.randint(1, 3)):
                k = rng.random()
                if k < 0.3:
                    out.append("    g = (g + %s) %% 1000003" % ex(1, ["n"], callable_))
                elif k < 0.45:
                    out.append("    h = h * 0.5 + %s" % ex(1, ["n"], callable_))
                elif k < 0.7:
                    out.append("    if %s %s %s:" % (ex(1, ["n"], callable_), rng.choice(["<", ">", "==", "!="]), ex(1, ["n"], callable_)))
                    out.append("        return %s" % ex(1, ["n"], callable_))
                else:
                    out.append("    n = %s" % ex(1, ["n"], callable_))
            out.append("    return %s" % ex(0, ["n"], callable_))
    out += ["func mk():", "    let c = 0", "    func inc(d):", "        c += d", "        return c", "    return inc"]
    out += ["let inc1 = mk()", "let inc2 = mk()", "let acc = 0", "let facc = 0.0"]
    drv = rng.choice([30, 120, 400])
    out.append("for i in range(%d):" % drv)
    for _ in range(rng.randint(2, 5)):
        nm = rng.choice(names)
        k = rng.random()
        arg = rng.choice(["i", "i % 7", "(i % 5) + 0.5", "3", "i * 2"]) if not nm in names[:0] else "i"
        if k < 0.45:
            out.append("    acc = (acc + %s(%s)) %% 1000003" % (nm, arg if "selfrec" else arg))
        elif k < 0.6:
            out.append("    facc = facc * 0.5 + %s(%s)" % (nm, arg))
        elif k < 0.8:
            out.append("    acc = (acc + inc%d(%s)) %% 1000003" % (rng.choice([1, 2]), rng.choice(["i", "1", "2.5", "i % 3"])))
        else:
            out.append("    try:")
            out.append("        acc = (acc + 1000 // (%s(%s) %% 3)) %% 1000003" % (nm, arg))
            out.append("    catch e:")
            out.append("        acc += 1")
    out.append("    if i % 17 == 0:")
    out.append("        print(i, acc, facc, g, h, cnt)")
    out.append("print(acc, facc, g, h, cnt, inc1(0), inc2(0))")
    return "\n".join(out) + "\n"


def gen3(rng):
    """classes with fields, init, methods that call each other, an overriding subclass used at the same call sites,
    constructors called from methods, field updates, and the occasional error"""
    def fe(depth, fields, meths):
        r = rng.random()
        if depth > 2 or r < 0.3:
            return rng.choice(["self.%s" % f for f in fields] + ["n", "1", "2", "3", "0.5", "g"])
        if r < 0.45 and meths:
            return "self.%s(%s)" % (rng.choice(meths), fe(depth + 1, fields, []))
        op = rng.choice(["+", "-", "*", "+", "-"])
        return "(%s %s %s)" % (fe(depth + 1, fields, meths), op, fe(depth + 1, fields, meths))
    fields = ["a", "b", "c"]
    out = ["let g = 2", "let live = 0"]
    out.append("class A:")
    for f in fields:
        out.append("    let %s = %s" % (f, rng.choice(["0", "1", "0.5", "7"])))
    out.append("    func init(self, a, b):")
    out.append("        self.a = a")
    out.append("        self.b = b")
    meths = []
    for i in range(rng.randint(2, 4)):
        nm = "m%d" % i
        out.append("    func %s(self, n):" % nm)
        for _ in range(rng.randint(0, 2)):
            out.append("        self.%s = %s %% 100003" % (rng.choice(fields), fe(1, fields, meths)))
        if rng.random() < 0.25:
            out.append("        if %s > %s:" % (fe(1, fields, []), fe(1, fields, [])))
            out.append("            return %s" % fe(1, fields, meths))
        out.append("        return %s" % fe(0, fields, meths))
        meths.append(nm)
    out.append("    func spawn(self, n):")
    out.append("        return A(self.a + n, self.b)")
    out.append("class B extends A:")
    out.append("    let d = 3")
    ov = rng.sample(meths, rng.randint(1, len(meths)))
    for nm in ov:
        out.append("    override func %s(self, n):" % nm)
        out.append("        return super.%s(n) * 2 + self.d" % nm)
    out += ["let objs = [A(1, 2), B(3, 4), A(5, 6)]", "let p = A(1, 1)", "let q = B(2, 2)", "let acc = 0", "let facc = 0.0", "let cnt = 0"]
    out.append("for i in range(%d):" % rng.choice([20, 80, 300]))
    for _ in range(rng.randint(2, 5)):
        k = rng.random()
        o = rng.choice(["p", "q", "objs[i % 3]"])
        m = rng.choice(meths)
        if k < 0.4:
            out.append("    acc = (acc + %s.%s(i %% 5)) %% 1000003" % (o, m))
        elif k < 0.55:
            out.append("    facc = facc * 0.5 + %s.%s(i %% 3)" % (o, m))
        elif k < 0.7:
            out.append("    %s.%s = (%s.%s + i) %% 977" % (o, rng.choice(fields), o, rng.choice(fields)))
        elif k < 0.8:
            out.append("    p = p.spawn(i % 4)")
        elif k < 0.9:
            out.append("    q = B(i, i + 1)")
        else:
            out.append("    try:")
            out.append("        %s.zz = 1" % o)
            out.append("    catch e:")
            out.append("        cnt += 1")
    out.append("    if i % 13 == 0:")
    out.append("        print(i, acc, facc, cnt, p.a, p.b, q.a, q.c)")
    out.append("print(acc, facc, cnt, p.a, q.a, objs[0].a, objs[1].b)")
    return "\n".join(out) + "\n"

def run(sprint, path, env):
    e = dict(os.environ); e.update(env)
    p = subprocess.run([sprint, "run", "--deterministic", path], capture_output=True, text=True, env=e, timeout=20)
    return p.returncode, p.stdout, p.stderr

def main():
    sprint = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    mode = sys.argv[4] if len(sys.argv) > 4 else "loops"
    bad = 0
    tmp = tempfile.mkdtemp(prefix="jitfuzz")
    for k in range(n):
        rng = random.Random(seed * 100003 + k)
        src = gen3(rng) if mode == "oop" else gen2(rng) if mode == "calls" else gen(rng)
        path = os.path.join(tmp, "fz_%d.sp" % k)
        open(path, "w").write(src)
        a = run(sprint, path, {"SPRINT_JIT": "0"})
        b = run(sprint, path, {"SPRINT_JIT_THRESHOLD": "1"})
        c = run(sprint, path, {"SPRINT_JIT_THRESHOLD": "3"})
        d = run(sprint, path, {"SPRINT_JIT_THRESHOLD": "2", "SPRINT_GC_STRESS": "1"}) if mode == "oop" else c   # a collection at every allocation
        if a != b or a != c or a != d:
            bad += 1
            print("MISMATCH", path)
            print(" interp:", a[0], a[1][-200:], a[2][-200:])
            print(" jit   :", b[0], b[1][-200:], b[2][-200:])
            if bad >= 5: break
        else:
            os.remove(path)
    print("done, mismatches:", bad)
main()
