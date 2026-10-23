#!/usr/bin/env python3
"""App updates on the guest (see run_update_test.sh): packages are made here with tools/mkupdate.py into the directory the host serves, the guest runs cosupdate."""
import json, os, re, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
SRV = os.environ["UPD_SRV"]; KEY = os.path.join(os.path.dirname(__file__), "..", "build", "update", "update_key.pem")
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
URL = "http://192.168.70.2:8123/"      # the host as the guest sees it (QEMU user networking, net=192.168.70.0/24)
PHASE = sys.argv[3] if len(sys.argv) > 3 else "1"
ok = True
def log(): return open(logp, errors="replace").read()
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def run(cmd, t=120):
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    end = time.time() + t; code = None; seg = ""
    while time.time() < end:
        seg = log()[start:]; c = seg.find("[TERMINAL] cmd: " + cmd)
        m = re.search(r"\[TERMINAL\] exit: \S+ (-?\d+)", seg[c:]) if c >= 0 else None
        if m: code = int(m.group(1)); break
        time.sleep(1)
    return code, "\n".join(l.split("out: ", 1)[1] for l in seg.splitlines() if "[TERMINAL] out: " in l)
def publish(serial, items, bad_sig=False, tamper=None):
    """replaces the served package; items: NAME:VERSION:PATH:FILE"""
    for f in os.listdir(SRV):
        if f.endswith(".bin") or f == "manifest.cosup": os.unlink(os.path.join(SRV, f))
    args = ["python3", os.path.join(os.path.dirname(__file__), "mkupdate.py"), "--out", SRV, "--key", KEY, "--serial", str(serial)] + (["--bad-signature"] if bad_sig else []) + items
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    if tamper:
        with open(os.path.join(SRV, tamper), "r+b") as f: f.seek(100); b = f.read(1); f.seek(100); f.write(bytes([b[0] ^ 0xFF]))
P2, P3 = os.path.join(SRV, "updtest2.c-os"), os.path.join(SRV, "updtest3.c-os")

def terminal():
    ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
if PHASE == "2":
    terminal()
    check("the boot kept the updated files (%s)" % (re.findall(r"\[UPDATE\] files kept from updates: \d+", log()) or ["no line"])[-1], bool(re.search(r"\[UPDATE\] files kept from updates: [1-9]", log())))
    code, out = run("/bin/test.c-os", 60)
    check("after the reboot /bin/test.c-os is still the updated one: %s" % out.strip(), "updtest version 2" in out)
    code, out = run("/bin/updtest.c-os", 60)
    check("and /bin/updtest.c-os still exists: %s" % out.strip(), "updtest version 2" in out)
    code, out = run("cosupdate status", 60)
    check("status lists the updated files", "/bin/test.c-os" in out and "/bin/updtest.c-os" in out)
    sys.exit(0 if ok else 1)

terminal()
code, out = run("cosupdate url " + URL, 30)
check("the update source is set (%s)" % out.strip(), code == 0 and URL in out)

# a manifest with a damaged signature is refused before anything is read from it
publish(100, ["updtest:2:/bin/updtest.c-os:" + P2], bad_sig=True)
code, out = run("cosupdate install", 120)
check("a manifest with a bad signature is refused (%s)" % out.strip().splitlines()[-1:], "manifest refused" in out and "signature" in out and code != 0)
code, out = run("/bin/updtest.c-os", 30)
check("nothing was installed", "updtest version" not in out)

# a good one: a new program and an update of a program that is part of the system image
publish(100, ["updtest:2:/bin/updtest.c-os:" + P2, "test:2:/bin/test.c-os:" + P2])
code, out = run("cosupdate check", 120)
check("check lists the two updates (%s)" % out.strip().splitlines()[-1:], "manifest ok" in out and "2 update(s) available" in out)
code, out = run("cosupdate install", 180)
check("install puts both in place (%s)" % out.strip().splitlines()[-1:], code == 0 and "2 installed, 0 failed" in out)
code, out = run("/bin/updtest.c-os", 60)
check("the new program runs: %s" % out.strip(), "updtest version 2" in out)
code, out = run("/bin/test.c-os", 60)
check("the system program was replaced by the update: %s" % out.strip(), "updtest version 2" in out)
code, out = run("cosupdate check", 120)
check("a second check finds nothing to do", "0 update(s) available" in out)

# a file that already is what the manifest names is not downloaded again, whatever its version number says (the first release equals the image it came from)
publish(99 + 6, ["updtest:9:/bin/updtest.c-os:" + P2])
code, out = run("cosupdate check", 120)
check("a byte-identical file needs no update even with a higher version (%s)" % out.strip().splitlines()[-1:], "0 update(s) available" in out)

# bad packages
publish(106, ["updtest:3:/bin/updtest.c-os:" + P3], tamper="updtest-3.bin")
code, out = run("cosupdate install", 180)
check("a download that is not what was signed is refused (%s)" % out.strip().splitlines()[-2:-1], "SHA-256 differs" in out and code != 0)
code, out = run("/bin/updtest.c-os", 60)
check("the installed program is untouched: %s" % out.strip(), "updtest version 2" in out)
publish(107, ["evil:9:/etc/evil.c-os:" + P3])
code, out = run("cosupdate install", 120)
check("a path outside the allowed places is refused (%s)" % out.strip().splitlines()[-1:], "not a place updates may write to" in out and code != 0)
publish(108, ["evil:9:/system/drivers/evil.c-osys:" + P3])
code, out = run("cosupdate install", 120)
check("kernel modules cannot be installed by an update", "not a place updates may write to" in out and code != 0)
publish(109, ["evil:9:/bin/../etc/x.c-os:" + P3])
code, out = run("cosupdate install", 120)
check("a path with .. is refused", "not a place updates may write to" in out)

# an old manifest is no update
publish(50, ["updtest:3:/bin/updtest.c-os:" + P3])
code, out = run("cosupdate install", 120)
check("an older manifest (serial 50 after 100) is refused (%s)" % out.strip().splitlines()[-1:], "older" in out and code != 0)

# a real update, then back
publish(110, ["updtest:3:/bin/updtest.c-os:" + P3])
code, out = run("cosupdate install", 180)
check("version 3 installs (%s)" % out.strip().splitlines()[-1:], code == 0 and "1 installed" in out)
code, out = run("/bin/updtest.c-os", 60)
check("it runs: %s" % out.strip(), "updtest version 3" in out)
code, out = run("cosupdate rollback /bin/updtest.c-os", 60)
check("rollback restores the version before (%s)" % out.strip().splitlines()[-1:], code == 0 and "restored" in out)
code, out = run("/bin/updtest.c-os", 60)
check("it is version 2 again: %s" % out.strip(), "updtest version 2" in out)
code, out = run("cosupdate rollback /bin/test.c-os", 60)
code, out = run("cosupdate status", 60)
check("rolling back the system program gives the image's own version back (status: version 0)", "/bin/test.c-os version 0" in out)
publish(120, ["test:2:/bin/test.c-os:" + P2])
code, out = run("cosupdate install", 180)
check("it can be updated again", code == 0 and "1 installed" in out)

# the update survives a reboot: the boot does not put the image's own copy back (the script restarts the guest on the same disk, then runs phase 2)
time.sleep(15)                                  # the file system writes its catalog a little after the last change
sys.exit(0 if ok else 1)
