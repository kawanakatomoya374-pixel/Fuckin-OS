#!/usr/bin/env python3
"""Black-box protocol test of a C-OS Message relay server over TLS: tools/cosmsg_server.py (the reference) and cosmsgd
(userland/programs/cosmsgd, built for the host or running inside C-OS) must both pass it.
   test_cosmsgd.py PORT [--host H] [--phase1 FILE | --phase2 FILE]   phase1/2 split around a server restart (persistence)."""
import asyncio, json, os, ssl, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_server import frame, unesc
args = sys.argv[1:]; port = int(args[0]); host = args[args.index("--host") + 1] if "--host" in args else "127.0.0.1"
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)

class C:
    async def open(self):
        self.r, self.w = await asyncio.open_connection(host, port, ssl=ctx); return self
    async def send(self, *f): self.w.write(frame(*f)); await self.w.drain()
    async def get(self, want=None, t=8):
        while True:
            line = await asyncio.wait_for(self.r.readline(), t)
            if not line: raise ConnectionError("closed")
            f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if want is None or f[0] == want: return f
    async def quiet(self, t=0.4):
        try: await asyncio.wait_for(self.r.readline(), t); return False
        except asyncio.TimeoutError: return True

async def newuser(name):
    c = await C().open(); await c.send("REG", name); r = await c.get("OK_REG"); await c.get("SYNCED"); return c, r

async def befriend(a, ra, b, rb):
    await a.send("FREQ", rb[1], "hi"); await a.get("OK_FREQ"); await b.get("FREQ_IN"); await b.send("FACC", ra[1]); await a.get("FRIEND"); await b.get("FRIEND"); await a.get("PRES"); await b.get("PRES")

async def phase1(path):
    a, ra = await newuser("Alice"); b, rb = await newuser("Bob")
    await befriend(a, ra, b, rb)
    await a.send("SEND", "m1", rb[1], "kept across restart"); await a.get("SENT")
    await b.get("MSG")                                           # live copy; not acknowledged (no GOT) -> stays queued
    await a.send("HANDLE", "alice.keep"); await a.get("OK_HANDLE"); await a.send("BIO", "persisted bio"); await a.get("OK_BIO"); await a.send("DISC", "1"); await a.get("OK_DISC")
    await b.send("BLOCK", "99999999"); await b.get("OK_BLOCK")
    await a.send("GNEW", "Keepers", "593724"); gid = (await a.get("OK_GNEW"))[1]; await b.send("GJOIN", gid, "593724"); await b.get("OK_GJOIN")
    await a.send("GANN", gid, "kept announcement"); await a.get("OK_GANN"); await a.send("GSEND", "g1", gid, "group text kept"); await a.get("GSENT")
    await asyncio.sleep(2.2)                                      # the server saves changed state after ~1.5 s
    json.dump({"a": ra, "b": rb, "g": gid}, open(path, "w")); check("phase 1 prepared", True)

async def phase2(path):
    d = json.load(open(path)); ra, rb = d["a"], d["b"]
    b = await C().open(); await b.send("LOGIN", rb[1], rb[2]); check("login with the old token after a restart", (await b.get("OK_LOGIN"))[2] == "Bob")
    fr = await b.get("FRIEND"); check("the friendship survived the restart", fr[1] == ra[1] and fr[3] == "alice.keep")
    gr = await b.get("GROUP"); check("the group survived the restart", gr[1] == d["g"] and gr[2] == "Keepers" and gr[3] == ra[1])
    m = await b.get("MSG"); check("an unacknowledged message is delivered again", m[4] == "kept across restart" and m[5] == "m1")
    gm = await b.get("GMSG"); check("a queued group message survived", gm[5] == "group text kept")
    a = await C().open(); await a.send("LOGIN", ra[1], ra[2]); await a.get("OK_LOGIN")
    await a.send("ME"); me = await a.get("ME"); check("profile fields survived", me[3] == "alice.keep" and me[4] == "persisted bio" and me[5] == "1" and len(me[7]) == 8)

