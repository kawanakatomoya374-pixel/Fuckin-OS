# gui: retained-mode widget toolkit on top of gfx.
#
# There is no native window. A program feeds the App a stream of events (from a
# script, from a host program through the embedding API, or from a recorded
# trace), the App updates widget state and calls the widgets' callbacks, and
# `draw()` renders the result to a gfx canvas that can be saved as PNG/PPM/BMP.
# Everything is integer arithmetic on top of gfx, so a given event stream always
# produces the same pixels.
#
#   import gui
#   let app = gui.App(240, 120, "demo")
#   let b = gui.Button("Hi", 10, 10, 60, 22)
#   b.on_click = func(w): print("clicked")
#   app.add(b)
#   app.click(20, 20)              # synthesises move/down/up
#   app.draw().save_png("demo.png")
import gfx
import json
import str

export const DARK = {
    bg: 0x23262e, panel: 0x2c303a, border: 0x555b6b, text: 0xe6e8ee, dim: 0x8a90a0,
    accent: 0x4c8dff, accent_hi: 0x6aa0ff, accent_dark: 0x2f63c0, hover: 0x3a4050, press: 0x1d2028,
    field: 0x181a20, ok: 0x4cc38a, shadow: 0x000000, radius: 4
}
export const LIGHT = {
    bg: 0xeef0f4, panel: 0xffffff, border: 0xb4bac8, text: 0x1d2330, dim: 0x7a8296,
    accent: 0x3a78e8, accent_hi: 0x5a92f0, accent_dark: 0x2858b0, hover: 0xdde3ee, press: 0xc8d0e0,
    field: 0xf8f9fc, ok: 0x2fa36c, shadow: 0x000000, radius: 4
}
# THEME is the live palette: widgets read it while drawing, set_theme copies a palette into it.
export const THEME = {
    bg: 0, panel: 0, border: 0, text: 0, dim: 0, accent: 0, accent_hi: 0, accent_dark: 0, hover: 0,
    press: 0, field: 0, ok: 0, shadow: 0, radius: 4
}
export func set_theme(p):
    let src = p
    if p == "dark":
        src = DARK
    elif p == "light":
        src = LIGHT
    for k, v in src:
        THEME[k] = v
set_theme(DARK)

# ---- events ------------------------------------------------------------------
export func mouse_move(x, y):
    return {type: "mouse_move", x: x, y: y}
# mods: "" or a "+"-joined subset of Ctrl, Alt, Shift held during the press; clicks: 1, 2, 3 for
# single / double / triple click (the host counts them, so replayed traces stay deterministic).
export func mouse_down(x, y, button = 1, mods = "", clicks = 1):
    return {type: "mouse_down", x: x, y: y, button: button, mods: mods, clicks: clicks}
export func mouse_up(x, y, button = 1, mods = ""):
    return {type: "mouse_up", x: x, y: y, button: button, mods: mods}
export func key(name):
    return {type: "key", key: name}
export func text(s):
    return {type: "text", text: s}
export func tick(ms):
    return {type: "tick", ms: ms}
export func wheel(x, y, dy):
    return {type: "wheel", x: x, y: y, dy: dy}
export func quit_event():
    return {type: "quit"}

func clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v

# Text metrics: the 5x7 Latin font advances 6 pixels per character, a Japanese glyph (12x12) 12.
func adv(s, scale = 1):
    return gfx.text_advance(s, scale)

