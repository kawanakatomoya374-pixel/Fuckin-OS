#!/usr/bin/env python3
"""Second user ("Bob") for tools/run_messenger_srv_test.sh. Drives the guest through QMP typing:
guest registers on the server -> Bob asks to be its friend -> guest accepts -> Bob sends a message -> guest replies."""
import asyncio, os, re, sqlite3, ssl, subprocess, sys, time
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_e2e import E2E, b64d, file_open, event, parse_event
from cosmsg_server import frame, unesc
port, db = int(sys.argv[1]), sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")

def type_text(t): subprocess.run(["python3", TYPE, t], stdout=subprocess.DEVNULL, check=False)

QMP = os.environ.get("COS_QMP", "/tmp/cos_gui_qmp.sock")
MINE = (0x2F, 0x7D, 0x4F)

def last_bubble(img):
    """Bounding box (x0,y0,x1,y1) of the lowest green (own) bubble on the screen."""
    w, h = img.size; px = img.load()
    rows = [y for y in range(h) if any(px[x, y] == MINE for x in range(w // 3, w))]
    if not rows: return None
    y1 = rows[-1]; y0 = y1
    while (y0 - 1) in rows: y0 -= 1
    xs = [x for y in range(y0, y1 + 1) for x in range(w // 3, w) if px[x, y] == MINE]
    return min(xs), y0, max(xs), y1

def find_lowest(img, rgb, x_from=395, y_from=200, y_to=665):
    """Bounding box of the lowest blob of colour `rgb` in the chat area."""
    w, h = img.size; px = img.load()
    rows = [y for y in range(y_from, min(h, y_to)) if any(px[x, y] == rgb for x in range(x_from, w))]
    if not rows: return None
    y1 = rows[-1]; y0 = y1
    while (y0 - 1) in rows: y0 -= 1
    xs = [x for y in range(y0, y1 + 1) for x in range(x_from, w) if px[x, y] == rgb]
    return min(xs), y0, max(xs), y1

def click_color(rgb, wait=2.0):
    def run(ui):
        ui.shot("/tmp/ms.ppm"); b = find_lowest(Image.open("/tmp/ms.ppm").convert("RGB"), rgb)
        if not b: return False
        ui.goto((b[0] + b[2]) // 2, (b[1] + b[3]) // 2); ui.click(); time.sleep(wait); return True
    return ui_session(run)

def screenshot(path):
    ui_session(lambda ui: ui.shot(path))

def press(qcode):
    ui_session(lambda ui: ui.key(qcode))

def ui_session(fn):
    ui = UI(QMP)
    try: return fn(ui)
    finally: ui.s.close()

def click_bubble_button(which):
    """Selects the lowest own bubble, then clicks its Edit (which=0) or Delete (which=1) button."""
    def run(ui):
        ui.shot("/tmp/ms.ppm"); b = last_bubble(Image.open("/tmp/ms.ppm").convert("RGB"))
        if not b: return False
        cy = (b[1] + b[3]) // 2
        ui.goto((b[0] + b[2]) // 2, cy); ui.click(); time.sleep(1.5)
        ex = b[0] - 8 - 58 - 4 - 58            # same geometry as draw_chat()
        ui.goto(ex + 29 + which * 62, cy); ui.click(); time.sleep(1.0)
        return True
    return ui_session(run)

def ui_type(text, enter=True):
    def run(ui):
        ui.type(text)
        if enter: ui.key("ret")
    ui_session(run)

async def get(r, want, t=20):
    while True:
        line = await asyncio.wait_for(r.readline(), t)
        if not line: raise ConnectionError("the server closed the connection")
        f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
        if f[0] == want: return f

async def main():
    ok = True
    def check(n, c):
        nonlocal ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
    type_text("/server 192.168.70.2")
    for tick in range(60):                                # wait for the guest's registration
        await asyncio.sleep(2)
        if os.environ.get("QUICK") == "google" and tick == 3: screenshot("/tmp/google_card.ppm")      # the sign-in window is up
        try: rows = sqlite3.connect(db).execute("SELECT id,name FROM users").fetchall()
        except sqlite3.Error: rows = []
        if rows: break
    check("guest registered on the server", bool(rows))
    gid = rows[0][0]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    if os.environ.get("QUICK") == "google":
        con = sqlite3.connect(db)
        grow = con.execute("SELECT email FROM google WHERE uid=?", (gid,)).fetchone()
        check("the guest's account is bound to a verified Google account", grow is not None and grow[0].endswith("@gmail.com"))
        screenshot("/tmp/google_done.ppm")
        xr, xw = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
        xw.write(frame("REG", "Bob")); await xw.drain(); check("an anonymous registration is refused", (await get(xr, "NEED_GOOGLE")) == ["NEED_GOOGLE"])
        xw.write(frame("GREG", "Bob")); await xw.drain(); code = await get(xr, "GOOGLE_CODE"); check("the server hands out a Google user code", code[1] == "WXYZ-1234")
        for _ in range(12):
            await asyncio.sleep(3.2); xw.write(frame("GPOLL")); await xw.drain()
            line = await xr.readline(); f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
            if f[0] == "OK_REG": break
        check("a second person can sign in the same way", f[0] == "OK_REG" and f[3] == "Bob")
        # the guest drops its token (as a lost device would) and signs in again: same account, new token
        type_text("/logout"); await asyncio.sleep(3)
        screenshot("/tmp/google_loggedout.ppm")
        sys.exit(0 if ok else 1)
    r, w = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
    w.write(frame("REG", "Bob")); await w.drain(); bob = await get(r, "OK_REG")
    e2e = E2E(bob[1]); w.write(e2e.publish_frame()); await w.drain()
    w.write(frame("FREQ", gid, "Hi, we have never met")); await w.drain(); await get(r, "OK_FREQ")
    await asyncio.sleep(4)
    type_text("/accept " + bob[1])
    fr = await get(r, "FRIEND"); check("guest accepted the friend request", fr[1] == gid)
    check("Bob learns the guest's end-to-end key", await e2e.learn(r, w, get, gid))
    import base64, io
    async def guest_to_bob():
        # Bob receives files from the guest
        def guest_send(ui):
            ui.type("/send /etc/hostname"); ui.key("ret"); time.sleep(3)
            ui.type("/send /etc/os-release"); ui.key("ret"); time.sleep(3)
        ui_session(guest_send)
        got_files = {}
        announced = []
        for _ in range(2):                              # both announcements first, then the downloads (get() skips other frames)
            am = await get(r, "MSG", 30); w.write(frame("GOT", am[1])); await w.drain()
            announced.append(e2e.text(am).split("|", 4))
        for parts in announced:
            fid, size, name, fkey = parts[1], int(parts[2]), parts[3], b64d(parts[4])
            w.write(frame("FGET", fid)); await w.drain(); data = b""; last = 0
            while not last:
                d = await get(r, "FDATA", 30); data += base64.b64decode(d[4]); last = int(d[3])
            check("the server only holds ciphertext of " + name, len(data) > size and (name != "os-release" or b"C-OS" not in data))
            data = file_open(fkey, data); got_files[name] = (data, size)
        check("hostname arrives intact", "hostname" in got_files and len(got_files["hostname"][0]) == got_files["hostname"][1] > 0)
        check("os-release arrives intact", "os-release" in got_files and len(got_files["os-release"][0]) == got_files["os-release"][1] > 20 and b"C-OS" in got_files["os-release"][0])
        screenshot("/tmp/att_sent.ppm")
    async def bob_group():
        # commands are local: Bob (a friend) must never see /help, and the guest sees the list
        type_text("/help"); await asyncio.sleep(3); screenshot("/tmp/grp_help.ppm")
        try:
            await get(r, "MSG", 4); check("a command is never sent to the other person", False)
        except asyncio.TimeoutError:
            check("a command is never sent to the other person", True)
        press("esc")
        type_text("/clear"); await asyncio.sleep(1)
        # read receipts: the guest has Bob's conversation open, so it reports the message as read
        w.write(e2e.send_frame("7000", gid, "dm ping")); await w.drain(); await get(r, "SENT")
        ri = await get(r, "READ_IN", 20); check("the guest reports a friend's message as read", ri[1] == gid and ri[2] == "7000")
        # the guest creates a group with a password of its own choosing; Bob and a stranger try to join
        type_text("/gnew Trains 482915"); await asyncio.sleep(5)
        row = sqlite3.connect(db).execute("SELECT gid FROM groups WHERE name='Trains'").fetchone()
        check("the guest created a group", row is not None)
        screenshot("/tmp/grp_created.ppm")
        if not row: return
        ggid = row[0]
        w.write(frame("GJOIN", ggid, "111222")); await w.drain(); check("a wrong password cannot join", (await get(r, "ERR"))[1] == "wrong group ID or password")
        w.write(frame("GJOIN", ggid, "482915")); await w.drain(); check("the right password joins", (await get(r, "OK_GJOIN"))[1] == ggid)
        type_text("/gapprove Bob")                                                      # the owner approves Bob; only then the guest makes the group key and hands it to him
        gk = await get(r, "GKEY_IN", 30); gep = int(gk[5]); e2e.unwrap_key(gid, ggid, gep, gk[6]); w.write(frame("GOT", gk[1])); await w.drain()
        check("the owner's key reaches the approved member", gk[2] == ggid and gk[3] == gid)
        async def next_chat():                                                          # (the signed roster is an ordinary group message too: skip it)
            while True:
                m_ = await get(r, "GMSG", 20); w.write(frame("GOT", m_[1])); await w.drain()
                if not e2e.gdecrypt(gid, ggid, m_[6], m_[5]).startswith("\x01R|"): return m_
        w.write(frame("GSEND", "7001", ggid, e2e.gencrypt(ggid, gep, "7001", "Hello from Bob in the group"))); await w.drain(); await get(r, "GSENT")
        gr = await get(r, "GREAD_IN", 20); check("the guest reports a group message as read", gr[1] == ggid and gr[2] == gid and gr[3] == "7001")
        await asyncio.sleep(3); screenshot("/tmp/grp_chat.ppm")
        type_text("Hi everyone, I am the guest")
        gm = await next_chat(); check("the guest's group message reaches Bob", gm[2] == ggid and e2e.gdecrypt(gid, ggid, gm[6], gm[5]) == "Hi everyone, I am the guest")
        w.write(frame("GOT", gm[1])); await w.drain()
        w.write(frame("GREAD", ggid, gid, gm[6])); await w.drain(); await asyncio.sleep(3); screenshot("/tmp/grp_read.ppm")
        # announcement, roles and notes
        type_text("/gannounce Rehearsal at 7 on Friday")
        an = await get(r, "GANN", 20); check("the guest's announcement reaches Bob (encrypted for the group)", an[1] == ggid and an[2].startswith("\x01G") and re.fullmatch(r"T\d+\|Rehearsal at 7 on Friday", e2e.gdecrypt(an[3], ggid, "0", an[2], kind=5)) is not None)
        type_text("/gadmin Bob")
        while True:
            gmf = await get(r, "GMEMBER", 20)
            if gmf[2] == bob[1] and gmf[4] == "1": break
        check("the guest made Bob a sub-admin", True)
        w.write(frame("GNOTE", ggid, e2e.gencrypt(ggid, gep, "0", "Bring your own sheet music", kind=6))); await w.drain(); await get(r, "OK_GNOTE")
        await asyncio.sleep(2); type_text("/gnotes"); await asyncio.sleep(3); screenshot("/tmp/grp_notes.ppm")
        type_text("/gmembers"); await asyncio.sleep(3); screenshot("/tmp/grp_members.ppm")
        type_text("/blocks"); await asyncio.sleep(3); screenshot("/tmp/grp_blocks.ppm")
        type_text("/report %s he keeps spamming" % bob[1]); await asyncio.sleep(4)
        rep_row = sqlite3.connect(db).execute("SELECT reporter,target,reason,evidence FROM reports").fetchone()
        check("the guest reported Bob with his message as evidence", rep_row is not None and rep_row[0] == gid and rep_row[1] == bob[1] and "Hello from Bob" in rep_row[3])
        screenshot("/tmp/grp_report.ppm")
        # ten strangers ask to be friends: the list overflows and scrolls with the wheel
        for i in range(10):
            xr, xw = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
            xw.write(frame("REG", "Person%d" % i)); await xw.drain(); await get(xr, "OK_REG")
            xw.write(frame("FREQ", gid, "hello %d" % i)); await xw.drain(); await get(xr, "OK_FREQ")
        await asyncio.sleep(5); screenshot("/tmp/list_top.ppm")
        ui_session(lambda ui: (ui.goto(270, 450), ui.wheel(6)))
        await asyncio.sleep(2); screenshot("/tmp/list_scrolled.ppm")
        a, b = Image.open("/tmp/list_top.ppm").convert("RGB").crop((146, 252, 392, 700)), Image.open("/tmp/list_scrolled.ppm").convert("RGB").crop((146, 252, 392, 700))
        from PIL import ImageChops
        check("the wheel scrolls the conversation list", ImageChops.difference(a, b).getbbox() is not None)
        ui_session(lambda ui: (ui.goto(270, 450), ui.wheel(30)))
        await asyncio.sleep(2); screenshot("/tmp/list_bottom.ppm")
    async def bob_lock():
        img = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "storage.img")
        def on_disk(text): return text.encode() in open(img, "rb").read()
        fp_hex = e2e.fingerprint().hex()                      # Bob's fingerprint is written to e2e_peers.txt as hex while the file is plain
        async def log_has(text, t=60):
            for _ in range(int(t * 2)):
                if text in open(os.environ["MSG_LOG"], errors="replace").read(): return True
                await asyncio.sleep(0.5)
                if _ % 20 == 19: w.write(frame("PING")); await w.drain()          # keep Bob's connection from idling out
            return False
        def got(text): return log_has("received a message (%d bytes)" % len(text.encode()), 30)
        w.write(e2e.send_frame("8001", gid, "Plaintext marker BEFORE the lock")); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(6)
        check("control: before the lock the message IS readable on the disk image", on_disk("Plaintext marker BEFORE the lock"))
        check("control: before the lock the peer key file IS readable on the disk image", on_disk(fp_hex))
        type_text("/lock on"); await asyncio.sleep(2); screenshot("/tmp/lock_set.ppm")
        type_text("Tr0ubador8x"); await asyncio.sleep(1); type_text("Tr0ubador8x")           # new password, then the confirmation
        await asyncio.sleep(25); screenshot("/tmp/lock_enabled.ppm")
        w.write(e2e.send_frame("8002", gid, "Encrypted marker AFTER the lock")); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(5)
        check("the message from before the lock is no longer readable on the disk", not on_disk("Plaintext marker BEFORE the lock"))
        check("a message after the lock never reaches the disk as plain text", not on_disk("Encrypted marker AFTER the lock"))
        check("turning the lock on converts the end-to-end peer key file as well", not on_disk(fp_hex))
        check("the end-to-end keys still load while the lock is on: an encrypted message is decrypted", await got("Encrypted marker AFTER the lock"))
        # lock now: nothing of the app may work while it is locked
        type_text("/lock now"); await asyncio.sleep(3); screenshot("/tmp/lock_locked.ppm")
        w.write(e2e.send_frame("8003", gid, "Message while locked")); await w.drain(); await get(r, "SENT")
        try:
            await get(r, "DELIV", 8); check("a locked app does not receive messages", False)
        except asyncio.TimeoutError:
            check("a locked app does not receive messages", True)
        type_text("wrongpass1"); check("a wrong password is reported", await log_has("wrong password #1")); screenshot("/tmp/lock_wrong1.ppm")
        type_text("wrongpass2"); await log_has("wrong password #2")
        type_text("wrongpass3"); await log_has("wrong password #3, wait 5 s")      # the third failure starts a 5 s wait
        type_text("Tr0ubador8x")                                                      # the right password inside the wait
        check("the waiting time blocks even the right password", await log_has("refused during the waiting time", 20)); screenshot("/tmp/lock_waiting.ppm")
        try:
            await get(r, "DELIV", 3); check("the app stayed locked during the wait", False)
        except asyncio.TimeoutError:
            check("the app stayed locked during the wait", True)
        await asyncio.sleep(4)
        type_text("Tr0ubador8x"); check("the right password unlocks after the wait", await log_has("lock: unlocked", 40)); await asyncio.sleep(3); screenshot("/tmp/lock_unlocked.ppm")
        dv = await get(r, "DELIV", 30); check("after unlocking the queued message is delivered", dv[1] == "8003")
        check("after unlocking the end-to-end keys are loaded again and the queued message is decrypted", await got("Message while locked"))
        w.write(e2e.send_frame("8004", gid, "Message after the unlock")); await w.drain(); await get(r, "SENT")
        check("a message sent after the unlock is decrypted", await got("Message after the unlock"))
        type_text("/lock off"); await asyncio.sleep(2); type_text("Tr0ubador8x"); await asyncio.sleep(15); screenshot("/tmp/lock_off.ppm")
        w.write(e2e.send_frame("8005", gid, "Message after the lock was removed")); await w.drain(); await get(r, "SENT")
        check("with the lock removed the end-to-end keys still work", await got("Message after the lock was removed"))
        await asyncio.sleep(5)
        check("with the lock removed the peer key file is plain again (converted back)", on_disk(fp_hex))
    async def bob_profile():
        # Carol is a stranger with a public profile; she is searched, added and accepts. (Bob stays the first friend.)
        cr, cw = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
        cw.write(frame("REG", "Carol")); await cw.drain(); carol = await get(cr, "OK_REG")
        # Bob sets up a public profile with a photo; the guest finds him by name, adds him and sets up its own profile
        im = Image.new("RGB", (96, 96)); px = im.load()
        for y in range(96):
            for x in range(96): px[x, y] = (255 - x * 2, 60 + y, 90 + x)
        pb = io.BytesIO(); im.save(pb, "PNG"); data = pb.getvalue()
        cw.write(frame("FUP", str(len(data)), "carol.png")); await cw.drain(); fid = (await get(cr, "FUP_OK"))[1]
        cw.write(frame("FCH", fid, "0", base64.b64encode(data).decode())); cw.write(frame("FEND", fid)); await cw.drain(); await get(cr, "FEND_OK")
        for cmd in (("HANDLE", "carol.trains"), ("BIO", "Train photographer in Osaka"), ("DISC", "1"), ("AVATAR", fid)):
            cw.write(frame(*cmd)); await cw.drain()
        for want in ("OK_HANDLE", "OK_BIO", "OK_DISC", "OK_AVATAR"): await get(cr, want)
        type_text("/find carol")
        await asyncio.sleep(8)                                    # results arrive, the photo is downloaded
        screenshot("/tmp/prof_find.ppm")
        check("guest could press Add in the search results", click_color((0x2F, 0x7D, 0x4F), 2.0))
        fq = await get(cr, "FREQ_IN", 20); check("friend request from the search arrives", fq[1] == gid and fq[3].startswith("Hi, I found you"))
        cw.write(frame("FACC", gid)); await cw.drain(); await get(cr, "FRIEND")
        await asyncio.sleep(4)
        for line in ("/handle guest.tester", "/bio hello from C-OS", "/discover on", "/photo /messenger/files/" + fid):
            type_text(line); await asyncio.sleep(3)
        await asyncio.sleep(6)
        cw.write(frame("PROFILE", gid)); await cw.drain(); pf = await get(cr, "PROFILE")
        check("guest handle and bio are visible to a stranger once discoverable", pf[3] == "guest.tester" and pf[4] == "hello from C-OS" and pf[5] != "")
        if pf[5]:
            cw.write(frame("FGET", pf[5])); await cw.drain(); got = b""; last = 0
            while not last:
                d = await get(cr, "FDATA", 30); got += base64.b64decode(d[4]); last = int(d[3])
            check("the guest's photo is a 96x96 BMP", got[:2] == b"BM" and len(got) == 54 + 96 * 96 * 3)
        type_text("/profile"); await asyncio.sleep(3)
        screenshot("/tmp/prof_profile.ppm"); press("esc"); await asyncio.sleep(2)
        screenshot("/tmp/prof_list.ppm")

    async def bob_text():
        notes = "C-OS Message text viewer test\n日本語の行もあります。\n" + "".join("row %d: the quick brown fox jumps over the lazy dog\n" % i for i in range(60))
        data = notes.encode()
        w.write(frame("FUP", str(len(data)), "notes.txt")); await w.drain(); fid = (await get(r, "FUP_OK"))[1]
        w.write(frame("FCH", fid, "0", base64.b64encode(data).decode())); w.write(frame("FEND", fid)); await w.drain(); await get(r, "FEND_OK")
        w.write(e2e.send_frame("20", gid, "\x01F|%s|%d|notes.txt" % (fid, len(data)))); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(2)
        check("press Download", click_color((0x2F, 0x5C, 0x9E), 4.0))
        check("press Open", click_color((0x2F, 0x5C, 0x9E), 3.0))
        screenshot("/tmp/att_text.ppm")
    async def bob_ux():
        THEIRS = (0x2A, 0x33, 0x50)
        def greens():                                           # green status text in the chat header (typing / online)
            screenshot("/tmp/ux_head.ppm"); im = Image.open("/tmp/ux_head.ppm").convert("RGB"); px = im.load()
            return sum(1 for x in range(487, 760) for y in range(220, 242) if px[x, y][1] > 120 and px[x, y][1] > px[x, y][0] + 40 and px[x, y][1] > px[x, y][2] + 30)
        await asyncio.sleep(2)
        check("Bob shows as online in the header", greens() > 8)
        w.write(frame("SETPRES", "0")); await w.drain(); await get(r, "OK_SETPRES"); await asyncio.sleep(2)
        check("hiding presence removes the online mark", greens() == 0)
        w.write(frame("SETPRES", "1")); await w.drain(); await get(r, "OK_SETPRES")
        w.write(frame("TYPING", gid)); await w.drain(); await asyncio.sleep(1.5)
        check("Bob's typing indicator is shown", greens() > 8)
        screenshot("/tmp/ux_typing.ppm")
        w.write(e2e.send_frame("30", gid, "see https://example.com/page ok")); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(3)
        def select_theirs(ui):
            ui.shot("/tmp/ms.ppm"); b = find_lowest(Image.open("/tmp/ms.ppm").convert("RGB"), THEIRS)
            if not b: return None
            ui.goto((b[0] + b[2]) // 2, (b[1] + b[3]) // 2); ui.click(); time.sleep(1.5)
            ui.shot("/tmp/ux_sel.ppm"); return b
        b = ui_session(select_theirs); check("Bob's bubble is selectable", bool(b))
        by = b[1] - 28 + 11
        def pressbtn(n):
            def run(ui): ui.goto(b[0] + 27 + n * 58, by); ui.click(); time.sleep(1.0)
            ui_session(run)
        pressbtn(0)                                                      # Reply
        screenshot("/tmp/ux_replying.ppm")
        ui_type("got it")
        m = await get(r, "MSG", 20)
        check("a reply carries the quote header and the body", e2e.text(m).startswith("\x01Q") and e2e.text(m).endswith("\x02got it") and "see https://example.com/page ok" in e2e.text(m))
        w.write(frame("GOT", m[1])); await w.drain()
        await asyncio.sleep(2)
        screenshot("/tmp/ux_quote.ppm")
        # Bob answers with a reply of his own; the guest must show it as a quote
        w.write(e2e.send_frame("31", gid, "\x01QBob\x1fsee https://example.com/page ok\x02thanks, a reply from Bob")); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(3)
        screenshot("/tmp/ux_bobreply.ppm")
        # Link: select Bob's first message again and copy its link, then paste it into the input and send it
        w.write(e2e.send_frame("32", gid, "link https://example.org/x?y=1 here")); await w.drain(); await get(r, "SENT")
        await asyncio.sleep(3)
        b = ui_session(select_theirs); check("a bubble with a link can be selected", bool(b))
        by = b[1] - 28 + 11
        pressbtn(2)                                                      # Link
        def paste(ui): ui.key("v", ctrl=True); time.sleep(1); ui.key("ret")
        ui_session(paste)
        m = await get(r, "MSG", 20); check("the copied link is pasted and sent", e2e.text(m) == "https://example.org/x?y=1")
        screenshot("/tmp/ux_end.ppm")
    async def bob_ja():
        # the interface follows the system language: switch to Japanese and look at the main window, the Menu pages and an Actions menu
        type_text("/lang ja"); await asyncio.sleep(3); screenshot("/tmp/ja_main.ppm")
        check("the Japanese language pack (a .c-osll shared library) was loaded with cos_dlopen", "i18n: Japanese language pack loaded" in open(os.environ["MSG_LOG"], errors="replace").read())
        def menu(ui):
            ui.goto(338, 189); ui.click(); time.sleep(2); ui.shot("/tmp/ja_menu0.ppm")
            for i in range(1, 6):
                ui.goto(700, 450); ui.wheel(8); time.sleep(1); ui.shot("/tmp/ja_menu%d.ppm" % i)
            ui.key("esc"); time.sleep(1)
        ui_session(menu)
        w.write(e2e.send_frame("9001", gid, "hello in Japanese mode")); await w.drain(); await get(r, "SENT"); await asyncio.sleep(3)
        def actions(ui):
            ui.goto(940, 188); ui.click(); time.sleep(2); ui.shot("/tmp/ja_actions.ppm"); ui.key("esc"); time.sleep(1)
            ui.goto(338, 189); ui.click(); time.sleep(2); ui.goto(772, 270); ui.click(); time.sleep(2); ui.shot("/tmp/ja_form.ppm"); ui.key("esc"); time.sleep(1); ui.key("esc"); time.sleep(1)
        ui_session(actions)
        type_text("/lock on"); await asyncio.sleep(3); screenshot("/tmp/ja_lockform.ppm"); press("esc")
        type_text("/help"); await asyncio.sleep(3); screenshot("/tmp/ja_help.ppm"); press("esc")
        type_text("/server"); await asyncio.sleep(2); screenshot("/tmp/ja_status.ppm")
        type_text("/e2e"); await asyncio.sleep(2); screenshot("/tmp/ja_e2e.ppm")
        type_text("/nonsense"); await asyncio.sleep(2); screenshot("/tmp/ja_unknown.ppm")
        type_text("/lang en"); await asyncio.sleep(2); screenshot("/tmp/ja_back.ppm")
    if os.environ.get("QUICK") == "ja":
        await bob_ja(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK") == "ux":
        await bob_ux(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK") == "lock":
        await bob_lock(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK") == "group":
        await bob_group(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK") == "profile":
        await bob_profile(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK") == "text":
        await bob_text(); sys.exit(0 if ok else 1)
    if os.environ.get("QUICK"):                 # QUICK=1: only the guest -> Bob file transfer
        await guest_to_bob(); sys.exit(0 if ok else 1)
    w.write(e2e.send_frame("1", gid, "Hello stranger from the internet")); await w.drain(); await get(r, "SENT")
    await asyncio.sleep(4)
    type_text("Nice to meet you Bob")
    m = await get(r, "MSG"); check("guest message arrives through the server", e2e.text(m) == "Nice to meet you Bob")
    w.write(frame("GOT", m[1])); await w.drain()
    first_id = m[5]
    await asyncio.sleep(3)                                      # the app repaints after the (slower, encrypted) send
    # the guest edits its own message through the UI (select the bubble, Edit, append text, Enter)
    check("guest could open the edit button", click_bubble_button(0))
    ui_type(" v2")
    e = await get(r, "EDIT_IN") if False else await get(r, "EDIT_IN", 20)
    check("guest edit reaches Bob (an encrypted EDIT)", e[4] == first_id and parse_event(e2e.text(e, 5, 1))[::2] == ("E", "Nice to meet you Bob v2"))
    w.write(frame("GOT", e[1])); await w.drain()
    # the guest deletes a fresh message through the UI
    type_text("temporary message")
    m2 = await get(r, "MSG"); w.write(frame("GOT", m2[1])); await w.drain()
    await asyncio.sleep(2)
    check("guest could open the delete button", click_bubble_button(1))
    d = await get(r, "EDIT_IN", 20); check("guest delete reaches Bob (an encrypted tombstone, never a plain DEL)", d[4] == m2[5] and parse_event(e2e.text(d, 5, 1))[0] == "D")
    w.write(frame("GOT", d[1])); await w.drain()
    # Japanese: the guest switches its IME on, types romaji, converts and sends; Bob answers in Japanese
    def ime_send(ui):
        ui.goto(144 + 740, 146 + 532); ui.click(); time.sleep(20)              # IME button (loads the dictionary)
        ui.type("konnichiha"); time.sleep(1); ui.key("spc"); time.sleep(2); ui.key("ret"); time.sleep(1); ui.key("ret")
    ui_session(ime_send)
    jm = await get(r, "MSG", 60); check("Japanese typed with the IME arrives", e2e.text(jm) == "こんにちは")
    w.write(frame("GOT", jm[1])); await w.drain()
    w.write(e2e.send_frame("9", gid, "こんにちは！日本語のメッセージです。「禁則処理」のテスト。")); await w.drain(); await get(r, "SENT")
    await asyncio.sleep(2)
    # ---- attachments ----
    import wave, struct as st, math
    async def upload(name, data, msgid):
        w.write(frame("FUP", str(len(data)), name)); await w.drain(); fid = (await get(r, "FUP_OK"))[1]
        for i in range(0, len(data), 6144):
            w.write(frame("FCH", fid, str(i // 6144), base64.b64encode(data[i:i + 6144]).decode())); await w.drain()
        w.write(frame("FEND", fid)); await w.drain(); await get(r, "FEND_OK")
        w.write(e2e.send_frame(msgid, gid, "\x01F|%s|%d|%s" % (fid, len(data), name))); await w.drain(); await get(r, "SENT")
        return fid
    notes = ("C-OS Message attachment test\n日本語のテキストファイルです。\n" + "line %d of the long text file\n" * 1) % 1 + "".join("row %d: the quick brown fox jumps over the lazy dog\n" % i for i in range(80))
    await upload("notes.txt", notes.encode(), "20")
    await asyncio.sleep(2)
    check("guest could press Download on the text file", click_color((0x2F, 0x5C, 0x9E), 4.0))
    check("guest could press Open on the text file", click_color((0x2F, 0x5C, 0x9E), 2.0))
    screenshot("/tmp/att_text.ppm"); press("esc"); await asyncio.sleep(1)
    buf = io.BytesIO(); wf = wave.open(buf, "wb"); wf.setnchannels(2); wf.setsampwidth(2); wf.setframerate(44100)
    wf.writeframes(b"".join(st.pack("<hh", int(8000 * math.sin(i / 20)), int(8000 * math.sin(i / 20))) for i in range(44100 * 2))); wf.close()
    await upload("tone.wav", buf.getvalue(), "21")
    await asyncio.sleep(2)
    check("guest could press Download on the audio file", click_color((0x2F, 0x5C, 0x9E), 4.0))
    check("guest could press Play", click_color((0xE5, 0x84, 0x5A), 3.0))
    screenshot("/tmp/att_audio.ppm")
    im = Image.new("RGB", (160, 100)); px = im.load()
    for y in range(100):
        for x in range(160): px[x, y] = (x * 255 // 160, y * 255 // 100, 128)
    wb = io.BytesIO(); im.save(wb, "WEBP", quality=85)
    await upload("pic.webp", wb.getvalue(), "22")
    pb = io.BytesIO(); im.save(pb, "PNG"); await upload("pic.png", pb.getvalue(), "23")
    await asyncio.sleep(6)                                              # images are downloaded automatically
    screenshot("/tmp/att_images.ppm")
    check("guest could open the image viewer", click_color((0x2A, 0x33, 0x50), 3.0))
    screenshot("/tmp/att_viewer.ppm"); press("esc"); await asyncio.sleep(1)
    await guest_to_bob()
    # Bob edits and deletes his own messages; the guest must show "編集しました" and the red deleted note
    w.write(e2e.send_frame("1", gid, event("E", "Hello stranger from the internet (edited)"), kind=1, cmd="EDIT")); await w.drain(); await get(r, "OK_EDIT")
    w.write(e2e.send_frame("3", gid, "oops wrong chat")); await w.drain(); await get(r, "SENT")
    await asyncio.sleep(2)
    w.write(frame("DEL", "3", gid)); await w.drain(); await get(r, "OK_DEL")              # a plain DEL from the server side must NOT delete anything:
    await asyncio.sleep(3); screenshot("/tmp/plain_del.ppm")
    w.write(e2e.send_frame("3", gid, event("D"), kind=1, cmd="EDIT")); await w.drain(); await get(r, "OK_EDIT")
    await asyncio.sleep(3)
    sys.exit(0 if ok else 1)
asyncio.run(main())
