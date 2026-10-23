#!/usr/bin/env python3
"""Drives the Terminal: the Py2C command line (/bin/python-code-to-c), the source tree under /apps/Py2C, and the full build of Py2C with the on-device
TinyCC through C-OS Studio (studio PATH --make install); after that the freshly built tool and runtime are used again.  Expected program output comes from CPython."""
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
def run(cmd, pat, t=60, prog=True):
    """types the command, waits for `pat`, and (for a program) for ITS exit line, so the next command starts from a quiet log"""
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    m = wait(pat, t, start)
    if prog and m is not None:
        end = time.time() + t
        while time.time() < end:
            l = log(); c = l.find("[TERMINAL] cmd: " + cmd, start)
            if c >= 0 and l.find("[TERMINAL] exit:", c) >= 0: break
            time.sleep(1)
    return m, log()[start:]
def outputs(seg): return [l.split("out: ", 1)[1] for l in seg.splitlines() if "[TERMINAL] out: " in l]
def expect_lines(name):
    return subprocess.run([sys.executable, os.path.join(EX, name)], capture_output=True, text=True).stdout.splitlines()

ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
run("cd /bin", r"cd /bin", 10, prog=False); time.sleep(1)
# a window program: the gui library on a real C-OS window (clicks in, results out, close ends the program)
def ui_do(fn):
    u = UI(qmp)
    try: return fn(u)
    finally: u.s.close()
def find_client():
    import numpy as np
    from PIL import Image
    ui_do(lambda u: u.shot("/tmp/p2gui.ppm"))
    a = np.asarray(Image.open("/tmp/p2gui.ppm").convert("RGB"))
    ys, xs = np.nonzero((a[:, :, 0] == 48) & (a[:, :, 1] == 56) & (a[:, :, 2] == 64))      # the keys and the entry (COLOR_KEY): the Terminal has no such colour
    if len(xs) < 20000: return None
    x0, y0 = int(xs.min()) - 8, int(ys.min()) - 8          # the entry starts 8 pixels inside the client area (ORIGIN)
    return x0, y0, x0 + 340, y0 + 280
start = len(log()); type_command(logp, qmp, TYPE, "./python-code-to-c run /apps/Py2C/examples/gui_calc.py")
rect = None; end = time.time() + 240
while time.time() < end and rect is None:
    time.sleep(4); rect = find_client()
check("gui_calc.py opens a C-OS window (client area found at %s)" % (rect,), rect is not None)
if rect:
    x0, y0, x1, y1 = rect
    def click(x, y):
        ui_do(lambda u: (u.goto(x0 + x, y0 + y), time.sleep(0.4), u.btn(True), time.sleep(0.35), u.btn(False), time.sleep(0.4)))
    for n, (cx, cy) in enumerate(((48, 84), (300, 240), (132, 84), (216, 240))):      # 7  +  8  =
        ui_do(lambda u: u.shot("/tmp/p2gui_before%d.ppm" % n))
        click(cx, cy)
        ui_do(lambda u: u.shot("/tmp/p2gui_after%d.ppm" % n))
        print("click", n, "client at", x0, y0, "->", x0 + cx, y0 + cy, flush=True)
    time.sleep(3); ui_do(lambda u: u.shot("/tmp/p2gui2.ppm"))
    m = wait(r"out: result 15", 60, start)
    seg = log()[start:]
    check("clicking 7 + 8 = in the window prints result 15 (keys: %s)" % re.findall(r"out: key (\S)", seg), m is not None)
    ui_do(lambda u: (u.goto(x1 - 20, y0 - 16), time.sleep(0.4), u.btn(True), time.sleep(0.35), u.btn(False)))   # the window's close button
    m = wait(r"out: closed 15", 60, start)
    check("closing the window ends mainloop (the program printed closed 15)", m is not None)
    end = time.time() + 60
    while time.time() < end and log().find("[TERMINAL] exit:", log().find("gui_calc.py", start)) < 0: time.sleep(1)


sys.exit(0 if ok else 1)
