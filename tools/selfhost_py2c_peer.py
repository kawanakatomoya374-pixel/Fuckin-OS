#!/usr/bin/env python3
"""Self-hosting: Py2C is cleaned and rebuilt from its source inside the guest, using only what C-OS has (tcc.c-os, C-OS Studio, the SDK) - the build is started
from Py2C Studio's Help menu. Then the freshly built tool and runtime library are used to convert, compile and run programs (see run_selfhost_py2c_test.sh)."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
EX = os.path.join(os.path.dirname(__file__), "..", "src", "third_party", "Py2C", "examples")
ok = True
def log(): return open(logp, errors="replace").read()
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def wait(pat, t=60, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(1)
    return None
def run(cmd, t=120):
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    end = time.time() + t; code = None; seg = ""
    while time.time() < end:
        seg = log()[start:]; c = seg.find("[TERMINAL] cmd: " + cmd)
        m = re.search(r"\[TERMINAL\] exit: \S+ (-?\d+)", seg[c:]) if c >= 0 else None
        if m: code = int(m.group(1)); break
        time.sleep(1)
    return code, [l.split("out: ", 1)[1] for l in seg.splitlines() if "[TERMINAL] out: " in l]
def ui_do(fn):
    u = UI(qmp)
    try: return fn(u)
    finally: u.s.close()
def hclick(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), u.btn(True), time.sleep(0.35), u.btn(False), time.sleep(1.0)))
def expect(name): return subprocess.run([sys.executable, os.path.join(EX, name)], capture_output=True, text=True).stdout.splitlines()

ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()

# 1. throw the build products away
start = len(log()); type_command(logp, qmp, TYPE, "studio /apps/Py2C --make clean")
m = wait(r"\[STUDIO\] make (done|failed): \w+", 300, start)
check("make clean in C-OS Studio removes the build products (%s)" % (m.group(0) if m else "timeout"), m is not None and "done" in m.group(0))
code, out = run("/apps/Py2C/build/python-code-to-c --version", 30)
check("the tool is gone (it cannot be started: exit %s)" % code, code != 0 or not any("Beta" in l for l in out))
code, out = run("tcc -v", 30)
check("the compiler is the one inside C-OS (%s)" % (out[0] if out else "?"), any("tcc version" in l.lower() for l in out))

# 2. build everything again from Py2C Studio's Help menu
m0 = len(log()); type_command(logp, qmp, TYPE, "Py2C")
check("Py2C Studio starts", wait(r"\[PY2C\] started", 60, m0) is not None); time.sleep(4)
hclick(290, 68)                                                              # Help
ui_do(lambda u: u.shot("/tmp/sh_help.ppm"))
m1 = len(log()); hclick(330, 145)                                            # Build Py2C with tcc (C-OS Studio)
m = wait(r"\[STUDIO\] make (done|failed): \w+", 1500, m1)
check("Help > Build Py2C with tcc: C-OS Studio runs the whole make with the on-device compiler (%s)" % (m.group(0) if m else "timeout"), m is not None and "done" in m.group(0))
if not (m and "done" in m.group(0)): print("log tail:", log()[m1:][-1500:])

# 3. use what was just built (back to the Terminal: its button on the taskbar)
hclick(133, 743); time.sleep(2)
code, out = run("/apps/Py2C/build/python-code-to-c --version", 40)
check("the tool built on the device starts (%s)" % (out[0] if out else "?"), code == 0 and any("Beta0.1" in l for l in out))
code, out = run("/apps/Py2C/build/python-code-to-c --self-test", 240)
res = [l for l in out if "Results:" in l]
check("it passes its own --self-test (%s)" % (res[0] if res else "no result"), code == 0 and res and ", 0 failed" in res[0])
code, out = run("/apps/Py2C/build/python-code-to-c /apps/Py2C/examples/containers.py -o /tmp/gen2.c", 120)
check("it converts a Python file to C (exit %s)" % code, code == 0)
code, out = run("tcc -I/apps/Py2C/include/runtime -I/apps/Py2C/include -U__linux__ -DPYTHON_CODE_TO_C_NO_TLS -c /tmp/gen2.c -o /tmp/gen2.o", 300)
check("tcc compiles the generated C (exit %s)" % code, code == 0)
if code != 0: print("\n".join(out[-6:]))
for name in ("fib.py", "class_counter.py", "precision.py"):
    want = expect(name)
    code, out = run("/apps/Py2C/build/python-code-to-c run /apps/Py2C/examples/" + name, 300)
    check("run %s with the tool and runtime built on the device: same output as CPython (%d lines)" % (name, len(want)), out[:len(want)] == want)
sys.exit(0 if ok else 1)
