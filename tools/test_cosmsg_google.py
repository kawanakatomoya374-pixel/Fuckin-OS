#!/usr/bin/env python3
"""Tests for Google sign-in in tools/cosmsg_server.py (a fake Google stands in for the real one) and for the ID-token checks."""
import asyncio, base64, json, os, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
import cosmsg_google as cg
import cosmsg_server as cs
from cosmsg_server import Server, frame
from test_cosmsg_server import C

try:
    from cryptography.hazmat.primitives.asymmetric import rsa, padding
    from cryptography.hazmat.primitives import hashes
except ImportError:
    print("SKIP: python 'cryptography' is needed to sign test tokens"); sys.exit(0)

def b64(b): return base64.urlsafe_b64encode(b).rstrip(b"=").decode()

KEY = rsa.generate_private_key(public_exponent=65537, key_size=2048)
NUM = KEY.public_key().public_numbers()
JWKS = {"keys": [{"kty": "RSA", "kid": "k1", "alg": "RS256", "n": b64(NUM.n.to_bytes(256, "big")), "e": b64(NUM.e.to_bytes(3, "big"))}]}
CID = "1234.apps.googleusercontent.com"

def make_token(claims, alg="RS256", kid="k1", key=KEY):
    h = b64(json.dumps({"alg": alg, "kid": kid, "typ": "JWT"}).encode()); p = b64(json.dumps(claims).encode())
    sig = key.sign((h + "." + p).encode(), padding.PKCS1v15(), hashes.SHA256()) if alg == "RS256" else b""
    return h + "." + p + "." + b64(sig)

def good(**kw):
    now = int(time.time())
    c = {"iss": "https://accounts.google.com", "aud": CID, "sub": "1100", "email": "taro@gmail.com", "email_verified": True, "iat": now, "exp": now + 3600}
    c.update(kw); return c

class FakeGoogle:
    """What GoogleBackend offers, with a script of answers instead of the network."""
    def __init__(self): self.script = {}; self.n = 0
    def start(self):
        self.n += 1
        return {"device_code": "dev%d" % self.n, "user_code": "ABCD-EFGH", "url": "https://www.google.com/device", "expires_in": 600, "interval": 3}
    def poll(self, dev): return self.script.get(dev, "pending")

