#!/usr/bin/env python3
"""sign_linuxpack.py DIR --key KEY.pem - signs DIR/linuxpack.manifest.body into DIR/linuxpack.manifest (what the installer, userland/programs/linuxpack, checks).

The signature is ECDSA P-256 over SHA-256 of the body, 128 hex digits (r||s) after the marker line --SIG--: the same scheme and the same key as the app updates
(tools/mkupdate.py; a release is signed with the private key matching release/update_public.pem, a validation build trusts build/update/update_key.pem).
The body starts with "COSLP 1", so a manifest of the update system cannot be taken for a pack manifest, nor the other way round.
Run it at release time with the private key; the key never goes into the repository."""
import argparse, os
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
ap = argparse.ArgumentParser(); ap.add_argument("dir"); ap.add_argument("--key", required=True); ap.add_argument("--bad-signature", action="store_true", help="tests: corrupt the signature")
a = ap.parse_args()
body = open(os.path.join(a.dir, "linuxpack.manifest.body"), "rb").read()
assert body.startswith(b"COSLP 1\n"), "not a pack manifest body"
key = serialization.load_pem_private_key(open(a.key, "rb").read(), None)
r, s = decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
sig = (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex()
if a.bad_signature: sig = sig[:-2] + ("00" if sig[-2:] != "00" else "01")
open(os.path.join(a.dir, "linuxpack.manifest"), "wb").write(body + b"--SIG--\n" + sig.encode() + b"\n")
print("signed", os.path.join(a.dir, "linuxpack.manifest"))
