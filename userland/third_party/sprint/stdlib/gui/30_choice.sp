export class ListBox extends Widget:
    let items = []
    let selected = -1
    let top = 0
    let on_select = nil
    override func init(self, x, y, w, h, items, on_select = nil):
        super(x, y, w, h)
        self.items = items
        self.on_select = on_select
        self.focusable = true
    func rows(self):
        return imax(1, (self.h - 4) // 10)
    func select(self, i):
        if len(self.items) == 0:
            return nil
        let n = clamp(i, 0, len(self.items) - 1)
        if n != self.selected:
            self.selected = n
            if n < self.top:
                self.top = n
            if n >= self.top + self.rows():
                self.top = n - self.rows() + 1
            self.dirty()
            call0(self.on_select, self)
    func current(self):
        if self.selected < 0:
            return nil
        return self.items[self.selected]
    override func on_mouse_down(self, px, py, button):
        let nt = self.bar_press(px, py, len(self.items), self.rows(), self.top, self.abs_y() + 2, self.h - 4)
        if nt >= 0:
            self.top = nt
            self.dirty()
            return nil
        let row = (py - self.abs_y() - 2) // 10
        if row >= 0 and row < self.rows() and self.top + row < len(self.items):
            self.select(self.top + row)
    override func on_drag(self, px, py):
        if self.bar_grab >= 0:
            self.top = self.bar_drag(py, len(self.items), self.rows(), self.abs_y() + 2, self.h - 4)
            self.dirty()
    override func on_wheel(self, dy):
        let n = len(self.items) - self.rows()
        if n <= 0:
            return false
        let nt = clamp(self.top + (1 if dy > 0 else -1) * 2, 0, n)
        if nt != self.top:
            self.top = nt
            self.dirty()
        return true
    override func on_key(self, k):
        if k == "Down":
            self.select(self.selected + 1)
        elif k == "Up":
            self.select(self.selected - 1)
        elif k == "Home":
            self.select(0)
        elif k == "End":
            self.select(len(self.items) - 1)
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.border)
        let cap = (self.w - 8) // 6
        for r in range(self.rows()):
            let i = self.top + r
            if i >= len(self.items):
                break
            let yy = ay + 2 + r * 10
            if i == self.selected:
                c.fill_rect(ax + 1, yy, self.w - 2, 10, THEME.accent_dark)
            c.text(ax + 4, yy + 1, clip(to_string(self.items[i]), cap), THEME.text)
        self.draw_bar(c, ax, ay + 2, self.h - 4, len(self.items), self.rows(), self.top)

# ---- tabs and dropdown -------------------------------------------------------
# Tabs shows one of its pages at a time; the header is a row of labels.
export class Tabs extends Widget:
    let labels = []
    let selected = 0
    let on_change = nil
    override func init(self, labels, pages, on_change = nil):
        super(0, 0, 0, 0)
        self.labels = labels
        self.on_change = on_change
        self.focusable = true
        for p in pages:
            self.add(p)
        self.show(0)
    func show(self, i):
        for k in range(len(self.children)):
            self.children[k].visible = k == i
        self.relayout()
    func select(self, i):
        let n = clamp(i, 0, len(self.labels) - 1)
        if n != self.selected:
            self.selected = n
            self.show(n)
            call0(self.on_change, self)
    func tab_x(self, i):
        let x = 0
        for k in range(i):
            x += gfx.text_advance(self.labels[k]) + 16
        return x
    override func pref(self):
        let w = 0
        let h = 0
        for ch in self.children:
            let p = ch.pref()
            w = imax(w, p[0])
            h = imax(h, p[1])
        return [imax(w, self.tab_x(len(self.labels))), h + 20]
    override func layout(self):
        for ch in self.children:
            ch.x = 0
            ch.y = 20
            ch.w = self.w
            ch.h = self.h - 20
            ch.layout()
    override func on_mouse_down(self, px, py, button):
        let rx = px - self.abs_x()
        if py - self.abs_y() < 18:
            for k in range(len(self.labels)):
                if rx >= self.tab_x(k) and rx < self.tab_x(k + 1):
                    self.select(k)
    override func on_key(self, k):
        if k == "Left":
            self.select(self.selected - 1)
        elif k == "Right":
            self.select(self.selected + 1)
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay + 18, self.w, self.h - 18, THEME.radius, THEME.panel)
        c.round_rect(ax, ay + 18, self.w, self.h - 18, THEME.radius, THEME.border)
        for k in range(len(self.labels)):
            let x = ax + self.tab_x(k)
            let w = gfx.text_advance(self.labels[k]) + 16
            let on = k == self.selected
            c.fill_round_rect(x, ay + 2, w, 18, THEME.radius, THEME.panel if on else THEME.field)
            c.round_rect(x, ay + 2, w, 18, THEME.radius, THEME.accent if on else THEME.border)
            if on:
                c.fill_rect(x + 1, ay + 18, w - 2, 2, THEME.panel)
            c.text(x + 8, ay + 7, self.labels[k], THEME.text if on else THEME.dim)

