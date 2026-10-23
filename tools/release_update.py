#!/usr/bin/env python3
"""release_update.py - turns the programs of a build into an update package for GitHub Pages (docs/app_update.md).

  python3 tools/release_update.py --key ~/keys/cos_update_key.pem --out site/updates --release 4.1.0 --notes "Faster boot, new Terminal" [--build build]

For every updatable program found in the build tree it writes the payload and a signed manifest.cosup into --out. An item's version is the number of the release in
which its bytes last changed: files that are the same as in the previous manifest in --out keep their version (and so are not offered again), changed ones get
the previous version + 1. Payloads nobody references any more are removed. --release makes the manifest an OS release (the taskbar shows the C-OS icon);
without it the manifest only offers app updates. The serial grows with every run (it is the time), so an old manifest cannot be replayed.
Only programs that live as files on the device can be updated: /bin/*.c-os, language packs, SDK files (kernel-embedded apps start from memory and
need a new image)."""
import argparse, hashlib, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
# name -> (install path on the device, file in the build tree)
ITEMS = [
    ("Terminal", "/bin/Terminal.c-os", "terminal.c-os"), ("Files", "/bin/files.c-os", "cos_files.c-os"), ("Py2C", "/bin/Py2C.c-os", "py2c/Py2C.c-os"),
    ("TinyCC", "/bin/tcc.c-os", "studio/tcc.c-os"), ("Studio", "/bin/studio.c-os", "studio/studio.c-os"), ("Music", "/bin/music.c-os", "cos_music.c-os"),
    ("Media", "/bin/Media.c-os", "media.c-os"), ("Photo", "/bin/Photo.c-os", "photo.c-os"), ("Updater", "/bin/cosupdate.c-os", "cosupdate.c-os"),
    ("MessageServer", "/bin/CosMsgServer.c-os", "cosmsgd.c-os"), ("Py2CCli", "/bin/python-code-to-c.c-os", "p2ccli/python-code-to-c.c-os"),
    ("LangJa", "/system/lang/ja.c-osll", "langpacks/ja.c-osll"),
]
ap = argparse.ArgumentParser()
ap.add_argument("--key", required=True, help="the PRIVATE signing key (PEM). Keep it off the build machine and out of the repository")
ap.add_argument("--out", required=True); ap.add_argument("--build", default="build"); ap.add_argument("--release"); ap.add_argument("--notes", default="")
ap.add_argument("--kernel", help="build/kernel.boot.gz: publish this kernel too (signed with --kernel-key; the serial comes from --kernel-serial or build/kernel_serial.h)")
ap.add_argument("--kernel-key"); ap.add_argument("--kernel-serial", type=int); ap.add_argument("--kernel-label", default="")
ap.add_argument("--only", action="append", default=[], help="limit to these item names"); ap.add_argument("--extra", action="append", default=[], help="NAME:INSTALL_PATH:FILE, an item that is not in the built-in table (a new program, data)"); ap.add_argument("--dry-run", action="store_true")
a = ap.parse_args()
prev = {}; prev_kernel = None
ITEMS = list(ITEMS)
for e in a.extra:
    n, ip, fp = e.split(":", 2); ITEMS.append((n, ip, os.path.abspath(fp) if os.path.isabs(fp) else fp))
mp = os.path.join(a.out, "manifest.cosup")
if os.path.exists(mp):
    for line in open(mp, errors="replace"):
        if line.startswith("kernel "): prev_kernel = line.strip()
        if line.startswith("item "):
            kv = dict(t.split("=", 1) for t in line[5:].split() if "=" in t)
            prev[kv["name"]] = (int(kv["version"]), kv["sha256"])
items, plan = [], []
for name, path, rel in ITEMS:
    if a.only and name not in a.only: continue
    f = rel if os.path.isabs(rel) or os.path.exists(rel) and name in [e.split(":", 1)[0] for e in a.extra] else os.path.join(a.build, rel)
    if not os.path.exists(f): print("skip %s: %s is not built" % (name, f)); continue
    if os.path.getsize(f) > 9 * 1024 * 1024 + 512 * 1024: print("skip %s: %d bytes is over the 9.5 MiB one file may have" % (name, os.path.getsize(f))); continue
    sha = hashlib.sha256(open(f, "rb").read()).hexdigest()
    if name in prev and prev[name][1] == sha: ver = prev[name][0]; why = "unchanged"
    else: ver = prev.get(name, (0, ""))[0] + 1; why = "new" if name not in prev else "changed"
    plan.append("%-14s %-34s v%-3d %s" % (name, path, ver, why)); items.append("%s:%d:%s:%s" % (name, ver, path, f))
print("\n".join(plan))
if a.dry_run: sys.exit(0)
if not items and not a.kernel: sys.exit("nothing to publish")
os.makedirs(a.out, exist_ok=True)
kcmd, keep = [], ""
if a.kernel:
    if not a.kernel_key: sys.exit("--kernel needs --kernel-key (the kernel signing key, a different one from the app key)")
    ks = a.kernel_serial
    if ks is None:
        m = re.search(r"COS_KERNEL_SERIAL\s+(\d+)", open(os.path.join(a.build, "kernel_serial.h")).read()); ks = int(m.group(1)) if m else 0
    if ks <= 0: sys.exit("the kernel serial is 0: build it with `make iso KERNEL_SERIAL=$(date +%s)` (or give --kernel-serial matching that build)")
    blob = os.path.join(a.build, "kernel.signed.bin")
    subprocess.run([sys.executable, os.path.join(HERE, "sign_kernel.py"), a.kernel, a.kernel_key, str(ks), blob], check=True)
    kcmd = ["--kernel", blob, "--kernel-serial", str(ks), "--kernel-label", a.kernel_label or (a.release or "")]
    print("%-14s %-34s serial %d (%d bytes)" % ("Kernel", "(boot disk, other slot)", ks, os.path.getsize(blob)))
elif prev_kernel:
    kcmd = ["--kernel-line", prev_kernel]; keep = dict(t.split("=", 1) for t in prev_kernel.split()[1:] if "=" in t).get("file", "")
    print("Kernel         (unchanged: %s)" % prev_kernel)
for f in os.listdir(a.out):
    if f == "manifest.cosup" or (f.endswith(".bin") and not (keep and f.startswith(keep + ".p"))): os.unlink(os.path.join(a.out, f))
cmd = [sys.executable, os.path.join(HERE, "mkupdate.py"), "--out", a.out, "--key", a.key, "--serial", str(int(time.time()))]
if a.release: cmd += ["--os-version", a.release, "--os-notes", a.notes]
subprocess.run(cmd + kcmd + items, check=True)
print("published to %s: %s" % (a.out, "OS release " + a.release if a.release else "app updates"))
