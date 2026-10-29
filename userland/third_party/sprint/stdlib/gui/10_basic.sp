# ---- widgets -----------------------------------------------------------------
export class Panel extends Widget:
    let title = ""
    let fill = true
    override func init(self, x, y, w, h, title = ""):
        super(x, y, w, h)
        self.title = title
    override func draw_self(self, c, ax, ay):
        if self.fill:
            c.fill_round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.panel)
            c.round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.border)
        if self.title != "":
            c.text(ax + 6, ay + 4, self.title, THEME.dim)

# Greedy word wrap to `cols` cells (6 pixels each) per line; "\n" always breaks, over-long words are cut.
# Japanese text breaks between any two characters; a run of other characters is a word.
export func wrap_lines(s, cols):
    let out = []
    let wide = s.char_count() != len(s)
    for para in s.split("\n"):
        if wide:
            wrap_wide(out, para, cols * 6)
            continue
        let cur = ""
        for word in para.split(" "):
            let wd = word
            while len(wd) > cols:
                if cur != "":
                    out.append(cur)
                    cur = ""
                out.append(wd.slice(0, cols))
                wd = wd.slice(cols)
            if cur == "":
                cur = wd
            elif len(cur) + 1 + len(wd) <= cols:
                cur = cur .. " " .. wd
            else:
                out.append(cur)
                cur = wd
        out.append(cur)
    return out

func wrap_wide(out, para, limit):
    # tokens: a run of ASCII characters other than space, a space, or one other character
    let toks = []
    let run = ""
    for cp in para.codepoints():
        if cp < 128 and cp != 32:
            run = run .. str.from_codepoint(cp)
        else:
            if run != "":
                toks.append(run)
                run = ""
            toks.append(str.from_codepoint(cp))
    if run != "":
        toks.append(run)
    let cur = ""
    let curw = 0
    for t in toks:
        let w = gfx.text_advance(t)
        if t == " " and cur == "":
            continue
        if curw + w > limit and cur != "":
            out.append(cur)
            cur = ""
            curw = 0
            if t == " ":
                continue
        while w > limit:
            let part = fit(t, limit)
            if part == "":
                break
            out.append(part)
            t = t.slice(len(part), len(t))
            w = gfx.text_advance(t)
        cur = cur .. t
        curw += w
    out.append(cur)

