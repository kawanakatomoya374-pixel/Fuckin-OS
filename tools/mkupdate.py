#!/usr/bin/env python3
"""mkupdate.py - makes an app-update package: the payload files and a signed manifest.cosup (docs/app_update.md).

  mkupdate.py --out DIR --key build/update/update_key.pem [--serial N] [--channel stable] [--os-version 4.1.0 --os-notes "Faster boot"] ITEM...
  ITEM = NAME:VERSION:INSTALL_PATH:FILE        e.g.  Calc:3:/bin/Calc.c-os:build/calc.c-os

Writes DIR/manifest.cosup and DIR/<NAME>-<VERSION>.bin for every item. The manifest is text; its signature (ECDSA P-256 over SHA-256 of everything
before the marker line) follows the marker "--SIG--" as 128 hex digits (r||s)."""
import argparse, hashlib, os, shutil, time
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
ap = argparse.ArgumentParser(); ap.add_argument("--out", required=True); ap.add_argument("--key", required=True); ap.add_argument("--serial", type=int)
ap.add_argument("--channel", default="stable"); ap.add_argument("--os-version", help="makes it an OS release with this label"); ap.add_argument("--os-notes", default="", help="release notes, one line"); ap.add_argument("--bad-signature", action="store_true", help="test packages: corrupt the signature"); ap.add_argument("--kernel", help="a signed kernel blob (tools/sign_kernel.py): split into 8 MiB parts and listed on a `kernel` line")
ap.add_argument("--kernel-serial", type=int, help="the serial inside the blob's signature"); ap.add_argument("--kernel-label", default="")
ap.add_argument("--kernel-line", help="carry an existing `kernel ...` line over unchanged (its parts must already be in --out)"); ap.add_argument("items", nargs="*")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
lines = ["COSUP 1", "channel %s" % a.channel, "serial %d" % (a.serial if a.serial is not None else int(time.time()))]
if a.os_version: lines.append("os version=%s notes=%s" % (a.os_version.replace(" ", "_"), a.os_notes.replace(" ", "_")))
for it in a.items:
    name, ver, path, f = it.split(":", 3)
    data = open(f, "rb").read(); fn = "%s-%s.bin" % (name, ver)
    shutil.copyfile(f, os.path.join(a.out, fn))
    lines.append("item name=%s version=%s path=%s sha256=%s size=%d file=%s" % (name, ver, path, hashlib.sha256(data).hexdigest(), len(data), fn))
if a.kernel:
    kd = open(a.kernel, "rb").read(); base = "Kernel-%d" % a.kernel_serial; PART = 8 * 1024 * 1024
    n = (len(kd) + PART - 1) // PART
    for i in range(n): open(os.path.join(a.out, "%s.p%02d.bin" % (base, i)), "wb").write(kd[i * PART:(i + 1) * PART])
    lines.append("kernel serial=%d sha256=%s size=%d parts=%d file=%s label=%s" % (a.kernel_serial, hashlib.sha256(kd).hexdigest(), len(kd), n, base, a.kernel_label.replace(" ", "_")))
elif a.kernel_line: lines.append(a.kernel_line)
body = ("\n".join(lines) + "\n").encode()
key = serialization.load_pem_private_key(open(a.key, "rb").read(), None)
r, s = decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
sig = (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex()
if a.bad_signature: sig = sig[:-2] + ("00" if sig[-2:] != "00" else "01")
open(os.path.join(a.out, "manifest.cosup"), "wb").write(body + b"--SIG--\n" + sig.encode() + b"\n")
print("package: %d item(s) in %s" % (len(a.items), a.out))
