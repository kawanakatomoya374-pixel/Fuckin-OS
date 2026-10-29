# ---- splitter, spinner, radio group -----------------------------------------
# Two panes separated by a draggable bar. vertical = false: left | right, true: top / bottom.
export class Splitter extends Widget:
    let vertical = false
    let pos = 100
    let min_a = 24
    let min_b = 24
    let dragging = false
    let on_change = nil
    override func init(self, a, b, vertical = false, pos = 100):
        super(0, 0, 240, 120)
        self.vertical = vertical
        self.pos = pos
        self.is_box = true  # panes that are boxes take the size the splitter gives them
        self.add(a)
        self.add(b)
    override func layout(self):
        let total = self.h if self.vertical else self.w
        self.pos = clamp(self.pos, self.min_a, imax(self.min_a, total - 4 - self.min_b))
        let a = self.children[0]
        let b = self.children[1]
        if self.vertical:
            a.x = 0
            a.y = 0
            a.w = self.w
            a.h = self.pos
            b.x = 0
            b.y = self.pos + 4
            b.w = self.w
            b.h = self.h - self.pos - 4
        else:
            a.x = 0
            a.y = 0
            a.w = self.pos
            a.h = self.h
            b.x = self.pos + 4
            b.y = 0
            b.w = self.w - self.pos - 4
            b.h = self.h
        a.layout()
        b.layout()
    func set_pos(self, p):
        self.pos = p
        self.relayout()
    override func on_mouse_down(self, px, py, button):
        self.dragging = true
        self.dirty()
    override func on_drag(self, px, py):
        if self.dragging:
            self.set_pos((py - self.abs_y() if self.vertical else px - self.abs_x()) - 2)
    override func on_mouse_up(self, px, py, button, inside):
        if self.dragging:
            self.dragging = false
            self.dirty()
            call0(self.on_change, self)
    override func draw_self(self, c, ax, ay):
        let col = THEME.accent if self.dragging or self.hover else THEME.border
        if self.vertical:
            c.fill_rect(ax, ay + self.pos, self.w, 4, col)
        else:
            c.fill_rect(ax + self.pos, ay, 4, self.h, col)

# Numeric stepper: click the upper / lower half of the right edge, or Up/Down/PageUp/PageDown/Home/End.
export class Spinner extends Widget:
    let value = 0
    let lo = 0
    let hi = 100
    let step = 1
    let on_change = nil
    override func init(self, value = 0, lo = 0, hi = 100, step = 1, on_change = nil):
        super(0, 0, 80, 18)
        self.lo = lo
        self.hi = hi
        self.step = step
        self.value = clamp(value, lo, hi)
        self.on_change = on_change
        self.focusable = true
    func set_value(self, v):
        let n = clamp(v, self.lo, self.hi)
        if n != self.value:
            self.value = n
            self.dirty()
            call0(self.on_change, self)
    override func on_mouse_down(self, px, py, button):
        if px >= self.abs_x() + self.w - 14:
            self.set_value(self.value + (self.step if py < self.abs_y() + self.h // 2 else -self.step))
    override func on_key(self, k):
        if k == "Up" or k == "Right":
            self.set_value(self.value + self.step)
        elif k == "Down" or k == "Left":
            self.set_value(self.value - self.step)
        elif k == "PageUp":
            self.set_value(self.value + self.step * 10)
        elif k == "PageDown":
            self.set_value(self.value - self.step * 10)
        elif k == "Home":
            self.set_value(self.lo)
        elif k == "End":
            self.set_value(self.hi)
    override func on_wheel(self, dy):
        if self.hover:
            self.set_value(self.value + (self.step if dy < 0 else -self.step))
            return true
        return false
    override func draw_self(self, c, ax, ay):
        let focus = self.app != nil and self.app.focus == self
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.accent if focus else THEME.border)
        let s = to_string(self.value)
        c.text(ax + self.w - 18 - gfx.text_advance(s), ay + 6, s, THEME.text if self.enabled else THEME.dim)
        let bx = ax + self.w - 12
        c.fill_triangle(bx, ay + 7, bx + 8, ay + 7, bx + 4, ay + 3, THEME.dim)
        c.fill_triangle(bx, ay + 11, bx + 8, ay + 11, bx + 4, ay + 15, THEME.dim)

# One-of-many list. Click an item or use Up/Down.
export class RadioGroup extends Widget:
    let items = []
    let selected = 0
    let on_change = nil
    override func init(self, items, selected = 0, on_change = nil):
        super(0, 0, 0, len(items) * 14)
        self.items = items
        self.selected = selected
        self.on_change = on_change
        self.focusable = true
        let w = 0
        for it in items:
            w = imax(w, len(to_string(it)))
        self.w = 20 + w * 6
        self.pw = self.w
    func current(self):
        return self.items[self.selected] if self.selected >= 0 and self.selected < len(self.items) else nil
    func select(self, i):
        let n = clamp(i, 0, len(self.items) - 1)
        if n != self.selected:
            self.selected = n
            self.dirty()
            call0(self.on_change, self)
    override func on_mouse_down(self, px, py, button):
        self.select((py - self.abs_y()) // 14)
    override func on_key(self, k):
        if k == "Down" or k == "Right":
            self.select(self.selected + 1)
        elif k == "Up" or k == "Left":
            self.select(self.selected - 1)
    override func draw_self(self, c, ax, ay):
        for i in range(len(self.items)):
            let y = ay + i * 14
            c.fill_circle(ax + 6, y + 6, 5, THEME.field)
            c.circle(ax + 6, y + 6, 5, THEME.accent if self.hover else THEME.border)
            if i == self.selected:
                c.fill_circle(ax + 6, y + 6, 3, THEME.ok)
            c.text(ax + 18, y + 3, to_string(self.items[i]), THEME.text if self.enabled else THEME.dim)
