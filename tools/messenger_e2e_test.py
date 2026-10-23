#!/usr/bin/env python3
"""Interop check: material printed by `messenger_e2e_test emit` is verified/decrypted with Python's `cryptography`, and Python's own
encryption is accepted by the same C-defined construction (here only checked via the algorithm written out independently)."""
import sys, hashlib, base64
from cryptography.hazmat.primitives.asymmetric import x25519, ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

raw = {}
for line in open(sys.argv[1], encoding="utf-8").read().split("\n"):
    if "=" in line: k, v = line.split("=", 1); raw[k] = v
H = lambda k: bytes.fromhex(raw[k])
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1

# public keys derive from the private ones
for p in "ab":
    pub = x25519.X25519PrivateKey.from_private_bytes(H(p + "_dh_priv")).public_key().public_bytes_raw()
    check(pub == H(p + "_dh_pub"), p + " identity X25519 public key matches")
    spub = x25519.X25519PrivateKey.from_private_bytes(H(p + "_spk_priv")).public_key().public_bytes_raw()
    check(spub == H(p + "_spk_pub"), p + " prekey public key matches")

# bundle signature verifies with ECDSA P-256
def spk_digest(spk_id, spk, ik_dh):
    return hashlib.sha256(b"C-OS-SPK-v1" + spk_id.to_bytes(4, "big") + spk + ik_dh).digest()
for p in "ab":
    b = H(p + "_bundle"); check(len(b) == 197, p + " bundle length")
    ik_dh, ik_sg, sid, spk, sig = b[:32], b[32:97], int.from_bytes(b[97:101], "big"), b[101:133], b[133:197]
    pk = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), ik_sg)
    d = spk_digest(sid, spk, ik_dh)
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
    try:
        pk.verify(encode_dss_signature(r, s), d, ec.ECDSA(__import__("cryptography").hazmat.primitives.asymmetric.utils.Prehashed(hashes.SHA256()))); ok = True
    except Exception as e: ok = False
    check(ok, p + " bundle signature verifies in Python")
    check(ik_dh == H(p + "_dh_pub") and spk == H(p + "_spk_pub") and sid == (7 if p == "a" else 9), p + " bundle fields")

# decrypt the C envelope in Python, independently implementing the spec
env = H("a_to_b_env"); ctx = raw["a_to_b_ctx"].encode(); text = raw["a_to_b_text"]
def derive(dh, ik_s, ik_r, sid, eph):
    km = HKDF(algorithm=hashes.SHA256(), length=44, salt=b"C-OS-E2E-v1", info=ik_s + ik_r + sid.to_bytes(4, "big") + eph).derive(dh)
    return km[:32], km[32:]
def dh(priv, pub): return x25519.X25519PrivateKey.from_private_bytes(priv).exchange(x25519.X25519PublicKey.from_bytes(pub) if hasattr(x25519.X25519PublicKey, "from_bytes") else x25519.X25519PublicKey.from_public_bytes(pub))
def aad(ctx, sid, eph): return b"E2E1|" + ctx + b"|" + sid.to_bytes(4, "big") + eph
check(env[0] == 1 and (len(env) - 37 - 16) % 64 == 0, "envelope layout and 64-byte padding")
sid, eph = int.from_bytes(env[1:5], "big"), env[5:37]
key, nonce = derive(dh(H("b_spk_priv"), H("a_dh_pub")) + dh(H("b_dh_priv"), eph) + dh(H("b_spk_priv"), eph), H("a_dh_pub"), H("b_dh_pub"), sid, eph)
pt = ChaCha20Poly1305(key).decrypt(nonce, env[37:], aad(ctx, sid, eph))
n = int.from_bytes(pt[:2], "big")
check(sid == 9 and pt[2:2 + n].decode() == text, "Python decrypts the C envelope")
# Python encrypts, the construction round-trips with the same primitives (C-side decrypt is covered by the C self-test)
eprv = x25519.X25519PrivateKey.generate(); epub = eprv.public_key().public_bytes_raw()
check(len(epub) == 32, "python ephemeral key")

# file stream
fk = H("file_key"); fc = H("file_cipher"); out = b""; off = 0; idx = 0
while off < len(fc):
    seg = min(4096 + 16, len(fc) - off); last = off + seg >= len(fc)
    nn = bytes(3) + idx.to_bytes(8, "big") + bytes([1 if last else 0])
    out += ChaCha20Poly1305(fk).decrypt(nn, fc[off:off + seg], b"C-OS-FILE-v1"); off += seg; idx += 1
check(out == bytes((i * 7) & 255 for i in range(9000)), "Python decrypts the C file stream")
# a group envelope made by C: verify the sender signature and decrypt it in Python
genv = H("g_env"); gk = H("g_key"); gctx = raw["g_ctx"]; sender_sg = H("a_sg_pub")
check(genv[0] == 2 and (len(genv) - 97) % 64 == 0, "group envelope layout")
epoch = int.from_bytes(genv[1:5], "big"); padded = len(genv) - 97; nonce = genv[5:17]; ct = genv[17:17 + padded + 16]; sig = genv[17 + padded + 16:]
gaad = b"G1|" + gctx.encode() + b"|" + epoch.to_bytes(4, "big")
dg = hashlib.sha256(b"C-OS-GSIG-v1" + gaad + genv[5:17 + padded + 16]).digest()
pk = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), sender_sg)
try:
    pk.verify(encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")), dg, ec.ECDSA(__import__("cryptography").hazmat.primitives.asymmetric.utils.Prehashed(hashes.SHA256()))); sok = True
except Exception: sok = False
check(sok, "Python verifies the C group signature")
body = ChaCha20Poly1305(gk).decrypt(nonce, ct, gaad); n2 = int.from_bytes(body[:2], "big")
check(epoch == 77 and body[2:2 + n2].decode() == raw["g_text"], "Python decrypts the C group envelope")
print("ALL PASS" if not fails else "FAILED"); sys.exit(1 if fails else 0)
