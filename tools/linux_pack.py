#!/usr/bin/env python3
"""linux_pack.py SRC OUTDIR --name firefox --title "Firefox 136" --version 136.0-1 - makes a downloadable Linux app pack for C-OS (docs/LINUX_COMPAT.md, "App packs").

SRC is a Linux root file system tree (a debootstrap tree with the apps installed). The pack is what tools/linux_rootfs_disk.py would copy to /linux, cut into
chunks a C-OS program can download one by one (a chunk is at most about 9 MiB, the most one HTTP download holds):

  NAME.pNNNN.bin    a zlib stream of records:  u16 path length | path (UTF-8, relative, no "..") | u8 kind (0) | u64 file size | u64 offset | u32 length | data
                    (a big file is cut across records: offset and length say where the piece goes)
  linuxpack.manifest.body   the list the installer trusts once tools/sign_linuxpack.py has signed it into linuxpack.manifest:
        COSLP 1
        pack name=NAME title=... version=... serial=N chunks=K files=F bytes=B download=D
        chunk file=NAME.p0001.bin size=S sha256=H raw=U

The last record is ".manifest", the file that carries the symbolic links ("L<TAB>path<TAB>target") and the executable bits ("X<TAB>path") FAT cannot store.
Nothing here needs a key; signing is a separate step done with the private key at release time."""
import argparse, hashlib, os, struct, sys, time, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_PREFIX = ("dev/", "proc/", "sys/", "run/", "tmp/", "var/cache/", "var/lib/apt/lists/", "var/lib/dpkg/info/", "usr/share/doc/", "usr/share/man/", "usr/share/info/",
               "usr/share/lintian/", "usr/share/locale/", "usr/share/bug/", "var/log/", "boot/", "usr/include/", "usr/lib/python3/dist-packages/__pycache__")
# what an X + Firefox run does not use, on top of the above (the bitmap fonts alone are 14 MiB and thousands of files Firefox would still read at start)
PACK_SKIP = ("usr/share/fonts/X11/", "usr/lib/python3.8/", "usr/lib/python3/", "usr/share/perl", "usr/lib/x86_64-linux-gnu/perl", "usr/lib/gcc/", "usr/share/gcc", "root/",
             "var/lib/apt/", "var/lib/dpkg/", "etc/apt/", "usr/share/apt/", "usr/share/dpkg/", "usr/share/zoneinfo-leaps", "usr/share/bash-completion/", "usr/share/vim/",
             "usr/share/man", "usr/share/pixmaps/debian", "usr/lib/udev/", "lib/udev/", "usr/lib/systemd/")
BAD = set('"*:<>?\\|')
CHUNK_RAW = 16 * 1024 * 1024
CHUNK_MAX = 9 * 1024 * 1024 + 256 * 1024          # cosupdate's one-file limit is 9.5 MiB; stay below it

ap = argparse.ArgumentParser()
ap.add_argument("src"); ap.add_argument("out")
ap.add_argument("--name", default="firefox"); ap.add_argument("--title", default="Firefox"); ap.add_argument("--version", default="1")
ap.add_argument("--skip", action="append", default=[], help="another path prefix (relative to SRC) to leave out"); ap.add_argument("--keep", action="append", default=[], help="a path prefix of PACK_SKIP to keep after all")
ap.add_argument("--dry-run", action="store_true", help="only print what would go in, by top-level directory"); ap.add_argument("--level", type=int, default=9)
a = ap.parse_args()
src = os.path.abspath(a.src)
skip = tuple(p for p in SKIP_PREFIX + PACK_SKIP if not any(p.startswith(k) for k in a.keep)) + tuple(a.skip)

# ---- what goes in (the same rules as tools/linux_rootfs_disk.py, so a pack and an image of the same tree agree)
manifest, entries, seen_lower, skipped = [], [], {}, []
for root, dirs, files in os.walk(src, followlinks=False):
    rel = os.path.relpath(root, src); rel = "" if rel == "." else rel
    if rel and any((rel + "/").startswith(p.rstrip("/") + "/") for p in skip):
        dirs[:] = []; continue
    keep = []
    for d in sorted(dirs):
        full = os.path.join(root, d); r = os.path.join(rel, d)
        if os.path.islink(full): manifest.append("L\t/%s\t%s" % (r, os.readlink(full)))
        elif any((r + "/").startswith(p.rstrip("/") + "/") for p in skip): continue
        else: keep.append(d)
    dirs[:] = keep
    for f in sorted(files):
        full = os.path.join(root, f); r = os.path.join(rel, f)
        if any(r.startswith(p) for p in skip): continue
        if any(ch in BAD for ch in f) or f.endswith(".") or f.endswith(" "): skipped.append((r, "name not allowed on FAT")); continue
        low = r.lower()
        if low in seen_lower: skipped.append((r, "differs only in case from " + seen_lower[low])); continue
        seen_lower[low] = r
        if os.path.islink(full): manifest.append("L\t/%s\t%s" % (r, os.readlink(full))); continue
        if not os.path.isfile(full): continue
        try: st = os.stat(full)
        except OSError: continue
        if st.st_size >= 4 * 1024 ** 3: skipped.append((r, "4 GiB or more")); continue
        if st.st_mode & 0o111: manifest.append("X\t/%s" % r)
        entries.append((r.replace(os.sep, "/"), full, st.st_size))
