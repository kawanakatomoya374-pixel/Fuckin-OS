#!/usr/bin/env python3
"""linux_rootfs_disk.py SRC OUT.img [--size-mb N] - puts a Linux root file system (a debootstrap tree, for example) onto a C-OS storage image as /linux.

C-OS keeps its files on a FAT32 volume that starts 512 MiB into the storage image (src/include/fatfs_diskio.h). FAT has no symbolic links and no permission bits, so
they travel in /linux/.manifest ("L<TAB>path<TAB>target", "X<TAB>path"; read by cos_linux.c). Regular files are copied as they are; what a run does not need
(documentation, caches, package lists, device nodes) is left out. Nothing is embedded in the kernel: the tree is as big as the disk.
Needs mtools (mformat, mmd, mcopy). usage: tools/linux_rootfs_disk.py /tmp/focal build/linuxdisk.img --size-mb 3072"""
import argparse, os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_PREFIX = ("dev/", "proc/", "sys/", "run/", "tmp/", "var/cache/", "var/lib/apt/lists/", "var/lib/dpkg/info/", "usr/share/doc/", "usr/share/man/", "usr/share/info/",
               "usr/share/lintian/", "usr/share/locale/", "usr/share/bug/", "var/log/", "boot/", "usr/include/", "usr/lib/python3/dist-packages/__pycache__")
ap = argparse.ArgumentParser(); ap.add_argument("src"); ap.add_argument("out"); ap.add_argument("--size-mb", type=int, default=3072)
ap.add_argument("--keep-staging", action="store_true"); ap.add_argument("--cos-env", help="a file of KEY=VALUE lines installed as /linux/etc/cos.env (the environment given to every Linux program that does not set it)")
ap.add_argument("--put", action="append", default=[], help="HOST:GUEST, an extra file (a test page, a script)"); a = ap.parse_args()
src = os.path.abspath(a.src); stage = tempfile.mkdtemp(prefix="cosroot.", dir=os.environ.get("TMPDIR", "/tmp"))
manifest, seen_lower, skipped, nfiles, nlinks, nexec, nbytes = [], {}, [], 0, 0, 0, 0
BAD = set('"*:<>?\\|')
for root, dirs, files in os.walk(src, followlinks=False):
    rel = os.path.relpath(root, src); rel = "" if rel == "." else rel
    if any((rel + "/").startswith(p.rstrip("/") + "/") for p in SKIP_PREFIX if rel):
        dirs[:] = []; continue
    os.makedirs(os.path.join(stage, rel), exist_ok=True)
    keep = []
    for d in dirs:
        full = os.path.join(root, d); r = os.path.join(rel, d)
        if os.path.islink(full):
            manifest.append("L\t/%s\t%s" % (r, os.readlink(full))); nlinks += 1
        elif any((r + "/").startswith(p) for p in SKIP_PREFIX): continue
        else: keep.append(d)
    dirs[:] = keep
    names_here = {}
    for f in files:
        full = os.path.join(root, f); r = os.path.join(rel, f)
        if any(r.startswith(p) for p in SKIP_PREFIX): continue
        if any(ch in BAD for ch in f) or f.endswith(".") or f.endswith(" "): skipped.append((r, "name not allowed on FAT")); continue
        low = r.lower()
        if low in seen_lower: skipped.append((r, "differs only in case from " + seen_lower[low])); continue
        seen_lower[low] = r
        if os.path.islink(full):
            manifest.append("L\t/%s\t%s" % (r, os.readlink(full))); nlinks += 1; continue
        if not os.path.isfile(full): continue
        try: st = os.stat(full)
        except OSError: continue
        if st.st_size >= 4 * 1024 ** 3: skipped.append((r, "4 GiB or more")); continue
        if st.st_mode & 0o111: manifest.append("X\t/%s" % r); nexec += 1
        try: os.link(full, os.path.join(stage, r))
        except OSError: shutil.copy2(full, os.path.join(stage, r))
        nfiles += 1; nbytes += st.st_size
for d in ("tmp", "var/tmp", "run", "root", "dev", "proc", "sys", "var/log", "var/cache"): os.makedirs(os.path.join(stage, d), exist_ok=True)
if a.cos_env: shutil.copy(a.cos_env, os.path.join(stage, "etc/cos.env"))
for spec in a.put:
    h, g = spec.split(":", 1); dst = os.path.join(stage, g.lstrip("/")); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy(h, dst)
with open(os.path.join(stage, ".manifest"), "w") as m: m.write("\n".join(manifest) + "\n")
print("files %d (%d MiB), links %d, executables %d, skipped %d" % (nfiles, nbytes >> 20, nlinks, nexec, len(skipped)))
for r, why in skipped[:20]: print("  skipped", r, why)
subprocess.run([sys.executable, os.path.join(HERE, "pack_storage.py"), "--out", a.out, "--size-mb", str(a.size_mb)], check=True)
OFF = 1048576 * 512; vol_sectors = (a.size_mb * 1024 * 1024 - OFF) // 512
img = "%s@@%d" % (a.out, OFF)
subprocess.run(["mformat", "-i", img, "-F", "-T", str(vol_sectors), "-c", "16", "-v", "COSROOT", "::"], check=True)
subprocess.run(["mmd", "-i", img, "::/linux"], check=True)
entries = sorted(os.listdir(stage)); 
for e in entries:
    r = subprocess.run(["mcopy", "-s", "-n", "-i", img, os.path.join(stage, e), "::/linux/"], capture_output=True, text=True)
    if r.returncode: print("mcopy", e, "failed:", r.stderr.strip()[:300])
print("image:", a.out)
if not a.keep_staging: shutil.rmtree(stage, ignore_errors=True)
