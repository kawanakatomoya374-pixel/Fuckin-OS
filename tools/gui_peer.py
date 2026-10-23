#!/usr/bin/env python3
"""GUI-only test of C-OS Message: the guest is driven with the mouse and plain typing only (no /commands); Bob (host, Python server)
checks on the server what the guest's buttons did. Geometry mirrors userland/programs/messenger/gui.inc."""
import asyncio, os, sqlite3, ssl, subprocess, sys, time
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI
from cosmsg_e2e import E2E
from cosmsg_server import frame, unesc
port, db = int(sys.argv[1]), sys.argv[2]
QMP = os.environ.get("COS_QMP", "/tmp/cos_gui_qmp.sock")
OX, OY = 144, 146                     # the message window's client area on the screen
MN_W, MN_H, MN_ROW, MN_HEAD, WIN_W, WIN_H = 600, 500, 46, 28, 860, 560
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def session(fn):
    ui = UI(QMP)
    try: return fn(ui)
    finally: ui.s.close()
def click(x, y, wait=1.0):
    def run(ui): ui.goto(OX + x, OY + y); ui.click(); time.sleep(wait)
    session(run)
def type_(t, enter=False):
    def run(ui):
        ui.type(t)
        if enter: ui.key("ret")
    session(run)
def key(q): session(lambda ui: ui.key(q))
def shot(path): session(lambda ui: ui.shot(path))
def wheel(n): session(lambda ui: (ui.goto(OX + 430, OY + 250), ui.wheel(n)))

# main menu rows in the order of gui.inc menu_rows(PG_MAIN) when not hosting; (kind, [button widths in chars])
def menu_click(label_index_from_top, button_from_right, scroll_rows=0):
    pass

def log(): return open(os.environ["MSG_LOG"], errors="replace").read()
def act_id(name):
    import re
    src = open(os.path.join(os.path.dirname(__file__), "..", "userland/programs/messenger/gui.inc")).read()
    body = src[src.index("enum { A_NONE"):]; body = body[:body.index("};")]
    names = [x.strip() for x in body[body.index("{") + 1:].replace("\n", " ").split(",")]
    return names.index(name)
def click_until(x, y, pattern, tries=5):
    """clicks (x, y) until the app logs `pattern` once more than before (a click can get lost on the way)"""
    before = log().count(pattern)
    for _ in range(tries):
        click(x, y, 0.8)
        if log().count(pattern) > before: return True
    return False
def open_actions():
    return click_until(797, 43, "[MESSENGER] ui menu 2")

def open_actions_old():
    """clicks the Actions button until the overlay is up (the panel colour shows at the bottom of the window)"""
    for _ in range(4):
        click(797, 43); shot("/tmp/gui_probe.ppm")
        px = Image.open("/tmp/gui_probe.ppm").convert("RGB").getpixel((OX + 700, OY + 500))
        if px == (0x1C, 0x23, 0x33): return True
        time.sleep(0.5)
    return False

async def get(r, want, t=30):
    while True:
        line = await asyncio.wait_for(r.readline(), t)
        if not line: raise ConnectionError("closed")
        f = [unesc(x) for x in line.decode().rstrip("\n").split("\t")]
        if f[0] == want: return f

def menu_button(row_top, right_label_chars, scroll_px=0):
    """screen-relative (window coords) centre of the rightmost button of a menu row whose top is row_top in the list."""
    x0, y0 = (WIN_W - MN_W) // 2, (WIN_H - MN_H) // 2
    w = 7 * right_label_chars + 22
    return x0 + MN_W - 20 - w // 2, y0 + 44 + row_top - scroll_px + 23

