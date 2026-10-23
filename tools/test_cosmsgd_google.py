#!/usr/bin/env python3
"""Google sign-in of cosmsgd against tools/fake_google_https.py (a stand-in for Google that speaks real HTTPS and signs real RS256 tokens).
   test_cosmsgd_google.py CMSG_PORT FAKE_PORT [--host H]  - the server must already use a google.conf that points at the fake."""
import asyncio, os, ssl, sys, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_server import frame, unesc
port, fport = int(sys.argv[1]), int(sys.argv[2]); host = sys.argv[sys.argv.index("--host") + 1] if "--host" in sys.argv else "127.0.0.1"
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
class C:
    async def open(self): self.r, self.w = await asyncio.open_connection(host, port, ssl=ctx); return self
    async def send(self, *f): self.w.write(frame(*f)); await self.w.drain()
    async def get(self, want=None, t=15):
        while True:
            line = await asyncio.wait_for(self.r.readline(), t)
            if not line: raise ConnectionError("closed")
            f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if want is None or f[0] == want: return f
def script(dev, **kw):
    kw["dev"] = dev; fctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); fctx.check_hostname = False; fctx.verify_mode = ssl.CERT_NONE
    urllib.request.urlopen(urllib.request.Request("https://localhost:%d/_script" % fport, urllib.parse.urlencode(kw).encode()), context=fctx).read()
async def signin(c, dev, **kw):
    """GREG/GLINK was sent; waits for the code, scripts the fake, polls until something other than PENDING comes back."""
    code = await c.get("GOOGLE_CODE"); script(dev, **kw)
    for _ in range(8):
        await asyncio.sleep(3.3); await c.send("GPOLL"); f = await c.get()
        while f[0] not in ("GOOGLE_PENDING", "ERR", "OK_REG", "GOOGLE_OK"): f = await c.get()
        if f[0] != "GOOGLE_PENDING": return code, f
    return code, f
async def main():
    c1 = await C().open(); await c1.send("REG", "Taro"); check("an anonymous account is refused when Google is required", (await c1.get("NEED_GOOGLE")) == ["NEED_GOOGLE"])
    await c1.send("GREG", "Taro"); code = await c1.get("GOOGLE_CODE")
    check("a sign-in returns a user code and the Google URL", code[1] == "ABCD-EFGH" and code[2].startswith("https://www.google.com/device") and code[4] == "3")
    await c1.send("GPOLL"); check("polling too early does not reach Google", (await c1.get("GOOGLE_PENDING")) == ["GOOGLE_PENDING"])
    await asyncio.sleep(3.3); await c1.send("GPOLL"); check("Google says pending", (await c1.get("GOOGLE_PENDING")) == ["GOOGLE_PENDING"])
    script("dev1", state="ok", sub="1100", email="taro@gmail.com"); await asyncio.sleep(3.3); await c1.send("GPOLL")
    reg = await c1.get("OK_REG"); gok = await c1.get("GOOGLE_OK"); uid1, tok1 = reg[1], reg[2]
    check("approval creates the account and says who signed in", reg[3] == "Taro" and gok[1] == "t***@gmail.com")
    st = await c1.get("GOOGLE_STATUS"); await c1.get("SYNCED"); await c1.send("GSTATUS"); gs = await c1.get("GOOGLE_STATUS"); check("status says linked + required + configured", gs[1:] == ["1", "t***@gmail.com", "1", "1"])
    c2 = await C().open(); await c2.send("GREG", "Taro"); _, reg2 = await signin(c2, "dev2", state="ok", sub="1100", email="taro@gmail.com")
    check("the same Google account gets the same C-OS account back", reg2[0] == "OK_REG" and reg2[1] == uid1 and reg2[2] != tok1)
    c3 = await C().open(); await c3.send("LOGIN", uid1, tok1); check("the old token stops working on the old device", (await c3.get("ERR"))[1] == "bad id or token")
    c4 = await C().open(); await c4.send("LOGIN", uid1, reg2[2]); check("the new token logs in", (await c4.get("OK_LOGIN"))[1] == uid1)
    await c4.send("LOGOUT_ALL"); newt = (await c4.get("OK_LOGOUT_ALL"))[1]
    c5 = await C().open(); await c5.send("LOGIN", uid1, reg2[2]); check("LOGOUT_ALL invalidates every other token", (await c5.get("ERR"))[1] == "bad id or token")
    # forged or wrong tokens
    for state, text in (("badsig", "bad signature"), ("badaud", "wrong audience"), ("expired", "token expired")):
        cx = await C().open(); await cx.send("GREG", "X"); _, f = await signin(cx, "dev%d" % (3 + ["badsig", "badaud", "expired"].index(state)), state=state, sub="7", email="x@gmail.com")
        check("a token with %s is refused" % text, f[0] == "ERR" and text in f[1])
    cx = await C().open(); await cx.send("GREG", "X"); _, f = await signin(cx, "dev6", state="ok", sub="8", email="x@corp.example"); check("a non-Gmail address is refused", f[0] == "ERR" and "only gmail.com" in f[1])
    cx = await C().open(); await cx.send("GREG", "X"); _, f = await signin(cx, "dev7", state="ok", sub="9", email="x@gmail.com", verified="0"); check("an unverified e-mail address is refused", f[0] == "ERR" and "not verified" in f[1])
    cx = await C().open(); await cx.send("GREG", "X"); _, f = await signin(cx, "dev8", state="denied"); check("cancelling at Google ends the sign-in", f[0] == "ERR" and "cancelled" in f[1])
    # linking: an account that exists but is not linked is held back under require
    legacy = None
    print("(a pre-existing unlinked account is covered by the Python test; here the server was started with Google required)")
    c9 = await C().open(); await c9.send("LOGIN", uid1, newt); await c9.get("OK_LOGIN"); await c9.send("GLINK"); _, f = await signin(c9, "dev9", state="ok", sub="1100", email="taro@gmail.com")
    check("linking the account's own Google account again is fine", f[0] == "GOOGLE_OK")
    c10 = await C().open(); await c10.send("GREG", "Other"); _, f = await signin(c10, "dev10", state="ok", sub="2200", email="other@gmail.com"); other = f
    c11 = await C().open(); await c11.send("LOGIN", other[1], other[2]); await c11.get("OK_LOGIN"); await c11.send("GLINK"); _, f = await signin(c11, "dev11", state="ok", sub="1100", email="taro@gmail.com")
    check("one Google account cannot be linked twice", f[0] == "ERR" and "already linked to another" in f[1])
    for i in range(5):
        c = await C().open(); await c.send("LOGIN", uid1, "wrong"); await c.get("ERR")
    c = await C().open(); await c.send("LOGIN", uid1, newt); check("five wrong tokens lock the account's login for a while", "too many failed logins" in (await c.get("ERR"))[1])
    print("ALL PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
asyncio.run(main())
