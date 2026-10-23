#!/usr/bin/env python3
"""Drives the Terminal to run the loader checks of tools/dltest (see run_dl_test.sh): each check prints "DLTEST PASS|FAIL ..." on the serial log."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ok = True
def log(): return open(logp, errors="replace").read()
def wait(pat, t=60, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(1)
    return None
def run(text):
    from qmp_ui import type_command
    type_command(logp, qmp, TYPE, text)
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
MODES = [("leak", 120), ("share", 120), ("open", 60), ("deps", 60), ("fail", 60), ("flags", 60), ("api", 60), ("env", 60), ("posix", 60), ("soak", 90)]
if os.environ.get("DL_MODES"): MODES = [m for m in MODES if m[0] in os.environ["DL_MODES"].split(",")]
for mode, patience in MODES:
    start = len(log())
    run("dltest " + mode)
    done = wait(r"DLTEST DONE failures=\d+", patience, start)
    seg = log()[start:]
    for m in re.finditer(r"DLTEST (PASS|FAIL|INFO) (.*)", seg):
        print(("PASS " if m.group(1) == "PASS" else "FAIL " if m.group(1) == "FAIL" else "INFO ") + m.group(2), flush=True)
        if m.group(1) == "FAIL": ok = False
    if not done: print("FAIL dltest %s did not finish" % mode, flush=True); ok = False
sys.exit(0 if ok else 1)
