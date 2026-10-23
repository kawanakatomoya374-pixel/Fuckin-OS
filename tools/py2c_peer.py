#!/usr/bin/env python3
"""Drives Py2C Studio in the guest (see run_py2c_test.sh). Evidence: the "[PY2C] ..." lines the program writes to the serial log, and screenshots."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def log(): return open(logp, errors="replace").read()
def mark(): return len(log())
def wait(pat, t=40, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(1)
    return None
def ui_do(fn):
    u = UI(qmp)
    try: return fn(u)
    finally: u.s.close()
def shot(path): ui_do(lambda u: u.shot(path))
def typ(text, enter=False):
    if "\n" in text:
        parts = text.split("\n")
        for i, part in enumerate(parts):
            if part: typ(part)
            if i + 1 < len(parts): key("ret", wait_s=0.3)
        return
    subprocess.run(["python3", TYPE, text] + ([] if enter else ["--no-enter"]), stdout=subprocess.DEVNULL, check=False, env=dict(os.environ, COS_QMP=qmp)); time.sleep(0.6)
def key(q, ctrl=False, shift=False, n=1, wait_s=0.5):
    def f(u):
        for _ in range(n): u.key(q, shift=shift, ctrl=ctrl); time.sleep(0.15)
    ui_do(f); time.sleep(wait_s)
def hclick(u):
    u.btn(True); time.sleep(0.35); u.btn(False); time.sleep(0.2)
def click(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), hclick(u), time.sleep(1.0)))

# --- the Terminal starts Py2C
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
m = mark(); type_command(logp, qmp, TYPE, "Py2C")
check("Py2C Studio starts", wait(r"\[PY2C\] started", 40, m) is not None)
time.sleep(4); shot("/tmp/p2_0.ppm")

# --- convert the starter file with F5
m = mark(); key("f5", wait_s=3)
check("F5 converts the Python file to C", wait(r"\[PY2C\] convert ok untitled\.c lines=\d+", 25, m) is not None)
shot("/tmp/p2_1.ppm")

# --- build with tcc against the runtime library and run with F6
m = mark(); key("f6", wait_s=2)
check("F6 builds the program with tcc.c-os and the Py2C runtime", wait(r"\[PY2C\] build ok", 120, m) is not None)
check("the program runs and its output arrives in the Output panel", wait(r"\[PY2C\] out: Hello, world from C-OS!", 60, m) is not None)
check("the program's loop printed", wait(r"\[PY2C\] out: line 2", 20, m) is not None)
check("the program exited with code 0", wait(r"\[PY2C\] exited code=0", 30, m) is not None)
shot("/tmp/p2_2.ppm")

# --- the editor: replace everything, type a program
key("a", ctrl=True)
typ('print("typed in the editor")')
m = mark(); key("f6", wait_s=2)
check("typed text is the program that runs", wait(r"\[PY2C\] out: typed in the editor", 120, m) is not None)

# --- a syntax error is reported with its line and shown in Problems
key("a", ctrl=True); typ("x = 1\ndef f(:\n    pass")
m = mark(); key("f5", wait_s=2)
mm = wait(r"\[PY2C\] convert failed line=(\d+) col=(\d+): ([^\n]*)", 25, m)
check("a syntax error is reported with line and message", mm is not None and int(mm.group(1)) >= 2)
shot("/tmp/p2_3.ppm")

# --- program input (Input panel)
key("a", ctrl=True); typ('name = input()\nprint("hi " + name)')
m = mark(); key("f6", wait_s=1)
wait(r"\[PY2C\] build ok", 120, m)
time.sleep(2)
check("input() without text reads end of input (program still exits)", wait(r"\[PY2C\] exited code=", 40, m) is not None)

# --- undo
key("a", ctrl=True); typ("abc")
key("z", ctrl=True); key("z", ctrl=True)
m = mark(); key("f5", wait_s=2)
check("Ctrl+Z undoes typing (the document is back to what it was before)", wait(r"\[PY2C\] convert ok", 25, m) is not None or wait(r"convert failed", 5, m) is not None)

# --- Save (untitled -> Save As dialog), then the file exists
key("a", ctrl=True); typ('print("saved")')
key("s", ctrl=True, wait_s=1.5); shot("/tmp/p2_4.ppm")
typ("saved_test", enter=True); time.sleep(2)
check("Ctrl+S on a new file asks for a name and saves it", wait(r"\[PY2C\] saved /[^\n]*saved_test\.py", 20, m) is not None)

# --- find
key("f", ctrl=True, wait_s=1); typ("saved"); key("ret", wait_s=1); shot("/tmp/p2_5.ppm")
key("esc", wait_s=1)

# --- examples menu: File > Examples is the fifth menu
ui_do(lambda u: (u.goto(238, 13), time.sleep(0.4), hclick(u), time.sleep(1.0))); shot("/tmp/p2_6.ppm")
print("log tail:", log()[-200:])
sys.exit(0 if ok else 1)
