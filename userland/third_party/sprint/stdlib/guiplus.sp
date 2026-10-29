# guiplus: richer widgets on top of gui - charts, a paint surface, a colour picker, a calendar,
# vector icons, toolbar and status bar. Like gui it is headless and deterministic: integer maths
# only (a built-in sine table instead of libm), so a given event stream gives identical pixels.
#
#   import gui
#   import guiplus as gp
#   let ch = gp.chart("line", [{name: "load", values: [3, 5, 4, 8]}], ["a", "b", "c", "d"], 240, 140)
import gfx
import gui
import math

const THEME = gui.THEME

func clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v

func imax(a, b):
    return a if a > b else b

func imin(a, b):
    return a if a < b else b

func call0(f, w):
    if f != nil:
        f(w)

# ---- shared drawing helpers -------------------------------------------------------------
export const PALETTE = [0x4c8dff, 0x4cc38a, 0xf2a93b, 0xe0585b, 0xa77bf0, 0x38c6d9, 0xe86fb0, 0x9aa3b5]

const SIN = [0, 17, 35, 52, 70, 87, 105, 122, 139, 156, 174, 191, 208, 225, 242, 259, 276, 292, 309, 326, 342, 358, 375, 391, 407, 423, 438, 454, 469, 485, 500, 515, 530, 545, 559, 574, 588, 602, 616, 629, 643, 656, 669, 682, 695, 707, 719, 731, 743, 755, 766, 777, 788, 799, 809, 819, 829, 839, 848, 857, 866, 875, 883, 891, 899, 906, 914, 921, 927, 934, 940, 946, 951, 956, 961, 966, 970, 974, 978, 982, 985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, 1000]

# sine / cosine of whole degrees, scaled by 1000 (integer table, no libm)
export func isin(deg):
    let d = deg % 360
    if d <= 90:
        return SIN[d]
    if d <= 180:
        return SIN[180 - d]
    if d <= 270:
        return -SIN[d - 180]
    return -SIN[360 - d]

export func icos(deg):
    return isin(deg + 90)

# "#rrggbb" for a 0xRRGGBB integer
export func hex_color(rgb):
    const digits = "0123456789abcdef"
    let s = "#"
    for sh in [20, 16, 12, 8, 4, 0]:
        let n = (rgb >> sh) & 15
        s = s .. digits.slice(n, n + 1)
    return s

# a "nice" tick step (1, 2 or 5 times a power of ten) giving at most `target` intervals over `span`
func nice_step(span, target):
    let raw = span / target
    let mag = 1.0
    while mag * 10 <= raw:
        mag = mag * 10
    while mag > raw:
        mag = mag / 10
    for m in [1, 2, 5, 10]:
        if m * mag >= raw:
            return m * mag
    return 10 * mag

