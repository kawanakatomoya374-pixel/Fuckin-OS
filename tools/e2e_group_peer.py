#!/usr/bin/env python3
"""Group end-to-end encryption test: the guest (C-OS Message in QEMU) owns a group; Bob and Carol (this script, each with own keys) join.
Checks the group key hand-out, sender-signed messages, forgery / tampering / plain-text refusal, a new key after a member is removed
(the removed one gets nothing), and encrypted group attachments - and that the server never sees any text."""
import asyncio, base64, collections, os, re, sqlite3, ssl, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_e2e import E2E, b64d, file_open
from cosmsg_server import frame, unesc
port, db, logp = int(sys.argv[1]), sys.argv[2], sys.argv[3]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
def type_text(t): subprocess.run(["python3", TYPE, t], stdout=subprocess.DEVNULL, check=False)
def log(): return open(logp, errors="replace").read()
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
class Conn:
    def __init__(self, r, w): self.r, self.w, self.q = r, w, collections.deque()
    async def send(self, *f): self.w.write(frame(*f)); await self.w.drain()
    async def get(self, want, t=25):
        for fr in list(self.q):
            if fr[0] == want: self.q.remove(fr); return fr
        end = time.time() + t
        while True:
            line = await asyncio.wait_for(self.r.readline(), max(0.1, end - time.time()))
            if not line: raise ConnectionError("closed")
            fr = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if fr[0] == want: return fr
            self.q.append(fr)
    async def none(self, want, t):
        try: await self.get(want, t); return False
        except (asyncio.TimeoutError, asyncio.CancelledError): return True
async def drain(c, t=1.5):
    """throw away whatever is still queued for this connection"""
    c.q.clear()
    while True:
        try: await asyncio.wait_for(c.r.readline(), t)
        except asyncio.TimeoutError: return
async def wait_log(pat, t=60):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log())
        if m: return m
        await asyncio.sleep(1)
    return None
def db_all(q, *a):
    c = sqlite3.connect(db); r = c.execute(q, a).fetchall(); c.close(); return r
async def member(name, ctx_):
    r, w = await asyncio.open_connection("127.0.0.1", port, ssl=ctx_); c = Conn(r, w)
    await c.send("REG", name); reg = await c.get("OK_REG"); e = E2E(reg[1]); w.write(e.publish_frame()); await w.drain(); return c, e, reg[1]
async def learn(c, e, uid):
    for _ in range(15):
        await c.send("KEYGET", uid); k = await c.get("KEY")
        if k[1] == uid and e.learn_key(uid, k[2]): return True
        await asyncio.sleep(2)
    return False

async def chat_msg(c, e, app, gid, t=40):
    """the next group message that is not a roster (admins post the signed member list as an ordinary encrypted group message)"""
    while True:
        gm = await c.get("GMSG", t)
        if gm[3] == app:
            try:
                if e.gdecrypt(app, gid, gm[6], gm[5]).startswith("\x01R|"): await c.send("GOT", gm[1]); continue
            except Exception: pass
        return gm

