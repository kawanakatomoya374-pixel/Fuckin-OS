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
m, seg = run("tcc -I/system/sdk/include /apps/Py2C/cos/fiotest.c -o /tmp/fiotest.c-os", r"exit: tcc", 120)
print("compile:", outputs(seg), flush=True)
m, seg = run("tcc -O1 -U__linux__ -DP2C_COS_PRELOAD -I/apps/Py2C/include -c /apps/Py2C/src/core/python_code_to_c.c -o /tmp/pc.o", r"exit: tcc", 240)
print("compile2:", outputs(seg)[:5], flush=True)
m, seg = run("/tmp/fiotest.c-os /tmp/pc.o", r"FIO DONE|exit: fiotest", 120)
print("\n".join(l for l in outputs(seg) if l.startswith("FIO")), flush=True)
m, seg = run("tcc -ar rcs /tmp/t.a /tmp/pc.o", r"exit: tcc", 120)
print("ar:", outputs(seg), flush=True)
m, seg = run("tcc -ar t /tmp/t.a", r"exit: tcc", 120)
print("ar t:", outputs(seg), flush=True)
