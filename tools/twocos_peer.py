#!/usr/bin/env python3
"""Two C-OS machines, nothing else: VM A runs C-OS Message and the C-OS relay server ("/host on"); VM B runs C-OS Message and
uses A as its server (through the host: B's 192.168.70.2 is the host, which forwards port 15151 into A). A and B become
friends, talk, and B sends A a file that A downloads. Everything is typed into the two machines through QMP."""
import os, re, subprocess, sys, time
logA, logB, qmpA, qmpB = sys.argv[1:5]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def type_to(qmp, t): subprocess.run(["python3", TYPE, t], env=dict(os.environ, COS_QMP=qmp), stdout=subprocess.DEVNULL)
def log(p): return open(p, errors="replace").read()
def wait(p, pat, t=120):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log(p))
        if m: return m
        time.sleep(2)
    return None
def count(p, pat): return len(re.findall(pat, log(p)))

type_to(qmpA, "/host on")
a = wait(logA, r"registered id=(\d+)"); check("machine A started the server and registered on it", a is not None)
check("the server program is running on A", wait(logA, r"COSMSGD\] listening") is not None)
type_to(qmpB, "/server 192.168.70.2:15151")
b = wait(logB, r"registered id=(\d+)"); check("machine B registered on A's server (through the host)", b is not None)
ida, idb = a.group(1), b.group(1)
type_to(qmpB, "/friend " + ida)
time.sleep(6); type_to(qmpA, "/accept " + idb)
time.sleep(5)
type_to(qmpB, "Hello from machine B")
check("A received B's message", wait(logA, r"received a message \(20 bytes\) from " + idb, 60) is not None)
type_to(qmpA, "Hi B, this is machine A")
check("B received A's message", wait(logB, r"received a message \(23 bytes\) from " + ida, 60) is not None)
type_to(qmpB, "/send /etc/os-release")
check("B's file reached A's server", wait(logA, r"COSMSGD\] upload stored: \d+ bytes", 90) is not None)
check("A was told about the attachment", count(logA, r"received a message") >= 2 or wait(logA, r"received a message \(\d+ bytes\) from " + idb + r"(?s:.*)received a message", 30) is not None)
sys.exit(0 if ok else 1)
