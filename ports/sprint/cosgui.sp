# cosgui: the C-OS window backend for `window` (and so for gui.App). Importing it registers the backend "cos":
#   import window
#   import cosgui
#   let win = window.open(640, 400, "My app", "cos")
#   window.run(app, win, -1, (app, w) => true)     # idle returns true to keep running while no events arrive
import _cosgui
import str
import window
import gui
import time

const SPECIAL = {1: "Enter", 2: "Backspace", 3: "Escape", 4: "Up", 5: "Down", 6: "Left", 7: "Right", 8: "Tab", 9: "Delete", 10: "Home", 11: "End", 12: "PageUp", 13: "PageDown", 14: "Insert"}

class CosBackend extends window.Backend:
    let down = 0
    let clicks = 1
    let last_click = 0
    let last_ms = -1.0
    override func init(self):
        self.name = "cos"
    override func open(self, w, h, title):
        self.width = w
        self.height = h
        self.title = title
        if _cosgui.open(w, h, title) == 0:
            throw error("RuntimeError", "cannot open a C-OS window")
    func mods_str(self, m):
        let out = []
        if m & 2:
            out.append("Ctrl")
        if m & 4:
            out.append("Alt")
        if m & 1:
            out.append("Shift")
        return out.join("+")
    # milliseconds since the previous poll (the toolkit's timers, tooltips and the apps' own on_tick run on this)
    func dt(self):
        let now = time.monotonic() * 1000.0
        let d = 0 if self.last_ms < 0.0 else to_int(now - self.last_ms)
        self.last_ms = now
        return d
    override func poll(self):
        let raw = _cosgui.poll(40)
        let evs = []
        let i = 0
        while i + 6 < len(raw):
            let t = raw[i]
            let x = raw[i + 1]
            let y = raw[i + 2]
            let b = raw[i + 3]
            let m = raw[i + 4]
            let sp = raw[i + 5]
            let a = raw[i + 6]
            i += 7
            if t == 2:
                evs.append(gui.mouse_move(x, y))
            elif t == 3:
                evs.append(gui.mouse_down(x, y, 1 if b == 1 else (3 if b == 2 else 2), self.mods_str(m), 1))
            elif t == 4:
                evs.append(gui.mouse_up(x, y, 1 if b == 1 else (3 if b == 2 else 2), self.mods_str(m)))
            elif t == 5:
                evs.append(gui.wheel(x, y, -b))
            elif t == 6:
                evs.append(gui.quit_event())
            elif t == 1:
                let ms = self.mods_str(m)
                if sp in SPECIAL:
                    let k = SPECIAL[sp]
                    evs.append(gui.key(k if ms == "" else ms .. "+" .. k))
                elif sp >= 16 and sp <= 27:
                    let k = "F" .. to_string(sp - 15)
                    evs.append(gui.key(k if ms == "" else ms .. "+" .. k))
                elif a >= 32 and a < 127:
                    let ch = str.from_codepoint(a)
                    if (m & 2) or (m & 4):
                        evs.append(gui.key(ms .. "+" .. ch.upper()))
                    else:
                        evs.append(gui.text(ch))
        if len(evs) == 0:
            evs.append(gui.tick(self.dt()))
        return evs
    override func present(self, canvas):
        _cosgui.present(canvas)
    override func set_title(self, s):
        self.title = s
        _cosgui.set_title(s)
    override func close(self):
        self.closed = true
        _cosgui.close()

window.register("cos", () => CosBackend())
