"""Python side of C-OS Message end-to-end encryption (the same construction as userland/programs/messenger/e2e.inc), used by the test peers
that play "the other person". `E2E` holds one account's keys; `learn()` fetches a friend's bundle once, then `send_frame` / `text` are synchronous."""
import base64, hashlib, os
from cryptography.hazmat.primitives.asymmetric import x25519, ec
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, decode_dss_signature, encode_dss_signature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cosmsg_server import frame, unesc

b64e = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
def b64d(s): return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))
def raw_pub(k): return k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def raw_priv(k): return k.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
def x_exchange(priv, pub): return priv.exchange(x25519.X25519PublicKey.from_public_bytes(pub))
def spk_digest(sid, spk, ik_dh): return hashlib.sha256(b"C-OS-SPK-v1" + sid.to_bytes(4, "big") + spk + ik_dh).digest()
def fingerprint(ik_dh, ik_sg): return hashlib.sha256(b"C-OS-FP-v1" + ik_dh + ik_sg).digest()
def derive(dh, ik_s, ik_r, sid, eph):
    km = HKDF(algorithm=hashes.SHA256(), length=44, salt=b"C-OS-E2E-v1", info=ik_s + ik_r + sid.to_bytes(4, "big") + eph).derive(dh)
    return km[:32], km[32:]
def aad(ctx, sid, eph): return b"E2E1|" + ctx.encode() + b"|" + sid.to_bytes(4, "big") + eph

def safety_number(fa, fb):
    lo, hi = (fa, fb) if fa <= fb else (fb, fa)
    h = hashlib.sha256(b"C-OS-SAFETY-v1" + lo + hi).digest(); h2 = hashlib.sha256(h + b"2").digest(); out = []
    for g in range(12):
        s = h if g < 6 else h2; k = (g % 6) * 5; out.append("%05d" % (int.from_bytes(s[k:k + 5], "big") % 100000))
    return " ".join(out)

def file_seal(key, data):
    out = b""; n = len(data); idx = 0; off = 0
    while True:
        seg = data[off:off + 4096]; last = off + len(seg) >= n
        out += ChaCha20Poly1305(key).encrypt(bytes(3) + idx.to_bytes(8, "big") + bytes([1 if last else 0]), seg, b"C-OS-FILE-v1"); off += len(seg); idx += 1
        if last: return out
def file_open(key, blob):
    out = b""; off = 0; idx = 0
    while off < len(blob):
        seg = blob[off:off + 4096 + 16]; last = off + len(seg) >= len(blob)
        out += ChaCha20Poly1305(key).decrypt(bytes(3) + idx.to_bytes(8, "big") + bytes([1 if last else 0]), seg, b"C-OS-FILE-v1"); off += len(seg); idx += 1
    return out