func fmt_num(v):
    let r = math.round(v * 1000)
    if r % 1000 == 0:
        return to_string(r // 1000)
    return to_string(r / 1000)

# ---- icons -------------------------------------------------------------------------------
# Vector icons drawn on a 12-unit grid scaled to `size` pixels. icon(canvas, name, x, y, size, colour)
export const ICONS = ["close", "check", "plus", "minus", "left", "right", "up", "down", "menu", "search", "folder", "file", "gear", "play", "pause", "stop", "trash", "star", "home"]

export func icon(c, name, x, y, size, col):
    let s = size
    # u(n): n grid units in pixels
    func u(n):
        return n * s // 12
    if name == "close":
        c.line_aa(x + u(2), y + u(2), x + u(10), y + u(10), col)
        c.line_aa(x + u(10), y + u(2), x + u(2), y + u(10), col)
    elif name == "check":
        c.line_aa(x + u(1), y + u(6), x + u(4), y + u(9), col)
        c.line_aa(x + u(4), y + u(9), x + u(11), y + u(2), col)
    elif name == "plus":
        c.fill_rect(x + u(5), y + u(1), imax(1, u(2)), u(10), col)
        c.fill_rect(x + u(1), y + u(5), u(10), imax(1, u(2)), col)
    elif name == "minus":
        c.fill_rect(x + u(1), y + u(5), u(10), imax(1, u(2)), col)
    elif name == "left":
        c.line_aa(x + u(8), y + u(2), x + u(4), y + u(6), col)
        c.line_aa(x + u(4), y + u(6), x + u(8), y + u(10), col)
    elif name == "right":
        c.line_aa(x + u(4), y + u(2), x + u(8), y + u(6), col)
        c.line_aa(x + u(8), y + u(6), x + u(4), y + u(10), col)
    elif name == "up":
        c.line_aa(x + u(2), y + u(8), x + u(6), y + u(4), col)
        c.line_aa(x + u(6), y + u(4), x + u(10), y + u(8), col)
    elif name == "down":
        c.line_aa(x + u(2), y + u(4), x + u(6), y + u(8), col)
        c.line_aa(x + u(6), y + u(8), x + u(10), y + u(4), col)
    elif name == "menu":
        for r in [2, 5, 8]:
            c.fill_rect(x + u(1), y + u(r), u(10), imax(1, u(2)), col)
    elif name == "search":
        c.circle(x + u(5), y + u(5), imax(2, u(3)), col)
        c.line_aa(x + u(7), y + u(7), x + u(11), y + u(11), col)
    elif name == "folder":
        c.rect(x + u(1), y + u(3), u(10), u(7), col)
        c.fill_rect(x + u(1), y + u(2), u(4), u(2), col)
    elif name == "file":
        c.rect(x + u(2), y + u(1), u(8), u(10), col)
        c.line(x + u(4), y + u(4), x + u(8), y + u(4), col)
        c.line(x + u(4), y + u(7), x + u(8), y + u(7), col)
    elif name == "gear":
        c.circle(x + u(6), y + u(6), imax(2, u(3)), col)
        c.fill_rect(x + u(5), y + u(0), imax(1, u(2)), u(2), col)
        c.fill_rect(x + u(5), y + u(10), imax(1, u(2)), u(2), col)
        c.fill_rect(x + u(0), y + u(5), u(2), imax(1, u(2)), col)
        c.fill_rect(x + u(10), y + u(5), u(2), imax(1, u(2)), col)
        c.fill_circle(x + u(6), y + u(6), imax(1, u(1)), col)
    elif name == "play":
        c.fill_triangle(x + u(3), y + u(1), x + u(3), y + u(11), x + u(10), y + u(6), col)
    elif name == "pause":
        c.fill_rect(x + u(2), y + u(1), imax(2, u(3)), u(10), col)
        c.fill_rect(x + u(7), y + u(1), imax(2, u(3)), u(10), col)
    elif name == "stop":
        c.fill_rect(x + u(2), y + u(2), u(8), u(8), col)
    elif name == "trash":
        c.fill_rect(x + u(1), y + u(2), u(10), imax(1, u(1)), col)
        c.fill_rect(x + u(4), y + u(0), u(4), imax(1, u(1)), col)
        c.rect(x + u(2), y + u(3), u(8), u(8), col)
        c.line(x + u(5), y + u(5), x + u(5), y + u(9), col)
        c.line(x + u(7), y + u(5), x + u(7), y + u(9), col)
    elif name == "star":
        let cx = x + u(6)
        let cy = y + u(6)
        let r = u(6)
        let pts = []
        for k in range(10):
            let rr = r if k % 2 == 0 else r * 2 // 5
            let ang = k * 36
            pts.append([cx + rr * isin(ang) // 1000, cy - rr * icos(ang) // 1000])
        for k in range(10):
            let p = pts[k]
            let q = pts[(k + 1) % 10]
            c.fill_triangle(cx, cy, p[0], p[1], q[0], q[1], col)
    elif name == "home":
        c.fill_triangle(x + u(6), y + u(1), x + u(0), y + u(6), x + u(12), y + u(6), col)
        c.rect(x + u(2), y + u(6), u(8), u(5), col)
        c.fill_rect(x + u(5), y + u(8), u(2), u(3), col)
    else:
        throw error("ValueError", "unknown icon: " .. to_string(name))

# A flat button showing an icon (on_click(widget)); `tip` becomes its tooltip.
export class IconButton extends gui.Widget:
    let glyph = ""
    let on_click = nil
    let toggled = false
    let toggle_mode = false
    override func init(self, glyph, on_click = nil, tip = ""):
        super(0, 0, 22, 22)
        self.glyph = glyph
        self.on_click = on_click
        self.tip = tip
        self.focusable = true
    func fire(self):
        if self.toggle_mode:
            self.toggled = not self.toggled
        self.dirty()
        call0(self.on_click, self)
    override func on_mouse_down(self, px, py, button):
        self.pressed = true
        self.dirty()
    override func on_mouse_up(self, px, py, button, inside):
        self.pressed = false
        self.dirty()
        if inside and self.enabled:
            self.fire()
    override func on_key(self, k):
        if k == "Enter" or k == "Space":
            self.fire()
    override func draw_self(self, c, ax, ay):
        if self.pressed or self.toggled:
            c.fill_round_rect(ax, ay, self.w, self.h, 3, THEME.accent_dark)
        elif self.hover:
            c.fill_round_rect(ax, ay, self.w, self.h, 3, THEME.hover)
        icon(c, self.glyph, ax + (self.w - 12) // 2, ay + (self.h - 12) // 2, 12, THEME.text if self.enabled else THEME.dim)

# A row of icon buttons; an entry is [icon, on_click, tip] or "-" for a separator.
export func toolbar(items):
    let ws = []
    for it in items:
        if it == "-":
            ws.append(gui.separator(true))
        else:
            ws.append(IconButton(it[0], it[1] if len(it) > 1 else nil, it[2] if len(it) > 2 else ""))
    let r = gui.row(ws, 2, 3)
    r.align = "center"
    r.framed = true
    return r

# A strip with text segments: set_text(i, s). The last segment is right-aligned.
export class StatusBar extends gui.Widget:
    let parts = []
    override func init(self, parts):
        super(0, 0, 0, 16)
        self.parts = parts
    func set_text(self, i, s):
        self.parts[i] = s
        self.dirty()
    override func pref(self):
        return [self.w, 16]
    override func draw_self(self, c, ax, ay):
        c.fill_rect(ax, ay, self.w, self.h, THEME.panel)
        c.fill_rect(ax, ay, self.w, 1, THEME.border)
        let x = ax + 6
        for i in range(len(self.parts)):
            let s = to_string(self.parts[i])
            if i == len(self.parts) - 1 and i > 0:
                c.text(ax + self.w - 6 - gfx.text_advance(s), ay + 4, s, THEME.dim)
            else:
                c.text(x, ay + 4, s, THEME.text)
                x += gfx.text_advance(s) + 16
                c.fill_rect(x - 9, ay + 3, 1, 10, THEME.border)

# ---- chart -------------------------------------------------------------------------------
# kind: "line" | "area" | "bar" | "scatter" | "pie" | "donut".
# series: [{name, values, color}] (values are numbers; for "scatter" they are [x, y] pairs; for pie/donut
# the first series' values are the slices and `labels` name them). A plain array of numbers also works.
export class Chart extends gui.Widget:
    let kind = "line"
    let series = []
    let labels = []
    let title = ""
    let legend = true
    let grid = true
    let show_values = false
    let y_min = nil
    let y_max = nil
    let selected = nil
    let on_pick = nil
    override func init(self, kind, series, labels = [], w = 260, h = 160):
        super(0, 0, w, h)
        self.kind = kind
        self.labels = labels
        self.set_series(series)
        self.focusable = false
    func set_series(self, series):
        let out = []
        for i in range(len(series)):
            let sr = series[i]
            if type(sr) == "array":
                sr = {name: "s" .. to_string(i + 1), values: sr}
            out.append({name: sr.name ?? ("s" .. to_string(i + 1)), values: sr.values, color: sr.color ?? PALETTE[i % len(PALETTE)]})
        self.series = out
        self.selected = nil
        self.dirty()
    func is_round(self):
        return self.kind == "pie" or self.kind == "donut"
    func color_of(self, si, pi):
        if self.is_round():
            return PALETTE[pi % len(PALETTE)]
        return self.series[si].color
    func count(self):
        let n = 0
        for sr in self.series:
            n = imax(n, len(sr.values))
        return n
    # the legend entries: [name, colour]
    func legend_items(self):
        let out = []
        if self.is_round():
            if len(self.series) > 0:
                for i in range(len(self.series[0].values)):
                    out.append([self.labels[i] if i < len(self.labels) else "#" .. to_string(i + 1), PALETTE[i % len(PALETTE)]])
        else:
            for sr in self.series:
                out.append([sr.name, sr.color])
        return out
    func show_legend(self):
        return self.legend and (len(self.legend_items()) > 1 or self.is_round())
    # data range of the y axis: [lo, hi]
    func y_range(self):
        let lo = nil
        let hi = nil
        for sr in self.series:
            for v in sr.values:
                let y = v[1] if self.kind == "scatter" else v
                if lo == nil or y < lo:
                    lo = y
                if hi == nil or y > hi:
                    hi = y
        if lo == nil:
            return [0, 1]
        if self.kind == "bar" or self.kind == "area":
            lo = imin(lo, 0)
            hi = imax(hi, 0)
        if self.y_min != nil:
            lo = self.y_min
        if self.y_max != nil:
            hi = self.y_max
        if hi <= lo:
            hi = lo + 1
        let step = nice_step(hi - lo, 4)
        if self.y_min == nil:
            lo = math.floor(lo / step) * step
        if self.y_max == nil:
            hi = math.ceil(hi / step) * step
        return [lo, hi]
    # the y axis tick values
    func ticks(self):
        let yr = self.y_range()
        let step = nice_step(yr[1] - yr[0], 4)
        let out = []
        for k in range(math.ceil(yr[0] / step - 0.000001), math.floor(yr[1] / step + 0.000001) + 1):
            out.append(k * step)
        return out
    func x_range(self):
        let lo = nil
        let hi = nil
        for sr in self.series:
            for v in sr.values:
                if lo == nil or v[0] < lo:
                    lo = v[0]
                if hi == nil or v[0] > hi:
                    hi = v[0]
        if lo == nil:
            return [0, 1]
        if hi <= lo:
            hi = lo + 1
        return [lo, hi]
    # [x, y, w, h] of the plotting area, relative to the widget
    func plot_rect(self):
        let top = 4
        if self.title != "":
            top += 12
        if self.show_legend():
            top += 12
        if self.is_round():
            return [4, top, self.w - 8, self.h - top - 4]
        let lw = 8
        for v in self.ticks():
            lw = imax(lw, gfx.text_advance(fmt_num(v)) + 8)
        return [lw, top, self.w - lw - 6, self.h - top - 19]
    func px_y(self, v, yr, r):
        return r[1] + r[3] - math.floor((v - yr[0]) * r[3] / (yr[1] - yr[0]) + 0.5)
    # pixel position (relative) of a point
    func point_pos(self, si, pi):
        let r = self.plot_rect()
        let yr = self.y_range()
        let v = self.series[si].values[pi]
        if self.kind == "scatter":
            let xr = self.x_range()
            return [r[0] + math.floor((v[0] - xr[0]) * (r[2] - 8) / (xr[1] - xr[0]) + 0.5) + 4, self.px_y(v[1], yr, r)]
        let n = self.count()
        if self.kind == "bar":
            let slot = r[2] // imax(1, n)
            let bw = imax(2, (slot - 4) // len(self.series))
            return [r[0] + slot * pi + 2 + bw * si + bw // 2, self.px_y(v, yr, r)]
        return [r[0] + (r[2] * pi // imax(1, n - 1) if n > 1 else r[2] // 2), self.px_y(v, yr, r)]
    func slice_degrees(self):
        let vals = self.series[0].values if len(self.series) > 0 else []
        let total = 0
        for v in vals:
            total += imax(0, v)
        let out = []
        let cum = 0
        for v in vals:
            let a = cum * 360 // total if total > 0 else 0
            cum += imax(0, v)
            out.append([a, cum * 360 // total if total > 0 else 0])
        return out
    func pie_geom(self):
        let r = self.plot_rect()
        let rad = imax(4, imin(r[2], r[3]) // 2 - 6)
        return [r[0] + r[2] // 2, r[1] + r[3] // 2, rad]
    # [series, point] under a position relative to the widget, or nil
    func hit_at(self, lx, ly):
        if len(self.series) == 0:
            return nil
        if self.is_round():
            let g = self.pie_geom()
            let dx = lx - g[0]
            let dy = ly - g[1]
            if dx * dx + dy * dy > g[2] * g[2]:
                return nil
            let best = 0
            let bd = -1000000000
            for d in range(360):
                let dot = isin(d) * dx - icos(d) * dy
                if dot > bd:
                    bd = dot
                    best = d
            let sl = self.slice_degrees()
            for i in range(len(sl)):
                if best >= sl[i][0] and best < sl[i][1]:
                    return [0, i]
            return nil
        let r = self.plot_rect()
        if lx < r[0] - 4 or lx > r[0] + r[2] + 4 or ly < r[1] - 4 or ly > r[1] + r[3] + 4:
            return nil
        if self.kind == "bar":
            let n = self.count()
            let slot = r[2] // imax(1, n)
            let pi = (lx - r[0]) // imax(1, slot)
            if pi < 0 or pi >= n:
                return nil
            let bw = imax(2, (slot - 4) // len(self.series))
            let si = clamp((lx - r[0] - slot * pi - 2) // bw, 0, len(self.series) - 1)
            if pi < len(self.series[si].values):
                return [si, pi]
            return nil
        let best = nil
        let bd = 100
        for si in range(len(self.series)):
            for pi in range(len(self.series[si].values)):
                let p = self.point_pos(si, pi)
                let d = (p[0] - lx) * (p[0] - lx) + (p[1] - ly) * (p[1] - ly)
                if d < bd:
                    bd = d
                    best = [si, pi]
        return best
    func describe(self, hit):
        let sr = self.series[hit[0]]
        if self.is_round():
            let total = 0
            for v in sr.values:
                total += v
            let name = self.labels[hit[1]] if hit[1] < len(self.labels) else "#" .. to_string(hit[1] + 1)
            return name .. ": " .. fmt_num(sr.values[hit[1]]) .. " (" .. to_string(sr.values[hit[1]] * 100 // imax(1, total)) .. "%)"
        let v = sr.values[hit[1]]
        let cat = ""
        if self.kind == "scatter":
            return sr.name .. ": (" .. fmt_num(v[0]) .. ", " .. fmt_num(v[1]) .. ")"
        if hit[1] < len(self.labels):
            cat = self.labels[hit[1]] .. " "
        return sr.name .. " " .. cat .. fmt_num(v)
    override func on_mouse_down(self, px, py, button):
        let h = self.hit_at(px - self.abs_x(), py - self.abs_y())
        self.selected = h
        self.dirty()
        if h != nil:
            call0(self.on_pick, self)
    func draw_axes(self, c, ax, ay):
        let r = self.plot_rect()
        let yr = self.y_range()
        for v in self.ticks():
            let y = ay + self.px_y(v, yr, r)
            let lab = fmt_num(v)
            if self.grid:
                c.set_alpha(70)
                c.fill_rect(ax + r[0], y, r[2], 1, THEME.border)
                c.set_alpha(255)
            c.text(ax + r[0] - 4 - gfx.text_advance(lab), y - 3, lab, THEME.dim)
        c.fill_rect(ax + r[0], ay + r[1], 1, r[3] + 1, THEME.border)
        let zero_y = ay + self.px_y(imax(0, yr[0]) if yr[0] < 0 else yr[0], yr, r)
        c.fill_rect(ax + r[0], ay + self.px_y(0, yr, r) if yr[0] <= 0 and yr[1] >= 0 else zero_y, r[2], 1, THEME.border)
        if self.kind != "scatter":
            let n = self.count()
            let every = imax(1, (n * 6 * 4) // imax(1, r[2]) + 1)
            for i in range(n):
                if i % every == 0 and i < len(self.labels):
                    let p = self.point_pos(0, i) if self.kind != "bar" else [r[0] + (r[2] // n) * i + (r[2] // n) // 2, 0]
                    let lab = to_string(self.labels[i])
                    c.text(ax + p[0] - len(lab) * 3, ay + r[1] + r[3] + 6, lab, THEME.dim)
    func draw_series(self, c, ax, ay):
        let r = self.plot_rect()
        let yr = self.y_range()
        let base = self.px_y(imax(0, yr[0]) if yr[0] < 0 else yr[0], yr, r)
        if yr[0] <= 0 and yr[1] >= 0:
            base = self.px_y(0, yr, r)
        for si in range(len(self.series)):
            let sr = self.series[si]
            let col = sr.color
            let n = len(sr.values)
            if self.kind == "bar":
                let cnt = self.count()
                let slot = r[2] // imax(1, cnt)
                let bw = imax(2, (slot - 4) // len(self.series))
                for pi in range(n):
                    let p = self.point_pos(si, pi)
                    let top = imin(p[1], base)
                    let hgt = imax(1, (p[1] - base if p[1] > base else base - p[1]))
                    c.fill_rect(ax + p[0] - bw // 2, ay + top, bw - 1, hgt, col)
                    if self.selected != nil and self.selected[0] == si and self.selected[1] == pi:
                        c.rect(ax + p[0] - bw // 2 - 1, ay + top - 1, bw + 1, hgt + 2, THEME.text)
                    if self.show_values:
                        let lab = fmt_num(sr.values[pi])
                        c.text(ax + p[0] - len(lab) * 3, ay + top - 9, lab, THEME.text)
            elif self.kind == "scatter":
                for pi in range(n):
                    let p = self.point_pos(si, pi)
                    c.fill_circle(ax + p[0], ay + p[1], 3, col)
                    if self.selected != nil and self.selected[0] == si and self.selected[1] == pi:
                        c.circle(ax + p[0], ay + p[1], 5, THEME.text)
            else:
                if self.kind == "area" and n > 1:
                    c.set_alpha(70)
                    for pi in range(n - 1):
                        let p = self.point_pos(si, pi)
                        let q = self.point_pos(si, pi + 1)
                        c.fill_triangle(ax + p[0], ay + p[1], ax + q[0], ay + q[1], ax + q[0], ay + base, col)
                        c.fill_triangle(ax + p[0], ay + p[1], ax + q[0], ay + base, ax + p[0], ay + base, col)
                    c.set_alpha(255)
                for pi in range(n - 1):
                    let p = self.point_pos(si, pi)
                    let q = self.point_pos(si, pi + 1)
                    c.line_aa(ax + p[0], ay + p[1], ax + q[0], ay + q[1], col)
                    c.line_aa(ax + p[0], ay + p[1] + 1, ax + q[0], ay + q[1] + 1, col)
                for pi in range(n):
                    let p = self.point_pos(si, pi)
                    c.fill_circle(ax + p[0], ay + p[1], 2, col)
                    if self.selected != nil and self.selected[0] == si and self.selected[1] == pi:
                        c.circle(ax + p[0], ay + p[1], 5, THEME.text)
                    if self.show_values:
                        let lab = fmt_num(sr.values[pi])
                        c.text(ax + p[0] - len(lab) * 3, ay + p[1] - 11, lab, THEME.text)
    func draw_pie(self, c, ax, ay):
        let g = self.pie_geom()
        let cx = ax + g[0]
        let cy = ay + g[1]
        let sl = self.slice_degrees()
        for i in range(len(sl)):
            let a = sl[i][0]
            let b = sl[i][1]
            let col = PALETTE[i % len(PALETTE)]
            let grow = 0
            if self.selected != nil and self.selected[1] == i:
                grow = 4
            let mid = (a + b) // 2
            let ox = grow * isin(mid) // 1000
            let oy = -grow * icos(mid) // 1000
            let d = a
            while d < b:
                let e = imin(d + 3, b)
                c.fill_triangle(cx + ox, cy + oy, cx + ox + g[2] * isin(d) // 1000, cy + oy - g[2] * icos(d) // 1000, cx + ox + g[2] * isin(e) // 1000, cy + oy - g[2] * icos(e) // 1000, col)
                d = e
            if len(sl) > 1 and b > a:
                c.line_aa(cx + ox, cy + oy, cx + ox + g[2] * isin(a) // 1000, cy + oy - g[2] * icos(a) // 1000, THEME.field)
        if self.kind == "donut":
            c.fill_circle(cx, cy, g[2] * 3 // 5, THEME.field)
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, 3, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 3, THEME.border)
        let y = ay + 4
        if self.title != "":
            c.text(ax + (self.w - gfx.text_advance(self.title)) // 2, y + 1, self.title, THEME.text)
            y += 12
        if self.show_legend():
            let items = self.legend_items()
            let total = 0
            for it in items:
                total += gfx.text_advance(it[0]) + 18
            let x = ax + imax(4, (self.w - total) // 2)
            for it in items:
                c.fill_rect(x, y + 1, 7, 7, it[1])
                c.text(x + 10, y + 1, it[0], THEME.dim)
                x += gfx.text_advance(it[0]) + 18
        if self.is_round():
            self.draw_pie(c, ax, ay)
        else:
            self.draw_axes(c, ax, ay)
            self.draw_series(c, ax, ay)
        if self.hover and self.app != nil:
            let lx = self.app.mx - ax
            let ly = self.app.my - ay
            let h = self.hit_at(lx, ly)
            if h != nil:
                let s = self.describe(h)
                let w = gfx.text_advance(s) + 8
                let tx = clamp(self.app.mx + 8, ax, ax + imax(0, self.w - w))
                let ty = clamp(self.app.my - 18, ay, ay + imax(0, self.h - 14))
                c.fill_rect(tx, ty, w, 13, THEME.panel)
                c.rect(tx, ty, w, 13, self.color_of(h[0], h[1]))
                c.text(tx + 4, ty + 3, s, THEME.text)

export func chart(kind, series, labels = [], w = 260, h = 160):
    return Chart(kind, series, labels, w, h)

# ---- paint surface -----------------------------------------------------------------------
# A fixed-size canvas of its own. on_draw(view, canvas) paints it (call redraw()); on_pointer(view, kind, x, y, button)
# gets "down" / "move" (while pressed) / "up" with coordinates relative to the surface.
export class CanvasView extends gui.Widget:
    let canvas = nil
    let bg = 0xffffff
    let on_draw = nil
    let on_pointer = nil
    override func init(self, w, h, on_draw = nil, bg = 0xffffff):
        super(0, 0, w, h)
        self.bg = bg
        self.canvas = gfx.canvas(w, h, bg)
        self.on_draw = on_draw
        if on_draw != nil:
            self.redraw()
    func clear(self):
        self.canvas.clear(self.bg)
        self.dirty()
    func redraw(self):
        self.canvas.clear(self.bg)
        if self.on_draw != nil:
            self.on_draw(self, self.canvas)
        self.dirty()
    func resize(self, w, h):
        self.canvas = gfx.canvas(w, h, self.bg)
        self.set_size(w, h)
        self.redraw()
    func send(self, kind, px, py, button):
        if self.on_pointer != nil:
            self.on_pointer(self, kind, px - self.abs_x(), py - self.abs_y(), button)
            self.dirty()
    override func on_mouse_down(self, px, py, button):
        self.pressed = true
        self.send("down", px, py, button)
    override func on_drag(self, px, py):
        if self.pressed:
            self.send("move", px, py, 1)
    override func on_mouse_up(self, px, py, button, inside):
        if self.pressed:
            self.pressed = false
            self.send("up", px, py, button)
    override func draw_self(self, c, ax, ay):
        c.blit(self.canvas, ax, ay)
        c.rect(ax, ay, self.canvas.width(), self.canvas.height(), THEME.border)

export func canvas_view(w, h, on_draw = nil, bg = 0xffffff):
    return CanvasView(w, h, on_draw, bg)

# ---- colour picker -----------------------------------------------------------------------
# Hue bar and saturation/value square. `color` is 0xRRGGBB; keys: Left/Right saturation, Up/Down value,
# PageUp/PageDown hue (Shift = ten steps).
export class ColorPicker extends gui.Widget:
    let hue = 210
    let sat = 70
    let val = 90
    let on_change = nil
    let mode = ""
    override func init(self, rgb = 0x4c8dff, on_change = nil):
        super(0, 0, 124, 124)
        self.on_change = on_change
        self.focusable = true
        self.set_color(rgb, false)
    func color(self):
        return gfx.hsv(self.hue, self.sat, self.val)
    func set_hsv(self, h, s, v):
        self.hue = h % 360
        self.sat = clamp(s, 0, 100)
        self.val = clamp(v, 0, 100)
        self.dirty()
        call0(self.on_change, self)
    func set_color(self, rgb, notify = true):
        let r = (rgb >> 16) & 255
        let g = (rgb >> 8) & 255
        let b = rgb & 255
        let mx = imax(r, imax(g, b))
        let mn = imin(r, imin(g, b))
        let d = mx - mn
        let h = 0
        if d > 0:
            if mx == r:
                h = (60 * (g - b) // d) % 360
            elif mx == g:
                h = 60 * (b - r) // d + 120
            else:
                h = 60 * (r - g) // d + 240
        self.hue = h % 360
        self.sat = 0 if mx == 0 else d * 100 // mx
        self.val = mx * 100 // 255
        self.dirty()
        if notify:
            call0(self.on_change, self)
    func choose(self, px, py):
        let lx = px - self.abs_x()
        let ly = py - self.abs_y()
        if self.mode == "hue":
            self.set_hsv(clamp(ly - 4, 0, 95) * 360 // 96, self.sat, self.val)
        elif self.mode == "sv":
            self.set_hsv(self.hue, clamp(lx - 4, 0, 95) * 100 // 95, 100 - clamp(ly - 4, 0, 95) * 100 // 95)
    override func on_mouse_down(self, px, py, button):
        let lx = px - self.abs_x()
        if lx >= 104:
            self.mode = "hue"
        elif lx < 102:
            self.mode = "sv"
        self.choose(px, py)
    override func on_drag(self, px, py):
        self.choose(px, py)
    override func on_mouse_up(self, px, py, button, inside):
        self.mode = ""
    override func on_key(self, k):
        let pk = gui.parse_key(k)
        let n = 10 if pk[2] else 1
        if pk[3] == "Left":
            self.set_hsv(self.hue, self.sat - n, self.val)
        elif pk[3] == "Right":
            self.set_hsv(self.hue, self.sat + n, self.val)
        elif pk[3] == "Up":
            self.set_hsv(self.hue, self.sat, self.val + n)
        elif pk[3] == "Down":
            self.set_hsv(self.hue, self.sat, self.val - n)
        elif pk[3] == "PageUp":
            self.set_hsv(self.hue + n * 6, self.sat, self.val)
        elif pk[3] == "PageDown":
            self.set_hsv(self.hue - n * 6, self.sat, self.val)
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, 3, THEME.panel)
        c.round_rect(ax, ay, self.w, self.h, 3, THEME.border)
        for r in range(96):
            let v = 100 - r * 100 // 95
            c.gradient_rect(ax + 4, ay + 4 + r, 96, 1, gfx.hsv(self.hue, 0, v), gfx.hsv(self.hue, 100, v), false)
            c.fill_rect(ax + 106, ay + 4 + r, 12, 1, gfx.hsv(r * 360 // 96, 100, 100))
        let mx = ax + 4 + self.sat * 95 // 100
        let my = ay + 4 + (100 - self.val) * 95 // 100
        c.circle(mx, my, 4, 0xffffff)
        c.circle(mx, my, 3, 0x000000)
        let hy = ay + 4 + self.hue * 96 // 360
        c.rect(ax + 104, hy - 1, 16, 3, 0xffffff)
        let col = self.color()
        c.fill_rect(ax + 4, ay + 104, 16, 14, col)
        c.rect(ax + 4, ay + 104, 16, 14, THEME.border)
        c.text(ax + 26, ay + 108, hex_color(col), THEME.text)

export func color_picker(rgb = 0x4c8dff, on_change = nil):
    return ColorPicker(rgb, on_change)

# ---- calendar ----------------------------------------------------------------------------
export func is_leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)

export func days_in_month(y, m):
    if m == 2:
        return 29 if is_leap(y) else 28
    return 30 if m == 4 or m == 6 or m == 9 or m == 11 else 31

# 0 = Sunday ... 6 = Saturday
export func weekday(y, m, d):
    const t = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4]
    let yy = y - 1 if m < 3 else y
    return (yy + yy // 4 - yy // 100 + yy // 400 + t[m - 1] + d) % 7

# days since 1970-01-01 (proleptic Gregorian) and back
export func days_from_civil(y, m, d):
    let yy = y - 1 if m <= 2 else y
    let era = yy // 400
    let yoe = yy - era * 400
    let doy = (153 * (m - 3 if m > 2 else m + 9) + 2) // 5 + d - 1
    let doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468

export func civil_from_days(n):
    let z = n + 719468
    let era = z // 146097
    let doe = z - era * 146097
    let yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    let doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    let mp = (5 * doy + 2) // 153
    let d = doy - (153 * mp + 2) // 5 + 1
    let m = mp + 3 if mp < 10 else mp - 9
    let y = yoe + era * 400
    return [y + 1 if m <= 2 else y, m, d]

const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]
const DAYS = ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"]

# Month grid. week_start: 0 = Sunday, 1 = Monday. today = [y, m, d] marks a day (the widget has no clock).
export class Calendar extends gui.Widget:
    let year = 2000
    let month = 1
    let day = 1
    let week_start = 0
    let today = nil
    let on_change = nil
    override func init(self, year, month, day, on_change = nil):
        super(0, 0, 148, 124)
        self.year = year
        self.month = month
        self.day = clamp(day, 1, days_in_month(year, month))
        self.on_change = on_change
        self.focusable = true
    func date(self):
        return [self.year, self.month, self.day]
    func set_date(self, y, m, d):
        let mm = clamp(m, 1, 12)
        self.year = y
        self.month = mm
        self.day = clamp(d, 1, days_in_month(y, mm))
        self.dirty()
        call0(self.on_change, self)
    func move_days(self, n):
        let t = civil_from_days(days_from_civil(self.year, self.month, self.day) + n)
        self.set_date(t[0], t[1], t[2])
    func move_months(self, n):
        let total = self.year * 12 + (self.month - 1) + n
        self.set_date(total // 12, total % 12 + 1, self.day)
    # column of the first day of the month
    func lead(self):
        return (weekday(self.year, self.month, 1) - self.week_start + 7) % 7
    func cell_at(self, lx, ly):
        if ly < 30 or lx < 4:
            return -1
        let col = (lx - 4) // 20
        let row = (ly - 30) // 14
        if col > 6 or row > 5:
            return -1
        let d = row * 7 + col - self.lead() + 1
        return d if d >= 1 and d <= days_in_month(self.year, self.month) else -1
    override func on_mouse_down(self, px, py, button):
        let lx = px - self.abs_x()
        let ly = py - self.abs_y()
        if ly < 16:
            if lx < 20:
                self.move_months(-1)
            elif lx >= self.w - 20:
                self.move_months(1)
            return nil
        let d = self.cell_at(lx, ly)
        if d > 0:
            self.set_date(self.year, self.month, d)
    override func on_key(self, k):
        if k == "Left":
            self.move_days(-1)
        elif k == "Right":
            self.move_days(1)
        elif k == "Up":
            self.move_days(-7)
        elif k == "Down":
            self.move_days(7)
        elif k == "PageUp":
            self.move_months(-1)
        elif k == "PageDown":
            self.move_months(1)
        elif k == "Home" and self.today != nil:
            self.set_date(self.today[0], self.today[1], self.today[2])
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, 3, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 3, THEME.border)
        let title = MONTHS[self.month - 1] .. " " .. to_string(self.year)
        c.text(ax + (self.w - gfx.text_advance(title)) // 2, ay + 5, title, THEME.text)
        icon(c, "left", ax + 4, ay + 2, 12, THEME.dim)
        icon(c, "right", ax + self.w - 16, ay + 2, 12, THEME.dim)
        c.fill_rect(ax + 4, ay + 16, self.w - 8, 1, THEME.border)
        for i in range(7):
            c.text(ax + 8 + i * 20, ay + 20, DAYS[(i + self.week_start) % 7], THEME.dim)
        let lead = self.lead()
        let n = days_in_month(self.year, self.month)
        for d in range(1, n + 1):
            let slot = lead + d - 1
            let x = ax + 4 + (slot % 7) * 20
            let y = ay + 30 + (slot // 7) * 14
            if d == self.day:
                c.fill_round_rect(x, y, 18, 12, 3, THEME.accent_dark)
            let t = self.today
            if t != nil and t[0] == self.year and t[1] == self.month and t[2] == d:
                c.round_rect(x, y, 18, 12, 3, THEME.ok)
            let s = to_string(d)
            c.text(x + 9 - len(s) * 3, y + 3, s, THEME.text)

export func calendar(year, month, day, on_change = nil):
    return Calendar(year, month, day, on_change)
