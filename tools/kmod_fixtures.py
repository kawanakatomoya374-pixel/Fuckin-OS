#!/usr/bin/env python3
"""kmod_fixtures.py SIGNED_MODULE OUTDIR SYSTEM_MODULE.so KEYDIR - makes the files the module loader must refuse:
  tampered      one byte of a signed module changed
  unsigned      no signature trailer
  wrongkey      signed with a key the kernel does not know (claiming key id 1)
  lowkey_system a module that declares the SYSTEM level, signed with the level-1 key (which may sign BASIC and SERVICE modules only)"""
import os, sys
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
src, out, system_so, keydir = sys.argv[1:5]; os.makedirs(out, exist_ok=True)
TRAILER = 8 + 4 + 1 + 64
def sign(obj, key, keyid):
    r, s = decode_dss_signature(key.sign(obj, ec.ECDSA(hashes.SHA256())))
    return obj + b"COSYSIG2" + (65).to_bytes(4, "little") + bytes([keyid]) + r.to_bytes(32, "big") + s.to_bytes(32, "big")
data = open(src, "rb").read(); obj = data[:-TRAILER]
t = bytearray(data); t[len(obj) // 2] ^= 0x01; open(os.path.join(out, "tampered.c-osys"), "wb").write(bytes(t))
open(os.path.join(out, "unsigned.c-osys"), "wb").write(obj)
open(os.path.join(out, "wrongkey.c-osys"), "wb").write(sign(obj, ec.generate_private_key(ec.SECP256R1()), 1))
k1 = serialization.load_pem_private_key(open(os.path.join(keydir, "kmod_key_l1.pem"), "rb").read(), None)
open(os.path.join(out, "lowkey_system.c-osys"), "wb").write(sign(open(system_so, "rb").read(), k1, 1))