class E2E:
    def __init__(self, my_id):
        self.me = my_id; self.dh = x25519.X25519PrivateKey.generate(); self.sg = ec.generate_private_key(ec.SECP256R1())
        self.spk = x25519.X25519PrivateKey.generate(); self.spk_id = 1
        self.ik_dh = raw_pub(self.dh); self.ik_sg = self.sg.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        self.peers = {}                                           # uid -> bundle fields
    def bundle(self):
        spk = raw_pub(self.spk); sig = self.sg.sign(spk_digest(self.spk_id, spk, self.ik_dh), ec.ECDSA(Prehashed(hashes.SHA256())))
        r, s = decode_dss_signature(sig)
        return self.ik_dh + self.ik_sg + self.spk_id.to_bytes(4, "big") + spk + r.to_bytes(32, "big") + s.to_bytes(32, "big")
    def publish_frame(self): return frame("KEYPUT", b64e(self.bundle()))
    def fingerprint(self): return fingerprint(self.ik_dh, self.ik_sg)
    def parse_bundle(self, b):
        ik_dh, ik_sg, sid, spk, sig = b[:32], b[32:97], int.from_bytes(b[97:101], "big"), b[101:133], b[133:197]
        pk = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), ik_sg)
        pk.verify(encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")), spk_digest(sid, spk, ik_dh), ec.ECDSA(Prehashed(hashes.SHA256())))
        return dict(ik_dh=ik_dh, ik_sg=ik_sg, sid=sid, spk=spk)
    def learn_key(self, uid, b64):
        if not b64: return False
        self.peers[uid] = self.parse_bundle(b64d(b64)); return True
    async def learn(self, r, w, get, uid, tries=15):
        """KEYGET until the friend (the app) has published its bundle"""
        import asyncio
        for _ in range(tries):
            w.write(frame("KEYGET", uid)); await w.drain(); k = await get(r, "KEY")
            if k[1] == uid and self.learn_key(uid, k[2]): return True
            await asyncio.sleep(2)
        return False
    def encrypt(self, uid, msgid, text, kind=0):
        return self.encrypt_ctx(uid, "%s|%s|%s|%d" % (self.me, uid, msgid, kind), text)
    def encrypt_ctx(self, uid, ctx, text):
        p = self.peers[uid]; eph = x25519.X25519PrivateKey.generate(); epub = raw_pub(eph)
        dh = x_exchange(self.dh, p["spk"]) + x_exchange(eph, p["ik_dh"]) + x_exchange(eph, p["spk"])
        key, nonce = derive(dh, self.ik_dh, p["ik_dh"], p["sid"], epub)
        pt = text.encode() if isinstance(text, str) else text
        padded = ((len(pt) + 2 + 63) // 64) * 64; body = len(pt).to_bytes(2, "big") + pt; body += bytes(padded - len(body))
        ct = ChaCha20Poly1305(key).encrypt(nonce, body, aad(ctx, p["sid"], epub))
        return "\x01E" + b64e(b"\x01" + p["sid"].to_bytes(4, "big") + epub + ct)
    def decrypt(self, uid, msgid, wire, kind=0):
        return self.decrypt_ctx(uid, "%s|%s|%s|%d" % (uid, self.me, msgid, kind), wire)
    def decrypt_ctx(self, uid, ctx, wire):
        env = b64d(wire[2:]); sid = int.from_bytes(env[1:5], "big"); eph = env[5:37]; p = self.peers[uid]
        assert sid == self.spk_id
        dh = x_exchange(self.spk, p["ik_dh"]) + x_exchange(self.dh, eph) + x_exchange(self.spk, eph)
        key, nonce = derive(dh, p["ik_dh"], self.ik_dh, sid, eph)
        body = ChaCha20Poly1305(key).decrypt(nonce, env[37:], aad(ctx, sid, eph))
        return body[2:2 + int.from_bytes(body[:2], "big")].decode()
    def text(self, m, ti=4, kind=0):
        """plain text of a MSG frame (index 4) or EDIT_IN (index 5) from a learnt friend"""
        t = m[ti]
        return self.decrypt(m[2], m[5] if kind == 0 else m[4], t, kind) if t.startswith("\x01E") else t
    def send_frame(self, msgid, uid, text, fid=None, kind=0, cmd="SEND"):
        wire = self.encrypt(uid, msgid, text, kind)
        if fid is None and text.startswith("\x01F|"): fid = text.split("|")[1]
        return frame(cmd, msgid, uid, wire, *([fid] if fid and cmd == "SEND" else []))

    # ---- groups: one key per epoch, handed to every member wrapped pairwise; messages are signed by their sender
    gkeys = None
    def gk_new(self, gid):
        if self.gkeys is None: self.gkeys = {}
        epoch = int.from_bytes(os.urandom(4), "big") or 1; self.gkeys[(gid, epoch)] = os.urandom(32); return epoch
    def wrap_key(self, uid, gid, epoch):
        import time as _t
        return self.encrypt_ctx(uid, "%s|%s|%s:%d|K" % (self.me, uid, gid, epoch), self.gkeys[(gid, epoch)] + int(_t.time()).to_bytes(4, "big"))
    def unwrap_key(self, from_uid, gid, epoch, wire):
        if self.gkeys is None: self.gkeys = {}
        ctx = "%s|%s|%s:%d|K" % (from_uid, self.me, gid, epoch)
        p = self.peers[from_uid]; env = b64d(wire[2:]); sid = int.from_bytes(env[1:5], "big"); eph = env[5:37]
        dh = x_exchange(self.spk, p["ik_dh"]) + x_exchange(self.dh, eph) + x_exchange(self.spk, eph)
        key, nonce = derive(dh, p["ik_dh"], self.ik_dh, sid, eph)
        body = ChaCha20Poly1305(key).decrypt(nonce, env[37:], aad(ctx, sid, eph)); k = body[2:2 + int.from_bytes(body[:2], "big")]
        assert len(k) == 36                                           # the key, then the maker's clock
        self.gkeys[(gid, epoch)] = k[:32]; return k[:32]
    def gencrypt(self, gid, epoch, msgid, text, kind=0, forge_sender=None):
        pt = text.encode() if isinstance(text, str) else text
        padded = ((len(pt) + 2 + 63) // 64) * 64; body = len(pt).to_bytes(2, "big") + pt; body += bytes(padded - len(body))
        nonce = os.urandom(12); ctx = "%s|%s|%s|%d" % (forge_sender or self.me, gid, msgid, kind)
        gaad = b"G1|" + ctx.encode() + b"|" + epoch.to_bytes(4, "big")
        ct = ChaCha20Poly1305(self.gkeys[(gid, epoch)]).encrypt(nonce, body, gaad)
        dg = hashlib.sha256(b"C-OS-GSIG-v1" + gaad + nonce + ct).digest()
        r, s = decode_dss_signature(self.sg.sign(dg, ec.ECDSA(Prehashed(hashes.SHA256()))))
        return "\x01G" + b64e(b"\x02" + epoch.to_bytes(4, "big") + nonce + ct + r.to_bytes(32, "big") + s.to_bytes(32, "big"))
    def gdecrypt(self, sender, gid, msgid, wire, kind=0):
        env = b64d(wire[2:]); epoch = int.from_bytes(env[1:5], "big"); nonce = env[5:17]; ct = env[17:-64]; sig = env[-64:]
        ctx = "%s|%s|%s|%d" % (sender, gid, msgid, kind); gaad = b"G1|" + ctx.encode() + b"|" + epoch.to_bytes(4, "big")
        dg = hashlib.sha256(b"C-OS-GSIG-v1" + gaad + nonce + ct).digest()
        pk = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), self.peers[sender]["ik_sg"])
        pk.verify(encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")), dg, ec.ECDSA(Prehashed(hashes.SHA256())))
        body = ChaCha20Poly1305(self.gkeys[(gid, epoch)]).decrypt(nonce, ct, gaad)
        return body[2:2 + int.from_bytes(body[:2], "big")].decode()

def event(kind, text="", ts=None):
    """the text of an end-to-end edit ("E") or delete ("D"): it travels inside an encrypted EDIT / GEDIT"""
    import time as _t
    return "%s%d|%s" % (kind, int(_t.time()) if ts is None else ts, text if kind == "E" else "")
def parse_event(p):
    k, rest = p[0], p[1:]; ts, _, text = rest.partition("|"); return k, int(ts), text
