#!/usr/bin/env python3
"""The OS-update notice on the guest (see run_osupdate_test.sh): the taskbar notices a signed OS release on the update server by itself, shows a toast, the C-OS icon
and a balloon; a click on the icon installs the release. Screenshots go to /tmp/osu_*.ppm."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
SRV = os.environ["UPD_SRV"]; KEY = os.path.join(os.path.dirname(__file__), "..", "build", "update", "update_key.pem")
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
URL = "http://192.168.70.2:8123/"
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
def ui_do(fn):
    u = UI(qmp)
    try: return fn(u)
    finally: u.s.close()
def shot(p): ui_do(lambda u: u.shot(p))
def click(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), u.btn(True), time.sleep(0.35), u.btn(False), time.sleep(1.0)))
def run(cmd, t=60):
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    end = time.time() + t; code = None; seg = ""
    while time.time() < end:
        seg = log()[start:]; c = seg.find("[TERMINAL] cmd: " + cmd)
        m = re.search(r"\[TERMINAL\] exit: \S+ (-?\d+)", seg[c:]) if c >= 0 else None
        if m: code = int(m.group(1)); break
        time.sleep(1)
    return code, "\n".join(l.split("out: ", 1)[1] for l in seg.splitlines() if "[TERMINAL] out: " in l)
def publish(serial, items, os_version=None, notes=""):
    for f in os.listdir(SRV):
        if f.endswith(".bin") or f == "manifest.cosup": os.unlink(os.path.join(SRV, f))
    args = ["python3", os.path.join(os.path.dirname(__file__), "mkupdate.py"), "--out", SRV, "--key", KEY, "--serial", str(serial)]
    if os_version: args += ["--os-version", os_version, "--os-notes", notes]
    subprocess.run(args + items, check=True, stdout=subprocess.DEVNULL)
P2 = os.path.join(SRV, "updtest2.c-os")

ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
shot("/tmp/osu_0.ppm")
check("no update icon before anything is published", "[TASKBAR] update available" not in log())

# the system is pointed at the server and told to look every minute; nothing is published yet
code, out = run("cosupdate url " + URL, 30)
code, out = run("cosupdate auto 1", 30)
check("the background check is set to every minute (%s)" % out.strip(), code == 0 and "every 1 minute" in out)
time.sleep(70)
check("the taskbar's own check ran and found nothing to do", "[TASKBAR] update check started" in log() and "[TASKBAR] update available" not in log())

# an OS release appears on the server
start = len(log())
publish(200, ["updtest:2:/bin/updtest.c-os:" + P2, "test:2:/bin/test.c-os:" + P2], "4.1.0", "Faster boot and new apps")
m = wait(r"\[TASKBAR\] update available os=4\.1\.0 serial=200", 150, start)
check("the taskbar noticed the OS release by itself (%s)" % (m.group(0) if m else "no line"), m is not None)
check("cosupdate announced it once", wait(r"\[UPDATE\] announced: serial 200 os 4\.1\.0", 10, start) is not None)
check("the toast was sent: C-OSのアップデートが配信されました", wait(r"\[NOTIFY\] toast: C-OS - C-OSのアップデートが配信されました", 10, start) is not None)
shot("/tmp/osu_1.ppm")
import numpy as np
from PIL import Image
a = np.asarray(Image.open("/tmp/osu_1.ppm").convert("RGB"))
reg = a[8:90, 690:1024]
toast = int(((reg[:, :, 0] == 0x50) & (reg[:, :, 1] == 0x82) & (reg[:, :, 2] == 0xC8)).sum())
check("the toast is on the screen (top right, %d border pixels)" % toast, toast > 100)
bal = a[560:740, 600:1024]
balloon = int(((bal[:, :, 0] == 0x5C) & (bal[:, :, 1] == 0x9B) & (bal[:, :, 2] == 0xFF)).sum())
check("the balloon above the taskbar is on the screen (%d border pixels)" % balloon, balloon > 200)
code, out = run("/bin/updtest.c-os", 15)
check("nothing was installed yet: an announcement is not an installation", "updtest version" not in out)

# a click on the C-OS icon installs it, with no further question
start = len(log())
ICON = (int(os.environ.get("ICON_X", 676)), int(os.environ.get("ICON_Y", 744)))
click(*ICON)
check("the click started the update", wait(r"\[TASKBAR\] update started", 20, start) is not None)
check("cosupdate installed the release", wait(r"\[UPDATE\] done: 2 installed, 0 failed", 180, start) is not None)
time.sleep(3); shot("/tmp/osu_2.ppm")
check("the taskbar shows that it finished", wait(r"\[TASKBAR\] update finished", 30, start) is not None)
code, out = run("/bin/updtest.c-os", 60)
check("the installed program runs: %s" % out.strip(), "updtest version 2" in out)
code, out = run("cosupdate status", 30)
check("status names the OS release (%s)" % out.strip().splitlines()[0][:100], "OS release: 4.1.0" in out)

# the same serial does not announce again
start = len(log())
code, out = run("cosupdate poll", 60)
check("a check with nothing new announces nothing", "announced" not in out and "[TASKBAR] update available" not in log()[start:])
sys.exit(0 if ok else 1)
