#!/usr/bin/env python3
"""app_pack.py SRC OUTDIR --id psxfunkin --title PSXFunkin --exec psxfunkin.c-os [--icon icon.png] [--version 1] - makes a downloadable app pack for C-OS Apps.

SRC is a plain directory (the program, its icon, its data). The pack is the same format as tools/linux_pack.py makes (zlib chunks of file records, a manifest the
installer userland/programs/linuxpack checks against the update key) but installs below /apps/ID instead of /linux and puts a shortcut on the desktop:
  pack name=ID title=... version=... serial=N chunks=K files=F bytes=B download=D root=/apps/ID exec=psxfunkin.c-os icon=icon.png
Sign OUTDIR/linuxpack.manifest.body with tools/sign_linuxpack.py (OUTDIR) afterwards; tools/make_catalog.py lists packs for the store."""
import argparse, hashlib, os, struct, sys, time, zlib
ap = argparse.ArgumentParser(); ap.add_argument("src"); ap.add_argument("out")
ap.add_argument("--id", required=True); ap.add_argument("--title", required=True); ap.add_argument("--exec", default="", dest="exe")  # no --exec: no desktop shortcut (a language, a library)
ap.add_argument("--icon", default="icon.png"); ap.add_argument("--version", default="1"); ap.add_argument("--level", type=int, default=9)
a = ap.parse_args()
BAD = set('"*:<>?\\|'); CHUNK_RAW = 16 * 1024 * 1024; CHUNK_MAX = 9 * 1024 * 1024 + 256 * 1024
src = os.path.abspath(a.src); entries = []
for root, dirs, files in os.walk(src):
    dirs.sort()
    for f in sorted(files):
        full = os.path.join(root, f); r = os.path.relpath(full, src).replace(os.sep, "/")
        if any(c in BAD for c in f) or f.endswith((".", " ")): sys.exit("name not allowed on FAT: " + r)
        entries.append((r, full, os.path.getsize(full)))
names = {e[0] for e in entries}
if a.exe and a.exe not in names: sys.exit("exec file %s is not in SRC" % a.exe)
if a.icon not in names: sys.exit("icon %s is not in SRC" % a.icon)
total = sum(e[2] for e in entries)
records = []
for r, f, size in entries:
    if size == 0: records.append((r, 0, 0, b"")); continue
    with open(f, "rb") as fh:
        off = 0
        while off < size:
            d = fh.read(min(CHUNK_RAW, size - off)); records.append((r, size, off, d)); off += len(d)
def rec(p, size, off, data): pb = p.encode(); return struct.pack("<H", len(pb)) + pb + struct.pack("<BQQI", 0, size, off, len(data)) + data
chunks, cur, cur_raw = [], [], 0
def flush():
    global cur, cur_raw
    if not cur: return
    raw = b"".join(cur); z = zlib.compress(raw, a.level)
    if len(z) > CHUNK_MAX and len(cur) > 1:
        h = len(cur) // 2
        for part in (cur[:h], cur[h:]):
            zz = zlib.compress(b"".join(part), a.level)
            if len(zz) > CHUNK_MAX: sys.exit("a record compresses to over the chunk limit")
            chunks.append((zz, len(b"".join(part))))
    else:
        if len(z) > CHUNK_MAX: sys.exit("a record compresses to over the chunk limit")
        chunks.append((z, len(raw)))
    cur, cur_raw = [], 0
for r in records:
    pr = rec(*r)
    if cur and cur_raw + len(pr) > CHUNK_RAW: flush()
    cur.append(pr); cur_raw += len(pr)
flush()
os.makedirs(a.out, exist_ok=True)
for f in os.listdir(a.out):
    if f.endswith(".bin") and f.startswith(a.id + ".p"): os.remove(os.path.join(a.out, f))
lines, dl = [], 0
for i, (z, raw) in enumerate(chunks, 1):
    fn = "%s.p%04d.bin" % (a.id, i); open(os.path.join(a.out, fn), "wb").write(z); dl += len(z)
    lines.append("chunk file=%s size=%d sha256=%s raw=%d" % (fn, len(z), hashlib.sha256(z).hexdigest(), raw))
head = "COSLP 1\npack name=%s title=%s version=%s serial=%d chunks=%d files=%d bytes=%d download=%d root=/apps/%s exec=%s icon=%s" % (
    a.id, a.title.replace(" ", "_"), a.version, int(time.time()), len(chunks), len(entries), total, dl, a.id, a.exe, a.icon)
open(os.path.join(a.out, "linuxpack.manifest.body"), "w").write(head + "\n" + "\n".join(lines) + "\n")
open(os.path.join(a.out, a.icon), "wb").write(open(os.path.join(src, a.icon), "rb").read())      # the store shows the icon before anything is installed
print(head.split("\n")[1]); print("%d chunk(s), download %.2f MiB, installed %.2f MiB" % (len(chunks), dl / 1048576, total / 1048576))
