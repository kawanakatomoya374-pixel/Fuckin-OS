# wm: a desktop environment on top of gui - overlapping windows with title bars (drag, resize, close,
# minimise, maximise), a taskbar, desktop icons, a software mouse pointer and a terminal widget that
# runs a proc.Shell. Everything is drawn into the App canvas, so it works headless and its output is
# deterministic; hand the Desktop to window.run() to show it on a backend.
#
#   import wm
#   let d = wm.Desktop(320, 200)
#   let f = d.open_window("Hello", 20, 20, 160, 90, gui.Label("hi"))
import gfx
import gui
import proc
import str

const THEME = gui.THEME
const TB = 16
const BORDER = 2
const GRIP = 10

func imax(a, b):
    return a if a > b else b

func imin(a, b):
    return a if a < b else b

func clampi(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)

# a top-level window: title bar, content area, buttons, resize grip
export class Frame extends gui.Widget:
    let title = ""
    let content = nil
    let state = "normal"
    let saved = [0, 0, 0, 0]
    let active = false
    let mode = ""
    let grab = [0, 0]
    let min_w = 80
    let min_h = 40
    let resizable = true
    let on_close = nil
    let desktop = nil
    override func init(self, title, x, y, w, h, content = nil):
        super(x, y, w, h)
        self.title = title
        self.shape_round(6)
        if content != nil:
            self.set_content(content)
    func set_content(self, wd):
        if self.content != nil:
            self.children = []
        self.content = wd
        self.add(wd)
        self.relayout()
    override func layout(self):
        if self.content != nil:
            let c = self.content
            c.x = BORDER
            c.y = TB
            c.w = self.w - 2 * BORDER
            c.h = self.h - TB - BORDER
            c.pw = c.w
            c.ph = c.h
            c.layout()
    # which part of the window a point (relative to its corner) is in
    func part_at(self, px, py):
        if py < TB:
            let bx = self.w - 14
            if px >= bx and px < bx + 11:
                return "close"
            if px >= bx - 13 and px < bx - 2:
                return "max"
            if px >= bx - 26 and px < bx - 15:
                return "min"
            return "title"
        if self.resizable and self.state == "normal" and px >= self.w - GRIP and py >= self.h - GRIP:
            return "grip"
        return "body"
    # the title bar and the grip belong to the frame even where the content reaches under them
    override func pick(self, px, py):
        if not self.visible or not self.contains(px, py):
            return nil
        if self.part_at(px - self.abs_x(), py - self.abs_y()) != "body":
            return self
        return super.pick(px, py)
    override func cursor_kind(self):
        if self.pointer != nil:
            return self.pointer
        if self.app == nil:
            return "arrow"
        let p = self.part_at(self.app.mx - self.abs_x(), self.app.my - self.abs_y())
        if self.mode == "move":
            return "move"
        return "resize_h" if p == "grip" or self.mode == "resize" else "arrow"
    override func on_mouse_down(self, px, py, button):
        let p = self.part_at(px - self.abs_x(), py - self.abs_y())
        self.mode = p
        self.grab = [px - self.x, py - self.y]
        if p == "grip":
            self.mode = "resize"
            self.grab = [px - self.x - self.w, py - self.y - self.h]
        elif p == "title":
            self.mode = "move"
            if self.app != nil and self.app.clicks == 2 and self.resizable:
                self.mode = ""
                self.desktop.toggle_max(self)
        self.dirty()
    override func on_drag(self, px, py):
        if self.desktop == nil:
            return nil
        let area = self.desktop.work_area()
        if self.mode == "move" and self.state == "normal":
            self.dirty()
            self.x = clampi(px - self.grab[0], 40 - self.w, area[2] - 40)
            self.y = clampi(py - self.grab[1], 0, area[3] - TB)
            self.dirty()
        elif self.mode == "resize":
            self.w = imax(self.min_w, px - self.grab[0] - self.x)
            self.h = imax(self.min_h, py - self.grab[1] - self.y)
            self.pw = self.w
            self.ph = self.h
            self.relayout()
            self.dirty()
    override func on_mouse_up(self, px, py, button, inside):
        let p = self.part_at(px - self.abs_x(), py - self.abs_y())
        let m = self.mode
        self.mode = ""
        if p == m:
            if m == "close":
                self.desktop.close_frame(self)
            elif m == "max":
                self.desktop.toggle_max(self)
            elif m == "min":
                self.desktop.minimize(self)
        self.dirty()
    override func draw_self(self, c, ax, ay):
        let bar = THEME.accent if self.active else THEME.border
        c.set_alpha(90)
        c.fill_round_rect(ax + 2, ay + 3, self.w, self.h, 6, THEME.shadow)
        c.set_alpha(255)
        c.fill_round_rect(ax, ay, self.w, self.h, 6, THEME.panel)
        c.fill_round_rect(ax, ay, self.w, TB + 4, 6, bar)
        c.fill_rect(ax, ay + TB - 1, self.w, 6, THEME.panel)
        c.round_rect(ax, ay, self.w, self.h, 6, THEME.border)
        c.text(ax + 6, ay + 4, gui.fit_text(self.title, self.w - 54), 0xffffff if self.active else THEME.dim)
        let bx = ax + self.w - 14
        c.fill_rect(bx, ay + 3, 11, 10, 0xc0392b)
        c.line(bx + 3, ay + 5, bx + 7, ay + 9, 0xffffff)
        c.line(bx + 7, ay + 5, bx + 3, ay + 9, 0xffffff)
        c.rect(bx - 13, ay + 3, 11, 10, 0xffffff)
        c.rect(bx - 11, ay + 5, 7, 6, 0xffffff)
        c.rect(bx - 26, ay + 3, 11, 10, 0xffffff)
        c.line(bx - 23, ay + 10, bx - 18, ay + 10, 0xffffff)
        if self.resizable and self.state == "normal":
            c.line(ax + self.w - 4, ay + self.h - 9, ax + self.w - 9, ay + self.h - 4, THEME.dim)
            c.line(ax + self.w - 4, ay + self.h - 6, ax + self.w - 6, ay + self.h - 4, THEME.dim)
    override func draw(self, c):
        if not self.visible:
            return nil
        let ax = self.abs_x()
        let ay = self.abs_y()
        self.draw_self(c, ax, ay)
        c.set_clip(ax + BORDER, ay + TB, self.w - 2 * BORDER, self.h - TB - BORDER)
        self.draw_kids(c, ax, ay)
        c.clear_clip()

