#!/usr/bin/env python3
"""Right-click menus in the apps (see run_ctxmenu_test.sh). Evidence: the "[CTXMENU] ..." lines the apps write to the serial log (a menu opened with N items,
the item that was chosen) and screenshots. Py2C Studio, C-OS Studio and the File Manager are started from the Terminal."""
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
def wait(pat, t=20, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(0.5)
    return None
def ui_do(fn):
    u = UI(qmp)
    try: return fn(u)
    finally: u.s.close()
def shot(path): ui_do(lambda u: u.shot(path))
def key(q, ctrl=False, shift=False, wait_s=0.6):
    ui_do(lambda u: u.key(q, shift=shift, ctrl=ctrl)); time.sleep(wait_s)
def hclick(u, button='left'):
    u.btn(True, button); time.sleep(0.35); u.btn(False, button); time.sleep(0.2)
def click(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), hclick(u), time.sleep(0.8)))
def rclick(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), hclick(u, 'right'), time.sleep(0.8)))
def typ(text, enter=False):
    subprocess.run(["python3", TYPE, text] + ([] if enter else ["--no-enter"]), stdout=subprocess.DEVNULL, check=False, env=dict(os.environ, COS_QMP=qmp)); time.sleep(0.6)

# the Terminal
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()

# --- Py2C Studio: the editor, a tab, the output panel
m = mark(); type_command(logp, qmp, TYPE, "Py2C")
check("Py2C Studio starts", wait(r"\[PY2C\] started", 40, m) is not None)
time.sleep(4); shot("/tmp/cm_0.ppm")
EDX, EDY = int(os.environ.get("ED_X", 500)), int(os.environ.get("ED_Y", 300))
m = mark(); rclick(EDX, EDY); shot("/tmp/cm_1.ppm")
mm = wait(r"\[CTXMENU\] open items=(\d+) first=([^\n]*)", 8, m)
check("right-click in the editor opens a menu", mm is not None and int(mm.group(1)) >= 10)
check("the system's window menu does not open over the app's own menu", wait(r"Window actions", 1, m) is None)
key("down"); key("down"); key("ret", wait_s=1.0)
check("Down, Down, Enter chooses Select All", wait(r"\[CTXMENU\] chose Select All", 6, m) is not None)
m = mark(); rclick(EDX, EDY)
mm = wait(r"\[CTXMENU\] open items=(\d+)", 6, m)
check("the editor menu opens again with a selection", mm is not None)
key("esc")
# the tab
m = mark(); rclick(350, 137); shot("/tmp/cm_2.ppm")
mm = wait(r"\[CTXMENU\] open items=(\d+) first=Close", 6, m)
check("right-click on a tab opens the tab menu", mm is not None and int(mm.group(1)) == 5)
key("esc")
# the bottom panel
m = mark(); rclick(600, 600); shot("/tmp/cm_3.ppm")
mm = wait(r"\[CTXMENU\] open items=(\d+) first=Copy", 6, m)
check("right-click in the bottom panel opens the panel menu", mm is not None)
key("esc")
# the sidebar file list
m = mark(); rclick(150, 250); shot("/tmp/cm_4.ppm")
mm = wait(r"\[CTXMENU\] open items=(\d+) first=Open", 6, m)
check("right-click on a file in the sidebar opens the file menu", mm is not None)
key("esc")
print("log tail:", log()[-300:])
sys.exit(0 if ok else 1)
