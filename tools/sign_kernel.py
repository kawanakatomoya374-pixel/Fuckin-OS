#!/usr/bin/env python3
"""sign_kernel.py KERNEL.gz KEY.pem SERIAL OUT - makes a kernel update blob: the gzip kernel (what GRUB loads) followed by the 81-byte trailer cos_bootupd.c checks:
    "COSKSIG1" | serial (u64, little endian) | key id (1 byte, 1) | ECDSA P-256 signature r||s (64 bytes)
over SHA-256( kernel bytes | "COSKSER1" | serial ). The serial is part of what is signed: it is the build number of that kernel (make KERNEL_SERIAL=N), a
kernel update is accepted only when its serial is higher than the running kernel's, so an older signed kernel cannot be put back."""
import hashlib, struct, sys
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
kernel, keyf, serial, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
data = open(kernel, "rb").read()
if data[:2] != b"\x1f\x8b": sys.exit("%s is not a gzip file (GRUB loads /boot/kernel*.gz)" % kernel)
if serial <= 0: sys.exit("the serial must be a positive number (make KERNEL_SERIAL=...)")
key = serialization.load_pem_private_key(open(keyf, "rb").read(), None)
msg = data + b"COSKSER1" + struct.pack("<Q", serial)
r, s = decode_dss_signature(key.sign(msg, ec.ECDSA(hashes.SHA256())))
open(out, "wb").write(data + b"COSKSIG1" + struct.pack("<Q", serial) + bytes([1]) + r.to_bytes(32, "big") + s.to_bytes(32, "big"))
print("signed kernel: %d bytes, serial %d -> %s" % (len(data), serial, out))
