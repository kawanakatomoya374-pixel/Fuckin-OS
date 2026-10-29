#!/usr/bin/env python3
"""make_catalog.py ROOT --key KEY.pem [--desc ID="text"]... - writes ROOT/catalog.manifest, the signed list of apps C-OS Apps shows.

ROOT holds one directory per app (made by tools/app_pack.py and signed by tools/sign_linuxpack.py): ROOT/ID/linuxpack.manifest, ROOT/ID/icon.png, the chunks.
Each app description comes from ROOT/ID/about.txt (one line). The body starts with "COSAPPS 1"; one line per app:
  app id=ID title=Title version=V download=BYTES installed=BYTES icon=icon.png iconsha=SHA256 desc=Words_with_underscores
signed like the pack manifests (ECDSA P-256 over SHA-256, after the line --SIG--)."""
import argparse, hashlib, os, re
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
ap = argparse.ArgumentParser(); ap.add_argument("root"); ap.add_argument("--key", required=True); a = ap.parse_args()
lines = ["COSAPPS 1"]
for d in sorted(os.listdir(a.root)):
    mf = os.path.join(a.root, d, "linuxpack.manifest.body")
    if not os.path.isfile(mf): continue
    head = open(mf).read().split("\n")[1]; kv = dict(t.split("=", 1) for t in head.split()[1:] if "=" in t)
    icon = kv.get("icon", "icon.png"); ic = open(os.path.join(a.root, d, icon), "rb").read()
    about = os.path.join(a.root, d, "about.txt"); desc = open(about).read().strip().replace(" ", "_") if os.path.isfile(about) else ""
    lines.append("app id=%s title=%s version=%s download=%s installed=%s icon=%s iconsha=%s desc=%s" % (
        kv["name"], kv["title"], kv["version"], kv["download"], kv["bytes"], icon, hashlib.sha256(ic).hexdigest(), desc))
body = ("\n".join(lines) + "\n").encode()
key = serialization.load_pem_private_key(open(a.key, "rb").read(), None)
r, s = decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
open(os.path.join(a.root, "catalog.manifest"), "wb").write(body + b"--SIG--\n" + (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex().encode() + b"\n")
print("catalog.manifest:", len(lines) - 1, "app(s)")
