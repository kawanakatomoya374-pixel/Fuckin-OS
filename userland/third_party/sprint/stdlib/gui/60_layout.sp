# ---- layout containers -----------------------------------------------------------
# Box lays its visible children out in one direction (a Column stacks them vertically,
# a Row horizontally). Children with `flex > 0` share the leftover main-axis space in
# proportion to their weights; the others get their preferred size.
#   align   (cross axis): "stretch" (default) | "start" | "center" | "end"
#   justify (main axis, only when nothing is flexible): "start" | "center" | "end" | "between"
export class Box extends Widget:
    let vertical = true
    let gap = 6
    let pad = 6
    let align = "stretch"
    let justify = "start"
    let auto = true
    let framed = false
    override func init(self, vertical, children = [], gap = 6, pad = 6):
        super(0, 0, 0, 0)
        self.vertical = vertical
        self.gap = gap
        self.pad = pad
        self.is_box = true
        for ch in children:
            self.add(ch)

    override func set_size(self, w, h):
        self.w = w
        self.h = h
        self.auto = false
        self.relayout()
        return self

    override func draw_self(self, c, ax, ay):
        if self.framed:
            c.fill_round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.panel)
            c.round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.border)

    func visible_children(self):
        return [ch for ch in self.children if ch.visible]

    override func pref(self):
        if not self.auto:
            return [self.w, self.h]
        let main = 0
        let cross = 0
        let vis = self.visible_children()
        for ch in vis:
            let p = ch.pref()
            if self.vertical:
                main += p[1]
                cross = imax(cross, p[0])
            else:
                main += p[0]
                cross = imax(cross, p[1])
        if len(vis) > 1:
            main += self.gap * (len(vis) - 1)
        main += 2 * self.pad
        cross += 2 * self.pad
        if self.vertical:
            return [cross, main]
        return [main, cross]

    override func layout(self):
        if self.auto and (self.parent == nil or not self.parent.is_box):
            let p = self.pref()
            self.w = p[0]
            self.h = p[1]
        let vert = self.vertical
        let vis = self.visible_children()
        let n = len(vis)
        if n == 0:
            return nil
        let avail_main = (self.h if vert else self.w) - 2 * self.pad
        let avail_cross = (self.w if vert else self.h) - 2 * self.pad
        let sizes = []
        let fixed = self.gap * (n - 1)
        let flex_sum = 0
        for ch in vis:
            let p = ch.pref()
            let m = p[1] if vert else p[0]
            if ch.flex > 0:
                flex_sum += ch.flex
                sizes.append(0)
            else:
                fixed += m
                sizes.append(m)
        let left = avail_main - fixed
        if flex_sum > 0:
            if left > 0:
                let given = 0
                let last = 0
                for i in range(n):
                    if vis[i].flex > 0:
                        sizes[i] = left * vis[i].flex // flex_sum
                        given += sizes[i]
                        last = i
                sizes[last] += left - given
            left = 0
        let off = 0
        let extra = 0
        if left > 0:
            if self.justify == "center":
                off = left // 2
            elif self.justify == "end":
                off = left
            elif self.justify == "between" and n > 1:
                extra = left // (n - 1)
        let pos = self.pad + off
        for i in range(n):
            let ch = vis[i]
            let p = ch.pref()
            let cm = p[0] if vert else p[1]
            let cs = avail_cross if self.align == "stretch" else cm
            let co = 0
            if self.align == "center":
                co = (avail_cross - cs) // 2
            elif self.align == "end":
                co = avail_cross - cs
            if vert:
                ch.x = self.pad + co
                ch.y = pos
                ch.w = cs
                ch.h = sizes[i]
            else:
                ch.x = pos
                ch.y = self.pad + co
                ch.w = sizes[i]
                ch.h = cs
            pos += sizes[i] + self.gap + extra
            ch.layout()

export class Grid extends Widget:
    let cols = 1
    let gap = 6
    let pad = 6
    let auto = true
    override func init(self, cols, children = [], gap = 6, pad = 6):
        super(0, 0, 0, 0)
        self.cols = imax(1, cols)
        self.gap = gap
        self.pad = pad
        self.is_box = true
        for ch in children:
            self.add(ch)

    func metrics(self):
        let widths = [0 for i in range(self.cols)]
        let heights = []
        let r = 0
        let i = 0
        for ch in self.children:
            let p = ch.pref()
            let col = i % self.cols
            if col == 0:
                heights.append(0)
                r = len(heights) - 1
            widths[col] = imax(widths[col], p[0])
            heights[r] = imax(heights[r], p[1])
            i += 1
        return [widths, heights]

    override func pref(self):
        let m = self.metrics()
        let w = 2 * self.pad
        let h = 2 * self.pad
        for cw in m[0]:
            w += cw
        for rh in m[1]:
            h += rh
        w += self.gap * imax(0, len(m[0]) - 1)
        h += self.gap * imax(0, len(m[1]) - 1)
        return [w, h]

    override func layout(self):
        if self.auto and (self.parent == nil or not self.parent.is_box):
            let p = self.pref()
            self.w = p[0]
            self.h = p[1]
        let m = self.metrics()
        let i = 0
        let y = self.pad
        let row = 0
        for ch in self.children:
            let col = i % self.cols
            if col == 0 and i > 0:
                y += m[1][row] + self.gap
                row += 1
            let x = self.pad
            for k in range(col):
                x += m[0][k] + self.gap
            ch.x = x
            ch.y = y
            ch.w = m[0][col]
            ch.h = m[1][row]
            ch.layout()
            i += 1

