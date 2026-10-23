#!/usr/bin/env python3
"""kupd_peer.py LOG QMP CMD... - opens a Terminal in the guest and runs each CMD in it, waiting for it to finish (run_kernelupd_test.sh reads the results from the serial log)."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
def log(): return open(logp, errors="replace").read()
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
rc = 0
for cmd in sys.argv[3:]:
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    end = time.time() + 400; code = None
    while time.time() < end:
        seg = log()[start:]; c = seg.find("[TERMINAL] cmd: " + cmd)
        m = re.search(r"\[TERMINAL\] exit: \S+ (-?\d+)", seg[c:]) if c >= 0 else None
        if m: code = int(m.group(1)); break
        time.sleep(1)
    print("cmd %r exit %s" % (cmd, code), flush=True)
    if code is None: rc = 1
sys.exit(rc)