async def main():
    type_text("/server 192.168.70.2")
    rows = []
    for _ in range(60):
        await asyncio.sleep(2)
        try: rows = db_all("SELECT id FROM users")
        except sqlite3.Error: rows = []
        if rows: break
    check("the guest registered on the server", bool(rows)); app = rows[0][0]
    sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); sslctx.check_hostname = False; sslctx.verify_mode = ssl.CERT_NONE
    bob, eb, bid = await member("Bob", sslctx); carol, ec, cid = await member("Carol", sslctx)
    await asyncio.sleep(3)
    type_text("/gnew Club 482915")
    ggid = None
    for _ in range(30):
        await asyncio.sleep(2)
        r = db_all("SELECT gid FROM groups WHERE name='Club'")
        if r: ggid = r[0][0]; break
    check("the guest made a group", ggid is not None)
    for c in (bob, carol): await c.send("GJOIN", ggid, "482915")
    await bob.get("OK_GJOIN"); await carol.get("OK_GJOIN")
    check("Bob can fetch the guest's key bundle without being its friend", await learn(bob, eb, app))
    check("Carol too", await learn(carol, ec, app))
    check("the guest sees both members", await wait_log(r"Carol joined Club", 30) is not None)

    # 0. nobody gets the group key until an admin approves them: the server's member list alone decides nothing
    await asyncio.sleep(4)
    check("members the owner has not approved get no key", await bob.none("GKEY_IN", 5) and await carol.none("GKEY_IN", 1))
    type_text("/gapprove all")
    check("approving members makes the guest post a roster and hand out the key", await wait_log(r"roster posted", 30) is not None)

    # 1. the key and the signed roster arrive; then the first message
    roster_txt = {}
    keys = {}
    for name, c, e in (("Bob", bob, eb), ("Carol", carol, ec)):
        gk = await c.get("GKEY_IN", 40); await c.send("GOT", gk[1])
        epoch = int(gk[5]); e.unwrap_key(app, ggid, epoch, gk[6]); keys[name] = epoch
        rm = await c.get("GMSG", 40); await c.send("GOT", rm[1]); roster_txt[name] = e.gdecrypt(app, ggid, rm[6], rm[5])
    check("the roster (signed by the owner, encrypted for the group) names both members and the owner", all(x.startswith("\x01R|") and (app + ":2") in x and (bid + ":0") in x and (cid + ":0") in x for x in roster_txt.values()))
    type_text("group-hello-ALPHA-1")
    for name, c, e in (("Bob", bob, eb), ("Carol", carol, ec)):
        gm = await chat_msg(c, e, app, ggid); await c.send("GOT", gm[1])
        check(name + " opens the key and reads the signed message", gk[6].startswith("\x01E") and gm[5].startswith("\x01G") and "ALPHA" not in gm[5] and e.gdecrypt(app, ggid, gm[6], gm[5]) == "group-hello-ALPHA-1")
    check("everybody got the same epoch", keys["Bob"] == keys["Carol"]); epoch1 = keys["Bob"]
    check("no group text or key in the server database", b"group-hello-ALPHA-1" not in open(db, "rb").read())

    # 1b. a stranger the server lists as a member (even as an admin) gets no key and cannot hand one out
    mal, em, mid = await member("Mallory", sslctx); await asyncio.sleep(2)
    await mal.send("GJOIN", ggid, "482915"); await mal.get("OK_GJOIN")
    check("Mallory (joined, not approved) learns the app's key", await learn(mal, em, app))
    type_text("/gadmin " + mid); await asyncio.sleep(4)                       # the server now calls her a sub-admin; the roster does not
    check("a member only the server calls an admin gets no group key", await mal.none("GKEY_IN", 8))
    n_ref = len(re.findall(r"a group key from somebody who is not an admin was refused", log()))
    fake_epoch = em.gk_new(ggid); await mal.send("GKEY", ggid, app, str(fake_epoch), em.wrap_key(app, ggid, fake_epoch)); await mal.get("OK_GKEY", 15)
    check("a group key from somebody the roster does not trust is refused", await wait_log(r"a group key from somebody who is not an admin was refused", 30) is not None and len(re.findall(r"a group key from somebody who is not an admin was refused", log())) == n_ref + 1)
    type_text("after-fake-key-ZETA")
    gm = await chat_msg(bob, eb, app, ggid); await bob.send("GOT", gm[1])
    check("messages still use the real key (the fake epoch was not taken)", int.from_bytes(b64d(gm[5][2:])[1:5], "big") == epoch1 and eb.gdecrypt(app, ggid, gm[6], gm[5]) == "after-fake-key-ZETA")
    await drain(mal, 1); await carol.send("GOT", (await chat_msg(carol, ec, app, ggid))[1])

    # 2. members write
    await bob.send("GSEND", "201", ggid, eb.gencrypt(ggid, epoch1, "201", "from-Bob-BETA-2")); await bob.get("GSENT")
    check("the app reads Bob's message", await wait_log(r"received a group message \(%d bytes\) from %s" % (len("from-Bob-BETA-2"), bid), 30) is not None)
    gm = await chat_msg(carol, ec, app, ggid); await carol.send("GOT", gm[1])
    ec.learn_key(bid, None) if False else None
    check("Carol reads Bob's message (sender signature checked against Bob's key)", await learn(carol, ec, bid) and ec.gdecrypt(bid, ggid, gm[6], gm[5]) == "from-Bob-BETA-2")

    # 3. tampering, forgery and plain text
    n0 = len(re.findall(r"could not decrypt a group message", log()))
    wire = bytearray(ec.gencrypt(ggid, epoch1, "301", "tamper-me-GAMMA").encode()); wire[40] ^= 1
    await carol.send("GSEND", "301", ggid, bytes(wire).decode()); await carol.get("GSENT")
    check("a tampered group message is refused", await wait_log(r"could not decrypt a group message", 30) is not None and len(re.findall(r"could not decrypt a group message", log())) == n0 + 1)
    await carol.send("GSEND", "302", ggid, ec.gencrypt(ggid, epoch1, "302", "forged-DELTA", forge_sender=bid)); await carol.get("GSENT")
    await asyncio.sleep(4)
    check("a message made to look like Bob's is refused", len(re.findall(r"could not decrypt a group message", log())) == n0 + 2)
    placeholder = "[暗号化されていないメッセージを受け取りましたが、設定により破棄しました]"
    await bob.send("GSEND", "202", ggid, "plain-group-text"); await bob.get("GSENT")
    check("plain text in the group is refused", await wait_log(r"received a group message \(%d bytes\) from %s" % (len(placeholder.encode()), bid), 30) is not None)

    # 3b. announcements carry a signed time: an older genuine one (or one without a time) cannot replace a newer one
    type_text("/gadmin " + bid); await asyncio.sleep(4); await drain(bob, 1)
    async def announce(text):
        await bob.send("GANN", ggid, eb.gencrypt(ggid, epoch1, "0", text, kind=5)); await bob.get("OK_GANN", 20)
    def count(pat): return len(re.findall(pat, log()))
    a0, i0 = count(r"announcement accepted"), count(r"an older announcement was ignored"); now = int(time.time())
    await announce("T%d|meeting at 7" % now)
    check("an admin's encrypted announcement is accepted", await wait_log(r"announcement accepted", 30) is not None and count(r"announcement accepted") == a0 + 1)
    await announce("T%d|meeting at 3 (older, replayed)" % (now - 1000)); await asyncio.sleep(4)
    check("an older genuine announcement is not taken back", count(r"an older announcement was ignored") == i0 + 1 and count(r"announcement accepted") == a0 + 1)
    await announce("untimed old announcement"); await asyncio.sleep(4)
    check("an announcement without a time cannot replace a timed one", count(r"an older announcement was ignored") == i0 + 2)
    await announce("T%d|meeting at 9" % (now + 5)); await asyncio.sleep(4)
    check("a newer announcement is accepted", count(r"announcement accepted") == a0 + 2)
    await drain(bob, 1); await drain(carol, 1)

    # 4. Bob is removed: a new key, and Bob does not get it
    await drain(bob); await drain(carol)
    type_text("/gkick " + bid)
    check("the guest makes a new group key after the removal", await wait_log(r"rekeyed a group after a member left", 40) is not None)
    gk = await carol.get("GKEY_IN", 40); await carol.send("GOT", gk[1]); epoch2 = int(gk[5]); ec.unwrap_key(app, ggid, epoch2, gk[6])
    check("Carol receives the new epoch", epoch2 != epoch1)
    check("the removed member gets no new key", await bob.none("GKEY_IN", 6))
    type_text("after-removal-EPSILON-5")
    gm = await chat_msg(carol, ec, app, ggid); await carol.send("GOT", gm[1])
    check("messages now use the new key and only Carol can read them", int.from_bytes(b64d(gm[5][2:])[1:5], "big") == epoch2 and ec.gdecrypt(app, ggid, gm[6], gm[5]) == "after-removal-EPSILON-5")
    check("Bob gets nothing any more", await bob.none("GMSG", 4))
    check("Mallory never got a key", await mal.none("GKEY_IN", 1))

    # 5. attachment in the group
    await drain(carol); await drain(bob)
    type_text("/send /etc/os-release")
    am = await chat_msg(carol, ec, app, ggid, 60); await carol.send("GOT", am[1])
    ann = ec.gdecrypt(app, ggid, am[6], am[5]).split("|")
    check("the group announcement carries the file key inside the encrypted message", ann[0] == "\x01F" and len(ann) == 5 and ann[3] == "os-release")
    check("the server never learns the file name", db_all("SELECT name FROM files WHERE fid=?", ann[1])[0][0] == "e2e.bin")
    await carol.send("FGET", ann[1]); data = b""; last = 0
    while not last:
        d = await carol.get("FDATA", 30); data += base64.b64decode(d[4]); last = int(d[3])
    check("the file on the server is ciphertext and opens with the key", b"C-OS" not in data and b"C-OS" in file_open(b64d(ann[4]), data))
    sys.exit(0 if ok else 1)
asyncio.run(main())
