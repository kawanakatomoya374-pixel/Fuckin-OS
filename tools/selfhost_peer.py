#!/usr/bin/env python3
"""Self-hosting check: the C-OS Message app inside the guest uses cosmsgd (started by "/host on", also inside the guest) as its server; Bob, on the
host, reaches the same server through QEMU's port forward and talks to the guest."""
import asyncio, os, re, ssl, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_server import frame, unesc
from cosmsg_e2e import E2E
port, logp = int(sys.argv[1]), sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
def type_text(t): subprocess.run(["python3", TYPE, t], stdout=subprocess.DEVNULL, check=False)
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
async def get(r, want, t=30):
    while True:
        line = await asyncio.wait_for(r.readline(), t)
        if not line: raise ConnectionError("closed")
        f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
        if f[0] == want: return f
def log(): return open(logp, errors="replace").read()
async def main():
    def click_gui(x, y):                                 # window coordinates -> the screen (the window's client area starts at 144,146)
        import qmp_ui
        ui = qmp_ui.UI(os.environ.get("COS_QMP", "/tmp/cos_gui_qmp.sock"))
        try: ui.goto(144 + x, 146 + y); ui.click(); time.sleep(1.0)
        finally: ui.s.close()
    click_gui(194, 44)                                   # Menu
    click_gui(681, 217)                                  # "Be the server for other C-OS machines" -> Start (starts the server and points the app at it)
    gid = None
    for _ in range(60):
        await asyncio.sleep(2)
        m = re.search(r"registered id=(\d+)", log())
        if m: gid = m.group(1); break
    check("the Message app registered on the C-OS-hosted server", gid is not None)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
    w.write(frame("REG", "Bob")); await w.drain(); bob = await get(r, "OK_REG")
    e2e = E2E(bob[1]); w.write(e2e.publish_frame()); await w.drain()
    w.write(frame("FREQ", gid, "Hi from another C-OS")); await w.drain(); await get(r, "OK_FREQ")
    await asyncio.sleep(4); type_text("/accept " + bob[1])
    fr = await get(r, "FRIEND"); check("the guest accepted Bob's friend request", fr[1] == gid)
    check("Bob learns the guest's end-to-end key", await e2e.learn(r, w, get, gid))
    w.write(e2e.send_frame("1", gid, "Hello through a C-OS server")); await w.drain(); await get(r, "SENT")
    await asyncio.sleep(3); type_text("Got it, Bob")
    m = await get(r, "MSG"); check("the guest's reply reaches Bob through the C-OS server, encrypted", m[4].startswith("\x01E") and "Got it" not in m[4] and e2e.text(m) == "Got it, Bob")
    w.write(frame("GOT", m[1])); await w.drain()
    type_text("日本語は?") if False else None
    w.write(e2e.send_frame("2", gid, "こんにちは、C-OS サーバー経由です")); await w.drain(); await get(r, "SENT")
    await asyncio.sleep(3)
    check("the guest's app is still connected", "server: " not in log().split("registered id=")[-1] or "Disconnected" not in log().split("registered id=")[-1])
    # the watchdog: the server is killed (as if it had crashed) and the app starts it again; its state survives and the app reconnects
    type_text("/host kill")
    started = False
    for _ in range(30):
        await asyncio.sleep(2)
        if "the server had stopped - started again" in log(): started = True; break
    check("the watchdog starts the hosted server again after it was killed", started)
    await asyncio.sleep(6)
    ok_login = False
    for _ in range(10):
        try:
            r2, w2 = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
            w2.write(frame("LOGIN", bob[1], bob[2])); await w2.drain(); await get(r2, "OK_LOGIN", 10); fr2 = await get(r2, "FRIEND", 10)
            ok_login = fr2[1] == gid; break
        except Exception: await asyncio.sleep(3)
    check("the restarted server kept its state (Bob logs in with the old token and still has his friend)", ok_login)
    sys.exit(0 if ok else 1)
asyncio.run(main())