# the strip along the bottom: one button per window, a clock on the right
export class Taskbar extends gui.Widget:
    let desktop = nil
    let clock = ""
    override func init(self, desktop, w, h):
        super(0, 0, w, h)
        self.desktop = desktop
    func slot_w(self):
        let n = len(self.desktop.wins)
        return 0 if n == 0 else imin(110, (self.w - 70) // n)
    override func on_mouse_down(self, px, py, button):
        let sw = self.slot_w()
        let i = (px - self.abs_x() - 4) // imax(1, sw + 2)
        if sw > 0 and i >= 0 and i < len(self.desktop.wins):
            let f = self.desktop.wins[i]
            if f.state == "min" or not f.active:
                self.desktop.activate(f)
            else:
                self.desktop.minimize(f)
    override func cursor_kind(self):
        return "hand"
    override func draw_self(self, c, ax, ay):
        c.fill_rect(ax, ay, self.w, self.h, THEME.panel)
        c.fill_rect(ax, ay, self.w, 1, THEME.border)
        let sw = self.slot_w()
        let x = ax + 4
        for f in self.desktop.wins:
            let col = THEME.accent if f.active and f.state != "min" else THEME.border
            c.fill_round_rect(x, ay + 3, sw, self.h - 6, 3, col)
            c.text(x + 4, ay + 6, gui.fit_text(f.title, sw - 8), 0xffffff if f.state != "min" else THEME.dim)
            x += sw + 2
        let cw = gfx.text_advance(self.clock)
        c.text(ax + self.w - cw - 6, ay + 6, self.clock, THEME.text)

# a launcher on the desktop: double click opens it
export class DesktopIcon extends gui.Widget:
    let label = ""
    let on_open = nil
    let selected = false
    override func init(self, label, x, y, on_open = nil):
        super(x, y, 52, 46)
        self.label = label
        self.on_open = on_open
    override func cursor_kind(self):
        return "hand"
    override func on_mouse_down(self, px, py, button):
        self.selected = true
        if self.app != nil and self.app.clicks == 2 and self.on_open != nil:
            self.on_open(self)
        self.dirty()
    override func draw_self(self, c, ax, ay):
        if self.selected:
            c.set_alpha(80)
            c.fill_round_rect(ax, ay, self.w, self.h, 4, THEME.accent)
            c.set_alpha(255)
        c.fill_round_rect(ax + 14, ay + 4, 24, 22, 3, 0xf1c40f)
        c.round_rect(ax + 14, ay + 4, 24, 22, 3, THEME.border)
        let t = gui.fit_text(self.label, self.w - 2)
        c.text(ax + (self.w - gfx.text_advance(t)) // 2, ay + 31, t, 0xffffff)

# a flat gradient behind everything
export class Wallpaper extends gui.Widget:
    override func draw_self(self, c, ax, ay):
        c.gradient_rect(ax, ay, self.w, self.h, 0x1d2b53, 0x2a7f9e, 1)

# a text console: scrollback, an input line and history; runs `shell.run(line)` on Enter
export class Terminal extends gui.Widget:
    let shell = nil
    let lines = []
    let input = ""
    let history = []
    let hpos = 0
    let top_pad = 3
    let on_command = nil
    override func init(self, shell, w = 200, h = 100):
        super(0, 0, w, h)
        self.shell = shell
        self.focusable = true
        self.lines = []
        self.history = []
        self.lines.append("Sprint terminal - type help")
    override func cursor_kind(self):
        return "ibeam"
    func rows(self):
        return imax(1, (self.h - 6) // 10)
    func print_line(self, s):
        for l in s.split("\n"):
            self.lines.append(l)
        self.dirty()
    func submit(self):
        let line = self.input
        self.lines.append(self.shell.prompt() .. line)
        if line != "":
            self.history.append(line)
        self.hpos = len(self.history)
        self.input = ""
        let out = self.shell.run(line)
        if self.on_command != nil:
            self.on_command(line, out)
        if out != "":
            self.print_line(out)
        self.dirty()
    override func on_text(self, s):
        for cp in s.codepoints():
            if cp >= 32:
                self.input = self.input .. str.from_codepoint(cp)
        self.dirty()
    override func on_key(self, k):
        if k == "Enter":
            self.submit()
        elif k == "Backspace" and len(self.input) > 0:
            let n = len(self.input) - 1
            while n > 0 and not self.input.is_boundary(n):
                n -= 1
            self.input = self.input.slice(0, n)
            self.dirty()
        elif k == "Up" and self.hpos > 0:
            self.hpos -= 1
            self.input = self.history[self.hpos]
            self.dirty()
        elif k == "Down" and self.hpos < len(self.history):
            self.hpos += 1
            self.input = self.history[self.hpos] if self.hpos < len(self.history) else ""
            self.dirty()
    override func draw_self(self, c, ax, ay):
        c.fill_rect(ax, ay, self.w, self.h, 0x0b0f14)
        let all = self.lines.copy()
        all.append(self.shell.prompt() .. self.input .. "_")
        let n = self.rows()
        let first = imax(0, len(all) - n)
        for i in range(first, len(all)):
            c.text(ax + 4, ay + self.top_pad + (i - first) * 10, gui.fit_text(all[i], self.w - 8), 0x7CFC98)

# the desktop: an App that manages Frames
export class Desktop extends gui.App:
    let wins = []
    let wall = nil
    let bar = nil
    let task_h = 20
    override func init(self, width, height, title = ""):
        super(width, height, title)
        self.wins = []
        self.show_cursor = true
        self.wall = Wallpaper(0, 0, width, height - self.task_h)
        self.bar = Taskbar(self, width, self.task_h)
        self.bar.y = height - self.task_h
        self.root.add(self.wall)
        self.root.add(self.bar)
    func set_clock(self, s):
        self.bar.clock = s
        self.redraw()
    # [x, y, w, h] free for windows
    func work_area(self):
        return [0, 0, self.width, self.height - self.task_h]
    func add_icon(self, label, x, y, on_open = nil):
        let ic = DesktopIcon(label, x, y, on_open)
        self.root.add(ic)
        self.stack()
        return ic
    # keep the z-order: wallpaper, icons, frames (back to front), taskbar
    func stack(self):
        let order = [self.wall]
        for ch in self.root.children:
            if ch != self.wall and ch != self.bar and not self.wins.contains(ch):
                order.append(ch)
        for f in self.wins:
            order.append(f)
        order.append(self.bar)
        self.root.children = order
        self.redraw()
    func open_window(self, title, x, y, w, h, content = nil):
        let f = Frame(title, x, y, w, h, content)
        f.desktop = self
        self.root.add(f)
        self.wins.append(f)
        self.activate(f)
        return f
    func activate(self, f):
        let rest = self.wins.filter(g => g != f)
        rest.append(f)
        self.wins = rest
        for g in self.wins:
            g.active = g == f
        f.state = "max" if f.state == "max" else "normal"
        f.visible = true
        self.stack()
        self.relayout()
    func top(self):
        for i in range(len(self.wins) - 1, -1, -1):
            if self.wins[i].visible:
                return self.wins[i]
        return nil
    func minimize(self, f):
        f.visible = false
        f.state = "min" if f.state != "max" else "min"
        f.active = false
        let t = self.top()
        if t != nil:
            self.activate(t)
        self.redraw()
    func close_frame(self, f):
        if f.on_close != nil and f.on_close(f) == false:
            return nil
        self.wins = self.wins.filter(g => g != f)
        self.root.children = self.root.children.filter(g => g != f)
        let t = self.top()
        if t != nil:
            self.activate(t)
        self.relayout()
        self.redraw()
    func toggle_max(self, f):
        let a = self.work_area()
        if f.state == "max":
            f.x = f.saved[0]
            f.y = f.saved[1]
            f.w = f.saved[2]
            f.h = f.saved[3]
            f.state = "normal"
        else:
            f.saved = [f.x, f.y, f.w, f.h]
            f.x = a[0]
            f.y = a[1]
            f.w = a[2]
            f.h = a[3]
            f.state = "max"
        f.pw = f.w
        f.ph = f.h
        self.relayout()
        self.redraw()
    # the Frame that contains a widget, if any
    func frame_of(self, w):
        while w != nil and not self.wins.contains(w):
            w = w.parent
        return w
    override func dispatch(self, ev):
        if ev.type == "mouse_down" and self.modal == nil and self.ctx == nil and self.popup == nil:
            let hit = self.pick_at(ev.x, ev.y)
            let f = self.frame_of(hit)
            if f != nil and not f.active:
                self.activate(f)
            elif f == nil and hit != nil and hit != self.bar:
                for g in self.wins:
                    g.active = false
                self.redraw()
        super.dispatch(ev)