const CLIPS = []

# A viewport onto a taller child. Wheel / Up / Down / PageUp / PageDown / Home / End scroll it.
# Wraps its children into rows no wider than its width (set_size, or the width the parent gives it).
export class Flow extends Widget:
    let hgap = 6
    let vgap = 6
    let pad = 6
    let wrap_px = 0
    override func init(self, children = [], hgap = 6, vgap = 6, pad = 6):
        super(0, 0, 0, 0)
        self.hgap = hgap
        self.vgap = vgap
        self.pad = pad
        self.is_box = true
        for ch in children:
            self.add(ch)
    func visible_children(self):
        return [ch for ch in self.children if ch.visible]
    # [rows], each row a list of children, and the total height, for a given width
    func rows_for(self, width):
        let rows = []
        let cur = []
        let x = self.pad
        for ch in self.visible_children():
            let p = ch.pref()
            if len(cur) > 0 and x + p[0] + self.pad > width:
                rows.append(cur)
                cur = []
                x = self.pad
            cur.append(ch)
            x += p[0] + self.hgap
        if len(cur) > 0:
            rows.append(cur)
        return rows
    func row_h(self, row):
        let h = 0
        for ch in row:
            h = imax(h, ch.pref()[1])
        return h
    override func pref(self):
        let width = self.wrap_px if self.wrap_px > 0 else self.w
        if width <= 0:
            let w = 2 * self.pad
            let h = 0
            let vis = self.visible_children()
            for ch in vis:
                w += ch.pref()[0]
                h = imax(h, ch.pref()[1])
            if len(vis) > 1:
                w += self.hgap * (len(vis) - 1)
            return [w, h + 2 * self.pad]
        let rows = self.rows_for(width)
        let h = 2 * self.pad
        for r in rows:
            h += self.row_h(r)
        if len(rows) > 1:
            h += self.vgap * (len(rows) - 1)
        return [width, h]
    override func layout(self):
        let rows = self.rows_for(self.w)
        let y = self.pad
        for r in rows:
            let rh = self.row_h(r)
            let x = self.pad
            for ch in r:
                let p = ch.pref()
                ch.x = x
                ch.y = y
                ch.w = p[0]
                ch.h = p[1]
                x += p[0] + self.hgap
                ch.layout()
            y += rh + self.vgap
        let need = self.pref()[1]
        if need != self.h:
            self.h = need
            self.relayout()

# Children on top of each other, each stretched over the whole area (later children draw above earlier ones).
export class Stack extends Widget:
    override func init(self, children = []):
        super(0, 0, 0, 0)
        self.is_box = true
        for ch in children:
            self.add(ch)
    override func pref(self):
        let w = 0
        let h = 0
        for ch in self.children:
            if ch.visible:
                let p = ch.pref()
                w = imax(w, p[0])
                h = imax(h, p[1])
        return [w, h]
    override func layout(self):
        let p = self.pref()
        if self.parent == nil or not self.parent.is_box:
            self.w = imax(self.w, p[0])
            self.h = imax(self.h, p[1])
        for ch in self.children:
            ch.x = 0
            ch.y = 0
            ch.w = self.w
            ch.h = self.h
            ch.layout()

