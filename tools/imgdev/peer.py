#!/usr/bin/env python3
"""Drives the Terminal: runs imgdev (tools/imgdev/imgdev.c) and compares each decoded image with the Pillow reference of manifest.json."""
import json, os, re, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "..", "qmp_type_text.py")
MAN = json.load(open(os.environ.get("IMGDEV_MANIFEST", "build/imgdev_assets/manifest.json")))
ok = True
def log(): return open(logp, errors="replace").read()
def wait(pat, t, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(1)
    return None
def check(name, cond):
    global ok
    print(("PASS " if cond else "FAIL ") + name, flush=True)
    if not cond: ok = False
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()

start = len(log())
type_command(logp, qmp, TYPE, "imgdev")
done = wait(r"IMGDEV DONE", 240, start)
seg = log()[start:]
check("imgdev finished (the kernel decoders neither crashed nor hung)", done is not None)
check("the test images are installed", "no test images" not in seg)
res = {}
for m in re.finditer(r"IMGDEV (\S+) rc=(-?\d+) w=(\d+) h=(\d+) ms=(\d+) grid=(\S*)", seg):
    res[m.group(1)] = dict(rc=int(m.group(2)), w=int(m.group(3)), h=int(m.group(4)), ms=int(m.group(5)), grid=m.group(6))
for name, exp in MAN.items():
    r = res.get(name)
    if r is None: check("%s: reported (%s)" % (name, exp["note"]), False); continue
    if exp["grid"] is None:
        print("INFO %s (%s): rc=%d size=%dx%d - no crash" % (name, exp["note"], r["rc"], r["w"], r["h"]), flush=True)
        continue
    good = r["rc"] == 0 and abs(r["w"] - exp["w"]) <= 2 and abs(r["h"] - exp["h"]) <= 2
    why = "rc=%d size=%dx%d expected %dx%d" % (r["rc"], r["w"], r["h"], exp["w"], exp["h"])
    if good and r["grid"]:
        cells = [[int(c[i:i + 2], 16) for i in (0, 2, 4)] for c in r["grid"].split(",")]
        diffs = [abs(a - b) for ca, cb in zip(cells, exp["grid"]) for a, b in zip(ca, cb)]
        mean, mx = sum(diffs) / len(diffs), max(diffs)
        good = mean <= 6 and mx <= 40
        why += " colours: mean diff %.1f max %d" % (mean, mx)
    check("%s: %s (%d ms) %s" % (name, exp["note"], r["ms"], why), good)
start = len(log())
type_command(logp, qmp, TYPE, "imgdev leak")
done = wait(r"IMGDEV DONE", 600, start)
m = re.search(r"IMGDEV LEAK kheap_used before=(\d+) after=(\d+) delta=(-?\d+)", log()[start:])
check("repeated decoding of every file leaves the kernel heap where it was (delta %s bytes)" % (m.group(3) if m else "?"), bool(m) and abs(int(m.group(3))) < 262144)
check("the guest still answers after all of it", wait(r"IMGDEV DONE", 5, start) is not None)
sys.exit(0 if ok else 1)