# the longest prefix of s (whole characters) that is at most px pixels wide
func fit(s, px):
    if gfx.text_advance(s) <= px:
        return s
    if s.char_count() == len(s):
        return s.slice(0, imax(0, px // 6))
    let out = ""
    let used = 0
    for cp in s.codepoints():
        let ch = str.from_codepoint(cp)
        let w = gfx.text_advance(ch)
        if used + w > px:
            break
        out = out .. ch
        used += w
    return out

# longest prefix of s that is at most px pixels wide
export func fit_text(s, px):
    return fit(s, px)

# clip to n cells of 6 pixels
func clip(s, n):
    return fit(s, n * 6)

# character boundaries inside a string (byte offsets)
func snap_down(s, i):
    let p = clamp(i, 0, len(s))
    while p > 0 and not s.is_boundary(p):
        p -= 1
    return p

func snap_up(s, i):
    let p = clamp(i, 0, len(s))
    while p < len(s) and not s.is_boundary(p):
        p += 1
    return p

func prev_b(s, i):
    return snap_down(s, i - 1)

func next_b(s, i):
    return snap_up(s, i + 1)

# byte offset of the character boundary nearest to pixel x (0 = left edge of s)
func col_at(s, px):
    if s.char_count() == len(s):
        return clamp((px + 3) // 6, 0, len(s))
    let x = 0
    let i = 0
    for cp in s.codepoints():
        let ch = str.from_codepoint(cp)
        let w = gfx.text_advance(ch)
        if px < x + w // 2 + 1:
            return i
        x += w
        i += len(ch)
    return len(s)

# ---- shapes -------------------------------------------------------------------------
# point (x, y) relative to a w x h box against a shape descriptor (integer maths only)
func shape_hit(sh, x, y, w, h):
    let k = sh[0]
    if k == "ellipse":
        # ((2x+1-w)/w)^2 + ((2y+1-h)/h)^2 <= 1  scaled to integers
        let dx = 2 * x + 1 - w
        let dy = 2 * y + 1 - h
        return dx * dx * h * h + dy * dy * w * w <= w * w * h * h
    if k == "round":
        let r = imin(sh[1], imin(w, h) // 2)
        let cx = x
        let cy = y
        if x < r:
            cx = r - 1 - x
        elif x >= w - r:
            cx = x - (w - r)
        else:
            return true
        if y < r:
            cy = r - 1 - y
        elif y >= h - r:
            cy = y - (h - r)
        else:
            return true
        # a corner cell: inside the quarter circle of radius r
        return (2 * cx + 1) * (2 * cx + 1) + (2 * cy + 1) * (2 * cy + 1) <= 4 * r * r
    if k == "poly":
        return point_in_poly(sh[1], 2 * x + 1, 2 * y + 1, 2)
    if k == "mask":
        let cv = sh[1]
        if x >= cv.width() or y >= cv.height():
            return false
        return cv.get(x, y) != sh[2]
    return true

# even-odd rule; pts is [[x, y], ...] in pixels, the point is given in `scale`ths of a pixel
func point_in_poly(pts, px, py, scale):
    let inside = false
    let n = len(pts)
    let j = n - 1
    for i in range(n):
        let xi = pts[i][0] * scale
        let yi = pts[i][1] * scale
        let xj = pts[j][0] * scale
        let yj = pts[j][1] * scale
        if (yi > py) != (yj > py):
            # px < xi + (py - yi) * (xj - xi) / (yj - yi)
            let lhs = (px - xi) * (yj - yi)
            let rhs = (py - yi) * (xj - xi)
            if (yj - yi > 0 and lhs < rhs) or (yj - yi < 0 and lhs > rhs):
                inside = not inside
        j = i
    return inside

export func in_polygon(pts, x, y):
    return point_in_poly(pts, 2 * x + 1, 2 * y + 1, 2)

func imax(a, b):
    return a if a > b else b

func imin(a, b):
    return a if a < b else b

const TIP_DELAY = 500
let STAMP = 0   # changes whenever a frame starts or ends, so cached absolute positions never outlive the draw

func call0(f, w):
    if f != nil:
        f(w)

# ---- widget base ---------------------------------------------------------------
export class Widget:
    let x = 0
    let y = 0
    let w = 0
    let h = 0
    let visible = true
    let enabled = true
    let focusable = false
    let hover = false
    let pressed = false
    let parent = nil
    let children = []
    let app = nil
    let name = ""
    let pw = 0
    let ph = 0
    let cax = 0       # absolute position cached by the parent while a frame is drawn (valid when cs == STAMP)
    let cay = 0
    let cs = -1
    let flex = 0
    let is_box = false
    let tip = ""
    let context = nil
    let bar_grab = -1
    let drag_data = nil
    let drag_label = ""
    let on_drop = nil
    let drop_ok = nil
    let shape = nil
    let pointer = nil
    let through = false

    func init(self, x, y, w, h):
        self.x = x
        self.y = y
        self.w = w
        self.h = h
        self.pw = w
        self.ph = h

    # preferred size [w, h]; layout containers use it for children that are not flexible
    func pref(self):
        return [self.pw, self.ph]

    # place the children (containers override); plain widgets only recurse
    func layout(self):
        for ch in self.children:
            ch.layout()

    func set_size(self, w, h):
        self.w = w
        self.h = h
        self.pw = w
        self.ph = h
        self.relayout()
        return self

    func set_flex(self, f):
        self.flex = f
        return self

    func on_wheel(self, dy):
        return false

    func add(self, child):
        child.parent = self
        self.children.append(child)
        self.attach(child, self.app)
        self.relayout()
        return child

    func attach(self, child, app):
        child.app = app
        for c in child.children:
            child.attach(c, app)

    func abs_x(self):
        if self.cs == STAMP:
            return self.cax
        if self.parent == nil:
            return self.x
        return self.parent.abs_x() + self.x

    func abs_y(self):
        if self.cs == STAMP:
            return self.cay
        if self.parent == nil:
            return self.y
        return self.parent.abs_y() + self.y

    func contains(self, px, py):
        let ax = self.abs_x()
        let ay = self.abs_y()
        if px < ax or py < ay or px >= ax + self.w or py >= ay + self.h:
            return false
        return self.shape == nil or shape_hit(self.shape, px - ax, py - ay, self.w, self.h)

    # ---- hit-test shape: which pixels of the rectangle count as "inside" (pointer picking only) ----
    func set_shape(self, shape):
        self.shape = shape
        return self
    func shape_ellipse(self):
        return self.set_shape(["ellipse"])
    func shape_round(self, r):
        return self.set_shape(["round", r])
    func shape_polygon(self, pts):
        return self.set_shape(["poly", pts])
    # canvas pixels equal to `key` (default: not transparent-key) are outside; pass the canvas to test
    func shape_mask(self, canvas, key):
        return self.set_shape(["mask", canvas, key])
    # a click-through widget is never picked itself; its children still are
    func set_through(self, v):
        self.through = v
        return self
    # mouse pointer shape name (field `pointer`) while the pointer is over this widget ("arrow", "ibeam", "hand", ...)
    func cursor_kind(self):
        return "arrow" if self.pointer == nil else self.pointer
    func hit_test(self, px, py):
        return self.contains(px, py)

    # topmost visible descendant (or self) under the point; nil if none
    func pick(self, px, py):
        if not self.visible or not self.contains(px, py):
            return nil
        let i = len(self.children) - 1
        while i >= 0:
            let hit = self.children[i].pick(px, py)
            if hit != nil:
                return hit
            i -= 1
        return nil if self.through else self

    func find(self, name):
        if self.name == name:
            return self
        for c in self.children:
            let r = c.find(name)
            if r != nil:
                return r
        return nil

    # this widget (and its shadow / focus ring) must be redrawn
    func dirty(self):
        let a = self.app
        if a != nil:
            if a.full:
                return nil
            let ax = self.abs_x()
            let ay = self.abs_y()
            a.add_damage(ax - 12, ay - 12, ax + self.w + 12, ay + self.h + 12)

    func relayout(self):
        if self.app != nil:
            self.app.needs_layout = true
            self.app.redraw()

    func set_enabled(self, v):
        self.enabled = v
        self.dirty()

    func set_visible(self, v):
        self.visible = v
        self.dirty()

    # overridable hooks; return values are ignored
    func on_mouse_down(self, px, py, button):
        pass
    func on_key(self, k):
        pass
    func on_text(self, s):
        pass
    func on_focus(self, v):
        pass
    func on_tick(self, ms):
        pass
    func on_drag(self, px, py):
        pass

    func draw_self(self, c, ax, ay):
        pass

    # ---- vertical scrollbar helpers: the bar is a 4px strip on the right edge of the track [y0, y0 + h) ----
    func bar_thumb(self, total, vis, top, h):
        let th = imax(10, h * vis // total)
        return [(h - th) * top // (total - vis), th]

    func draw_bar(self, c, ax, y0, h, total, vis, top):
        if total <= vis:
            return nil
        let t = self.bar_thumb(total, vis, top, h)
        c.set_alpha(70)
        c.fill_round_rect(ax + self.w - 6, y0, 4, h, 2, THEME.border)
        c.set_alpha(255)
        c.fill_round_rect(ax + self.w - 6, y0 + t[0], 4, t[1], 2, THEME.accent if self.bar_grab >= 0 else THEME.dim)

    func bar_top(self, py_in_track, grab, total, vis, h):
        let th = imax(10, h * vis // total)
        let span = h - th
        if span <= 0:
            return 0
        return clamp(((py_in_track - grab) * (total - vis) + span // 2) // span, 0, total - vis)

    # a press on the bar strip: returns the new top (and starts a drag), or -1 if the press missed the bar
    func bar_press(self, px, py, total, vis, top, y0, h):
        if total <= vis or px < self.abs_x() + self.w - 9 or py < y0 or py >= y0 + h:
            return -1
        let t = self.bar_thumb(total, vis, top, h)
        let rel = py - y0
        if rel >= t[0] and rel < t[0] + t[1]:
            self.bar_grab = rel - t[0]
        else:
            self.bar_grab = t[1] // 2
        return self.bar_top(rel, self.bar_grab, total, vis, h)

    func bar_drag(self, py, total, vis, y0, h):
        return self.bar_top(py - y0, self.bar_grab, total, vis, h)

    func on_mouse_up(self, px, py, button, inside):
        self.bar_grab = -1

    func draw(self, c):
        if not self.visible:
            return nil
        let ax = self.abs_x()
        let ay = self.abs_y()
        let k = self.app.dclip if self.app != nil else nil
        if k != nil and (ax + self.w + 12 <= k[0] or ay + self.h + 12 <= k[1] or ax - 12 >= k[2] or ay - 12 >= k[3]):
            return nil
        self.draw_self(c, ax, ay)
        for ch in self.children:
            ch.cax = ax + ch.x
            ch.cay = ay + ch.y
            ch.cs = STAMP
            ch.draw(c)
        if self.app != nil and self.app.focus == self and self.focusable:
            c.rect(ax - 1, ay - 1, self.w + 2, self.h + 2, THEME.accent)

    # draw the children of a widget whose absolute position is (ax, ay)
    func draw_kids(self, c, ax, ay):
        for ch in self.children:
            ch.cax = ax + ch.x
            ch.cay = ay + ch.y
            ch.cs = STAMP
            ch.draw(c)

    func tick_all(self, ms):
        self.on_tick(ms)
        for c in self.children:
            c.tick_all(ms)

    func focus_chain(self, out):
        if self.visible and self.enabled:
            if self.focusable:
                out.append(self)
            for c in self.children:
                c.focus_chain(out)