# A closed field showing the current item; clicking opens a list drawn above everything else.
export class Dropdown extends Widget:
    let items = []
    let selected = 0
    let is_open = false
    let on_change = nil
    override func init(self, items, selected = 0, on_change = nil):
        super(0, 0, 140, 18)
        self.items = items
        self.selected = selected
        self.on_change = on_change
        self.focusable = true
    func current(self):
        if self.selected < 0 or self.selected >= len(self.items):
            return nil
        return self.items[self.selected]
    func select(self, i):
        let n = clamp(i, 0, len(self.items) - 1)
        if n != self.selected:
            self.selected = n
            self.dirty()
            call0(self.on_change, self)
    func open(self):
        self.is_open = true
        if self.app != nil:
            self.app.popup = self
            self.app.redraw()
    func close(self):
        self.is_open = false
        if self.app != nil:
            if self.app.popup == self:
                self.app.popup = nil
            self.app.redraw()
    override func on_mouse_down(self, px, py, button):
        if self.is_open:
            self.close()
        else:
            self.open()
    override func on_key(self, k):
        if k == "Down":
            self.select(self.selected + 1)
        elif k == "Up":
            self.select(self.selected - 1)
        elif k == "Enter" or k == "Space":
            if self.is_open:
                self.close()
            else:
                self.open()
        elif k == "Escape":
            self.close()
    # called by the App for a press while this dropdown's list is open; returns true (consumed)
    func popup_click(self, px, py):
        let ax = self.abs_x()
        let ay = self.abs_y() + self.h
        if px >= ax and px < ax + self.w and py >= ay + 2 and py < ay + 2 + len(self.items) * 12:
            self.select((py - ay - 2) // 12)
        self.close()
        return true
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.accent if self.is_open else THEME.border)
        let cur = self.current()
        c.text(ax + 5, ay + 6, clip("" if cur == nil else to_string(cur), (self.w - 24) // 6), THEME.text)
        let tx = ax + self.w - 12
        c.fill_triangle(tx, ay + 7, tx + 6, ay + 7, tx + 3, ay + 11, THEME.dim)
    func draw_popup(self, c):
        let ax = self.abs_x()
        let ay = self.abs_y() + self.h
        let h = len(self.items) * 12 + 4
        c.set_alpha(80)
        c.fill_round_rect(ax + 2, ay + 3, self.w, h, 2, THEME.shadow)
        c.set_alpha(255)
        c.fill_round_rect(ax, ay, self.w, h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, h, 2, THEME.accent)
        for i in range(len(self.items)):
            let yy = ay + 2 + i * 12
            if i == self.selected:
                c.fill_rect(ax + 1, yy, self.w - 2, 12, THEME.accent_dark)
            c.text(ax + 5, yy + 3, clip(to_string(self.items[i]), (self.w - 10) // 6), THEME.text)