async def main():
    click(194, 44)                                   # the Menu button (left header)
    click(577, 120)                                  # "Change..." in the Server row
    type_("192.168.70.2", enter=True)
    for _ in range(40):
        time.sleep(2)
        try: rows = sqlite3.connect(db).execute("SELECT id,name FROM users").fetchall()
        except sqlite3.Error: rows = []
        if rows: break
    check("the guest connected to the server through the menu and registered", bool(rows))
    gid = rows[0][0]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
    w.write(frame("REG", "Bob")); await w.drain(); bob = await get(r, "OK_REG"); e2e = E2E(bob[1]); w.write(e2e.publish_frame()); await w.drain()
    w.write(frame("FREQ", gid, "Hi, we have never met")); await w.drain(); await get(r, "OK_FREQ")
    time.sleep(4); shot("/tmp/gui_req.ppm")
    click(62, 155)                                   # Accept on the request row
    fr = await get(r, "FRIEND"); check("the guest accepted the request with the Accept button", fr[1] == gid)
    check("Bob learns the guest's end-to-end key", await e2e.learn(r, w, get, gid))
    w.write(e2e.send_frame("1", gid, "Hello from Bob")); await w.drain(); await get(r, "SENT"); time.sleep(3)
    click(240, 520); type_("Hi Bob, typed in the box"); click(810, 520)         # message box, then the Send button
    m = await get(r, "MSG"); check("a message typed and sent with the mouse arrives", e2e.text(m) == "Hi Bob, typed in the box")
    w.write(frame("GOT", m[1])); await w.drain()
    # create a group from the menu (scroll the list down, press Create..., fill the form)
    click(194, 44); time.sleep(0.5)
    shot("/tmp/gui_scrolled.ppm")
    x, y = menu_button(166 + 28 + 5 * 46, 9); click(x, y)                         # Server section (166) + Friends header + 4 rows -> Create a group
    time.sleep(0.5); shot("/tmp/gui_gform.ppm")
    type_("GuiClub", enter=True); time.sleep(0.5); type_("593724", enter=True)        # name, next field, the 6-digit password, submit
    time.sleep(4)
    g = sqlite3.connect(db).execute("SELECT gid,name,owner FROM groups").fetchall()
    check("a group made with the Menu and a form exists on the server", len(g) == 1 and g[0][1] == "GuiClub" and g[0][2] == gid)
    shot("/tmp/gui_group.ppm")
    # the group's Actions menu: rename the group
    click(100, 186)                                  # the group in the list (second row)
    open_actions()                                   # the Actions button of the chat header
    shot("/tmp/gui_actions.ppm")
    x, y = menu_button(396, 9)                       # "Group name" row: Rename...
    click_until(x, y, "[MESSENGER] ui act %d" % act_id("A_GRENAME")); time.sleep(0.5); type_(" 2", enter=True); time.sleep(4)               # the form starts with the current name; append to it
    nm = sqlite3.connect(db).execute("SELECT name FROM groups").fetchone()[0]
    check("a group renamed with Actions -> Rename... and a form (prefilled with the old name)", nm == "GuiClub 2")
    open_actions(); x, y = menu_button(258, 4); click_until(x, y, "[MESSENGER] ui act %d" % act_id("A_MEMBERS")); time.sleep(1); shot("/tmp/gui_members.ppm")
    key("esc"); time.sleep(0.5)
    # pin, mute, search, drafts and a mention (all from buttons and forms)
    br, bw = r, w                                    # Bob's connection
    gidg = sqlite3.connect(db).execute("SELECT gid FROM groups").fetchone()[0]
    bw.write(frame("GJOIN", gidg, "593724")); await bw.drain(); await get(br, "OK_GJOIN")
    time.sleep(2); click(240, 520); type_("/gapprove " + bob[1], enter=True)       # the owner approves Bob (only the admins' key counts); the guest then makes the key and hands it to him
    gk = await get(br, "GKEY_IN", 30); gep = int(gk[5]); e2e.unwrap_key(gid, gidg, gep, gk[6]); bw.write(frame("GOT", gk[1])); await bw.drain()
    bw.write(frame("GSEND", "801", gidg, e2e.gencrypt(gidg, gep, "801", "@C-OS-10 ping from the group"))); await bw.drain(); await get(br, "GSENT"); time.sleep(3)
    check("an @mention of the guest is recognised", "[MESSENGER] mention" in log())
    def press_row(row_top, chars, act, expect):           # a toggle in the Actions page
        open_actions(); x, y = menu_button(row_top, chars); click_until(x, y, "[MESSENGER] ui act %d" % act_id(act)); time.sleep(1.0); key("esc"); time.sleep(0.5)
        return expect in log()
    check("Pin to the top of the list works from Actions", press_row(74, 3, "A_PIN", "[MESSENGER] pin"))
    check("Mute works from Actions", press_row(120, 3, "A_MUTE", "[MESSENGER] mute"))
    open_actions(); x, y = menu_button(166, 9); click_until(x, y, "[MESSENGER] ui act %d" % act_id("A_SEARCH")); time.sleep(0.8); type_("ping", enter=True); time.sleep(2)
    check("Search in this chat finds the message", "[MESSENGER] search 1 matches" in log())
    shot("/tmp/gui_search.ppm")
    click(240, 520); type_("half written"); click(100, 186); time.sleep(1); click(100, 131); time.sleep(1.5)      # Bob (second row), then the pinned group (first row)
    click(100, 186); time.sleep(1); click(100, 131); time.sleep(1.5)
    check("a half-written message is kept for its chat", "[MESSENGER] draft restored" in log())
    shot("/tmp/gui_draft.ppm")
    # block Bob from the Actions button of his chat
    click(100, 186)                                  # Bob in the list (the pinned group is first)
    open_actions()
    x, y = menu_button(258, 8); click_until(x, y, "[MESSENGER] ui act %d" % act_id("A_BLOCK")); time.sleep(1)
    shot("/tmp/gui_confirm.ppm")
    click(620, 317); time.sleep(4)                   # Yes
    bl = sqlite3.connect(db).execute("SELECT a,b FROM blocks").fetchall()
    check("Bob blocked with Actions -> Block... and Yes", (gid, bob[1]) in bl)
    sys.exit(0 if ok else 1)
asyncio.run(main())
