#!/usr/bin/env python3
"""End-to-end encryption test: the guest (C-OS Message in QEMU) and Bob (this script, own keys) talk through the reference server on the host.
Checks what the SERVER can see (it must hold only ciphertext), the safety number, plain-text downgrade refusal, a changed friend key
(sending stops, incoming messages wait, "Trust new key" releases them) and encrypted attachments."""
import asyncio, os, re, sqlite3, ssl, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_e2e import E2E, b64d, file_open, fingerprint, safety_number
from cosmsg_server import frame, unesc
port, db, logp = int(sys.argv[1]), sys.argv[2], sys.argv[3]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
def type_text(t): subprocess.run(["python3", TYPE, t], stdout=subprocess.DEVNULL, check=False)
def log(): return open(logp, errors="replace").read()
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
async def get(r, want, t=25):
    while True:
        line = await asyncio.wait_for(r.readline(), t)
        if not line: raise ConnectionError("closed")
        f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
        if f[0] == want: return f
async def quiet(r, want, t):
    try: await get(r, want, t); return False
    except asyncio.TimeoutError: return True
async def wait_log(pat, t=60):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log())
        if m: return m
        await asyncio.sleep(1)
    return None
def db_all(q, *a):
    c = sqlite3.connect(db); r = c.execute(q, a).fetchall(); c.close(); return r
def db_bytes(): return open(db, "rb").read() + b"".join(open(os.path.join(db + ".files", f), "rb").read() for f in os.listdir(db + ".files")) if os.path.isdir(db + ".files") else open(db, "rb").read()

async def main():
    type_text("/server 192.168.70.2")
    rows = []
    for _ in range(60):
        await asyncio.sleep(2)
        try: rows = db_all("SELECT id FROM users")
        except sqlite3.Error: rows = []
        if rows: break
    check("the guest registered on the server", bool(rows)); gid = rows[0][0]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
    w.write(frame("REG", "Bob")); await w.drain(); bob = await get(r, "OK_REG"); bid = bob[1]
    e = E2E(bid); w.write(e.publish_frame()); w.write(frame("FREQ", gid, "hi")); await w.drain(); await get(r, "OK_FREQ")
    await asyncio.sleep(4); type_text("/accept " + bid)
    await get(r, "FRIEND")
    check("Bob learns the guest's key", await e.learn(r, w, get, gid))
    check("the app published a well-formed bundle (signature verified)", gid in e.peers)
    check("the app pinned Bob's key on first sight", await wait_log(r"e2e: pinned a key", 20) is not None)

    # 1. Bob -> guest: encrypted on the wire, readable in the app
    secret1 = "secret-marker-ALPHA-1"
    w.write(e.send_frame("101", gid, secret1)); await w.drain(); await get(r, "SENT")
    check("the app decrypts Bob's message", await wait_log(r"received a message \(%d bytes\) from %s" % (len(secret1), bid), 30) is not None)

    # 2. guest -> Bob
    type_text("guest-secret-BETA-2")
    m = await get(r, "MSG")
    check("Bob decrypts the guest's message", e.text(m) == "guest-secret-BETA-2" and m[4].startswith("\x01E") and "BETA" not in m[4])
    w.write(frame("GOT", m[1])); await w.drain()
    check("the guest's text is nowhere in the server's database", b"guest-secret-BETA-2" not in open(db, "rb").read() and b"secret-marker-ALPHA-1" not in open(db, "rb").read())

    # 3. attachment
    type_text("/send /etc/os-release")
    am = await get(r, "MSG", 60); w.write(frame("GOT", am[1])); await w.drain()
    ann = e.text(am).split("|"); check("the announcement is end-to-end encrypted and carries the file key", ann[0] == "\x01F" and len(ann) == 5 and len(b64d(ann[4])) == 32 and ann[3] == "os-release")
    frow = db_all("SELECT name,size FROM files WHERE fid=?", ann[1])
    check("the server never learns the file name", frow and frow[0][0] == "e2e.bin")
    import base64
    w.write(frame("FGET", ann[1])); await w.drain(); data = b""; last = 0
    while not last:
        d = await get(r, "FDATA", 30); data += base64.b64decode(d[4]); last = int(d[3])
    check("the file on the server is ciphertext", b"C-OS" not in data and len(data) == int(ann[2]) + 16)
    plain = file_open(b64d(ann[4]), data)
    check("Bob opens it with the key from the message", b"C-OS" in plain and len(plain) == int(ann[2]))
    bad = bytearray(data); bad[3] ^= 1
    try: file_open(b64d(ann[4]), bytes(bad)); tam = False
    except Exception: tam = True
    check("a tampered file is rejected", tam)

    # 4. safety number
    type_text("/safety"); await asyncio.sleep(3)
    mm = await wait_log(r"e2e: safety number ([0-9 ]{71})", 20)
    app_fp = fingerprint(e.peers[gid]["ik_dh"], e.peers[gid]["ik_sg"])
    check("the safety number matches the one computed on the other side", mm is not None and mm.group(1) == safety_number(app_fp, e.fingerprint()))
    from qmp_ui import UI
    ui = UI(os.environ.get("COS_QMP", "/tmp/cos_gui_qmp.sock"))
    try: ui.key("esc")
    finally: ui.s.close()
    await asyncio.sleep(1)

    # 5. plain text from a friend who has a key is refused
    before = len(re.findall(r"received a message", log()))
    w.write(frame("SEND", "102", gid, "plain-downgrade-text")); await w.drain(); await get(r, "SENT")
    placeholder = "[暗号化されていないメッセージを受け取りましたが、設定により破棄しました]"
    check("a downgrade to plain text is refused", await wait_log(r"received a message \(%d bytes\)" % len(placeholder.encode()), 30) is not None)

    # 6. the friend's key changes (new device / impostor)
    await asyncio.sleep(22)                                       # a failed decryption only triggers a fresh key lookup when the cached one is 20 s old
    e2 = E2E(bid); w.write(e2.publish_frame()); await w.drain(); await get(r, "OK_KEYPUT")
    check("Bob learns the guest's key again", await e2.learn(r, w, get, gid))
    secret3 = "message-from-the-new-key-GAMMA"
    n_rcv = len(re.findall(r"received a message", log()))
    w.write(e2.send_frame("103", gid, secret3)); await w.drain(); await get(r, "SENT")
    check("the app notices the changed key", await wait_log(r"e2e: friend key CHANGED", 40) is not None)
    await asyncio.sleep(2)
    check("a message under the unknown key is held back, not shown", len(re.findall(r"received a message", log())) == n_rcv)
    refused = False
    for _ in range(4):                                            # (a typed line can get lost on its way into the guest)
        type_text("sent-after-change-DELTA")
        if await wait_log(r"status: Bob's security key changed", 8): refused = True; break
    check("sending to a friend whose key changed is refused", refused and await quiet(r, "MSG", 3))
    type_text("/e2e trust")
    check("the user trusts the new key", await wait_log(r"e2e: new key trusted", 20) is not None)
    check("the held message is now delivered", await wait_log(r"received a message \(%d bytes\)" % len(secret3), 30) is not None)
    type_text("sent-after-trust-EPSILON")
    m = await get(r, "MSG", 40)
    check("messages flow again under the new key", e2.text(m) == "sent-after-trust-EPSILON")
    sys.exit(0 if ok else 1)
asyncio.run(main())
