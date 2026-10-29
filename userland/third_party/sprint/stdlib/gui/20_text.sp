# ---- text editing ------------------------------------------------------------------
# Keys carry modifiers as a prefix in the fixed order Ctrl+Alt+Shift+ (for example "Ctrl+Shift+Left").
# Returns [ctrl, alt, shift, base].
export func parse_key(k):
    if len(k) < 2 or k.find("+") <= 0:
        return [false, false, false, k]
    let parts = k.split("+")
    let n = len(parts)
    if parts[n - 1] == "":
        # the key itself is "+" ("Ctrl++")
        parts = parts.slice(0, n - 2)
        parts.append("+")
    let base = parts[len(parts) - 1]
    let ctrl = false
    let alt = false
    let shift = false
    for i in range(len(parts) - 1):
        if parts[i] == "Ctrl":
            ctrl = true
        elif parts[i] == "Alt":
            alt = true
        elif parts[i] == "Shift":
            shift = true
    return [ctrl, alt, shift, base]

func is_word_byte(c):
    return (c >= 48 and c <= 57) or (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95 or c >= 128

# Shared editing core: caret, selection, clipboard, undo/redo, word movement.
export class TextBase extends Widget:
    let text = ""
    let cursor = 0
    let anchor = -1
    let on_change = nil
    let readonly = false
    let max_len = 0
    let undo_stack = []
    let redo_stack = []
    let typing = false
    override func cursor_kind(self):
        return self.pointer if self.pointer != nil else "ibeam"
    override func init(self, x, y, w, h):
        super(x, y, w, h)
        self.focusable = true
    func has_selection(self):
        return self.anchor >= 0 and self.anchor != self.cursor
    func sel_range(self):
        if not self.has_selection():
            return [self.cursor, self.cursor]
        return [imin(self.anchor, self.cursor), imax(self.anchor, self.cursor)]
    func selected_text(self):
        let r = self.sel_range()
        return self.text.slice(r[0], r[1])
    func select_all(self):
        self.anchor = 0
        self.cursor = len(self.text)
        self.typing = false
        self.dirty()
    func select_range(self, a, b):
        self.anchor = clamp(a, 0, len(self.text))
        self.cursor = clamp(b, 0, len(self.text))
        self.dirty()
    func push_undo(self):
        self.undo_stack.append([self.text, self.cursor])
        if len(self.undo_stack) > 200:
            self.undo_stack.remove_at(0)
        self.redo_stack = []
    func set_text(self, s):
        self.text = s
        self.cursor = len(s)
        self.anchor = -1
        self.undo_stack = []
        self.redo_stack = []
        self.typing = false
        self.moved()
    # hook: the caret or text changed (subclasses scroll the caret into view)
    func moved(self):
        self.dirty()
    func changed(self):
        self.moved()
        call0(self.on_change, self)
    # replace the selection (or insert at the caret) with s; typing runs share one undo step
    func replace_sel(self, s, typing = false):
        if self.readonly:
            return nil
        let r = self.sel_range()
        if self.max_len > 0:
            # max_len counts characters
            let room = self.max_len - (self.text.char_count() - self.text.slice(r[0], r[1]).char_count())
            if s.char_count() > room:
                s = take_chars(s, imax(0, room))
                if s == "" and r[0] == r[1]:
                    return nil
        if not (typing and self.typing and r[0] == r[1]):
            self.push_undo()
        self.typing = typing
        let t = self.text
        self.text = t.slice(0, r[0]) .. s .. t.slice(r[1], len(t))
        self.cursor = r[0] + len(s)
        self.anchor = -1
        self.changed()
    func erase(self, lo, hi):
        if self.readonly or hi <= lo:
            return nil
        self.push_undo()
        self.typing = false
        self.text = self.text.slice(0, lo) .. self.text.slice(hi, len(self.text))
        self.cursor = lo
        self.anchor = -1
        self.changed()
    func move_to(self, pos, extend):
        let p = snap_down(self.text, pos)
        if extend:
            if self.anchor < 0:
                self.anchor = self.cursor
        else:
            self.anchor = -1
        self.cursor = p
        self.typing = false
        self.moved()
    func word_left(self, pos):
        let t = self.text
        let i = pos
        while i > 0 and not is_word_byte(t[i - 1]):
            i -= 1
        while i > 0 and is_word_byte(t[i - 1]):
            i -= 1
        return i
    func word_right(self, pos):
        let t = self.text
        let i = pos
        while i < len(t) and not is_word_byte(t[i]):
            i += 1
        while i < len(t) and is_word_byte(t[i]):
            i += 1
        return i
    func copy_sel(self):
        if self.has_selection() and self.app != nil:
            self.app.clipboard = self.selected_text()
    func paste(self):
        if self.app != nil and self.app.clipboard != "":
            self.replace_sel(self.filter_text(self.app.clipboard))
    func do_undo(self):
        if len(self.undo_stack) == 0:
            return nil
        self.redo_stack.append([self.text, self.cursor])
        let st = self.undo_stack.pop()
        self.text = st[0]
        self.cursor = st[1]
        self.anchor = -1
        self.typing = false
        self.changed()
    func do_redo(self):
        if len(self.redo_stack) == 0:
            return nil
        self.undo_stack.append([self.text, self.cursor])
        let st = self.redo_stack.pop()
        self.text = st[0]
        self.cursor = st[1]
        self.anchor = -1
        self.typing = false
        self.changed()
    # printable ASCII and every character the built-in font has a glyph for (Japanese, 12 pixels wide);
    # newlines are kept when multiline
    func filter_text(self, s):
        let add = ""
        for cp in s.codepoints():
            if (cp >= 32 and cp < 127) or (cp == 10 and self.multiline()):
                add = add .. (chr_of(cp) if cp >= 32 else "\n")
            elif cp >= 128 and gfx.text_advance(str.from_codepoint(cp)) == 12:
                add = add .. str.from_codepoint(cp)
        return add
    func multiline(self):
        return false
    override func on_text(self, s):
        let add = self.filter_text(s)
        if add != "":
            self.replace_sel(add, true)
    # keys shared by single-line and multi-line editors; returns true when handled
    func edit_key(self, k):
        let pk = parse_key(k)
        let ctrl = pk[0]
        let shift = pk[2]
        let base = pk[3]
        let t = self.text
        if ctrl and not pk[1]:
            if base == "a" or base == "A":
                self.select_all()
                return true
            elif base == "c" or base == "C":
                self.copy_sel()
                return true
            elif base == "x" or base == "X":
                if self.has_selection():
                    self.copy_sel()
                    self.replace_sel("")
                return true
            elif base == "v" or base == "V":
                self.paste()
                return true
            elif base == "z" or base == "Z":
                if shift:
                    self.do_redo()
                else:
                    self.do_undo()
                return true
            elif base == "y" or base == "Y":
                self.do_redo()
                return true
        if base == "Backspace":
            if self.has_selection():
                self.replace_sel("")
            elif ctrl:
                self.erase(self.word_left(self.cursor), self.cursor)
            else:
                self.erase(prev_b(t, self.cursor), self.cursor)
            return true
        if base == "Delete":
            if self.has_selection():
                self.replace_sel("")
            elif ctrl:
                self.erase(self.cursor, self.word_right(self.cursor))
            else:
                self.erase(self.cursor, next_b(t, self.cursor))
            return true
        if base == "Left":
            if self.has_selection() and not shift and not ctrl:
                self.move_to(self.sel_range()[0], false)
            else:
                self.move_to(self.word_left(self.cursor) if ctrl else prev_b(t, self.cursor), shift)
            return true
        if base == "Right":
            if self.has_selection() and not shift and not ctrl:
                self.move_to(self.sel_range()[1], false)
            else:
                self.move_to(self.word_right(self.cursor) if ctrl else next_b(t, self.cursor), shift)
            return true
        return false
    # mouse: a click places the caret (Shift-click extends); double click selects a word, triple the line
    func mouse_pos(self, px, py):
        return 0
    override func on_mouse_down(self, px, py, button):
        let p = self.mouse_pos(px, py)
        let clicks = self.app.clicks if self.app != nil else 1
        if clicks >= 3:
            let ends = self.line_ends(p)
            self.select_range(ends[0], ends[1])
        elif clicks == 2:
            self.select_range(self.word_left(next_b(self.text, p)), self.word_right(prev_b(self.text, p)))
            if not self.has_selection():
                self.select_range(p, p)
        elif self.app != nil and self.app.mods.find("Shift") >= 0:
            self.move_to(p, true)
        else:
            self.move_to(p, false)
            self.anchor = p
        self.moved()
    override func on_drag(self, px, py):
        if self.anchor >= 0:
            self.cursor = self.mouse_pos(px, py)
            self.moved()
    func line_ends(self, p):
        return [0, len(self.text)]
    func draw_selection(self, c, x, y, w, h):
        c.set_alpha(110)
        c.fill_rect(x, y, w, h, THEME.accent)
        c.set_alpha(255)

export class TextInput extends TextBase:
    let on_submit = nil
    let placeholder = ""
    let password = false
    let first = 0
    override func init(self, x = 0, y = 0, w = 120, text = "", on_change = nil):
        super(x, y, w, 16)
        self.text = text
        self.cursor = len(text)
        self.on_change = on_change
    func cap(self):
        return imax(1, (self.w - 8) // 6)
    # the displayed string (password: one star per character) and the maps between text offsets
    # and offsets into it
    func vis(self):
        return "*".repeat(self.text.char_count()) if self.password else self.text
    func vpos(self, p):
        return self.text.slice(0, p).char_count() if self.password else p
    func tpos(self, k):
        if not self.password:
            return k
        let n = 0
        let i = 0
        for cp in self.text.codepoints():
            if n >= k:
                break
            i += len(str.from_codepoint(cp))
            n += 1
        return i
    override func moved(self):
        let t = self.vis()
        let pc = self.w - 8
        let cu = self.vpos(self.cursor)
        if cu < self.first:
            self.first = cu
        if t.char_count() == len(t):
            let cp = self.cap()
            if cu > self.first + cp:
                self.first = cu - cp
            self.first = clamp(self.first, 0, imax(0, len(t) - cp + 1))
        else:
            self.first = snap_down(t, self.first)
            while self.first < cu and gfx.text_advance(t.slice(self.first, cu)) > pc:
                self.first = next_b(t, self.first)
            let want = (pc // 6 - 1) * 6
            while self.first > 0 and gfx.text_advance(t.slice(self.first, len(t))) < want:
                self.first = prev_b(t, self.first)
        self.dirty()
    override func mouse_pos(self, px, py):
        let t = self.vis()
        let k = self.first + col_at(t.slice(self.first, len(t)), px - self.abs_x() - 4)
        return self.tpos(clamp(k, 0, len(t)))
    override func on_key(self, k):
        if self.edit_key(k):
            return nil
        let pk = parse_key(k)
        if pk[3] == "Home":
            self.move_to(0, pk[2])
        elif pk[3] == "End":
            self.move_to(len(self.text), pk[2])
        elif pk[3] == "Enter":
            call0(self.on_submit, self)
    override func draw_self(self, c, ax, ay):
        let focus = self.app != nil and self.app.focus == self
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.accent if focus else THEME.border)
        let pc = self.w - 8
        let t = self.vis()
        if t == "" and self.placeholder != "":
            c.text(ax + 4, ay + 4, fit(self.placeholder, pc), THEME.dim)
        else:
            let shown = fit(t.slice(self.first, len(t)), pc)
            if self.has_selection():
                let r = self.sel_range()
                let a = imax(self.vpos(r[0]), self.first)
                let b = imin(self.vpos(r[1]), self.first + len(shown))
                if b > a:
                    self.draw_selection(c, ax + 4 + gfx.text_advance(t.slice(self.first, a)), ay + 2,
                                        gfx.text_advance(t.slice(a, b)), self.h - 4)
            c.text(ax + 4, ay + 4, shown, THEME.text if self.enabled else THEME.dim)
        if focus:
            let cx = ax + 4 + gfx.text_advance(t.slice(self.first, self.vpos(self.cursor)))
            c.line(cx, ay + 2, cx, ay + self.h - 3, THEME.text)

func str_of(cp):
    return chr_of(cp)

# one-character string from an ASCII code point
const ASCII = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
func chr_of(cp):
    if cp >= 128:
        return str.from_codepoint(cp)
    return ASCII.slice(cp - 32, cp - 31)

# the first n characters of s
func take_chars(s, n):
    let out = ""
    let k = 0
    for cp in s.codepoints():
        if k >= n:
            break
        out = out .. str.from_codepoint(cp)
        k += 1
    return out

# ---- multi-line editor -------------------------------------------------------
# Multi-line text editor (ASCII and Japanese). Enter inserts a newline; Up/Down keep the column; Shift extends the
# selection; Ctrl+A/C/X/V/Z/Y work as in any editor. Long lines scroll horizontally.
export class TextArea extends TextBase:
    let top = 0
    let left = 0
    let want_col = -1
    override func init(self, x = 0, y = 0, w = 200, h = 80, text = "", on_change = nil):
        super(x, y, w, h)
        self.text = text
        self.cursor = len(text)
        self.on_change = on_change
    override func multiline(self):
        return true
    func lines(self):
        return self.text.split("\n")
    func rows(self):
        return imax(1, (self.h - 6) // 10)
    func cols(self):
        return imax(1, (self.w - 8) // 6)
    # [line, column] of the cursor
    func position(self):
        let before = self.text.slice(0, self.cursor)
        let ls = before.split("\n")
        return [len(ls) - 1, len(ls[len(ls) - 1])]
    func offset_of(self, ln, col):
        let ls = self.lines()
        let l = clamp(ln, 0, len(ls) - 1)
        let off = 0
        for i in range(l):
            off += len(ls[i]) + 1
        return off + snap_down(ls[l], col)
    # offset of the character boundary of line ln nearest to pixel column px
    func offset_px(self, ln, px):
        let ls = self.lines()
        let l = clamp(ln, 0, len(ls) - 1)
        let off = 0
        for i in range(l):
            off += len(ls[i]) + 1
        return off + col_at(ls[l], px)
    # pixel x of the cursor within its line (without scrolling)
    func cursor_px(self):
        let p = self.position()
        let ls = self.lines()
        return gfx.text_advance(ls[p[0]].slice(0, p[1]))
    func scroll_into_view(self):
        let p = self.position()
        if p[0] < self.top:
            self.top = p[0]
        if p[0] >= self.top + self.rows():
            self.top = p[0] - self.rows() + 1
        if p[1] < self.left:
            self.left = p[1]
        let line = self.lines()[p[0]]
        if line.char_count() == len(line):
            if p[1] > self.left + self.cols():
                self.left = p[1] - self.cols()
        else:
            self.left = snap_up(line, self.left)
            while self.left < p[1] and gfx.text_advance(line.slice(self.left, p[1])) > self.w - 8:
                self.left = next_b(line, self.left)
    override func moved(self):
        self.scroll_into_view()
        self.dirty()
    func insert(self, s):
        self.replace_sel(s)
    override func on_mouse_down(self, px, py, button):
        let nt = self.bar_press(px, py, len(self.lines()), self.rows(), self.top, self.abs_y() + 3, self.h - 6)
        if nt >= 0:
            self.top = nt
            self.dirty()
            return nil
        super.on_mouse_down(px, py, button)
    override func on_drag(self, px, py):
        if self.bar_grab >= 0:
            self.top = self.bar_drag(py, len(self.lines()), self.rows(), self.abs_y() + 3, self.h - 6)
            self.dirty()
        else:
            super.on_drag(px, py)
    override func mouse_pos(self, px, py):
        let ln = self.top + (py - self.abs_y() - 3) // 10
        let ls = self.lines()
        let line = ls[clamp(ln, 0, len(ls) - 1)]
        let st = snap_up(line, self.left)
        let col = st + col_at(line.slice(st, len(line)), px - self.abs_x() - 4)
        return self.offset_of(ln, col)
    override func line_ends(self, p):
        let ls = self.lines()
        let off = 0
        for i in range(len(ls)):
            if p <= off + len(ls[i]):
                return [off, off + len(ls[i])]
            off += len(ls[i]) + 1
        return [off, off]
    override func on_wheel(self, dy):
        let n = len(self.lines()) - self.rows()
        if n <= 0:
            return false
        self.top = clamp(self.top + (1 if dy > 0 else -1) * 2, 0, n)
        self.dirty()
        return true
    override func on_key(self, k):
        let pk = parse_key(k)
        let base = pk[3]
        let shift = pk[2]
        let p = self.position()
        if base == "Up" or base == "Down":
            if self.want_col < 0:
                self.want_col = self.cursor_px()
            let d = -1 if base == "Up" else 1
            self.move_to(self.offset_px(p[0] + d, self.want_col), shift)
            return nil
        if self.edit_key(k):
            self.want_col = -1
            return nil
        if base == "Enter" and not pk[0]:
            self.replace_sel("\n")
        elif base == "Home":
            self.move_to(0 if pk[0] else self.offset_of(p[0], 0), shift)
        elif base == "End":
            self.move_to(len(self.text) if pk[0] else self.offset_of(p[0], 100000), shift)
        elif base == "PageUp":
            self.move_to(self.offset_px(p[0] - self.rows(), self.cursor_px()), shift)
        elif base == "PageDown":
            self.move_to(self.offset_px(p[0] + self.rows(), self.cursor_px()), shift)
        self.want_col = -1
    override func draw_self(self, c, ax, ay):
        let focus = self.app != nil and self.app.focus == self
        c.fill_round_rect(ax, ay, self.w, self.h, 2, THEME.field)
        c.round_rect(ax, ay, self.w, self.h, 2, THEME.accent if focus else THEME.border)
        let ls = self.lines()
        let pc = self.w - 8
        let sel = self.sel_range()
        let off = 0
        for i in range(self.top):
            off += len(ls[i]) + 1
        for r in range(self.rows()):
            let i = self.top + r
            if i >= len(ls):
                break
            let line = ls[i]
            let st = snap_up(line, self.left)
            let shown = fit(line.slice(st, len(line)), pc)
            let lo = imax(sel[0] - off, 0)
            let hi = imin(sel[1] - off, len(line) + 1)
            if self.has_selection() and hi > lo and sel[0] <= off + len(line) and sel[1] > off:
                let a = imax(lo, st)
                let end = st + len(shown)
                let b = imin(hi, end)
                let extra = 0
                if hi > len(line) and end == len(line) and gfx.text_advance(shown) + 6 <= pc:
                    extra = 6
                    b = imin(hi, len(line))
                if b > a or extra > 0:
                    self.draw_selection(c, ax + 4 + gfx.text_advance(line.slice(st, a)), ay + 3 + r * 10,
                                        gfx.text_advance(line.slice(a, imax(a, b))) + extra, 10)
            c.text(ax + 4, ay + 4 + r * 10, shown, THEME.text if self.enabled else THEME.dim)
            off += len(line) + 1
        self.draw_bar(c, ax, ay + 3, self.h - 6, len(ls), self.rows(), self.top)
        if focus:
            let p = self.position()
            let cy = ay + 3 + (p[0] - self.top) * 10
            let cline = ls[p[0]]
            let cx = ax + 4 + gfx.text_advance(cline.slice(snap_up(cline, self.left), p[1]))
            c.line(cx, cy, cx, cy + 8, THEME.text)