total = sum(e[2] for e in entries)
if a.dry_run:
    by = {}
    for r, f, s in entries: top = "/".join(r.split("/")[:2]); by[top] = by.get(top, 0) + s
    for k, v in sorted(by.items(), key=lambda kv: -kv[1])[:25]: print("%8.1f MiB  %s" % (v / 1048576, k))
    print("files %d, %.1f MiB, links/exec lines %d, skipped %d" % (len(entries), total / 1048576, len(manifest), len(skipped)))
    sys.exit(0)

# ---- chunks
os.makedirs(a.out, exist_ok=True)
for f in os.listdir(a.out):
    if f.startswith(a.name + ".p") and f.endswith(".bin"): os.remove(os.path.join(a.out, f))
records = []       # (path, size, offset, bytes)
for r, f, size in entries:
    if size == 0: records.append((r, 0, 0, b"")); continue
    with open(f, "rb") as fh:
        off = 0
        while off < size:
            data = fh.read(min(CHUNK_RAW, size - off)); records.append((r, size, off, data)); off += len(data)
records.append((".manifest", 0, 0, ("\n".join(manifest) + "\n").encode("utf-8")))
records[-1] = (".manifest", len(records[-1][3]), 0, records[-1][3])

def pack_record(p, size, off, data):
    pb = p.encode("utf-8")
    return struct.pack("<H", len(pb)) + pb + struct.pack("<BQQI", 0, size, off, len(data)) + data

chunks, cur, cur_raw = [], [], 0
def flush():
    global cur, cur_raw
    if not cur: return
    raw = b"".join(cur); z = zlib.compress(raw, a.level)
    if len(z) > CHUNK_MAX and len(cur) > 1:                       # too big once compressed (data that does not compress): split the chunk in two
        h = len(cur) // 2; left, right = cur[:h], cur[h:]
        for part in (left, right):
            zz = zlib.compress(b"".join(part), a.level)
            if len(zz) > CHUNK_MAX: sys.exit("a single record compresses to %d bytes, over the chunk limit" % len(zz))
            chunks.append((zz, len(b"".join(part))))
    else:
        if len(z) > CHUNK_MAX: sys.exit("a single record compresses to %d bytes, over the chunk limit" % len(z))
        chunks.append((z, len(raw)))
    cur, cur_raw = [], 0
for rec in records:
    pr = pack_record(*rec)
    if cur and cur_raw + len(pr) > CHUNK_RAW: flush()
    cur.append(pr); cur_raw += len(pr)
flush()

lines, dl = [], 0
for i, (z, raw) in enumerate(chunks, 1):
    fn = "%s.p%04d.bin" % (a.name, i)
    open(os.path.join(a.out, fn), "wb").write(z); dl += len(z)
    lines.append("chunk file=%s size=%d sha256=%s raw=%d" % (fn, len(z), hashlib.sha256(z).hexdigest(), raw))
head = "COSLP 1\npack name=%s title=%s version=%s serial=%d chunks=%d files=%d bytes=%d download=%d" % (a.name, a.title.replace(" ", "_"), a.version, int(time.time()), len(chunks), len(entries), total, dl)
open(os.path.join(a.out, "linuxpack.manifest.body"), "w").write(head + "\n" + "\n".join(lines) + "\n")
if os.path.exists(os.path.join(a.out, "linuxpack.manifest")): os.remove(os.path.join(a.out, "linuxpack.manifest"))       # an old signed manifest does not belong to these chunks
print(head.split("\n")[1])
print("%d chunks, download %.1f MiB, installed %.1f MiB, %d files, %d links/exec lines, skipped %d" % (len(chunks), dl / 1048576, total / 1048576, len(entries), len(manifest), len(skipped)))
for r, why in skipped[:10]: print("  skipped", r, why)