export class Label extends Widget:
    let text = ""
    let color = nil
    let dim = false
    let scale = 1
    let wrap = 0
    let align = "left"
    let shown = []
    let lh = 8
    override func init(self, text, x = 0, y = 0, scale = 1):
        super(x, y, 0, 0)
        self.scale = scale
        self.set_text(text)
    # wrap > 0: break lines to at most `wrap` pixels; align: "left" | "center" | "right" (within the width)
    func set_wrap(self, px):
        self.wrap = px
        self.set_text(self.text)
        return self
    func set_text(self, s):
        let old_pw = self.pw
        let old_ph = self.ph
        let old_w = self.w
        let old_h = self.h
        self.text = s
        let cw = 6 * self.scale
        self.shown = wrap_lines(s, imax(1, self.wrap // cw)) if self.wrap > 0 else s.split("\n")
        let widest = 0
        for l in self.shown:
            widest = imax(widest, gfx.text_advance(l, self.scale))
        self.w = (self.wrap if self.wrap > 0 else widest)
        # Japanese text needs taller lines (12-pixel glyphs)
        self.lh = (8 if s.char_count() == len(s) else 12) * self.scale
        self.h = len(self.shown) * self.lh
        self.pw = self.w
        self.ph = self.h
        if self.pw == old_pw and self.ph == old_ph and self.app != nil:
            # same preferred size: the layout stands, only the pixels change
            self.w = old_w
            self.h = old_h
            self.dirty()
        else:
            self.relayout()
    override func draw_self(self, c, ax, ay):
        let col = THEME.text if self.color == nil else self.color
        if self.dim:
            col = THEME.dim
        if not self.enabled:
            col = THEME.dim
        let lh = self.lh
        for i in range(len(self.shown)):
            let l = self.shown[i]
            let x = ax
            if self.align == "center":
                x = ax + (self.w - gfx.text_advance(l, self.scale)) // 2
            elif self.align == "right":
                x = ax + self.w - gfx.text_advance(l, self.scale)
            c.text(x, ay + i * lh, l, col, self.scale)

export class Button extends Widget:
    let text = ""
    let on_click = nil
    override func cursor_kind(self):
        return self.pointer if self.pointer != nil else "hand"
    override func init(self, text, x = 0, y = 0, w = 0, h = 0, on_click = nil):
        super(x, y, w if w > 0 else gfx.text_advance(text) + 20, h if h > 0 else 20)
        self.text = text
        self.on_click = on_click
        self.focusable = true
    override func on_mouse_down(self, px, py, button):
        self.pressed = true
        self.dirty()
    override func on_mouse_up(self, px, py, button, inside):
        self.pressed = false
        self.dirty()
        if inside and self.enabled:
            call0(self.on_click, self)
    override func on_key(self, k):
        if k == "Enter" or k == "Space":
            call0(self.on_click, self)
    override func draw_self(self, c, ax, ay):
        let bg = THEME.accent
        if not self.enabled:
            bg = THEME.border
        elif self.pressed:
            bg = THEME.accent_dark
        elif self.hover:
            bg = THEME.accent_hi
        c.fill_round_rect(ax, ay, self.w, self.h, THEME.radius, bg)
        if self.enabled and not self.pressed:
            c.set_alpha(48)
            c.fill_round_rect(ax + 1, ay + 1, self.w - 2, self.h // 2, THEME.radius, 0xffffff)
            c.set_alpha(255)
        c.round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.border)
        let tw = c.text_width(self.text)
        let off = 1 if self.pressed else 0
        c.text(ax + (self.w - tw) // 2 + off, ay + (self.h - 7) // 2 + off, self.text,
               0xffffff if self.enabled else THEME.dim)

export class Checkbox extends Widget:
    let text = ""
    let checked = false
    let on_change = nil
    override func cursor_kind(self):
        return self.pointer if self.pointer != nil else "hand"
    override func init(self, text, x = 0, y = 0, checked = false, on_change = nil):
        super(x, y, 18 + gfx.text_advance(text), 12)
        self.text = text
        self.checked = checked
        self.on_change = on_change
        self.focusable = true
    func toggle(self):
        self.checked = not self.checked
        self.dirty()
        call0(self.on_change, self)
    override func on_mouse_down(self, px, py, button):
        self.pressed = true
        self.dirty()
    override func on_mouse_up(self, px, py, button, inside):
        self.pressed = false
        self.dirty()
        if inside and self.enabled:
            self.toggle()
    override func on_key(self, k):
        if k == "Space" or k == "Enter":
            self.toggle()
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, 12, 12, 2, THEME.field)
        c.round_rect(ax, ay, 12, 12, 2, THEME.accent if self.hover else THEME.border)
        if self.checked:
            c.line_aa(ax + 3, ay + 6, ax + 5, ay + 9, THEME.ok)
            c.line_aa(ax + 5, ay + 9, ax + 9, ay + 3, THEME.ok)
            c.line_aa(ax + 3, ay + 5, ax + 5, ay + 8, THEME.ok)
            c.line_aa(ax + 5, ay + 8, ax + 9, ay + 2, THEME.ok)
        c.text(ax + 18, ay + 2, self.text, THEME.text if self.enabled else THEME.dim)

export class Slider extends Widget:
    let lo = 0
    let hi = 100
    let value = 0
    let on_change = nil
    override func cursor_kind(self):
        return self.pointer if self.pointer != nil else "hand"
    override func init(self, x = 0, y = 0, w = 120, lo = 0, hi = 100, value = 0, on_change = nil):
        super(x, y, w, 14)
        self.lo = lo
        self.hi = hi
        self.value = clamp(value, lo, hi)
        self.on_change = on_change
        self.focusable = true
    func set_value(self, v):
        let nv = clamp(v, self.lo, self.hi)
        if nv != self.value:
            self.value = nv
            self.dirty()
            call0(self.on_change, self)
    func from_x(self, px):
        let span = self.w - 8
        let rel = clamp(px - self.abs_x() - 4, 0, span)
        if span <= 0 or self.hi == self.lo:
            return self.lo
        return self.lo + (rel * (self.hi - self.lo) + span // 2) // span
    override func on_mouse_down(self, px, py, button):
        self.pressed = true
        self.set_value(self.from_x(px))
    override func on_mouse_up(self, px, py, button, inside):
        self.pressed = false
        self.dirty()
    override func on_key(self, k):
        let step = imax(1, (self.hi - self.lo) // 20)
        if k == "Left" or k == "Down":
            self.set_value(self.value - step)
        elif k == "Right" or k == "Up":
            self.set_value(self.value + step)
        elif k == "Home":
            self.set_value(self.lo)
        elif k == "End":
            self.set_value(self.hi)
    override func on_drag(self, px, py):
        if self.pressed:
            self.set_value(self.from_x(px))
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay + 5, self.w, 4, 2, THEME.field)
        c.round_rect(ax, ay + 5, self.w, 4, 2, THEME.border)
        let span = self.w - 8
        let kx = ax + 4
        if self.hi != self.lo:
            kx = ax + 4 + (self.value - self.lo) * span // (self.hi - self.lo)
        c.fill_round_rect(ax + 1, ay + 6, kx - ax - 1, 2, 1, THEME.accent)
        c.fill_round_rect(kx - 3, ay + 1, 7, 12, 3, THEME.accent_dark if self.pressed else THEME.accent)
        c.round_rect(kx - 3, ay + 1, 7, 12, 3, THEME.border)

export class ProgressBar extends Widget:
    let value = 0
    let maximum = 100
    override func init(self, x = 0, y = 0, w = 120, h = 10, value = 0, maximum = 100):
        super(x, y, w, h)
        self.value = value
        self.maximum = maximum
    func set_value(self, v):
        self.value = clamp(v, 0, self.maximum)
        self.dirty()
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.field)
        let fw = 0
        if self.maximum > 0:
            fw = (self.w - 2) * self.value // self.maximum
        if fw > 0:
            c.gradient_rect(ax + 1, ay + 1, fw, self.h - 2, THEME.ok, THEME.accent, 0)
        c.round_rect(ax, ay, self.w, self.h, THEME.radius, THEME.border)

# ---- more widgets ----------------------------------------------------------------
export class Separator extends Widget:
    let vertical = false
    override func init(self, vertical = false):
        super(0, 0, 1 if vertical else 8, 8 if vertical else 1)
        self.vertical = vertical
    override func draw_self(self, c, ax, ay):
        if self.vertical:
            c.fill_rect(ax, ay, 1, self.h, THEME.border)
        else:
            c.fill_rect(ax, ay, self.w, 1, THEME.border)

# shows a gfx canvas, optionally magnified by an integer factor
export class ImageView extends Widget:
    let image = nil
    let zoom = 1
    override func init(self, image, zoom = 1):
        super(0, 0, image.width() * zoom, image.height() * zoom)
        self.image = image
        self.zoom = zoom
    func set_image(self, image):
        self.image = image
        self.pw = image.width() * self.zoom
        self.ph = image.height() * self.zoom
        self.relayout()
    override func draw_self(self, c, ax, ay):
        if self.zoom == 1:
            c.blit(self.image, ax, ay)
        else:
            c.blit_scaled(self.image, ax, ay, self.zoom)
