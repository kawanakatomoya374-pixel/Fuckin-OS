#!/usr/bin/env python3
"""Opens the Terminal in the guest, starts a program given on the command line and takes screenshots (/tmp/mc_N.ppm) every few seconds.
usage: mc_peer.py LOG QMP "command" [seconds] [after-start keys...]"""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
secs = int(sys.argv[4]) if len(sys.argv) > 4 else 30
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
app = os.environ.get("MC_START", "term")
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type(app); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
for c in ([] if app != "term" else cmd.split("|||")):
    type_command(logp, qmp, TYPE, c); time.sleep(2)
for spec in filter(None, os.environ.get("MC_CLICKS", "").split(";")):
    x, y = map(int, spec.split(","))
    ui = UI(qmp)
    try: ui.goto(x, y); time.sleep(0.5); ui.click()
    finally: ui.s.close()
    time.sleep(3)
t0 = time.time(); n = 0
while time.time() - t0 < secs:
    time.sleep(6)
    ui = UI(qmp)
    try: ui.shot("/tmp/mc_%d.ppm" % n)
    finally: ui.s.close()
    n += 1
