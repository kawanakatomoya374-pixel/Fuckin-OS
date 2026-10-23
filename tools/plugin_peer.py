#!/usr/bin/env python3
"""Drives the plugin experiment (see run_plugin_test.sh and docs/plugins.md): C-OS Message and the File Manager are running while plugins are
switched on, used, replaced and switched off from the Terminal. Evidence: "[MESSENGER] ..." / "[FILES] ..." lines on the serial log."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI, type_command
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def log(): return open(logp, errors="replace").read()
def run(text): return type_command(logp, qmp, TYPE, text)
def wait(pat, t=40, since=0):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log()[since:])
        if m: return m
        time.sleep(1)
    return None
def shot(path):
    ui = UI(qmp)
    try: ui.shot(path)
    finally: ui.s.close()
def hclick(u, button="left"):
    """a click that is held for a moment: with seven windows the compositor can need 300 ms for a frame, and a press+release inside one frame is not seen"""
    u.btn(True, button); time.sleep(0.35); u.btn(False, button); time.sleep(0.2)
def ui_do(fn):
    ui = UI(qmp)
    try: return fn(ui)
    finally: ui.s.close()
def typ(text, enter=True):
    subprocess.run(["python3", TYPE, text] + ([] if enter else ["--no-enter"]), stdout=subprocess.DEVNULL, check=False, env=dict(os.environ, COS_QMP=qmp))
def mark(): return len(log())

STEP = os.environ.get("PLUGIN_STEP", "all")
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
shot("/tmp/plg_0.ppm")
ui_do(lambda u: (u.open_start_menu(), time.sleep(1), u.type("mess"), time.sleep(2), u.key("ret"), time.sleep(8)))
ui_do(lambda u: (u.open_start_menu(), time.sleep(1), u.type("file"), time.sleep(2), u.key("ret"), time.sleep(8)))
order = ["term", "msg", "files"]            # the taskbar: a button that was clicked moves to the end
def focus(w):
    i = order.index(w); x = 133 + 46 * i
    order.append(order.pop(i))
    ui_do(lambda u: (u.goto(x, 742), hclick(u), time.sleep(1.5)))
AV, ON = "/system/plugins/available", "/system/plugins"
focus("term"); shot("/tmp/plg_a.ppm")
m = mark(); run("cp %s/files/checksum.c-osll %s/files/checksum.c-osll" % (AV, ON))
check("files: a plugin copied into /system/plugins/files is loaded by the running File Manager", wait(r"\[FILES\] plugin loaded: checksum", 20, m) is not None)
m = mark(); run("cp %s/files/hexview.c-osll %s/messenger/hexview.c-osll" % (AV, ON))
check("messenger: a plugin made for another program is rejected", wait(r"\[MESSENGER\] plugin rejected: hexview", 20, m) is not None)
m = mark(); run("cp %s/messenger/dice.c-osll %s/messenger/dice.c-osll" % (AV, ON))
check("messenger: dice is loaded by the running Message app", wait(r"\[MESSENGER\] plugin loaded: dice", 20, m) is not None)
focus("files")
def lclick(x, y): ui_do(lambda u: (u.goto(x, y), time.sleep(0.4), hclick(u), time.sleep(1.2)))     # the pointer must have arrived (the compositor works at frame rate)
def pix(x, y): return ui_do(lambda u: u.pixel(x, y))
# --- File Manager: the new command is in the File menu of the running program, and it works
lclick(350, 671)
def menu_pick(x, y, snap=None, mx=83):
    """opens File and, without leaving the menu (hovering the menu bar would switch menus), clicks the item at (x, y)"""
    def f(u):
        u.goto(mx, 112); hclick(u); time.sleep(1.0)
        r = u.pixel(200, 380) if snap else None
        if x: u.move(x, y); time.sleep(0.4); hclick(u); time.sleep(1.2)
        return r
    return ui_do(f)
with_plugin = menu_pick(0, 0, snap=True)
ui_do(lambda u: (u.key("esc"), time.sleep(0.5)))
m = mark(); menu_pick(150, 339)
check("files: the plugin's command works inside the running File Manager (CRC-32 of a file)", wait(r"\[FILES\] plugin action 'Checksum \(CRC-32\)' rc=0: CRC-32: [0-9A-F]{8}", 15, m) is not None)
shot("/tmp/plg_c.ppm")
ui_do(lambda u: (u.key("ret"), time.sleep(1)))
# --- switched off while open: the command disappears
focus("term")
m = mark(); run("rm %s/files/checksum.c-osll" % ON)
check("files: removing the plugin file unloads it from the running program", wait(r"\[FILES\] plugin unloaded", 20, m) is not None)
focus("files")
check("files: the menu entry is gone", menu_pick(0, 0, snap=True) != with_plugin)
shot("/tmp/plg_d.ppm"); ui_do(lambda u: (u.key("esc"), time.sleep(0.5)))
# --- reload on change: a different library under the same file name replaces the loaded one
focus("term")
m = mark(); run("cp %s/files/hexview.c-osll %s/files/x.c-osll" % (AV, ON))
check("files: another plugin is loaded", wait(r"\[FILES\] plugin loaded: hexview", 20, m) is not None)
m = mark(); run("cp %s/files/checksum.c-osll %s/files/x.c-osll" % (AV, ON))
check("files: the file changed -> the old plugin is unloaded and the new one loaded (reload)", wait(r"\[FILES\] plugin unloaded[\s\S]*\[FILES\] plugin loaded: checksum", 20, m) is not None)
# --- a viewer: the preview pane shows a hex dump of a program file
m = mark(); run("cp %s/files/hexview.c-osll %s/files/hexview.c-osll" % (AV, ON))
check("files: viewer plugin loaded", wait(r"\[FILES\] plugin loaded: hexview", 20, m) is not None)
focus("files"); lclick(350, 671); menu_pick(150, 265, mx=168); time.sleep(2)
check("files: the preview pane uses the plugin's viewer", wait(r"\[FILES\] plugin viewer used", 10, m) is not None)
shot("/tmp/plg_e.ppm")
# --- Message: commands are added and removed while it runs
focus("msg"); lclick(640, 692)
m = mark(); typ("/dice 6")
check("messenger: /dice (from the plugin) runs inside the running app", wait(r"\[MESSENGER\] status: Open a chat first", 15, m) is not None)
m = mark(); typ("/plugins off dice")
check("messenger: '/plugins off dice' unloads the plugin", wait(r"\[MESSENGER\] plugin unloaded", 15, m) is not None)
m = mark(); typ("/dice 6")
check("messenger: /dice is no longer a command", wait(r"\[MESSENGER\] unknown command", 15, m) is not None)
m = mark(); typ("/plugins on dice")
check("messenger: '/plugins on dice' loads it again", wait(r"\[MESSENGER\] plugin loaded: dice", 15, m) is not None)
m = mark(); typ("/dice 6")
check("messenger: /dice works again", wait(r"\[MESSENGER\] status: Open a chat first", 15, m) is not None and "unknown command" not in log()[m:])
shot("/tmp/plg_g.ppm")
# ================= more programs: Terminal, the extension daemon, Settings, Studio, Py2C =================
focus("term")
m = mark(); run("plugin on calc")
check("terminal: 'plugin on calc' loads the plugin into the running Terminal", wait(r"\[TERMINAL\] plugin loaded: calc", 20, m) is not None)
m = mark(); run("calc (2+3)*4")
check("terminal: the new command calc works", wait(r"\[TERMINAL\] pout: = 20", 15, m) is not None)
m = mark(); run("plugin on textutils"); wait(r"plugin loaded: textutils", 20, m)
m = mark(); run("wc /etc/hostname")
check("terminal: wc (textutils) counts a file", wait(r"\[TERMINAL\] pout:\s+\d+\s+\d+\s+\d+ /etc/hostname", 15, m) is not None)
m = mark(); run("plugin on alias"); wait(r"plugin loaded: alias", 20, m)
run("alias hi=calc 6*7")
m = mark(); run("hi")
check("terminal: an alias rewrites the line before it runs", wait(r"\[TERMINAL\] pout: = 42", 15, m) is not None)
m = mark(); run("plugin off calc")
check("terminal: 'plugin off calc' unloads it", wait(r"\[TERMINAL\] plugin unloaded", 15, m) is not None)
m = mark(); run("calc 1+1"); time.sleep(3)
check("terminal: calc is gone after it was switched off", "pout: = 2" not in log()[m:])
m = mark(); run("plugin on sysinfo"); wait(r"plugin loaded: sysinfo", 20, m)
m = mark(); run("uptime")
check("terminal: uptime (sysinfo) answers", wait(r"\[TERMINAL\] pout: up \d+ h", 15, m) is not None)

# the extension daemon: a plugin of the system applies a desktop colour at once, and gives the wallpaper back when it is removed
p0 = pix(900, 60)
m = mark(); run("cp %s/system/desktopcolor.c-osll %s/system/desktopcolor.c-osll" % (AV, ON))
check("system: the extension daemon loads a plugin copied into /system/plugins/system", wait(r"\[EXTD\] plugin loaded: desktopcolor", 25, m) is not None)
m = mark(); run("echo enabled=1 > %s/config/desktopcolor.cfg" % ON)
check("system: the stored setting is picked up by the daemon", wait(r"\[DESKTOPCOLOR\] enabled=1 top=1E3A5F bottom=0F2A44 gradient=1", 20, m) is not None)
time.sleep(1.5); p1 = pix(900, 60); shot("/tmp/plg_h.ppm")
check("system: the desktop background really changed colour (%s -> %s)" % (p0, p1), abs(p1[0] - 29) <= 8 and abs(p1[1] - 56) <= 8 and abs(p1[2] - 92) <= 8 and p1 != p0)
m = mark(); run("rm %s/config/desktopcolor.cfg" % ON)
check("system: with the setting gone the wallpaper comes back", wait(r"\[DESKTOPCOLOR\] enabled=0", 20, m) is not None)
time.sleep(1.5); check("system: the wallpaper pixel is as before", all(abs(a - b) <= 6 for a, b in zip(pix(900, 60), p0)))
m = mark(); run("rm %s/system/desktopcolor.c-osll" % ON)
check("system: the daemon unloads it", wait(r"\[EXTD\] plugin unloaded", 20, m) is not None)

# Settings: extension items appear on the Extensions tab
m = mark(); run("cp %s/settings/desktopcolor.c-osll %s/settings/desktopcolor.c-osll" % (AV, ON))
run("cp %s/settings/layout.c-osll %s/settings/layout.c-osll" % (AV, ON))
ui_do(lambda u: (u.open_start_menu(), time.sleep(1), u.type("sett"), time.sleep(2), u.key("ret"), time.sleep(8)))
check("settings: the Settings app loads the plugins that were already in its folder", wait(r"\[SETTINGS\] plugin loaded: desktopcolor", 20, m) is not None)
order.append("sett")
# Settings: the Extensions tab shows the plugin's items; a click changes the real desktop colour
lclick(100, 640)
p0 = pix(900, 10)
m = mark(); lclick(603, 222)
if wait(r"\[DESKTOPCOLOR\] enabled=1", 6, m) is None: print("INFO the first click on the toggle was lost; clicking again", flush=True); m = mark(); lclick(603, 222)
check("settings: the plugin's toggle on the Extensions tab is applied (desktop colour on)", wait(r"\[DESKTOPCOLOR\] enabled=1", 15, m) is not None)
time.sleep(1.5); p1 = pix(900, 10); shot("/tmp/plg_i.ppm")
check("settings: the desktop background changed (%s -> %s)" % (p0, p1), p1 != p0 and abs(p1[2] - 95) <= 10)
m = mark(); lclick(603, 222)
check("settings: switching it off gives the wallpaper back", wait(r"\[DESKTOPCOLOR\] enabled=0", 15, m) is not None)
time.sleep(1.5); check("settings: the wallpaper pixel is as before", all(abs(a - b) <= 6 for a, b in zip(pix(900, 10), p0)))
lclick(100, 672); shot("/tmp/plg_j.ppm")
lclick(100, 640)
# Studio
ui_do(lambda u: (u.open_start_menu(), time.sleep(1), u.type("stud"), time.sleep(2), u.key("ret"), time.sleep(10)))
order.append("studio")
focus("term")
m = mark()
for pl in ("texttools", "todo", "docstats"): run("cp %s/studio/%s.c-osll %s/studio/%s.c-osll" % (AV, pl, ON, pl))
check("studio: plugins copied while Studio runs are loaded", wait(r"\[STUDIO\] plugin loaded: docstats", 25, m) is not None)
focus("studio")
m = mark(); ui_do(lambda u: (u.goto(315, 93), hclick(u), time.sleep(1), u.move(352, 484), time.sleep(0.4), hclick(u), time.sleep(1)))
check("studio: a Tools menu item added by a plugin runs", wait(r"\[STUDIO\] plugin command: Document statistics", 15, m) is not None)
m = mark(); ui_do(lambda u: (u.goto(315, 93), hclick(u), time.sleep(1), u.move(327, 259), time.sleep(0.4), hclick(u), time.sleep(1.5))); shot("/tmp/plg_k.ppm")
ui_do(lambda u: (u.key("esc"), time.sleep(0.5)))
focus("term"); m = mark(); run("rm %s/studio/texttools.c-osll" % ON)
check("studio: removing a plugin removes its menu items", wait(r"\[STUDIO\] plugin unloaded", 20, m) is not None)
# Py2C (from the terminal: it is not in the Start menu); its own Plugins panel switches plugins on while it runs
m = mark(); run("Py2C"); time.sleep(8); order.append("py2c")
lclick(755, 102); time.sleep(1); shot("/tmp/plg_l.ppm")
m = mark(); lclick(527, 322 + 40 * 0)
check("py2c: a plugin switched on in the Plugins panel is loaded by the running Py2C Studio", wait(r"\[PY2C\] plugin loaded: header", 20, m) is not None)
lclick(527, 322 + 40 * 1); lclick(527, 322 + 40 * 2); lclick(527, 322 + 40 * 3); shot("/tmp/plg_m.ppm")
lclick(716, 498); time.sleep(1)
m = mark(); lclick(248, 102)
check("py2c: Convert runs the plugin's output filter", wait(r"\[P2C-HEADER\] header added", 20, m) is not None)
shot("/tmp/plg_n.ppm")
print("log tail:", log()[-300:])
sys.exit(0 if ok else 1)