# A header that shows or hides its content (click or Enter/Space/Left/Right).
export class Expander extends Widget:
    let title = ""
    let content = nil
    let is_open = true
    let on_toggle = nil
    override func init(self, title, content, is_open = true):
        super(0, 0, 0, 0)
        self.title = title
        self.content = content
        self.is_open = is_open
        self.focusable = true
        self.is_box = true
        self.add(content)
        content.visible = is_open
    func set_open(self, v):
        if v == self.is_open:
            return nil
        self.is_open = v
        self.content.visible = v
        self.relayout()
        call0(self.on_toggle, self)
    override func pref(self):
        if not self.is_open:
            return [gfx.text_advance(self.title) + 24, 16]
        let p = self.content.pref()
        return [imax(p[0], gfx.text_advance(self.title) + 24), 18 + p[1]]
    override func layout(self):
        let p = self.pref()
        if self.parent == nil or not self.parent.is_box:
            self.w = p[0]
            self.h = p[1]
        self.content.x = 0
        self.content.y = 18
        self.content.w = self.w
        self.content.h = imax(0, self.h - 18)
        if self.is_open:
            self.content.layout()
    override func on_mouse_down(self, px, py, button):
        if py - self.abs_y() < 16:
            self.set_open(not self.is_open)
    override func on_key(self, k):
        if k == "Enter" or k == "Space":
            self.set_open(not self.is_open)
        elif k == "Right":
            self.set_open(true)
        elif k == "Left":
            self.set_open(false)
    override func draw_self(self, c, ax, ay):
        let hot = self.hover
        c.fill_rect(ax, ay, self.w, 16, THEME.hover if hot else THEME.panel)
        c.fill_rect(ax, ay + 15, self.w, 1, THEME.border)
        if self.is_open:
            c.fill_triangle(ax + 4, ay + 5, ax + 10, ay + 5, ax + 7, ay + 10, THEME.dim)
        else:
            c.fill_triangle(ax + 5, ay + 3, ax + 5, ay + 12, ax + 10, ay + 7, THEME.dim)
        c.text(ax + 16, ay + 4, self.title, THEME.text)

export class ScrollView extends Widget:
    let content = nil
    let scroll_y = 0
    override func init(self, content, w, h):
        super(0, 0, w, h)
        self.content = content
        self.focusable = true
        self.add(content)
    func max_scroll(self):
        return imax(0, self.content.h - self.h)
    func scroll_to(self, y):
        let ny = clamp(y, 0, self.max_scroll())
        if ny != self.scroll_y:
            self.scroll_y = ny
            self.content.y = -ny
            self.dirty()
    override func layout(self):
        self.content.w = self.w - 6
        let p = self.content.pref()
        self.content.h = imax(p[1], self.h)
        self.content.x = 0
        self.scroll_y = clamp(self.scroll_y, 0, self.max_scroll())
        self.content.y = -self.scroll_y
        self.content.layout()
    override func on_mouse_down(self, px, py, button):
        let nt = self.bar_press(px, py, self.content.h, self.h, self.scroll_y, self.abs_y(), self.h)
        if nt >= 0:
            self.scroll_to(nt)
    override func on_drag(self, px, py):
        if self.bar_grab >= 0:
            self.scroll_to(self.bar_drag(py, self.content.h, self.h, self.abs_y(), self.h))
    override func on_wheel(self, dy):
        if self.max_scroll() == 0:
            return false
        self.scroll_to(self.scroll_y + (12 if dy > 0 else -12) * (dy if dy > 0 else -dy))
        return true
    override func on_key(self, k):
        if k == "Down":
            self.scroll_to(self.scroll_y + 12)
        elif k == "Up":
            self.scroll_to(self.scroll_y - 12)
        elif k == "PageDown":
            self.scroll_to(self.scroll_y + self.h - 12)
        elif k == "PageUp":
            self.scroll_to(self.scroll_y - self.h + 12)
        elif k == "Home":
            self.scroll_to(0)
        elif k == "End":
            self.scroll_to(self.max_scroll())
    override func draw(self, c):
        if not self.visible:
            return nil
        let ax = self.abs_x()
        let ay = self.abs_y()
        let k = self.app.dclip if self.app != nil else nil
        if k != nil and (ax + self.w + 12 <= k[0] or ay + self.h + 12 <= k[1] or ax - 12 >= k[2] or ay - 12 >= k[3]):
            return nil
        c.fill_rect(ax, ay, self.w, self.h, THEME.field)
        # clip to the viewport (intersected with an enclosing viewport, if any)
        let x0 = ax
        let y0 = ay
        let x1 = ax + self.w
        let y1 = ay + self.h
        if len(CLIPS) > 0:
            let top = CLIPS[len(CLIPS) - 1]
            x0 = imax(x0, top[0])
            y0 = imax(y0, top[1])
            x1 = imin(x1, top[2])
            y1 = imin(y1, top[3])
        CLIPS.append([x0, y0, x1, y1])
        c.set_clip(x0, y0, imax(0, x1 - x0), imax(0, y1 - y0))
        let ct = self.content
        ct.cax = ax + ct.x
        ct.cay = ay + ct.y
        ct.cs = STAMP
        ct.draw(c)
        CLIPS.pop()
        if len(CLIPS) > 0:
            let t = CLIPS[len(CLIPS) - 1]
            c.set_clip(t[0], t[1], t[2] - t[0], t[3] - t[1])
        else:
            c.clear_clip()
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.border)
        self.draw_bar(c, ax, ay, self.h, self.content.h, self.h, self.scroll_y)
