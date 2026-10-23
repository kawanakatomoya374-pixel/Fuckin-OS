#!/usr/bin/env python3
"""Abuse limits of cosmsgd (the C server; the Python reference does not have them): aggregate backlog caps, replay size, hash budget,
connection reaping, FCH flooding.   test_cosmsgd_limits.py PORT"""
import asyncio, os, socket, ssl, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_server import frame, unesc
port = int(sys.argv[1]); host = "127.0.0.1"
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)

class C:
    async def open(self):
        self.r, self.w = await asyncio.open_connection(host, port, ssl=ctx); return self
    async def send(self, *f): self.w.write(frame(*f)); await self.w.drain()
    async def get(self, want=None, t=10):
        while True:
            line = await asyncio.wait_for(self.r.readline(), t)
            if not line: raise ConnectionError("closed")
            f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if want is None or f[0] == want: return f
async def newuser(name):
    c = await C().open(); await c.send("REG", name); r = await c.get("OK_REG"); await c.get("SYNCED"); return c, r
async def befriend(a, ra, b, rb):
    await a.send("FREQ", rb[1], "hi"); await a.get("OK_FREQ"); await b.get("FREQ_IN"); await b.send("FACC", ra[1]); await a.get("FRIEND"); await b.get("FRIEND"); await a.get("PRES"); await b.get("PRES")
async def login(r):
    c = await C().open(); await c.send("LOGIN", r[1], r[2]); await c.get("OK_LOGIN"); return c

async def stalled_tcp(t):                     # a TCP connection that never starts TLS: it must be dropped
    loop = asyncio.get_running_loop(); s = socket.socket(); s.setblocking(False); await loop.sock_connect(s, (host, port))
    t0 = time.time()
    try:
        while time.time() - t0 < t:
            try:
                if await asyncio.wait_for(loop.sock_recv(s, 100), 1) == b"": return time.time() - t0
            except asyncio.TimeoutError: pass
            except ConnectionError: return time.time() - t0
    finally: s.close()
    return None

async def idle_before_login(t):               # TLS up, but no login: dropped after ~20 s
    c = await C().open(); t0 = time.time()
    try:
        while time.time() - t0 < t:
            try:
                if not await asyncio.wait_for(c.r.readline(), 1): return time.time() - t0
            except asyncio.TimeoutError: await c.send("PING")      # pings before login must not keep the connection
            except ConnectionError: return time.time() - t0
    except ConnectionError: return time.time() - t0
    return None

async def fch_flood():
    c = await C().open(); t0 = time.time()
    try:
        for i in range(400): c.w.write(frame("FCH", "x", "0", "1", "AAAA")); 
        await c.w.drain()
        while time.time() - t0 < 10:
            if not await asyncio.wait_for(c.r.readline(), 5): return True
    except (ConnectionError, asyncio.TimeoutError): return True
    return False

async def sender_total_cap():
    a, ra = await newuser("Spammer"); peers = []
    for i in range(4):
        b, rb = await newuser("Peer%d" % i); await befriend(a, ra, b, rb); peers.append((b, rb))
    conns = [await login(ra) for _ in range(8)]                    # each connection has its own flood counter
    replies = []
    async def burst(ci, c):
        for k in range(160):
            rb = peers[ci % 4][1]; await c.send("SEND", "s%d_%d" % (ci, k), rb[1], "hello")
            while True:
                fr = await c.get()
                if fr[0] in ("SENT", "NOSEND"): replies.append(fr[0]); break
            if k % 30 == 29: await asyncio.sleep(5.3)
    await asyncio.gather(*[burst(i, c) for i, c in enumerate(conns)])
    sent = replies.count("SENT")
    check("one sender can leave at most 1200 messages waiting over all recipients (%d accepted of %d)" % (sent, len(replies)), sent == 1200 and replies.count("NOSEND") == 80 and len(replies) == 1280)

async def replay_fits():
    a, ra = await newuser("Talker"); v, rv = await newuser("Victim"); await befriend(a, ra, v, rv)
    v.w.close()                                                   # the victim goes offline with nothing read
    await asyncio.sleep(0.5)
    big = "あ" * 600; accepted = 0
    for i in range(200):
        await a.send("SEND", "b%d" % i, rv[1], big)
        fr = await a.get()
        while fr[0] not in ("SENT", "NOSEND"): fr = await a.get()
        if fr[0] == "SENT": accepted += 1
        if i % 30 == 29: await asyncio.sleep(5.3)
    check("a mailbox holds at most about 320 KB of text (%d messages of 1800 bytes accepted)" % accepted, 150 <= accepted <= 190)
    v2 = await login(rv); t0 = time.time(); got = 0
    try:
        while True:
            fr = await v2.get(t=20)
            if fr[0] == "MSG": got += 1
            if fr[0] == "SYNCED": break
        check("the login replay of a full mailbox ends with SYNCED (%d messages, %.1f s)" % (got, time.time() - t0), got == accepted)
    except (ConnectionError, asyncio.TimeoutError):
        check("the login replay of a full mailbox ends with SYNCED", False)

async def hash_budget():
    a, ra = await newuser("Hasher"); await a.send("GNEW", "Budget", "593724"); gid = (await a.get("OK_GNEW"))[1]
    b, rb = await newuser("Guesser"); busy = 0; wrong = 0
    for i in range(35):
        await b.send("GJOIN", gid, "%06d" % (111111 + i * 7))
        fr = await b.get("ERR")
        if "too many password operations" in fr[1]: busy += 1
        else: wrong += 1
        if fr[1].startswith("too many wrong passwords"): wrong += 0
    check("a connection gets a budget of password hashes (%d answered, %d told to wait)" % (wrong, busy), busy >= 1 and wrong >= 20)

async def main():
    t = await asyncio.gather(stalled_tcp(20), idle_before_login(40), fch_flood(), hash_budget())
    check("a connection that never starts TLS is dropped within ~10 s (%s)" % (None if t[0] is None else "%.1f s" % t[0]), t[0] is not None and 8 <= t[0] <= 14)
    check("a connection that does not log in is dropped after ~20 s even when it sends pings (%s)" % (None if t[1] is None else "%.1f s" % t[1]), t[1] is not None and 18 <= t[1] <= 30)
    check("unauthenticated FCH lines count against the flood limit", t[2] is True)
    await sender_total_cap()
    await replay_fits()
    # the server still answers
    a, ra = await newuser("Still"); check("the server still serves new users after all of it", ra[1].isdigit())
    print("LIMITS TEST: " + ("ALL PASS" if ok else "FAILED")); sys.exit(0 if ok else 1)
asyncio.run(main())
