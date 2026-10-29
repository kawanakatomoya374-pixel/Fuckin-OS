#!/usr/bin/env python3
"""make_recipe.py UPSTREAM_GIT_DIR DATA_DIR OUT_DIR - writes OUT_DIR/recipe.txt (+ FILES.TXT, OVERLAYS.TXT) for the on-device data setup (ports/psxfunkin/setup).

The recipe says where each file of the data directory comes from: a file of the PSXFunkin repository, fetched on the user's device from raw.githubusercontent.com at a pinned
commit and checked against the SHA-256 listed here. Nothing of the game's assets goes into the C-OS app pack - only this list of names and hashes.
  commit <sha1>
  copy <out path> <repo path> <sha256> <size>
  tim  <out path> <repo png> <sha256> <size> <repo png.txt> <sha256> <size>     (the PNG becomes a PlayStation TIM like tools/funkintimpak does)
DATA_DIR is a data directory made by prep_assets.py: it says which files the port needs."""
import hashlib, os, subprocess, sys
repo, data, out = sys.argv[1:4]
def git(*a): return subprocess.check_output(["git", "-C", repo, *a])
commit = git("rev-parse", "HEAD").decode().strip()
def blob(p): return git("show", "HEAD:" + p)
lines = ["commit " + commit]; total = 0
for root, _, files in sorted(os.walk(data)):
    for f in sorted(files):
        rel = os.path.relpath(os.path.join(root, f), data).replace(os.sep, "/")
        if rel in ("FILES.TXT", "OVERLAYS.TXT"): continue
        if rel.endswith(".tim"):
            png = "iso/" + rel[:-4] + ".png"; tx = png + ".txt"; a, b = blob(png), blob(tx)
            lines.append("tim %s %s %s %d %s %s %d" % (rel, png, hashlib.sha256(a).hexdigest(), len(a), tx, hashlib.sha256(b).hexdigest(), len(b))); total += len(a) + len(b)
        else:
            src = "iso/" + rel; a = blob(src)
            lines.append("copy %s %s %s %d" % (rel, src, hashlib.sha256(a).hexdigest(), len(a))); total += len(a)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "recipe.txt"), "w").write("\n".join(lines) + "\n")
for n in ("FILES.TXT", "OVERLAYS.TXT"): open(os.path.join(out, n), "wb").write(open(os.path.join(data, n), "rb").read())
print("recipe: %d files, %.1f MiB to download, commit %s" % (len(lines) - 1, total / 1048576, commit))
