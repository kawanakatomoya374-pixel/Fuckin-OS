# ---- menu bar --------------------------------------------------------------
# MenuBar: a row of headers; a press opens that header's list above everything else.
# menus: [[label, entries], ...]; an entry is [label, action(app)] or "-" for a separator.
export class MenuBar extends Widget:
    let menus = []
    let open_at = -1
    let sel = -1
    override func init(self, menus):
        super(0, 0, 0, 16)
        self.menus = menus
        self.focusable = true
        self.w = self.header_x(len(menus)) + 4
    func header_x(self, i):
        let x = 4
        for k in range(i):
            x += gfx.text_advance(self.menus[k][0]) + 14
        return x
    func entries(self, m):
        return self.menus[m][1]
    func list_w(self, m):
        return menu_w(self.entries(m))
    func list_h(self, m):
        return menu_h(self.entries(m))
    func row_y(self, m, idx):
        return menu_y(self.entries(m), idx)
    func open_menu(self, m):
        self.open_at = m
        self.sel = -1
        if self.app != nil:
            self.app.popup = self
            self.app.redraw()
    func close(self):
        self.open_at = -1
        self.sel = -1
        if self.app != nil:
            if self.app.popup == self:
                self.app.popup = nil
            self.app.redraw()
    func header_at(self, px, py):
        if py < self.abs_y() or py >= self.abs_y() + self.h:
            return -1
        let rx = px - self.abs_x()
        for k in range(len(self.menus)):
            if rx >= self.header_x(k) - 4 and rx < self.header_x(k + 1) - 4:
                return k
        return -1
    func entry_at(self, px, py):
        if self.open_at < 0:
            return -1
        let lx = self.abs_x() + self.header_x(self.open_at) - 4
        let ly = self.abs_y() + self.h
        if px < lx or px >= lx + self.list_w(self.open_at):
            return -1
        let es = self.entries(self.open_at)
        for i in range(len(es)):
            let top = ly + self.row_y(self.open_at, i)
            if es[i] != "-" and py >= top and py < top + 12:
                return i
        return -1
    func activate(self, m, i):
        let e = self.entries(m)[i]
        let app = self.app
        self.close()
        if len(e) > 1 and e[1] != nil:
            e[1](app)
    override func on_mouse_down(self, px, py, button):
        let k = self.header_at(px, py)
        if k >= 0:
            self.open_menu(k)
    # a press while a list is open: an entry runs, another header switches, anything else closes
    func popup_click(self, px, py):
        let i = self.entry_at(px, py)
        if i >= 0:
            self.activate(self.open_at, i)
            return true
        let lx = self.abs_x() + self.header_x(self.open_at) - 4
        let ly = self.abs_y() + self.h
        let inside = px >= lx and px < lx + self.list_w(self.open_at) and py >= ly and py < ly + self.list_h(self.open_at)
        if inside:
            return true
        let k = self.header_at(px, py)
        if k >= 0 and k != self.open_at:
            self.open_menu(k)
        else:
            self.close()
        return true
    func step(self, d):
        let es = self.entries(self.open_at)
        let i = self.sel
        for n in range(len(es)):
            i = (i + d + len(es)) % len(es)
            if es[i] != "-":
                break
        self.sel = i
        self.dirty()
    override func on_key(self, k):
        if self.open_at < 0:
            if k == "Down" or k == "Enter" or k == "Space":
                self.open_menu(0)
            return nil
        if k == "Left":
            self.open_menu((self.open_at - 1 + len(self.menus)) % len(self.menus))
        elif k == "Right":
            self.open_menu((self.open_at + 1) % len(self.menus))
        elif k == "Down":
            self.step(1)
        elif k == "Up":
            self.step(-1)
        elif k == "Enter" or k == "Space":
            if self.sel >= 0:
                self.activate(self.open_at, self.sel)
        elif k == "Escape":
            self.close()
    override func draw_self(self, c, ax, ay):
        c.fill_rect(ax, ay, self.w, self.h, THEME.panel)
        c.fill_rect(ax, ay + self.h - 1, self.w, 1, THEME.border)
        for k in range(len(self.menus)):
            let x = ax + self.header_x(k)
            let w = gfx.text_advance(self.menus[k][0]) + 14
            if k == self.open_at:
                c.fill_rect(x - 4, ay, w, self.h - 1, THEME.accent_dark)
            c.text(x + 3, ay + 4, self.menus[k][0], THEME.text)
    func draw_popup(self, c):
        if self.open_at >= 0:
            draw_menu(c, self.entries(self.open_at), self.abs_x() + self.header_x(self.open_at) - 4, self.abs_y() + self.h, self.sel)

# ---- popup menu (context menu) ---------------------------------------------
# Entries are shared with MenuBar: [label, action(app)] or "-" for a separator line.
func menu_w(es):
    let w = 40
    for e in es:
        if e != "-":
            w = imax(w, gfx.text_advance(e[0]) + 20)
    return w

func menu_y(es, idx):
    let y = 2
    for i in range(idx):
        y += 5 if es[i] == "-" else 12
    return y

func menu_h(es):
    return menu_y(es, len(es)) + 2

func draw_menu(c, es, lx, ly, sel):
    let w = menu_w(es)
    let h = menu_h(es)
    c.set_alpha(80)
    c.fill_rect(lx + 2, ly + 3, w, h, THEME.shadow)
    c.set_alpha(255)
    c.fill_rect(lx, ly, w, h, THEME.field)
    c.rect(lx, ly, w, h, THEME.accent)
    for i in range(len(es)):
        let y = ly + menu_y(es, i)
        if es[i] == "-":
            c.fill_rect(lx + 4, y + 2, w - 8, 1, THEME.border)
        else:
            if i == sel:
                c.fill_rect(lx + 1, y, w - 2, 12, THEME.accent_dark)
            c.text(lx + 8, y + 3, es[i][0], THEME.text)

# A floating menu opened at a point: app.context_menu(x, y, entries), or set widget.context to an
# entries array (or a function(widget) returning one) and right-click (button 3) the widget.
export class PopupMenu:
    let entries = []
    let x = 0
    let y = 0
    let sel = -1
    let app = nil
    func init(self, app, entries, x, y):
        self.app = app
        self.entries = entries
        self.x = clamp(x, 0, imax(0, app.width - menu_w(entries)))
        self.y = clamp(y, 0, imax(0, app.height - menu_h(entries)))
    func entry_at(self, px, py):
        if px < self.x or px >= self.x + menu_w(self.entries):
            return -1
        for i in range(len(self.entries)):
            let top = self.y + menu_y(self.entries, i)
            if self.entries[i] != "-" and py >= top and py < top + 12:
                return i
        return -1
    func close(self):
        self.app.ctx = nil
        self.app.redraw()
    func activate(self, i):
        let e = self.entries[i]
        let app = self.app
        self.close()
        if len(e) > 1 and e[1] != nil:
            e[1](app)
    func click(self, px, py):
        let i = self.entry_at(px, py)
        if i >= 0:
            self.activate(i)
        else:
            self.close()
    func step(self, d):
        let n = len(self.entries)
        let i = self.sel
        for k in range(n):
            i = (i + d + n) % n
            if self.entries[i] != "-":
                break
        self.sel = i
        self.app.redraw()
    func on_key(self, k):
        if k == "Down":
            self.step(1)
        elif k == "Up":
            self.step(-1)
        elif k == "Enter" or k == "Space":
            if self.sel >= 0:
                self.activate(self.sel)
        elif k == "Escape":
            self.close()
    func draw(self, c):
        draw_menu(c, self.entries, self.x, self.y, self.sel)
