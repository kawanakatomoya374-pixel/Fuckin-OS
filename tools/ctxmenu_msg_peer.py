#!/usr/bin/env python3
"""Right-click menus in Messenger (see run_ctxmenu_msg_test.sh); evidence: "[CTXMENU] ..." lines on the serial log."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI
logp, qmp = sys.argv[1], sys.argv[2]
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def log(): return open(logp, errors="replace").read()
def mark(): return len(log())
def wait(pat, t=8, since=0):
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
def key(q): ui_do(lambda u: u.key(q)); time.sleep(0.6)
def rclick(x, y):
    def f(u):
        u.goto(x, y); time.sleep(0.4); u.btn(True, 'right'); time.sleep(0.35); u.btn(False, 'right'); time.sleep(0.8)
    ui_do(f)
def shot(p): ui_do(lambda u: u.shot(p))
def case(name, x, y, first, minimum=1, shotname=None):
    m = mark(); rclick(x, y)
    if shotname: shot(shotname)
    mm = wait(r"\[CTXMENU\] open items=(\d+) first=" + first, 6, m)
    check(name, mm is not None and int(mm.group(1)) >= minimum)
    return m
import subprocess
subprocess.run(["python3", os.path.join(os.path.dirname(__file__), "qmp_type_text.py"), "context menu test"], stdout=subprocess.DEVNULL, env=dict(os.environ, COS_QMP=qmp)); time.sleep(2)
case("Messenger: right-click on your own card", 250, 163, "Copy my ID", 4)
key("esc")
case("Messenger: right-click on a conversation", 270, 277, "Open", 4, "/tmp/cmm_1.ppm")
key("esc")
case("Messenger: right-click on a message", 926, 603, "Reply", 5, "/tmp/cmm_2.ppm")
m = mark(); key("down"); key("down"); key("ret")
check("Messenger: Down, Down, Enter chooses Copy", wait(r"\[CTXMENU\] chose Copy", 6, m) is not None)
case("Messenger: right-click on the input line", 650, 678, "Paste", 6, "/tmp/cmm_3.ppm")
key("esc")
case("Messenger: right-click on the conversation header", 600, 190, "Actions", 2)
key("esc")
case("Messenger: right-click on the message area", 600, 400, "Jump", 2)
key("esc")
sys.exit(0 if ok else 1)