async def main():
    if "--phase1" in args: await phase1(args[args.index("--phase1") + 1]); return
    if "--phase2" in args: await phase2(args[args.index("--phase2") + 1]); return
    a, ra = await newuser("Alice"); b, rb = await newuser("Bob")
    check("register gives id+token", len(ra[1]) == 8 and len(ra[2]) == 24 and ra[3] == "Alice")
    c = await C().open(); await c.send("REG", "  " + "x" * 40 + "  "); rc = await c.get("OK_REG"); check("names are trimmed to 23 characters", rc[3] == "x" * 23)
    c2 = await C().open(); await c2.send("REG", "日本語の名前" * 6); r2 = await c2.get("OK_REG"); check("names count characters, not bytes", r2[3] == ("日本語の名前" * 6)[:23])
    await a.send("SEND", "m0", rb[1], "hi"); check("no messaging before friendship", (await a.get("NOSEND"))[2] == "not friends")
    await a.send("FIND", rb[1]); check("find a stranger by id", (await a.get("FOUND"))[2] == "Bob")
    await a.send("FIND", "00000000"); check("find an unknown id", (await a.get("ERR"))[1] == "no such user")
    await a.send("FREQ", ra[1], "me"); check("no request to yourself", (await a.get("ERR"))[0] == "ERR")
    await a.send("FREQ", rb[1], "Hello, I am Alice"); await a.get("OK_FREQ")
    fi = await b.get("FREQ_IN"); check("request delivered with intro", fi[1] == ra[1] and fi[2] == "Alice" and fi[3] == "Hello, I am Alice")
    await a.send("FREQ", rb[1], "again"); await a.get("OK_FREQ")
    await b.send("FACC", ra[1]); fa, fb = await a.get("FRIEND"), await b.get("FRIEND"); check("both get FRIEND", fa[2] == "Bob" and fb[2] == "Alice")
    await a.send("FREQ", rb[1], "x"); check("already friends", (await a.get("ERR"))[1] == "already friends")
    await b.send("FACC", "00000000"); check("accepting a request that does not exist", (await b.get("ERR"))[1] == "no such request")
    await a.send("SEND", "m1", rb[1], "tab\there\nline2 \\ backslash 日本語"); await a.get("SENT")
    m = await b.get("MSG"); check("message round trip incl. escapes", m[2] == ra[1] and m[4] == "tab\there\nline2 \\ backslash 日本語" and m[5] == "m1" and int(m[3]) > 1.7e9)
    await b.send("GOT", m[1]); check("delivery receipt", (await a.get("DELIV"))[1:] == ["m1", rb[1]])
    await a.send("SEND", "m1b", rb[1], "y" * 700); mm = await b.get("MSG"); check("messages are cut at 600 characters", len(mm[4]) == 600); await b.send("GOT", mm[1])
    await a.send("EDIT", "m1", rb[1], "edited text"); await a.get("OK_EDIT")
    e = await b.get("EDIT_IN"); check("edit reaches the receiver", e[2] == ra[1] and e[4] == "m1" and e[5] == "edited text"); await b.send("GOT", e[1])
    await a.send("DEL", "m1", rb[1]); await a.get("OK_DEL")
    d = await b.get("DEL_IN"); check("delete reaches the receiver", d[2] == ra[1] and d[4] == "m1"); await b.send("GOT", d[1])
    await a.send("SEND", "f2", rb[1], "\x01F|deadbeefdeadbeef|10|x.bin"); check("announcing a file you did not upload is refused", (await a.get("NOSEND"))[1] == "f2")
    b.w.close(); await asyncio.sleep(0.3)
    await a.send("SEND", "m2", rb[1], "while you were away"); await a.get("SENT")
    b = await C().open(); await b.send("LOGIN", rb[1], rb[2]); check("login", (await b.get("OK_LOGIN"))[2] == "Bob")
    await a.send("EDIT", "m2", rb[1], "while you were away (fixed)"); await a.get("OK_EDIT")
    m = await b.get("MSG"); check("offline message delivered at login", m[4] == "while you were away")
    e = await b.get("EDIT_IN"); check("offline edit follows its message in order", e[4] == "m2" and e[5] == "while you were away (fixed)")
    x = await C().open(); await x.send("LOGIN", rb[1], "wrong"); check("wrong token rejected", (await x.get("ERR"))[1] == "bad id or token")
    x = await C().open(); await x.send("SEND", "q", rb[1], "hi"); check("commands need a login", (await x.get("ERR"))[1] == "login first")
    await b.send("NOPE"); check("unknown command", (await b.get("ERR"))[1] == "unknown command")
    await b.send("PING"); check("ping", (await b.get("PONG")) == ["PONG"])
    await b.send("NAME", "Bobby"); check("rename", (await b.get("OK_NAME"))[1] == "Bobby"); check("friends hear about the new name", (await a.get("FRIEND"))[2] == "Bobby")
    await b.send("BLOCK", ra[1]); await b.get("OK_BLOCK"); await b.get("FRIEND_DEL")
    await a.send("SEND", "m3", rb[1], "x"); check("blocked sender cannot send", (await a.get("NOSEND"))[1] == "m3")
    await a.send("FREQ", rb[1], "again"); check("blocked sender cannot re-request", (await a.get("ERR"))[0] == "ERR")
    await b.send("BLOCKS"); bl = await b.get("BLOCKED"); await b.get("BLOCKS_END"); check("the block list shows who is blocked", bl[1] == ra[1] and bl[2] == "Alice")
    await b.send("UNBLOCK", ra[1]); await b.get("OK_UNBLOCK")
    await a.send("FREQ", rb[1], "friends again?"); await a.get("OK_FREQ")
    # ---- discovery ----
    d, rd = await newuser("Dana"); e, re_ = await newuser("Eve")
    await d.send("HANDLE", "Dana.Photo"); check("handle is normalised to lowercase", (await d.get("OK_HANDLE"))[1] == "dana.photo")
    await e.send("HANDLE", "dana.photo"); check("a taken handle is refused", (await e.get("ERR"))[1] == "that handle is taken")
    await e.send("HANDLE", "x"); check("an invalid handle is refused", (await e.get("ERR"))[1].startswith("handle:"))
    await e.send("HANDLE", "@Eve_01"); check("a leading @ is dropped", (await e.get("OK_HANDLE"))[1] == "eve_01")
    await d.send("BIO", "I take photos of trains"); await d.get("OK_BIO")
    await e.send("SEARCH", "dana"); check("a non-discoverable user is not found", (await e.get("RESULT_END"))[1] == "0")
    await d.send("DISC", "1"); await d.get("OK_DISC")
    await e.send("SEARCH", "@dana"); res = await e.get("RESULT"); await e.get("RESULT_END")
    check("a discoverable user is found by @handle", res[1] == rd[1] and res[3] == "dana.photo" and res[4] == "I take photos of trains")
    await e.send("SEARCH", "dan"); check("search matches a name prefix too", (await e.get("RESULT"))[1] == rd[1]); await e.get("RESULT_END")
    await e.send("SEARCH", "a"); check("one character is too short to search", (await e.get("RESULT_END"))[1] == "0")
    await e.send("PROFILE", rd[1]); pf = await e.get("PROFILE"); check("profile of a discoverable user", pf[3] == "dana.photo" and pf[4] == "I take photos of trains")
    await d.send("DISC", "0"); await d.get("OK_DISC")
    await e.send("PROFILE", rd[1]); pf = await e.get("PROFILE"); check("a private profile shows only the name", pf[2] == "Dana" and pf[3] == "" and pf[4] == "")
    await e.send("PROFILE", "00000000"); check("profile of nobody", (await e.get("ERR"))[1] == "no such user")
    await d.send("ME"); me = await d.get("ME"); code = me[7]
    check("ME returns the profile and an invite code", me[1] == rd[1] and me[3] == "dana.photo" and len(code) == 8 and me[5] == "0")
    await e.send("JOIN", code[:4] + "-" + code[4:].lower()); check("an invite code starts a friend request (dashes/case ignored)", (await e.get("OK_FREQ"))[1] == rd[1])
    fq = await d.get("FREQ_IN"); check("the request carries the sender's profile fields", fq[1] == re_[1] and fq[3] == "Joined with your invite code" and fq[4] == "eve_01")
    await d.send("INVITE_NEW"); newcode = (await d.get("INVITE"))[1]
    await e.send("JOIN", code); check("a replaced invite code stops working", (await e.get("ERR"))[1] == "invalid invite code" and newcode != code)
    await d.send("FDEC", re_[1]); await e.send("PING"); await e.get("PONG")
    await d.send("FACC", re_[1]); check("a declined request is gone", (await d.get("ERR"))[1] == "no such request")
    # ---- reports ----
    reps = []
    for i in range(5):
        rr, rrr = await newuser("Rep%d" % i); reps.append((rr, rrr))
    s, rs = await newuser("Spammy"); await s.send("HANDLE", "spammy.x"); await s.get("OK_HANDLE"); await s.send("DISC", "1"); await s.get("OK_DISC")
    for rr, rrr in reps:
        await rr.send("REPORT", rs[1], "spam", "buy now"); check("report accepted", (await rr.get("OK_REPORT"))[1] == rs[1])
    await reps[0][0].send("REPORT", rs[1], "again", ""); check("a second report the same day is refused", (await reps[0][0].get("ERR"))[1] == "you already reported that user today")
    await e.send("SEARCH", "spammy"); check("a restricted account disappears from search", (await e.get("RESULT_END"))[1] == "0")
    await s.send("FREQ", re_[1], "hi"); check("a restricted account cannot send requests", "restricted" in (await s.get("ERR"))[1])
    # ---- read receipts ----
    d1, rd1 = await newuser("Reader"); d2, rd2 = await newuser("Writer"); await befriend(d1, rd1, d2, rd2)
    await d1.send("READ", rd2[1], "123"); check("the writer is told how far the friend has read", (await d2.get("READ_IN"))[1:] == [rd1[1], "123"])
    d2.w.close(); await asyncio.sleep(0.3); await d1.send("READ", rd2[1], "456"); await asyncio.sleep(0.2)
    d2 = await C().open(); await d2.send("LOGIN", rd2[1], rd2[2]); await d2.get("OK_LOGIN")
    check("the last read position is re-sent at login", (await d2.get("READ_IN"))[2] == "456")
    await e.send("READ", rd2[1], "5"); await e.send("PING"); check("read from a non-friend is ignored", (await e.get("PONG")) == ["PONG"])
    # ---- typing and presence ----
    t1, rt1 = await newuser("Tina"); t2, rt2 = await newuser("Toru"); await befriend(t1, rt1, t2, rt2)
    await t1.send("TYPING", rt2[1]); ty = await t2.get("TYPING_IN"); check("a friend sees that you are typing", ty[1] == rt1[1])
    await t2.send("TYPING", rd[1]); await t1.send("PING"); check("typing to a stranger is ignored", (await t1.get("PONG")) == ["PONG"])
    t2.w.close(); await asyncio.sleep(0.4)
    pr = await t1.get("PRES"); check("a friend going offline is announced with the time", pr[1] == rt2[1] and pr[2] == "0" and int(pr[3]) > 0)
    t2 = await C().open(); await t2.send("LOGIN", rt2[1], rt2[2]); await t2.get("OK_LOGIN")
    pr = await t1.get("PRES"); check("coming back online is announced", pr[1] == rt2[1] and pr[2] == "1")
    await t2.send("SETPRES", "0"); check("SETPRES answers", (await t2.get("OK_SETPRES"))[1] == "0")
    pr = await t1.get("PRES"); check("a user who hides their presence appears as hidden", pr[1] == rt2[1] and pr[2] == "2")
    t1.w.close(); await asyncio.sleep(0.4)
    t1 = await C().open(); await t1.send("LOGIN", rt1[1], rt1[2]); await t1.get("OK_LOGIN")
    got = {}
    while "PRES" not in got:
        f = await t1.get(); got[f[0]] = f
    check("presence of every friend is sent at login", got["PRES"][1] == rt2[1] and got["PRES"][2] == "2")
    # ---- attachments ----
    import base64
    fa, rfa = await newuser("Filer"); fb, rfb = await newuser("Fan"); fc, rfc = await newuser("Fred"); await befriend(fa, rfa, fb, rfb)
    blob = os.urandom(20000)
    await fa.send("FUP", str(len(blob)), "dir/pic.png"); fid = (await fa.get("FUP_OK"))[1]
    for i in range(0, len(blob), 6144): await fa.send("FCH", fid, str(i // 6144), base64.b64encode(blob[i:i + 6144]).decode())
    await fa.send("FEND", fid); check("an upload completes", (await fa.get("FEND_OK"))[1] == fid)
    await fb.send("FGET", fid); check("download is refused before the file is sent to you", (await fb.get("FERR"))[2] == "no such file")
    await fa.send("SEND", "f1", rfb[1], "\x01F|%s|%d|pic.png" % (fid, len(blob))); await fa.get("SENT")
    got = await fb.get("MSG"); check("attachment announcement arrives", got[4].startswith("\x01F|" + fid)); await fb.send("GOT", got[1])
    await fb.send("FGET", fid); data = b""; last = 0
    while not last:
        d = await fb.get("FDATA"); data += base64.b64decode(d[4]); last = int(d[3])
    check("file downloads byte-exact", data == blob)
    await fa.send("FGET", fid); data = b""; last = 0
    while not last:
        d = await fa.get("FDATA"); data += base64.b64decode(d[4]); last = int(d[3])
    check("the owner can download it too", data == blob)
    await fc.send("FGET", fid); check("a stranger cannot download it", (await fc.get("FERR"))[2] == "no such file")
    await fa.send("FUP", str(11 * 1024 * 1024), "big.bin"); check("oversized upload is refused", (await fa.get("FERR"))[2].startswith("file too large"))
    await fa.send("FUP", "100", "bad.bin"); bf = (await fa.get("FUP_OK"))[1]
    await fa.send("FCH", bf, "0", base64.b64encode(b"x" * 60).decode()); await fa.send("FEND", bf); check("a short upload is refused at the end", (await fa.get("FERR"))[2] == "size mismatch")
    await fa.send("FUP", "100", "bad2.bin"); bf = (await fa.get("FUP_OK"))[1]
    await fa.send("FCH", bf, "1", base64.b64encode(b"x" * 60).decode()); check("a chunk out of order is refused", (await fa.get("FERR"))[2] == "bad chunk")
    await fa.send("FUP", "3000", "me.bmp"); afid = (await fa.get("FUP_OK"))[1]
    await fa.send("FCH", afid, "0", base64.b64encode(b"B" * 3000).decode()); await fa.send("FEND", afid); await fa.get("FEND_OK")
    await fa.send("AVATAR", afid); check("an uploaded image becomes the avatar", (await fa.get("OK_AVATAR"))[1] == afid)
    await fc.send("FGET", afid); data = b""; last = 0
    while not last:
        d = await fc.get("FDATA"); data += base64.b64decode(d[4]); last = int(d[3])
    check("an avatar can be downloaded by a stranger", data == b"B" * 3000)
    await fa.send("DISC", "1"); await fa.get("OK_DISC"); await fc.send("PROFILE", rfa[1]); pf = await fc.get("PROFILE"); check("the profile shows the avatar", pf[5] == afid)
    await fa.send("AVATAR", fid + "zz"); check("an unknown file cannot be the avatar", (await fa.get("ERR"))[0] == "ERR")
    big = os.urandom(3 * 1024 * 1024); await fa.send("FUP", str(len(big)), "big.dat"); bfid = (await fa.get("FUP_OK"))[1]
    for i in range(0, len(big), 6144): await fa.send("FCH", bfid, str(i // 6144), base64.b64encode(big[i:i + 6144]).decode())
    await fa.send("FEND", bfid); await fa.get("FEND_OK", 30)
    await fa.send("SEND", "f9", rfb[1], "\x01F|%s|%d|big.dat" % (bfid, len(big))); await fa.get("SENT"); await fb.get("MSG")
    await fb.send("FGET", bfid); data = bytearray(); last = 0
    while not last:
        d = await fb.get("FDATA", 30); data += base64.b64decode(d[4]); last = int(d[3])
    check("a 3 MB file streams through intact", bytes(data) == big)
    await fa.send("PING"); check("the connection still answers after big transfers", (await fa.get("PONG")) == ["PONG"])
    # ---- end-to-end encryption support ----
    ea, rea = await newuser("EAlice"); eb, reb = await newuser("EBob"); ec, rec = await newuser("EEve"); await befriend(ea, rea, eb, reb)
    bundle = "A" * 263
    await ea.send("KEYGET", reb[1]); r = await ea.get("KEY"); check("no key published yet = empty bundle", r[1] == reb[1] and r[2] == "")
    await eb.send("KEYPUT", "short"); check("a malformed key bundle is refused", (await eb.get("ERR")) is not None)
    await eb.send("KEYPUT", bundle); await eb.get("OK_KEYPUT")
    await ea.send("KEYGET", reb[1]); r = await ea.get("KEY"); check("a friend gets the published bundle", r[2] == bundle)
    await ec.send("KEYGET", reb[1]); check("a stranger cannot fetch a bundle", (await ec.get("ERR"))[1] == "not friends")
    env = "\x01E" + "B" * 2900
    await ea.send("SEND", "e1", reb[1], env); await ea.get("SENT"); got = await eb.get("MSG"); check("a long end-to-end envelope is relayed intact", got[4] == env)
    await ea.send("SEND", "e2", reb[1], "\x01E" + "B" * 3300); check("an oversized envelope is refused, not cut", (await ea.get("NOSEND"))[2] == "too long")
    await ea.send("EDIT", "e1", reb[1], env[:-1] + "C"); await ea.get("OK_EDIT"); got = await eb.get("EDIT_IN"); check("an edited envelope is relayed intact", got[5][-1] == "C" and len(got[5]) == len(env))
    ga, rga = await newuser("EGA"); gb, rgb = await newuser("EGB"); await befriend(ga, rga, gb, rgb)
    await ga.send("FUP", "100", "x.bin"); fid = (await ga.get("FUP_OK"))[1]; await ga.send("FCH", fid, "0", base64.b64encode(os.urandom(100)).decode()); await ga.send("FEND", fid); await ga.get("FEND_OK")
    await ga.send("SEND", "x1", rgb[1], "\x01E" + "D" * 200, fid); check("an encrypted message can grant a file by its fid", (await ga.get("SENT"))[1] == "x1")
    await gb.send("FGET", fid); check("the receiver may fetch it", (await gb.get("FDATA"))[1] == fid)
    await ga.send("SEND", "x2", rgb[1], "\x01E" + "D" * 200, "deadbeef"); check("a fid the sender does not own is refused", (await ga.get("NOSEND"))[2] == "file not uploaded")

    # ---- groups ----
    g1, r1 = await newuser("Gina"); g2, r2 = await newuser("Hugo"); g3, r3 = await newuser("Ivy")
    await g1.send("GNEW", "Trains", "123456"); check("a trivial password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains", "12ab56"); check("a non-numeric password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains", "48291"); check("a 5-digit password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains"); ng = await g1.get("OK_GNEW"); gid, gpw = ng[1], ng[3]
    check("a group gets an id and a generated 6-digit password", gid.startswith("g") and len(gpw) == 6 and gpw.isdigit())
    check("the creator is told about the group and its members", (await g1.get("GROUP"))[2] == "Trains" and (await g1.get("GMEMBER"))[3] == "Gina")
    await g2.send("GJOIN", gid, "000001"); check("joining with a wrong password fails", (await g2.get("ERR"))[1] == "wrong group ID or password")
    await g2.send("GJOIN", "g99999999", "123457"); check("an unknown group gives the same error", (await g2.get("ERR"))[1] == "wrong group ID or password")
    await g2.send("GSEND", "x1", gid, "let me in"); check("a non-member cannot send", (await g2.get("ERR"))[1] == "you are not in that group")
    await g2.send("GJOIN", gid, gpw); check("joining with the right password works", (await g2.get("OK_GJOIN"))[2] == "Trains")
    jm = await g1.get("GMEMBER"); check("existing members see the newcomer", jm[2] == r2[1] and jm[3] == "Hugo")
    await g1.send("GSEND", "gm1", gid, "hello group"); check("sender gets GSENT", (await g1.get("GSENT"))[1] == "gm1")
    gm = await g2.get("GMSG"); check("members receive group messages", gm[2] == gid and gm[3] == r1[1] and gm[5] == "hello group" and gm[6] == "gm1")
    await g2.send("GOT", gm[1])
    await g1.send("GEDIT", "gm1", gid, "hello again"); await g1.get("OK_GEDIT")
    ge = await g2.get("GEDIT_IN"); check("group edits reach members", ge[2] == gid and ge[3] == r1[1] and ge[5] == "gm1" and ge[6] == "hello again"); await g2.send("GOT", ge[1])
    await g1.send("GDEL", "gm1", gid); await g1.get("OK_GDEL")
    gd = await g2.get("GDEL_IN"); check("group deletes reach members", gd[5] == "gm1"); await g2.send("GOT", gd[1])
    g2.w.close(); await asyncio.sleep(0.3)
    await g1.send("GSEND", "gm2", gid, "while you were away"); await g1.get("GSENT")
    g2 = await C().open(); await g2.send("LOGIN", r2[1], r2[2]); await g2.get("OK_LOGIN")
    check("group state and missed messages arrive after login", (await g2.get("GROUP"))[1] == gid and (await g2.get("GMSG"))[5] == "while you were away")
    for i in range(5):
        await g3.send("GJOIN", gid, "%06d" % (900000 + i)); wrong = await g3.get("ERR")
    check("wrong guesses are answered without hints", wrong[1] == "wrong group ID or password")
    await g3.send("GJOIN", gid, gpw); check("the lock also blocks the correct password for that user", "too many wrong passwords" in (await g3.get("ERR"))[1])
    await g2.send("GPASS", gid, "654321"); check("only the owner can change the password", (await g2.get("ERR"))[1] == "only the group owner can do that")
    await g1.send("GPASS", gid, "482915"); check("the owner can set their own password", (await g1.get("OK_GPASS"))[2] == "482915")
    g4, r4 = await newuser("Jon")
    await g4.send("GJOIN", gid, gpw); check("the old password stops working", (await g4.get("ERR"))[1] == "wrong group ID or password")
    await g4.send("GJOIN", gid, "482915"); check("the new password works", (await g4.get("OK_GJOIN"))[1] == gid)
    await g1.send("GKICK", gid, r4[1]); await g1.get("OK_GKICK"); check("the kicked member is told", (await g4.get("GROUP_DEL"))[1] == gid)
    await g1.send("GLEAVE", gid); await g1.get("OK_GLEAVE")
    own = await g2.get("GROUP"); check("when the owner leaves the oldest member becomes owner", own[3] == r2[1])
    await g2.send("GLEAVE", gid); await g2.get("OK_GLEAVE")
    await g1.send("GINFO", gid); check("an empty group is deleted", (await g1.get("ERR"))[1] == "you are not in that group")
    o, ro = await newuser("Owen"); a_, ra2 = await newuser("Abby"); m1, rm1 = await newuser("Mia"); m2, rm2 = await newuser("Max")
    await o.send("GNEW", "Club", "593724"); gid2 = (await o.get("OK_GNEW"))[1]
    for c, r in ((a_, ra2), (m1, rm1), (m2, rm2)):
        await c.send("GJOIN", gid2, "593724"); await c.get("OK_GJOIN")
    await asyncio.sleep(0.3)
    async def roles(c, count):
        await c.send("PING")
        while (await c.get())[0] != "PONG": pass
        await c.send("GINFO", gid2); await c.get("GROUP"); out = {}
        for _ in range(count):
            f = await c.get("GMEMBER"); out[f[2]] = f[4]
        return out
    rl = await roles(o, 4); check("the owner is role 2, everybody else role 0", rl[ro[1]] == "2" and rl[ra2[1]] == "0")
    await a_.send("GROLE", gid2, rm1[1], "1"); check("a member cannot hand out roles", (await a_.get("ERR"))[1] == "only the group owner can do that")
    await o.send("GROLE", gid2, ra2[1], "1"); check("the owner makes a sub-admin", (await o.get("OK_GROLE"))[3] == "1")
    check("everybody sees the new role", (await roles(m1, 4))[ra2[1]] == "1")
    await m1.send("GANN", gid2, "Meeting at 7"); check("a plain member cannot post an announcement", "only the group owner or a sub-admin" in (await m1.get("ERR"))[1])
    await a_.send("GANN", gid2, "Meeting at 7 on Friday"); await a_.get("OK_GANN")
    an = await m2.get("GANN"); check("a sub-admin can announce, members receive it", an[2] == "Meeting at 7 on Friday" and an[3] == ra2[1])
    m3, rm3 = await newuser("Mel"); await m3.send("GJOIN", gid2, "593724"); await m3.get("OK_GJOIN")
    check("a newcomer gets the pinned announcement", (await m3.get("GANN"))[2] == "Meeting at 7 on Friday")
    await a_.send("GKICK", gid2, ro[1]); check("a sub-admin cannot remove the owner", (await a_.get("ERR"))[1] in ("only the owner can remove an admin", "no such member"))
    await o.send("GROLE", gid2, rm1[1], "1"); await o.get("OK_GROLE")
    await a_.send("GKICK", gid2, rm1[1]); check("a sub-admin cannot remove another sub-admin", (await a_.get("ERR"))[1] == "only the owner can remove an admin")
    await a_.send("GKICK", gid2, rm2[1]); check("a sub-admin can remove a plain member", (await a_.get("OK_GKICK"))[2] == rm2[1])
    check("the removed member is told", (await m2.get("GROUP_DEL"))[1] == gid2)
    await m1.send("GNOTE", gid2, "Rehearsal room: B2"); nid = (await m1.get("OK_GNOTE"))[2]
    nt = await a_.get("GNOTE"); check("a note reaches the members", nt[3] == rm1[1] and nt[5] == "Rehearsal room: B2")
    await m3.send("GNOTES", gid2); lst = await m3.get("GNOTE"); await m3.get("GNOTES_END"); check("notes can be listed later", lst[2] == nid)
    await m3.send("GNOTEDEL", gid2, nid); check("a plain member cannot delete somebody else's note", "only the author or an admin" in (await m3.get("ERR"))[1])
    await a_.send("GNOTEDEL", gid2, nid); await a_.get("OK_GNOTEDEL"); check("a sub-admin can delete a note", (await m1.get("GNOTE_DEL"))[2] == nid)
    await o.send("GOWNER", gid2, ra2[1]); await o.get("OK_GOWNER")
    await o.send("GINFO", gid2); await o.get("GROUP"); got = {}
    for _ in range(4):
        f = await o.get("GMEMBER"); got[f[2]] = f[4]
    check("handing over the group changes the owner", got[ra2[1]] == "2" and got[ro[1]] == "1")
    await a_.send("GREAD", gid2, rm1[1], "77"); await asyncio.sleep(0.2)
    check("group read positions go to the author", (await m1.get("GREAD_IN"))[1:] == [gid2, ra2[1], "77"])
    await a_.send("GTYPING", gid2); gt = await m1.get("GTYPING_IN"); check("group members see who is typing", gt[1] == gid2 and gt[2] == ra2[1])
    await a_.send("GSEND", "ga1", gid2, "group file"); await a_.get("GSENT")
    gmsg = await m1.get("GMSG"); await m1.send("GOT", gmsg[1])
    gfile = os.urandom(5000); await a_.send("FUP", "5000", "g.bin"); gf = (await a_.get("FUP_OK"))[1]; await a_.send("FCH", gf, "0", __import__("base64").b64encode(gfile).decode()); await a_.send("FEND", gf); await a_.get("FEND_OK")
    await a_.send("GSEND", "ga2", gid2, "\x01F|%s|5000|g.bin" % gf); check("a file can be sent to a group", (await a_.get("GSENT"))[1] == "ga2")
    gm2 = await m3.get("GMSG"); await m3.send("FGET", gf); d = await m3.get("FDATA"); check("every member may download a group file", __import__("base64").b64decode(d[4]) == gfile)
    # ---- group end-to-end encryption support ----
    ga1, rga1 = await newuser("GKA"); gb1, rgb1 = await newuser("GKB"); gc1, rgc1 = await newuser("GKC"); gd1, rgd1 = await newuser("GKD")
    await ga1.send("GNEW", "Secret club", "593724"); gidk = (await ga1.get("OK_GNEW"))[1]
    await gb1.send("GJOIN", gidk, "593724"); await gb1.get("OK_GJOIN"); await gc1.send("GJOIN", gidk, "593724"); await gc1.get("OK_GJOIN")
    await gb1.send("KEYPUT", "B" * 263); await gb1.get("OK_KEYPUT")
    await gc1.send("KEYGET", rgb1[1]); r = await gc1.get("KEY"); check("a fellow group member (not a friend) can fetch a key bundle", r[2] == "B" * 263)
    await gd1.send("KEYGET", rgb1[1]); check("a stranger still cannot", (await gd1.get("ERR"))[1] == "not friends")
    await ga1.send("GKEY", gidk, rgb1[1], "7", "\x01E" + "W" * 150); r = await ga1.get("OK_GKEY"); check("a wrapped group key is accepted", r[1] == gidk and r[3] == "7")
    got = await gb1.get("GKEY_IN"); check("the member receives it with the epoch and sender", got[2] == gidk and got[3] == rga1[1] and got[5] == "7" and got[6] == "\x01E" + "W" * 150)
    await gb1.send("GOT", got[1]); check("no delivery receipt is made for a key", await ga1.quiet(0.6))
    await ga1.send("GKEY", gidk, rgd1[1], "7", "\x01E" + "W" * 150); check("keys only go to members of the group", (await ga1.get("ERR"))[1] == "not a member of that group")
    await ga1.send("GKEY", gidk, rgb1[1], "7", "plain"); check("a plain key is refused", (await ga1.get("ERR"))[1] == "bad group key")
    genv = "\x01G" + "H" * 2500
    await ga1.send("GSEND", "g-e1", gidk, genv); await ga1.get("GSENT"); got = await gc1.get("GMSG"); check("a long group envelope is relayed intact", got[5] == genv)
    await ga1.send("GSEND", "g-e2", gidk, "\x01G" + "H" * 3300); check("an oversized group envelope is refused, not cut", (await ga1.get("NOSEND"))[2] == "too long")
    await ga1.send("GEDIT", "g-e1", gidk, genv[:-1] + "I"); await ga1.get("OK_GEDIT"); got = await gc1.get("GEDIT_IN"); check("an edited group envelope is relayed intact", got[6] == genv[:-1] + "I")
    await gc1.send("FUP", "50", "g.bin"); gfid = (await gc1.get("FUP_OK"))[1]; await gc1.send("FCH", gfid, "0", base64.b64encode(os.urandom(50)).decode()); await gc1.send("FEND", gfid); await gc1.get("FEND_OK")
    await gc1.send("GSEND", "g-f1", gidk, "\x01G" + "J" * 200, gfid); await gc1.get("GSENT")
    await gb1.send("FGET", gfid); check("every member may fetch a file announced by an encrypted group message", (await gb1.get("FDATA"))[1] == gfid)
    await ga1.send("GANN", gidk, "\x01G" + "K" * 1500); await ga1.get("OK_GANN"); got = await gb1.get("GANN"); check("an encrypted announcement is relayed intact", got[2] == "\x01G" + "K" * 1500 and got[3] == rga1[1])
    await gc1.send("GNOTE", gidk, "\x01G" + "N" * 900); got = await gb1.get("GNOTE"); check("an encrypted note is relayed intact", got[5] == "\x01G" + "N" * 900 and got[3] == rgc1[1])
    await ga1.send("GANN", gidk, "\x01G" + "K" * 3300); check("an oversized announcement is refused", "too long" in (await ga1.get("ERR"))[1])

    # ---- abuse limits ----
    ka, rka = await newuser("LimA"); kb, rkb = await newuser("LimB"); await befriend(ka, rka, kb, rkb)
    alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    res = []
    for i in range(14):
        await kb.send("KEYPUT", "A" * 262 + alpha[i]); res.append((await kb.get())[0])
        if i % 8 == 7: await asyncio.sleep(5.2)                 # (the flood limit counts commands per 5 s)
    check("the same key bundle again is accepted without counting", True)
    check("key churn is limited per hour (12 changes, then refused)", res[:12] == ["OK_KEYPUT"] * 12 and res[12] == "ERR")
    await kb.w.drain(); kb.w.close(); await asyncio.sleep(0.5)       # Bob goes offline: his mailbox fills up
    replies = []
    for i in range(405):
        await ka.send("SEND", "q%d" % i, rkb[1], "x")
        while True:
            fr = await ka.get()
            if fr[0] in ("SENT", "NOSEND"): replies.append(fr[0]); break
        if i % 35 == 34: await asyncio.sleep(5.2)
    check("one sender can leave at most 400 messages in a mailbox", replies[:400] == ["SENT"] * 400 and set(replies[400:]) == {"NOSEND"})

    # ---- one sender cannot fill a member's mailbox through a group either ----
    fa, rfa = await newuser("FloodGA"); fz, rfz = await newuser("FloodGZ")
    await fa.send("GNEW", "Flood club", "593724"); gidf = (await fa.get("OK_GNEW"))[1]
    await fz.send("GJOIN", gidf, "593724"); await fz.get("OK_GJOIN"); await fz.w.drain(); fz.w.close(); await asyncio.sleep(0.5)
    for i in range(405):
        await fa.send("GSEND", "fg%d" % i, gidf, "x")
        while (await fa.get())[0] not in ("GSENT", "NOSEND"): pass
        if i % 35 == 34: await asyncio.sleep(5.2)
    fz2 = await C().open(); await fz2.send("LOGIN", rfz[1], rfz[2]); await fz2.get("OK_LOGIN"); cnt = 0
    while True:
        try: fr = await fz2.get(None, 1.5)
        except asyncio.TimeoutError: break
        if fr[0] == "GMSG": cnt += 1
    check("one sender can leave at most 400 group messages for a member", cnt == 400)

    # ---- sessions ----
    t1 = await C().open(); await t1.send("LOGIN", rt1[1], rt1[2]); await t1.get("OK_LOGIN")      # (the old connection idled out during the flood tests)
    await t1.send("LOGOUT_ALL"); nt = (await t1.get("OK_LOGOUT_ALL"))[1]; check("a new token is issued", len(nt) == 24 and nt != rt1[2])
    x = await C().open(); await x.send("LOGIN", rt1[1], rt1[2]); check("the old token stops working", (await x.get("ERR"))[1] == "bad id or token")
    x = await C().open(); await x.send("LOGIN", rt1[1], nt); check("the new token works", (await x.get("OK_LOGIN"))[0] == "OK_LOGIN")
    await t1.send("GSTATUS"); check("no Google on this server", (await t1.get("GOOGLE_STATUS"))[1] == "0")
    # ---- login lockout ----
    for i in range(5):
        y = await C().open(); await y.send("LOGIN", rd[1], "bad%d" % i); await y.get("ERR")
    y = await C().open(); await y.send("LOGIN", rd[1], rd[2]); check("five failed logins lock the account for a while", "too many failed logins" in (await y.get("ERR"))[1])
    # ---- flood ----
    z, rz = await newuser("Flood")
    for i in range(60): z.w.write(frame("PING"))
    await z.w.drain(); await asyncio.sleep(1)
    closed = False
    try:
        while True:
            data = await asyncio.wait_for(z.r.read(100000), 4)
            if not data: closed = True; break
    except asyncio.TimeoutError: pass
    except Exception: closed = True
    check("a flood of commands gets the connection dropped", closed)
    print("ALL PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)

asyncio.run(main())
