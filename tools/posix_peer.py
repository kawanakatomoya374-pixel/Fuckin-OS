#!/usr/bin/env python3
"""Runs posixtest in the Terminal (see run_posix_test.sh): every check prints "POSIXTEST PASS|FAIL ..." on the serial log."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
def log(): return open(logp, errors="replace").read()
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
start = len(log())
type_command(logp, qmp, TYPE, os.environ.get("POSIX_CMD", "posixtest"))
end = time.time() + (180 if "POSIX_CMD" not in os.environ else 40)
while time.time() < end and not re.search(r"POSIXTEST DONE failures=\d+", log()[start:]): time.sleep(1)
seg = log()[start:]
ok = True
for m in re.finditer(r"POSIXTEST (PASS|FAIL|INFO) (.*)", seg):
    print(m.group(1) + " " + m.group(2), flush=True)
    if m.group(1) == "FAIL": ok = False
d = re.search(r"POSIXTEST DONE failures=(\d+)", seg)
if not d: print("FAIL posixtest did not finish", flush=True); ok = False
sys.exit(0 if ok else 1)
