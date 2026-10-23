#!/usr/bin/env python3
"""Single-header libraries built on the guest with the on-device TinyCC (see run_shdr_test.sh): the SDK examples (stb_image.h, minimp3.h, stb_truetype.h,
a two-file mini_vec.h project) and Py2C's own python_code_to_c_single.h. Expected output comes from host builds (gcc) of the same sources."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ROOT = os.path.join(os.path.dirname(__file__), "..")
EXD = os.path.join(ROOT, "userland", "sdk", "examples", "singleheader")
TP = os.path.join(ROOT, "userland", "third_party")
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
def run(cmd, t=120, until=None):
    """types the command and waits for ITS exit line; returns (exit code or None, output lines)"""
    start = len(log()); type_command(logp, qmp, TYPE, cmd)
    end = time.time() + t; code = None; seg = ""
    while time.time() < end:
        seg = log()[start:]; c = seg.find("[TERMINAL] cmd: " + cmd)
        m = re.search(r"\[TERMINAL\] exit: \S+ (-?\d+)", seg[c:]) if c >= 0 else None
        if m: code = int(m.group(1)); break
        time.sleep(1)
    return code, [l.split("out: ", 1)[1] for l in seg.splitlines() if "[TERMINAL] out: " in l]
def host(src, args=()):
    exe = "/tmp/shdr_host_" + os.path.basename(src)[:-2]
    subprocess.run(["gcc", "-O1", "-w", "-I" + TP, "-o", exe, os.path.join(EXD, src), "-lm"], check=True)
    return subprocess.run([exe, *args], capture_output=True, text=True).stdout.splitlines()

# assets the guest has: the same bytes on the host
def asset(name, out):
    s = open(os.path.join(ROOT, "src", "assets", name + ".c")).read(); body = s[s.index("{"):s.rindex("}")]
    open(out, "wb").write(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)))
asset("sample_photo_jpg", "/tmp/shdr_photo.jpg"); asset("sample_beep_mp3", "/tmp/shdr_beep.mp3")
want = {"stb_image_demo": host("stb_image_demo.c", ["/tmp/shdr_photo.jpg"]), "minimp3_demo": host("minimp3_demo.c", ["/tmp/shdr_beep.mp3"]),
        "stb_truetype_demo": host("stb_truetype_demo.c", [os.path.join(TP, "NotoSansJP-subset.ttf")])}
subprocess.run(["gcc", "-O1", "-w", "-o", "/tmp/shdr_host_vec", os.path.join(EXD, "vec_main.c"), os.path.join(EXD, "vec_util.c")], check=True)
want["vec_demo"] = subprocess.run(["/tmp/shdr_host_vec"], capture_output=True, text=True).stdout.splitlines()

ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
run("cd /system/sdk/examples/singleheader", 10)
for name, srcs in (("stb_image_demo", "stb_image_demo.c"), ("minimp3_demo", "minimp3_demo.c"), ("stb_truetype_demo", "stb_truetype_demo.c"), ("vec_demo", "vec_main.c vec_util.c")):
    code, out = run("tcc %s -o /tmp/%s.c-os" % (srcs, name), 600)
    check("tcc builds %s (exit %s)" % (name, code), code == 0)
    if code != 0: print("\n".join(out[-8:])); continue
    code, out = run("/tmp/%s.c-os" % name, 120)
    check("%s prints what the host build prints (%d lines)" % (name, len(want[name])), code == 0 and out[:len(want[name])] == want[name])
    if out[:len(want[name])] != want[name]: print("want", want[name], "\ngot", out[:12])

# Py2C's own single-header distribution: the converter and the runtime in one header
code, out = run("tcc -U__linux__ -I/apps/Py2C/include -DPYTHON_CODE_TO_C_NO_TLS -O1 /apps/Py2C/tests/test_single_header.c -o /tmp/tsh.c-os", 1200)
check("tcc builds Py2C's python_code_to_c_single.h (tests/test_single_header.c, exit %s)" % code, code == 0)
if code != 0: print("\n".join(out[-10:]))
else:
    code, out = run("/tmp/tsh.c-os", 120)
    check("the program that embeds the converter converts Python to C (exit %s)" % code, code == 0)
code, out = run("tcc -I/apps/Py2C/include -DPYTHON_CODE_TO_C_NO_STDLIB -O1 -c /apps/Py2C/tests/test_single_header_freestanding.c -o /tmp/tshf.o", 1200)
check("the freestanding (NO_STDLIB) configuration of the single header compiles (exit %s)" % code, code == 0)
if code != 0: print("\n".join(out[-10:]))

# the same project built by C-OS Studio
start = len(log()); type_command(logp, qmp, TYPE, "studio /system/sdk/examples/singleheader --build")
m = wait(r"\[STUDIO\] build (ok|failed)[^\n]*", 300, start)
check("C-OS Studio builds the two-file single-header project (%s)" % (m.group(0) if m else "timeout"), m is not None and "failed" not in m.group(0))
print("log tail:", log()[-600:])
sys.exit(0 if ok else 1)
