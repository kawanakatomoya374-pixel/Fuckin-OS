#!/usr/bin/env python3
"""Drive the C-OS GUI through QMP (PS/2 relative mouse + keyboard) for repeatable interaction tests.

  from qmp_ui import UI
  ui = UI('/tmp/x.qmp'); ui.goto(615, 215); ui.click(); ui.type('hello'); ui.key('ret'); ui.shot('/tmp/a.ppm')
goto() first homes the pointer into the top-left corner, then moves by exact deltas, so it is absolute."""
import json, socket, time

QC = {c: c for c in 'abcdefghijklmnopqrstuvwxyz0123456789'}
QC.update({' ': 'spc', '-': 'minus', '.': 'dot', ',': 'comma', '/': 'slash'})

class UI:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.recv(4096)
        self.cmd({'execute': 'qmp_capabilities'})
        self.x = self.y = None

    def cmd(self, o):
        self.s.sendall((json.dumps(o) + '\n').encode())
        buf = b''
        while True:
            buf += self.s.recv(65536)
            for line in buf.decode(errors='replace').split('\n'):
                if line.strip():
                    try: j = json.loads(line)
                    except Exception: continue
                    if 'return' in j or 'error' in j: return j

    def ev(self, events):
        return self.cmd({'execute': 'input-send-event', 'arguments': {'events': events}})

    def rel(self, dx, dy):
        """Relative motion, split into pieces of at most 100 so USB boot-protocol mice (8-bit deltas) keep it all."""
        dx, dy = int(dx), int(dy)
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
            evs = []
            if sx: evs.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
            if sy: evs.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            self.ev(evs)
            dx -= sx; dy -= sy
            time.sleep(0.01)

    def home(self):
        for _ in range(12): self.rel(-300, -300); time.sleep(0.02)
        self.x = self.y = 0

    def move(self, x, y, step=40, delay=0.012):
        """Smooth relative move from the current position (so hover events fire along the way)."""
        if self.x is None: self.home()
        while (self.x, self.y) != (x, y):
            dx = max(-step, min(step, x - self.x)); dy = max(-step, min(step, y - self.y))
            self.rel(dx, dy); self.x += dx; self.y += dy
            time.sleep(delay)

    def goto(self, x, y):
        self.home(); self.move(x, y)

    def click(self, button='left'):
        self.ev([{'type': 'btn', 'data': {'button': button, 'down': True}}]); time.sleep(0.05)
        self.ev([{'type': 'btn', 'data': {'button': button, 'down': False}}]); time.sleep(0.15)

    def key(self, qcode, shift=False, ctrl=False):
        evs = []
        if ctrl: evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': 'ctrl'}, 'down': True}})
        if shift: evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': 'shift'}, 'down': True}})
        evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': qcode}, 'down': True}})
        evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': qcode}, 'down': False}})
        if shift: evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': 'shift'}, 'down': False}})
        if ctrl: evs.append({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': 'ctrl'}, 'down': False}})
        self.ev(evs); time.sleep(0.08)

    def type(self, text):
        for ch in text:
            if ch.isupper(): self.key(ch.lower(), shift=True)
            else: self.key(QC.get(ch, ch))

    def btn(self, down, button='left'):
        self.ev([{'type': 'btn', 'data': {'button': button, 'down': down}}]); time.sleep(0.15)

    def wheel(self, n):
        for _ in range(abs(n)):
            b = 'wheel-up' if n > 0 else 'wheel-down'
            self.ev([{'type': 'btn', 'data': {'button': b, 'down': True}}]); self.ev([{'type': 'btn', 'data': {'button': b, 'down': False}}]); time.sleep(0.05)

    def shot(self, path):
        self.cmd({'execute': 'screendump', 'arguments': {'filename': path}}); time.sleep(0.4)

    def pixel(self, x, y):
        """Colour (r, g, b) of one pixel of the screen right now."""
        import os, tempfile
        fd, path = tempfile.mkstemp(suffix='.ppm'); os.close(fd)
        try:
            self.shot(path)
            with open(path, 'rb') as f:
                f.readline(); w, h = map(int, f.readline().split()); f.readline()
                data = f.read()
            o = (y * w + x) * 3
            return tuple(data[o:o + 3])
        finally:
            os.unlink(path)

    def open_start_menu(self, tries=12, wait=4.0):
        """Clicks the Start button until the menu is really open (the panel is light, the desktop dark). Right after boot the button can take a
        while to respond and a click before that is lost, so a fixed delay is not enough: retry, but never click again while it is open (that would close it)."""
        for _ in range(tries):
            self.goto(50, 746); self.click(); time.sleep(wait)
            if self.pixel(300, 300)[0] > 200:
                return True
        return False


def type_command(logp, qmp, type_script, text, attempts=3):
    """Types `text` and Enter into the guest terminal and checks, from the "[TERMINAL] cmd: ..." line the terminal logs on Enter, that exactly this
    command was run. A key event can be lost when the guest is busy (a letter missing, an Enter lost so the next line is glued to this one); then
    the line that was run is wrong, and the command is typed again. Commands used by the tests are safe to run again."""
    import os, re, subprocess
    def cmds():
        return re.findall(r"\[TERMINAL\] cmd: ([^\n]*)", open(logp, errors="replace").read())
    for _ in range(attempts):
        before = len(cmds())
        subprocess.run(["python3", type_script, text], stdout=subprocess.DEVNULL, check=False, env=dict(os.environ, COS_QMP=qmp))
        end = time.time() + 8
        while time.time() < end and len(cmds()) <= before:
            time.sleep(0.3)
        if len(cmds()) > before and cmds()[-1] == text:
            return True
        if len(cmds()) <= before:                      # the Enter was lost: submit whatever is in the line, then start over
            ui = UI(qmp)
            try: ui.key("ret")
            finally: ui.s.close()
            time.sleep(1)
    return False

