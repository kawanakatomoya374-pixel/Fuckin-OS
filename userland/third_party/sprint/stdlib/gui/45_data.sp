# ---- tree and table ----------------------------------------------------------
# A tree node is a table {label, children, open}; build them with gui.node.
export func node(label, children = []):
    return {label: label, children: children, open: false}

export class Tree extends Widget:
    let roots = []
    let selected = nil
    let top = 0
    let on_select = nil
    override func init(self, roots, w = 160, h = 100, on_select = nil):
        super(0, 0, w, h)
        self.roots = roots
        self.on_select = on_select
        self.focusable = true
    # visible rows: [[node, depth], ...]
    func rows_list(self):
        let out = []
        self.walk(self.roots, 0, out)
        return out
    func walk(self, nodes, depth, out):
        for nd in nodes:
            out.append([nd, depth])
            if nd.open:
                self.walk(nd.children, depth + 1, out)
    func visible_rows(self):
        return imax(1, (self.h - 4) // 12)
    func index_of(self, rows, nd):
        for i in range(len(rows)):
            if rows[i][0] == nd:
                return i
        return -1
    func select(self, nd):
        if nd == self.selected:
            return nil
        self.selected = nd
        let i = self.index_of(self.rows_list(), nd)
        if i >= 0:
            if i < self.top:
                self.top = i
            if i >= self.top + self.visible_rows():
                self.top = i - self.visible_rows() + 1
        self.dirty()
        call0(self.on_select, self)
    func toggle(self, nd):
        if len(nd.children) > 0:
            nd.open = not nd.open
            self.dirty()
    func set_open(self, nodes, v):
        for nd in nodes:
            nd.open = v
            self.set_open(nd.children, v)
    func expand_all(self):
        self.set_open(self.roots, true)
        self.dirty()
    func collapse_all(self):
        self.set_open(self.roots, false)
        self.top = 0
        self.dirty()
    override func on_drag(self, px, py):
        if self.bar_grab >= 0:
            self.top = self.bar_drag(py, len(self.rows_list()), self.visible_rows(), self.abs_y() + 2, self.h - 4)
            self.dirty()
    override func on_mouse_down(self, px, py, button):
        let rows = self.rows_list()
        let nt = self.bar_press(px, py, len(rows), self.visible_rows(), self.top, self.abs_y() + 2, self.h - 4)
        if nt >= 0:
            self.top = nt
            self.dirty()
            return nil
        let r = self.top + (py - self.abs_y() - 2) // 12
        if r < 0 or r >= len(rows):
            return nil
        let nd = rows[r][0]
        let ax = self.abs_x() + 4 + rows[r][1] * 10
        if len(nd.children) > 0 and px >= ax and px < ax + 9:
            self.toggle(nd)
        else:
            self.select(nd)
    override func on_wheel(self, dy):
        let n = len(self.rows_list()) - self.visible_rows()
        if n <= 0:
            return false
        self.top = clamp(self.top + (2 if dy > 0 else -2), 0, n)
        self.dirty()
        return true
    override func on_key(self, k):
        let rows = self.rows_list()
        if len(rows) == 0:
            return nil
        let i = self.index_of(rows, self.selected)
        if k == "Down":
            self.select(rows[clamp(i + 1, 0, len(rows) - 1)][0])
        elif k == "Up":
            self.select(rows[clamp(i - 1, 0, len(rows) - 1)][0])
        elif k == "Home":
            self.select(rows[0][0])
        elif k == "End":
            self.select(rows[len(rows) - 1][0])
        elif k == "Right" and i >= 0:
            let nd = rows[i][0]
            if len(nd.children) > 0 and not nd.open:
                self.toggle(nd)
            elif len(nd.children) > 0:
                self.select(nd.children[0])
        elif k == "Left" and i >= 0:
            let nd = rows[i][0]
            if nd.open:
                self.toggle(nd)
            else:
                # the parent is the nearest row above with a smaller depth
                let j = i - 1
                while j >= 0 and rows[j][1] >= rows[i][1]:
                    j -= 1
                if j >= 0:
                    self.select(rows[j][0])
        elif k == "Enter" or k == "Space":
            if self.selected != nil:
                self.toggle(self.selected)
    override func draw_self(self, c, ax, ay):
        let focus = self.app != nil and self.app.focus == self
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.accent if focus else THEME.border)
        let rows = self.rows_list()
        for r in range(self.visible_rows()):
            let i = self.top + r
            if i >= len(rows):
                break
            let nd = rows[i][0]
            let depth = rows[i][1]
            let y = ay + 2 + r * 12
            if nd == self.selected:
                c.fill_rect(ax + 1, y, self.w - 2, 12, THEME.accent_dark)
            let x = ax + 4 + depth * 10
            if len(nd.children) > 0:
                if nd.open:
                    c.fill_triangle(x + 1, y + 4, x + 7, y + 4, x + 4, y + 8, THEME.dim)
                else:
                    c.fill_triangle(x + 2, y + 2, x + 2, y + 10, x + 6, y + 6, THEME.dim)
            c.text(x + 11, y + 3, clip(to_string(nd.label), (self.w - 16) // 6 - depth * 2 - 1), THEME.text)
        self.draw_bar(c, ax, ay + 2, self.h - 4, len(rows), self.visible_rows(), self.top)

# Sortable table. columns: names or [name, width] (width 0 shares the leftover space);
# rows: arrays of cells. Click a header to sort (again to reverse); Up/Down/Home/End select.
export class Table extends Widget:
    let cols = []
    let rows = []
    let order = []
    let sort_col = -1
    let sort_desc = false
    let selected = -1
    let top = 0
    let on_select = nil
    override func init(self, columns, rows = [], w = 240, h = 120, on_select = nil):
        super(0, 0, w, h)
        self.cols = [{title: c if type(c) == "string" else c[0], width: 0 if type(c) == "string" else c[1]} for c in columns]
        self.on_select = on_select
        self.focusable = true
        self.set_rows(rows)
    func set_rows(self, rows):
        self.rows = rows
        self.selected = -1
        self.top = 0
        self.rebuild()
        self.dirty()
    func rebuild(self):
        if self.sort_col < 0:
            self.order = [i for i in range(len(self.rows))]
            return nil
        let c = self.sort_col
        let keyed = [[self.rows[i][c], i] for i in range(len(self.rows))]
        if self.sort_desc:
            keyed.sort((p, q) => p[0] > q[0])
        else:
            keyed.sort((p, q) => p[0] < q[0])
        self.order = keyed.map(p => p[1])
    func col_w(self, i):
        if self.cols[i].width > 0:
            return self.cols[i].width
        let used = 0
        let free = 0
        for c in self.cols:
            if c.width > 0:
                used += c.width
            else:
                free += 1
        return imax(12, (self.w - 2 - used) // free)
    func col_x(self, i):
        let x = 1
        for k in range(i):
            x += self.col_w(k)
        return x
    func visible_rows(self):
        return imax(1, (self.h - 16) // 12)
    func sort_by(self, c, desc = false):
        self.sort_col = c
        self.sort_desc = desc
        self.rebuild()
        self.dirty()
    func selected_row(self):
        return self.rows[self.selected] if self.selected >= 0 else nil
    func select_pos(self, pos):
        if len(self.order) == 0:
            return nil
        let p = clamp(pos, 0, len(self.order) - 1)
        if self.order[p] == self.selected:
            return nil
        self.selected = self.order[p]
        if p < self.top:
            self.top = p
        if p >= self.top + self.visible_rows():
            self.top = p - self.visible_rows() + 1
        self.dirty()
        call0(self.on_select, self)
    func pos_of_selected(self):
        for p in range(len(self.order)):
            if self.order[p] == self.selected:
                return p
        return -1
    override func on_drag(self, px, py):
        if self.bar_grab >= 0:
            self.top = self.bar_drag(py, len(self.order), self.visible_rows(), self.abs_y() + 15, self.h - 16)
            self.dirty()
    override func on_mouse_down(self, px, py, button):
        let ry = py - self.abs_y()
        let nt = self.bar_press(px, py, len(self.order), self.visible_rows(), self.top, self.abs_y() + 15, self.h - 16)
        if nt >= 0:
            self.top = nt
            self.dirty()
            return nil
        if ry < 14:
            let rx = px - self.abs_x()
            for i in range(len(self.cols)):
                if rx >= self.col_x(i) and rx < self.col_x(i) + self.col_w(i):
                    self.sort_by(i, self.sort_col == i and not self.sort_desc)
            return nil
        self.select_pos(self.top + (ry - 15) // 12)
    override func on_wheel(self, dy):
        let n = len(self.order) - self.visible_rows()
        if n <= 0:
            return false
        self.top = clamp(self.top + (2 if dy > 0 else -2), 0, n)
        self.dirty()
        return true
    override func on_key(self, k):
        let p = self.pos_of_selected()
        if k == "Down":
            self.select_pos(p + 1)
        elif k == "Up":
            self.select_pos(p - 1 if p > 0 else 0)
        elif k == "Home":
            self.select_pos(0)
        elif k == "End":
            self.select_pos(len(self.order) - 1)
    override func draw_self(self, c, ax, ay):
        let focus = self.app != nil and self.app.focus == self
        c.fill_rect(ax, ay, self.w, self.h, THEME.field)
        c.fill_rect(ax, ay, self.w, 14, THEME.panel)
        for i in range(len(self.cols)):
            let x = ax + self.col_x(i)
            c.text(x + 4, ay + 4, clip(self.cols[i].title, (self.col_w(i) - 18) // 6), THEME.text)
            if i == self.sort_col:
                let tx = x + self.col_w(i) - 11
                if self.sort_desc:
                    c.fill_triangle(tx, ay + 5, tx + 6, ay + 5, tx + 3, ay + 9, THEME.accent)
                else:
                    c.fill_triangle(tx, ay + 9, tx + 6, ay + 9, tx + 3, ay + 5, THEME.accent)
            if i > 0:
                c.fill_rect(x - 1, ay, 1, self.h, THEME.border)
        c.fill_rect(ax, ay + 14, self.w, 1, THEME.border)
        for r in range(self.visible_rows()):
            let p = self.top + r
            if p >= len(self.order):
                break
            let ri = self.order[p]
            let y = ay + 15 + r * 12
            if ri == self.selected:
                c.fill_rect(ax + 1, y, self.w - 2, 12, THEME.accent_dark)
            elif r % 2 == 1:
                c.set_alpha(40)
                c.fill_rect(ax + 1, y, self.w - 2, 12, THEME.border)
                c.set_alpha(255)
            for i in range(len(self.cols)):
                c.text(ax + self.col_x(i) + 4, y + 3, clip(to_string(self.rows[ri][i]), (self.col_w(i) - 8) // 6), THEME.text)
        self.draw_bar(c, ax, ay + 15, self.h - 16, len(self.order), self.visible_rows(), self.top)
        c.rect(ax, ay, self.w, self.h, THEME.accent if focus else THEME.border)