async def main():
    ok = True
    def check(name, cond):
        nonlocal ok; print(("PASS " if cond else "FAIL ") + name); ok &= bool(cond)
    # --- the ID-token checks
    v = lambda t, **kw: cg.verify_id_token(t, JWKS, CID, **kw)
    def fails(t, text, **kw):
        try: v(t, **kw)
        except cg.GoogleError as e: return text in str(e)
        return False
    check("a correct token is accepted", v(make_token(good())) == {"sub": "1100", "email": "taro@gmail.com"})
    check("a forged signature is refused", fails(make_token(good(), key=rsa.generate_private_key(public_exponent=65537, key_size=2048)), "bad signature"))
    check("alg=none is refused", fails(make_token(good(), alg="none"), "unsupported"))
    check("a token for another app is refused", fails(make_token(good(aud="evil.apps.googleusercontent.com")), "audience"))
    check("an expired token is refused", fails(make_token(good(exp=int(time.time()) - 5)), "expired"))
    check("another issuer is refused", fails(make_token(good(iss="https://evil.example")), "issuer"))
    check("an unverified e-mail address is refused", fails(make_token(good(email_verified=False)), "not verified"))
    check("a non-Gmail address is refused", fails(make_token(good(email="a@corp.example")), "only gmail.com"))
    check("an unknown key id is refused", fails(make_token(good(), kid="zzz"), "unknown signing key"))
    t = make_token(good()); h, p, sg = t.split(".")
    tampered = h + "." + b64(json.dumps(good(sub="9999")).encode()) + "." + sg
    check("changing the payload breaks the signature", fails(tampered, "bad signature"))
    check("the e-mail address is masked for display", cg.mask_email("taro@gmail.com") == "t***@gmail.com")

    # --- the server with Google
    fake = FakeGoogle()
    srv = Server(os.path.join(tempfile.mkdtemp(), "g.db"), google=fake, require_google=True)
    server = await asyncio.start_server(srv.handle, "127.0.0.1", 0); port = server.sockets[0].getsockname()[1]
    c1 = await C().open(port)
    await c1.send("REG", "Taro"); check("an anonymous account is refused when Google is required", (await c1.get("NEED_GOOGLE")) == ["NEED_GOOGLE"])
    await c1.send("GREG", "Taro"); code = await c1.get("GOOGLE_CODE")
    check("a sign-in returns a user code and the Google URL", code[1] == "ABCD-EFGH" and code[2].startswith("https://www.google.com/device"))
    await c1.send("GPOLL"); check("polling too early does not reach Google", (await c1.get("GOOGLE_PENDING")) == ["GOOGLE_PENDING"])
    st = srv.q("SELECT COUNT(*) FROM users").fetchone()[0]; check("no account exists before Google approves", st == 0)
    fake.script["dev1"] = {"sub": "1100", "email": "taro@gmail.com"}
    # Google asks to be polled no faster than `interval` seconds; the server enforces that itself
    await asyncio.sleep(3.2)
    await c1.send("GPOLL"); reg = await c1.get("OK_REG"); gok = await c1.get("GOOGLE_OK")
    uid1, tok1 = reg[1], reg[2]
    check("approval creates the account and says who signed in", reg[3] == "Taro" and gok[1] == "t***@gmail.com")
    check("the token is stored only as a hash", srv.q("SELECT token FROM users WHERE id=?", uid1).fetchone()[0].startswith("h:") and tok1 not in srv.q("SELECT token FROM users WHERE id=?", uid1).fetchone()[0])
    await c1.get("GOOGLE_STATUS"); await c1.send("GSTATUS"); gs = await c1.get("GOOGLE_STATUS"); check("status says linked + required", gs[1:] == ["1", "t***@gmail.com", "1", "1"])
    # a second device of the same person
    c2 = await C().open(port); await c2.send("GREG", "Taro"); await c2.get("GOOGLE_CODE"); fake.script["dev2"] = {"sub": "1100", "email": "taro@gmail.com"}
    await asyncio.sleep(3.2); await c2.send("GPOLL"); reg2 = await c2.get("OK_REG")
    check("the same Google account gets the same C-OS account back", reg2[1] == uid1 and reg2[2] != tok1)
    c3 = await C().open(port); await c3.send("LOGIN", uid1, tok1); check("the old token stops working on the old device", (await c3.get("ERR"))[1] == "bad id or token")
    c4 = await C().open(port); await c4.send("LOGIN", uid1, reg2[2]); check("the new token logs in", (await c4.get("OK_LOGIN"))[1] == uid1)
    await c4.send("LOGOUT_ALL"); newt = (await c4.get("OK_LOGOUT_ALL"))[1]
    c5 = await C().open(port); await c5.send("LOGIN", uid1, reg2[2]); check("LOGOUT_ALL invalidates every other token", (await c5.get("ERR"))[1] == "bad id or token")
    c6 = await C().open(port); await c6.send("LOGIN", uid1, newt); check("the rotated token works", (await c6.get("OK_LOGIN"))[1] == uid1)
    # an old unlinked account under --require-google can only link
    srv.q("INSERT INTO users(id,token,name,created) VALUES('11111111','legacytoken000000000000','Old',0)"); srv.db.commit()
    c7 = await C().open(port); await c7.send("LOGIN", "11111111", "legacytoken000000000000"); await c7.get("OK_LOGIN")
    check("a legacy plain-text token is upgraded to a hash at login", srv.q("SELECT token FROM users WHERE id='11111111'").fetchone()[0].startswith("h:"))
    await c7.send("FIND", uid1); check("an unlinked account is held back", "sign in with Google first" in (await c7.get("ERR"))[1])
    await c7.send("GLINK"); await c7.get("GOOGLE_CODE"); fake.script["dev3"] = {"sub": "1100", "email": "taro@gmail.com"}
    await asyncio.sleep(3.2); await c7.send("GPOLL"); check("one Google account cannot be linked twice", "already linked to another" in (await c7.get("ERR"))[1])
    await c7.send("GLINK"); await c7.get("GOOGLE_CODE"); fake.script["dev4"] = {"sub": "2200", "email": "old@gmail.com"}
    await asyncio.sleep(3.2); await c7.send("GPOLL"); check("a legacy account can link its own Google account", (await c7.get("GOOGLE_OK"))[1] == "o***@gmail.com")
    await c7.send("FIND", uid1); check("after linking it works normally", (await c7.get("FOUND"))[1] == uid1)
    cx = await C().open(port); await cx.send("GREG", "x"); await cx.get("GOOGLE_CODE"); fake.script["dev5"] = "denied"; await asyncio.sleep(3.2)
    await cx.send("GPOLL"); check("cancelling at Google ends the sign-in", "cancelled" in (await cx.get("ERR"))[1])
    # five wrong logins lock the id for a while
    cl = []
    for i in range(5):
        c = await C().open(port); await c.send("LOGIN", uid1, "wrong"); await c.get("ERR")
    c = await C().open(port); await c.send("LOGIN", uid1, newt); check("five wrong tokens lock the account's login for a while", "too many failed logins" in (await c.get("ERR"))[1])
    # without Google configured everything works as before
    plain = Server(os.path.join(tempfile.mkdtemp(), "p.db"))
    ps = await asyncio.start_server(plain.handle, "127.0.0.1", 0); pp = ps.sockets[0].getsockname()[1]
    p1 = await C().open(pp); await p1.send("REG", "Anon"); check("an open server still allows anonymous accounts", (await p1.get("OK_REG"))[3] == "Anon")
    await p1.send("GLINK"); check("no Google configured: GLINK says so", "no Google sign-in" in (await p1.get("ERR"))[1])
    print("ALL PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
asyncio.run(main())
