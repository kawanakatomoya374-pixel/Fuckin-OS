#!/usr/bin/env python3
"""Protocol test for tools/cosmsg_server.py: two strangers, friend request, messaging, offline delivery, block."""
import asyncio, os, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_server import Server, frame, unesc, make_context
import ssl

class C:
    async def open(self, port):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", port); return self
    async def send(self, *f): self.w.write(frame(*f)); await self.w.drain()
    async def get(self, want=None, t=3):
        while True:
            line = await asyncio.wait_for(self.r.readline(), t)
            f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if want is None or f[0] == want: return f

async def main():
    srv = Server(os.path.join(tempfile.mkdtemp(), "t.db"))
    server = await asyncio.start_server(srv.handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    ok = True
    def check(name, cond):
        nonlocal ok; print(("PASS " if cond else "FAIL ") + name); ok &= bool(cond)
    a = await C().open(port); await a.send("REG", "Alice"); ra = await a.get("OK_REG")
    b = await C().open(port); await b.send("REG", "Bob"); rb = await b.get("OK_REG")
    check("register gives id+token", len(ra[1]) == 8 and len(ra[2]) == 24)
    await a.send("SEND", "m0", rb[1], "hi"); check("no messaging before friendship", (await a.get("NOSEND"))[2] == "not friends")
    await a.send("FIND", rb[1]); check("find a stranger by id", (await a.get("FOUND"))[2] == "Bob")
    await a.send("FREQ", rb[1], "Hello, I am Alice"); await a.get("OK_FREQ")
    fi = await b.get("FREQ_IN"); check("request delivered with intro", fi[1] == ra[1] and fi[3] == "Hello, I am Alice")
    await b.send("FACC", ra[1]); check("both get FRIEND", (await a.get("FRIEND"))[2] == "Bob" and (await b.get("FRIEND"))[2] == "Alice")
    await a.send("SEND", "m1", rb[1], "tab\there\nline2"); await a.get("SENT")
    m = await b.get("MSG"); check("message round trip incl. escapes", m[2] == ra[1] and m[4] == "tab\there\nline2" and m[5] == "m1")
    await b.send("GOT", m[1]); check("delivery receipt", (await a.get("DELIV"))[1] == "m1")
    await a.send("EDIT", "m1", rb[1], "edited text"); await a.get("OK_EDIT")
    e = await b.get("EDIT_IN"); check("edit reaches the receiver", e[2] == ra[1] and e[4] == "m1" and e[5] == "edited text")
    await b.send("GOT", e[1])
    await a.send("DEL", "m1", rb[1]); await a.get("OK_DEL")
    d = await b.get("DEL_IN"); check("delete reaches the receiver", d[2] == ra[1] and d[4] == "m1")
    await b.send("GOT", d[1])
    # attachments: upload in chunks, only the addressed friend may download, a stranger may not
    import base64, os as _os
    blob = _os.urandom(20000)
    await a.send("FUP", str(len(blob)), "pic.png"); fid = (await a.get("FUP_OK"))[1]
    for i in range(0, len(blob), 6144):
        await a.send("FCH", fid, str(i // 6144), base64.b64encode(blob[i:i + 6144]).decode())
    await a.send("FEND", fid); await a.get("FEND_OK")
    await b.send("FGET", fid); check("download is refused before the file is sent to you", (await b.get("FERR"))[2] == "no such file")
    await a.send("SEND", "f1", rb[1], "\x01F|%s|%d|pic.png" % (fid, len(blob))); await a.get("SENT")
    got = (await b.get("MSG")); check("attachment announcement arrives", got[4].startswith("\x01F|" + fid)); await b.send("GOT", got[1])
    await b.send("FGET", fid); data = b""; last = 0
    while not last:
        d = await b.get("FDATA"); data += base64.b64decode(d[4]); last = int(d[3])
    check("file downloads byte-exact", data == blob)
    cc = await C().open(port); await cc.send("REG", "Carol"); rc = await cc.get("OK_REG")
    await cc.send("FGET", fid); check("a stranger cannot download it", (await cc.get("FERR"))[2] == "no such file")
    await a.send("FUP", str(11 * 1024 * 1024), "big.bin"); check("oversized upload is refused", (await a.get("FERR"))[2].startswith("file too large"))
    await a.send("SEND", "f2", rb[1], "\x01F|deadbeefdeadbeef|10|x.bin"); check("announcing a file you did not upload is refused", (await a.get("NOSEND"))[1] == "f2")
    b.w.close(); await asyncio.sleep(0.2)
    await a.send("SEND", "m2", rb[1], "while you were away"); await a.get("SENT")
    b = await C().open(port); await b.send("LOGIN", rb[1], rb[2]); await b.get("OK_LOGIN")
    await a.send("EDIT", "m2", rb[1], "while you were away (fixed)"); await a.get("OK_EDIT")
    await b.get("OK_LOGIN") if False else None
    m = await b.get("MSG"); check("offline message delivered at login", m[4] == "while you were away")
    e = await b.get("EDIT_IN"); check("offline edit follows its message in order", e[4] == "m2" and e[5] == "while you were away (fixed)")
    c = await C().open(port); await c.send("LOGIN", rb[1], "wrong"); check("wrong token rejected", (await c.get("ERR"))[0] == "ERR")
    await b.send("BLOCK", ra[1]); await b.get("OK_BLOCK")
    await a.send("SEND", "m3", rb[1], "x"); check("blocked sender cannot send", (await a.get("NOSEND"))[1] == "m3")
    await a.send("FREQ", rb[1], "again"); check("blocked sender cannot re-request", (await a.get("ERR"))[0] == "ERR")
    # ---- discovery: handles, search, profiles, avatars, invite codes ----
    d = await C().open(port); await d.send("REG", "Dana"); rd = await d.get("OK_REG")
    e = await C().open(port); await e.send("REG", "Eve"); re_ = await e.get("OK_REG")
    await d.send("HANDLE", "Dana.Photo"); check("handle is normalised to lowercase", (await d.get("OK_HANDLE"))[1] == "dana.photo")
    await e.send("HANDLE", "dana.photo"); check("a taken handle is refused", (await e.get("ERR"))[1] == "that handle is taken")
    await e.send("HANDLE", "x"); check("an invalid handle is refused", (await e.get("ERR"))[1].startswith("handle:"))
    await d.send("BIO", "I take photos of trains"); await d.get("OK_BIO")
    await e.send("SEARCH", "dana"); check("a non-discoverable user is not found", (await e.get("RESULT_END"))[1] == "0")
    await d.send("DISC", "1"); await d.get("OK_DISC")
    await e.send("SEARCH", "@dana"); res = await e.get("RESULT"); await e.get("RESULT_END")
    check("a discoverable user is found by @handle", res[1] == rd[1] and res[3] == "dana.photo" and res[4] == "I take photos of trains")
    await e.send("SEARCH", "dan"); check("search matches a name prefix too", (await e.get("RESULT"))[1] == rd[1]); await e.get("RESULT_END")
    await d.send("FUP", "3000", "me.bmp"); afid = (await d.get("FUP_OK"))[1]
    await d.send("FCH", afid, "0", base64.b64encode(b"B" * 3000).decode()); await d.send("FEND", afid); await d.get("FEND_OK")
    await d.send("AVATAR", afid); await d.get("OK_AVATAR")
    await e.send("FGET", afid); got = b""; last = 0
    while not last:
        x = await e.get("FDATA"); got += base64.b64decode(x[4]); last = int(x[3])
    check("an avatar can be downloaded by a stranger", got == b"B" * 3000)
    await e.send("PROFILE", rd[1]); pf = await e.get("PROFILE"); check("profile shows handle, bio and avatar of a discoverable user", pf[3] == "dana.photo" and pf[5] == afid)
    await d.send("DISC", "0"); await d.get("OK_DISC")
    await e.send("PROFILE", rd[1]); pf = await e.get("PROFILE"); check("a private profile shows only the name", pf[2] == "Dana" and pf[3] == "" and pf[5] == "")
    await d.send("ME"); me = await d.get("ME"); code = me[7]
    check("ME returns the profile and an invite code", me[3] == "dana.photo" and len(code) == 8)
    await e.send("JOIN", code[:4] + "-" + code[4:].lower()); check("an invite code starts a friend request (dashes/case ignored)", (await e.get("OK_FREQ"))[1] == rd[1])
    fq = await d.get("FREQ_IN"); check("the request carries the sender's profile fields", fq[1] == re_[1] and fq[3] == "Joined with your invite code")
    await d.send("INVITE_NEW"); newcode = (await d.get("INVITE"))[1]
    await e.send("JOIN", code); check("a replaced invite code stops working", (await e.get("ERR"))[1] == "invalid invite code" and newcode != code)
    # groups: a 6-digit password is required to join; messages fan out to members only
    import cosmsg_server as cs
    cs.USER_TRIES = 5
    g1 = await C().open(port); await g1.send("REG", "Gina"); r1 = await g1.get("OK_REG")
    g2 = await C().open(port); await g2.send("REG", "Hugo"); r2 = await g2.get("OK_REG")
    g3 = await C().open(port); await g3.send("REG", "Ivy"); r3 = await g3.get("OK_REG")
    await g1.send("GNEW", "Trains", "123456"); check("a trivial password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains", "12ab56"); check("a non-numeric password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains", "48291"); check("a 5-digit password is refused", "6 digits" in (await g1.get("ERR"))[1])
    await g1.send("GNEW", "Trains"); ng = await g1.get("OK_GNEW"); gid, gpw = ng[1], ng[3]
    check("a group gets an id and a generated 6-digit password", gid.startswith("g") and len(gpw) == 6 and gpw.isdigit())
    check("the creator is told about the group and its members", (await g1.get("GROUP"))[2] == "Trains" and (await g1.get("GMEMBER"))[3] == "Gina")
    row = srv.q("SELECT pwhash FROM groups WHERE gid=?", gid).fetchone()[0]
    check("the password is stored only as a salted hash", gpw not in row and ":" in row and len(row) > 80)
    await g2.send("GJOIN", gid, "000001"); check("joining with a wrong password fails", (await g2.get("ERR"))[1] == "wrong group ID or password")
    await g2.send("GJOIN", "g99999999", "123457"); check("an unknown group gives the same error", (await g2.get("ERR"))[1] == "wrong group ID or password")
    await g2.send("GSEND", "x1", gid, "let me in"); check("a non-member cannot send", (await g2.get("ERR"))[1] == "you are not in that group")
    await g2.send("GJOIN", gid, gpw); check("joining with the right password works", (await g2.get("OK_GJOIN"))[2] == "Trains")
    jm = await g1.get("GMEMBER"); check("existing members see the newcomer", jm[2] == r2[1] and jm[3] == "Hugo")
    await g1.send("GSEND", "gm1", gid, "hello group"); check("sender gets GSENT", (await g1.get("GSENT"))[1] == "gm1")
    gm = await g2.get("GMSG"); check("members receive group messages", gm[2] == gid and gm[3] == r1[1] and gm[5] == "hello group" and gm[6] == "gm1")
    await g2.send("GOT", gm[1])
    await g3.send("FIND", r1[1])      # Ivy is not a member: she never sees group traffic
    await g1.send("GEDIT", "gm1", gid, "hello again"); await g1.get("OK_GEDIT")
    ge = await g2.get("GEDIT_IN"); check("group edits reach members", ge[2] == gid and ge[3] == r1[1] and ge[5] == "gm1" and ge[6] == "hello again"); await g2.send("GOT", ge[1])
    await g1.send("GDEL", "gm1", gid); await g1.get("OK_GDEL")
    gd = await g2.get("GDEL_IN"); check("group deletes reach members", gd[5] == "gm1"); await g2.send("GOT", gd[1])
    g2.w.close(); await asyncio.sleep(0.2)
    await g1.send("GSEND", "gm2", gid, "while you were away"); await g1.get("GSENT")
    g2 = await C().open(port); await g2.send("LOGIN", r2[1], r2[2]); await g2.get("OK_LOGIN")
    check("group state and missed messages arrive after login", (await g2.get("GROUP"))[1] == gid and (await g2.get("GMSG"))[5] == "while you were away")
    for i in range(cs.USER_TRIES):
        await g3.send("GJOIN", gid, "%06d" % (900000 + i)); wrong = await g3.get("ERR")
    check("wrong guesses are answered without hints", wrong[1] == "wrong group ID or password")
    await g3.send("GJOIN", gid, gpw); check("the lock also blocks the correct password for that user", "too many wrong passwords" in (await g3.get("ERR"))[1])
    await g2.send("GPASS", gid, "654321"); check("only the owner can change the password", (await g2.get("ERR"))[1] == "only the group owner can do that")
    await g1.send("GPASS", gid, "482915"); check("the owner can set their own password", (await g1.get("OK_GPASS"))[2] == "482915")
    g4 = await C().open(port); await g4.send("REG", "Jon"); r4 = await g4.get("OK_REG")
    await g4.send("GJOIN", gid, gpw); check("the old password stops working", (await g4.get("ERR"))[1] == "wrong group ID or password")
    await g4.send("GJOIN", gid, "482915"); check("the new password works", (await g4.get("OK_GJOIN"))[1] == gid)
    await g1.send("GKICK", gid, r4[1]); await g1.get("OK_GKICK"); check("the kicked member is told", (await g4.get("GROUP_DEL"))[1] == gid)
    await g1.send("GLEAVE", gid); await g1.get("OK_GLEAVE")
    own = await g2.get("GROUP"); check("when the owner leaves the oldest member becomes owner", own[3] == r2[1])
    await g2.send("GLEAVE", gid); await g2.get("OK_GLEAVE")
    check("an empty group is deleted", srv.q("SELECT COUNT(*) FROM groups WHERE gid=?", gid).fetchone()[0] == 0)
    # group roles, announcements, notes
    async def newuser(name):
        c = await C().open(port); await c.send("REG", name); r = await c.get("OK_REG"); return c, r
    o, ro = await newuser("Owen"); a_, ra2 = await newuser("Abby"); m1, rm1 = await newuser("Mia"); m2, rm2 = await newuser("Max")
    await o.send("GNEW", "Club", "593724"); gid2 = (await o.get("OK_GNEW"))[1]
    for c, r in ((a_, ra2), (m1, rm1), (m2, rm2)):
        await c.send("GJOIN", gid2, "593724"); await c.get("OK_GJOIN")
    await asyncio.sleep(0.2)
    async def roles(c):
        await c.send("PING"); await c.get("PONG")           # drop what is still queued, then ask for the state
        await c.send("GINFO", gid2); await c.get("GROUP"); out = {}
        for _ in srv.members(gid2):
            f = await c.get("GMEMBER"); out[f[2]] = f[4]
        return out
    rl = await roles(o); check("the owner is role 2, everybody else role 0", rl[ro[1]] == "2" and rl[ra2[1]] == "0")
    await a_.send("GROLE", gid2, rm1[1], "1"); check("a member cannot hand out roles", (await a_.get("ERR"))[1] == "only the group owner can do that")
    await o.send("GROLE", gid2, ra2[1], "1"); check("the owner makes a sub-admin", (await o.get("OK_GROLE"))[3] == "1")
    check("everybody sees the new role", (await roles(m1))[ra2[1]] == "1")
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
    check("handing over the group changes the owner", srv.q("SELECT owner FROM groups WHERE gid=?", gid2).fetchone()[0] == ra2[1] and srv.role_of(gid2, ro[1]) == 1)
    # blocking, reporting, restriction
    await m3.send("BLOCKS"); check("the block list starts empty", (await m3.get("BLOCKS_END"))[1] == "0")
    await m3.send("BLOCK", rm1[1]); await m3.get("OK_BLOCK")
    await m3.send("BLOCKS"); bl = await m3.get("BLOCKED"); check("a blocked user is listed with the name", bl[1] == rm1[1] and bl[2] == "Mia")
    await m3.send("UNBLOCK", rm1[1]); await m3.get("OK_UNBLOCK"); await m3.send("BLOCKS"); check("unblocking empties the list", (await m3.get("BLOCKS_END"))[1] == "0")
    spammer, rs = await newuser("Spammy"); await spammer.send("DISC", "1"); await spammer.get("OK_DISC"); await spammer.send("HANDLE", "spammy.bot"); await spammer.get("OK_HANDLE")
    reporters = [await newuser("Rep%d" % i) for i in range(cs.REPORT_LIMIT)]
    for i, (c, r) in enumerate(reporters):
        await c.send("REPORT", rs[1], "sends advertisements", "buy now\nbuy now"); check("report %d is accepted" % (i + 1), (await c.get("OK_REPORT"))[1] == rs[1])
    await reporters[0][0].send("REPORT", rs[1], "again"); check("the same user cannot report twice a day", "already reported" in (await reporters[0][0].get("ERR"))[1])
    await spammer.send("FREQ", ro[1], "hi"); check("an account reported by five people is restricted", "restricted" in (await spammer.get("ERR"))[1])
    await o.send("SEARCH", "spammy"); check("a restricted account disappears from search", (await o.get("RESULT_END"))[1] == "0")
    check("reports are stored with their evidence", srv.q("SELECT COUNT(*), MAX(evidence) FROM reports WHERE target=?", rs[1]).fetchone() == (cs.REPORT_LIMIT, "buy now\nbuy now"))
    # read receipts: friends (DM) and group members; positions survive a reconnect
    d1, rd1 = await newuser("Reader"); d2, rd2 = await newuser("Writer")
    await d1.send("FREQ", rd2[1], "hi"); await d1.get("OK_FREQ"); await d2.get("FREQ_IN"); await d2.send("FACC", rd1[1]); await d1.get("FRIEND"); await d2.get("FRIEND")
    await d1.send("READ", rd2[1], "123"); await asyncio.sleep(0.1)
    check("the writer is told how far the friend has read", (await d2.get("READ_IN"))[1:] == [rd1[1], "123"])
    d2.w.close(); await asyncio.sleep(0.2); await d1.send("READ", rd2[1], "456"); await asyncio.sleep(0.1)
    d2 = await C().open(port); await d2.send("LOGIN", rd2[1], rd2[2]); await d2.get("OK_LOGIN")
    check("the last read position is re-sent at login", (await d2.get("READ_IN"))[2] == "456")
    await a_.send("GREAD", gid2, rm1[1], "77"); await asyncio.sleep(0.1)
    check("group read positions go to the author", (await m1.get("GREAD_IN"))[1:] == [gid2, ra2[1], "77"])
    await o.send("READ", rd2[1], "5"); await asyncio.sleep(0.1)    # not friends: ignored
    # typing indicators and presence
    t1, rt1 = await newuser("Tina"); t2, rt2 = await newuser("Toru")
    await t1.send("FREQ", rt2[1], "hi"); await t1.get("OK_FREQ"); await t2.get("FREQ_IN"); await t2.send("FACC", rt1[1]); await t1.get("FRIEND"); await t2.get("FRIEND")
    await t1.get("PRES"); await t2.get("PRES")                  # becoming friends announces the online state both ways
    await t1.send("TYPING", rt2[1]); ty = await t2.get("TYPING_IN"); check("a friend sees that you are typing", ty[1] == rt1[1])
    await t2.send("TYPING", rm1[1]); await asyncio.sleep(0.1)    # not friends: nothing is delivered (and nothing breaks)
    await t1.send("PING"); check("typing to a stranger is ignored", (await t1.get("PONG")) == ["PONG"])
    await t1.send("SETPRES", "1"); await t1.get("OK_SETPRES")
    t2.w.close(); await asyncio.sleep(0.3)
    pr = await t1.get("PRES"); check("a friend going offline is announced with the time", pr[1] == rt2[1] and pr[2] == "0" and int(pr[3]) > 0)
    t2 = await C().open(port); await t2.send("LOGIN", rt2[1], rt2[2]); await t2.get("OK_LOGIN")
    pr = await t1.get("PRES"); check("coming back online is announced", pr[1] == rt2[1] and pr[2] == "1")
    await t2.send("SETPRES", "0"); await t2.get("OK_SETPRES")
    pr = await t1.get("PRES"); check("a user who hides their presence appears as hidden", pr[1] == rt2[1] and pr[2] == "2")
    t1.w.close(); await asyncio.sleep(0.3)
    t1 = await C().open(port); await t1.send("LOGIN", rt1[1], rt1[2]); await t1.get("OK_LOGIN")
    got = {}
    while "PRES" not in got:
        f = await t1.get(); got[f[0]] = f
    check("presence of every friend is sent at login", got["PRES"][1] == rt2[1] and got["PRES"][2] == "2")
    await a_.send("GTYPING", gid2); gt = await m1.get("GTYPING_IN"); check("group members see who is typing", gt[1] == gid2 and gt[2] == ra2[1])
    # TLS listener: a TLS 1.2 client (what BearSSL speaks) can log in; a plaintext client cannot
    d = tempfile.mkdtemp()
    tserver = await asyncio.start_server(srv.handle, "127.0.0.1", 0, ssl=make_context(d + "/c.crt", d + "/c.key"))
    tport = tserver.sockets[0].getsockname()[1]
    cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); cctx.check_hostname = False; cctx.verify_mode = ssl.CERT_NONE
    cctx.maximum_version = ssl.TLSVersion.TLSv1_2
    tr, tw = await asyncio.open_connection("127.0.0.1", tport, ssl=cctx)
    tw.write(frame("LOGIN", ra[1], ra[2])); await tw.drain()
    check("login over TLS 1.2", unesc((await asyncio.wait_for(tr.readline(), 3)).decode().split("\t")[0]) == "OK_LOGIN")
    pr, pw = await asyncio.open_connection("127.0.0.1", tport)
    pw.write(frame("LOGIN", ra[1], ra[2])); await pw.drain()
    try: data = await asyncio.wait_for(pr.read(100), 3)
    except Exception: data = b""
    check("plaintext client gets no service on the TLS port", b"OK_LOGIN" not in data)
    print("ALL PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
if __name__ == "__main__":
    asyncio.run(main())
