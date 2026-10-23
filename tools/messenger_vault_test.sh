#!/usr/bin/env bash
# Host check of the app-lock crypto (userland/programs/messenger/vault_crypto.inc): scrypt against Python's hashlib.scrypt (RFC 7914
# vectors and random inputs) and AES-256-GCM against the `cryptography` package, both directions, plus tamper detection.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=$(mktemp -d /tmp/vault_test.XXXXXX)
BS="$ROOT/src/third_party/bearssl"
gcc -O2 -w -I"$BS/inc" -I"$BS/src" -o "$OUT/vt" "$ROOT/tools/messenger_vault_test/test.c" $(find "$BS/src" -name '*.c' ! -name sysrng.c)
python3 - "$OUT/vt" <<'PY'
import sys, subprocess, hashlib, os, secrets
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
vt = sys.argv[1]; bad = 0
def run(*a): return subprocess.run([vt, *map(str, a)], capture_output=True, text=True).stdout.split()
def check(n, c):
    global bad; print(("PASS " if c else "FAIL ") + n); bad += not c
# RFC 7914 section 12 (the first two vectors; the 1 MiB one is checked below with N=2^14)
check("scrypt RFC 7914 vector 1 (empty password, N=16)", run("scrypt", "", "", 16, 1, 1, 64) == [hashlib.scrypt(b"", salt=b"", n=16, r=1, p=1, dklen=64).hex()])
check("scrypt RFC 7914 vector 2", run("scrypt", "password", b"NaCl".hex(), 1024, 8, 16, 64) == [hashlib.scrypt(b"password", salt=b"NaCl", n=1024, r=8, p=16, dklen=64).hex()])
check("scrypt matches Python for the app's parameters (N=2^14, r=8, p=1)", run("scrypt", "correct horse", "00112233445566778899aabbccddeeff", 16384, 8, 1, 32) == [hashlib.scrypt(b"correct horse", salt=bytes.fromhex("00112233445566778899aabbccddeeff"), n=16384, r=8, p=1, dklen=32).hex()])
for i in range(3):
    pw, salt = secrets.token_hex(5), secrets.token_bytes(16)
    check("scrypt matches Python for random input %d" % i, run("scrypt", pw, salt.hex(), 2048, 8, 1, 32) == [hashlib.scrypt(pw.encode(), salt=salt, n=2048, r=8, p=1, dklen=32).hex()])
for i, n in enumerate((0, 1, 15, 16, 17, 1000, 3000)):
    key, nonce, aad, pt = secrets.token_bytes(32), secrets.token_bytes(12), "chat_u1234.txt", secrets.token_bytes(n)
    ref = AESGCM(key).encrypt(nonce, pt, aad.encode())
    out = subprocess.run([vt, "seal", key.hex(), nonce.hex(), aad, pt.hex()], capture_output=True, text=True).stdout.split("\n")
    ct, tag = out[0], out[1]
    check("AES-GCM seal of %d bytes equals the reference" % n, bytes.fromhex(ct) + bytes.fromhex(tag) == ref)
    o = subprocess.run([vt, "open", key.hex(), nonce.hex(), aad, ref[:-16].hex(), ref[-16:].hex()], capture_output=True, text=True).stdout.strip()
    check("AES-GCM opens what the reference sealed (%d bytes)" % n, o == pt.hex())
key, nonce, pt = secrets.token_bytes(32), secrets.token_bytes(12), b"top secret chat history"
ref = AESGCM(key).encrypt(nonce, pt, b"a.txt"); ct, tag = ref[:-16], ref[-16:]
op = lambda aad, c, t: subprocess.run([vt, "open", key.hex(), nonce.hex(), aad, c.hex(), t.hex()], capture_output=True, text=True).stdout.strip()
check("a changed ciphertext is rejected", op("a.txt", bytes([ct[0] ^ 1]) + ct[1:], tag) == "FAIL")
check("a changed tag is rejected", op("a.txt", ct, bytes([tag[0] ^ 1]) + tag[1:]) == "FAIL")
check("a ciphertext moved to another file name is rejected", op("b.txt", ct, tag) == "FAIL")
print("ALL PASS" if not bad else "FAILED"); sys.exit(1 if bad else 0)
PY
