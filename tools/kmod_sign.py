#!/usr/bin/env python3
"""kmod_sign.py OBJECT KEY.pem OUT KEYID - appends the signature trailer the kernel checks: "COSYSIG2", a 32-bit length (65), the key id (1 byte: the highest module level
that key may sign) and the raw ECDSA P-256 signature r||s (64 bytes) over the SHA-256 of the object bytes that come before the trailer."""
import hashlib, sys
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
obj = open(sys.argv[1], "rb").read()
key = serialization.load_pem_private_key(open(sys.argv[2], "rb").read(), None)
keyid = int(sys.argv[4])
r, s = decode_dss_signature(key.sign(obj, ec.ECDSA(hashes.SHA256())))
open(sys.argv[3], "wb").write(obj + b"COSYSIG2" + (65).to_bytes(4, "little") + bytes([keyid]) + r.to_bytes(32, "big") + s.to_bytes(32, "big"))
