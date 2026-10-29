# ---- easing, traces, snapshot checks ---------------------------------------------
# t and the result are in 0..1000
func ease_value(name, t):
    if name == "linear":
        return t
    if name == "in":
        return t * t // 1000
    if name == "out":
        return 1000 - (1000 - t) * (1000 - t) // 1000
    if name == "in_out":
        if t < 500:
            return 2 * t * t // 1000
        return 1000 - 2 * (1000 - t) * (1000 - t) // 1000
    throw error("ValueError", "unknown easing: " .. to_string(name))

# recorded events <-> JSON text (to keep a trace in a file)
export func encode_trace(events):
    return json.stringify(events)
export func decode_trace(text):
    return json.parse(text)

# number of pixels that differ between two canvases of the same size
export func pixel_diff(a, b):
    return a.diff(b)

# Raise an AssertionError unless the app currently renders to the pixels whose hash is `expected`.
export func expect_snapshot(app, expected, label = "snapshot"):
    let got = app.snapshot()
    if got != expected:
        throw error("AssertionError", label .. ": rendered hash " .. to_string(got) .. ", expected " .. to_string(expected))

# A modal message box: `labels` become buttons; on_choice(index) runs after it closes.
export func dialog(app, title, message, labels, on_choice = nil):
    func choose(i):
        app.close_modal()
        if on_choice != nil:
            on_choice(i)
    let btns = [button(labels[i], w => choose(i)) for i in range(len(labels))]
    let t = label(title)
    t.dim = true
    let box = column([t, label(message), row(btns, 6, 0)], 8, 12)
    box.framed = true
    box.align = "start"
    app.open_modal(box, w => choose(-1))
    return box

# ---- application ---------------------------------------------------------------
export class App:
    let width = 0
    let height = 0
    let title = ""
    let root = nil
    let queue = []
    let focus = nil
    let hover = nil
    let capture = nil
    let needs_draw = true
    # damage tracking: `full` = the whole canvas is stale; otherwise `damage` = [x0, y0, x1, y1] of the stale
    # part (nil with needs_draw set means a full redraw, e.g. when a program sets needs_draw itself)
    let full = true
    let damage = nil
    let dclip = nil
    let last_rect = [0, 0, 0, 0]
    let ov_prev = false
    let running = true
    let canvas = nil
    let timers = []
    let now = 0
    let frames = 0
    let log = []
    let needs_layout = true
    let modal = nil
    let modal_cancel = nil
    let popup = nil
    let ctx = nil
    let mods = ""
    let clicks = 1
    let clipboard = ""
    let keymap = {}
    let drag_src = nil
    let drag_from = [0, 0]
    let drag_on = false
    let drag_payload = nil
    let drop_hot = nil
    let mx = 0
    let my = 0
    let hover_ms = 0
    let toasts = []
    let tweens = []
    let recording = false
    let rec = []
    let show_cursor = false
    let cursor_override = nil

    func init(self, width, height, title = ""):
        self.width = width
        self.height = height
        self.title = title
        self.root = Widget(0, 0, width, height)
        self.root.app = self
        self.canvas = gfx.canvas(width, height, THEME.bg)

    # the whole canvas must be redrawn
    func redraw(self):
        self.needs_draw = true
        self.full = true
        self.damage = nil

    # only the rectangle [x0, x1) x [y0, y1) is stale
    func add_damage(self, x0, y0, x1, y1):
        if self.full or (self.needs_draw and self.damage == nil):
            return nil
        x0 = imax(x0, 0)
        y0 = imax(y0, 0)
        x1 = imin(x1, self.width)
        y1 = imin(y1, self.height)
        if x1 <= x0 or y1 <= y0:
            return nil
        let d = self.damage
        if d == nil:
            self.damage = [x0, y0, x1, y1]
        else:
            d[0] = imin(d[0], x0)
            d[1] = imin(d[1], y0)
            d[2] = imax(d[2], x1)
            d[3] = imax(d[3], y1)
        self.needs_draw = true

    # anything drawn above the widget tree that moves on its own (these force full redraws)
    func overlay_on(self):
        if self.modal != nil or self.popup != nil or self.ctx != nil or self.drag_on or len(self.toasts) > 0:
            return true
        let h = self.hover
        return h != nil and h.tip != "" and self.hover_ms >= TIP_DELAY

    func add(self, widget):
        self.root.add(widget)
        self.redraw()
        return widget

    # add a Box / Grid stretched over the whole window
    func fill(self, box):
        box.set_size(self.width, self.height)
        self.add(box)
        self.relayout()
        return box

    func find(self, name):
        return self.root.find(name)

    func set_theme(self, p):
        set_theme(p)
        self.redraw()

    func post(self, ev):
        self.queue.append(ev)

    func set_focus(self, w):
        if w == self.focus:
            return nil
        let old = self.focus
        self.focus = w
        if old != nil:
            old.on_focus(false)
        if w != nil:
            w.on_focus(true)
        if old != nil:
            old.dirty()
        if w != nil:
            w.dirty()

    func focus_next(self, step):
        let chain = []
        if self.modal != nil:
            self.modal.focus_chain(chain)
        else:
            self.root.focus_chain(chain)
        if len(chain) == 0:
            return nil
        let i = -1
        for k in range(len(chain)):
            if chain[k] == self.focus:
                i = k
        if i < 0:
            self.set_focus(chain[0] if step > 0 else chain[len(chain) - 1])
        else:
            self.set_focus(chain[(i + step + len(chain)) % len(chain)])

    func after(self, ms, fn):
        self.timers.append({at: self.now + ms, fn: fn, every: 0})

    func every(self, ms, fn):
        self.timers.append({at: self.now + ms, fn: fn, every: ms})

    func quit(self):
        self.running = false

    func pick_at(self, x, y):
        if self.modal != nil:
            return self.modal.pick(x, y)
        return self.root.pick(x, y)

    # Show `widget` (usually a framed Box) centred over a dimmed window; events reach only it.
    func open_modal(self, widget, on_cancel = nil):
        self.root.attach(widget, self)
        widget.parent = nil
        self.modal = widget
        self.modal_cancel = on_cancel
        self.set_focus(nil)
        self.relayout()
        self.redraw()

    func close_modal(self):
        self.modal = nil
        self.modal_cancel = nil
        self.set_focus(nil)
        self.redraw()

    func set_hover(self, w):
        if w == self.hover:
            return nil
        if self.hover != nil:
            self.hover.hover = false
        let old = self.hover
        self.hover = w
        self.hover_ms = 0
        if w != nil and w.enabled:
            w.hover = true
        if old != nil:
            old.dirty()
        if w != nil:
            w.dirty()

    # Dispatch one event now.
    func dispatch(self, ev):
        let t = ev.type
        if self.needs_layout:
            self.relayout()
        if self.recording:
            self.rec.append(ev)
        if t == "wheel":
            let hit = self.pick_at(ev.x, ev.y)
            while hit != nil:
                if hit.on_wheel(ev.dy):
                    break
                hit = hit.parent
        elif t == "mouse_move":
            self.mx = ev.x
            if self.show_cursor:
                self.redraw()
            self.my = ev.y
            let hit = self.pick_at(ev.x, ev.y)
            if hit == self.root:
                hit = nil
            self.set_hover(hit)
            let cap = self.capture
            if cap != nil:
                cap.on_drag(ev.x, ev.y)
            if self.drag_src != nil:
                self.drag_move(ev.x, ev.y)
        elif t == "mouse_down" and self.ctx != nil:
            self.ctx.click(ev.x, ev.y)
        elif t == "mouse_down" and self.popup != nil:
            self.popup.popup_click(ev.x, ev.y)
        elif t == "mouse_down":
            self.mods = ev.mods ?? ""
            self.clicks = ev.clicks ?? 1
            let hit = self.pick_at(ev.x, ev.y)
            if hit == self.root:
                hit = nil
            self.set_hover(hit)
            self.hover_ms = 0
            if hit != nil and hit.enabled:
                let fw = hit
                while fw != nil and not fw.focusable:
                    fw = fw.parent
                if fw != nil and fw.enabled:
                    self.set_focus(fw)
                if ev.button == 3:
                    self.open_context(hit, ev.x, ev.y)
                else:
                    self.capture = hit
                    hit.on_mouse_down(ev.x, ev.y, ev.button)
                    let ds = hit
                    while ds != nil and ds.drag_data == nil:
                        ds = ds.parent
                    self.drag_src = ds
                    self.drag_from = [ev.x, ev.y]
                    self.drag_on = false
            else:
                self.set_focus(nil)
        elif t == "mouse_up":
            let cap = self.capture
            self.capture = nil
            let dragged = self.drag_on
            if dragged:
                self.finish_drag(ev.x, ev.y)
            self.drag_src = nil
            if cap != nil:
                cap.on_mouse_up(ev.x, ev.y, ev.button, cap.contains(ev.x, ev.y) and not dragged)
        elif t == "key":
            let bound = self.keymap[ev.key]
            if ev.key == "Escape" and self.drag_on:
                self.cancel_drag()
            elif bound != nil and self.ctx == nil:
                bound(self)
            elif self.ctx != nil:
                self.ctx.on_key(ev.key)
            elif ev.key == "Escape" and self.popup != nil:
                self.popup.close()
            elif ev.key == "Escape" and self.modal != nil:
                let cb = self.modal_cancel
                self.close_modal()
                call0(cb, self)
            elif ev.key == "Tab":
                self.focus_next(1)
            elif ev.key == "ShiftTab":
                self.focus_next(-1)
            elif self.focus != nil and self.focus.enabled:
                self.focus.on_key(ev.key)
        elif t == "text":
            if self.focus != nil and self.focus.enabled:
                self.focus.on_text(ev.text)
        elif t == "tick":
            self.step(ev.ms)
        elif t == "quit":
            self.running = false
        else:
            throw error("ValueError", "unknown event type: " .. to_string(t))

    # advance the virtual clock (recorded as a tick event)
    func advance(self, ms):
        self.dispatch(tick(ms))

    # Tween a number from `a` to `b` over `ms` virtual milliseconds, calling on_update(value)
    # after every step. ease: "linear" | "in" | "out" | "in_out".
    func animate(self, a, b, ms, on_update, ease = "out", on_done = nil):
        ease_value(ease, 0)
        self.tweens.append({from: a, to: b, ms: ms, t: 0, up: on_update, ease: ease, done: on_done})
        on_update(a)
        self.redraw()

    func run_tweens(self, dt):
        let live = []
        let finished = []
        for tw in self.tweens:
            tw.t += dt
            let p = 1000 if tw.ms <= 0 or tw.t >= tw.ms else tw.t * 1000 // tw.ms
            tw.up(tw.from + (tw.to - tw.from) * ease_value(tw.ease, p) // 1000)
            if p >= 1000:
                finished.append(tw)
            else:
                live.append(tw)
        self.tweens = live
        for tw in finished:
            if tw.done != nil:
                tw.done(self)
        self.redraw()

    # open a floating menu at (x, y); entries as for MenuBar
    func context_menu(self, x, y, entries):
        self.popup = nil
        self.ctx = PopupMenu(self, entries, x, y)
        self.redraw()
        return self.ctx

    func open_context(self, w, x, y):
        let src = w
        while src != nil and src.context == nil:
            src = src.parent
        if src != nil:
            let es = src.context
            if type(es) == "function":
                es = es(src)
            if es != nil and len(es) > 0:
                self.context_menu(x, y, es)

    # show a short message in the lower right corner for `ms` virtual milliseconds
    func toast(self, msg, ms = 2000):
        self.toasts.append({text: msg, left: ms})
        self.redraw()

    func step(self, ms):
        self.now += ms
        if self.hover != nil and self.hover.tip != "":
            let before = self.hover_ms
            self.hover_ms += ms
            if before < TIP_DELAY and self.hover_ms >= TIP_DELAY:
                self.redraw()
        if len(self.toasts) > 0:
            let live = []
            for tt in self.toasts:
                tt.left -= ms
                if tt.left > 0:
                    live.append(tt)
            self.toasts = live
            self.redraw()
        if len(self.tweens) > 0:
            self.run_tweens(ms)
        self.root.tick_all(ms)
        let due = []
        let rest = []
        for tm in self.timers:
            if tm.at <= self.now:
                due.append(tm)
            else:
                rest.append(tm)
        self.timers = rest
        for tm in due:
            tm.fn(self)
            if tm.every > 0:
                tm.at += tm.every
                self.timers.append(tm)

    # Process every queued event; returns how many were handled.
    func pump(self):
        let n = 0
        while len(self.queue) > 0 and self.running:
            let batch = self.queue
            self.queue = []
            for ev in batch:
                if self.running:
                    self.dispatch(ev)
                    n += 1
        return n

    # Event loop: `source()` returns the next event or nil at end of input.
    # Redraws after every event when `on_frame` is set and the UI changed.
    func run(self, source, on_frame = nil):
        while self.running:
            let ev = source()
            if ev == nil:
                break
            self.dispatch(ev)
            self.pump()
            if on_frame != nil and self.needs_draw:
                self.draw()
                on_frame(self)
        return self.frames

    func relayout(self):
        # a container may ask for another pass while laying out (a Flow whose height changed)
        for pass_no in range(4):
            self.needs_layout = false
            self.root.layout()
            if self.modal != nil:
                self.modal.layout()
                self.modal.x = (self.width - self.modal.w) // 2
                self.modal.y = (self.height - self.modal.h) // 2
            if not self.needs_layout:
                break
        self.needs_layout = false

    func draw(self):
        let c = self.canvas
        if self.needs_layout:
            self.relayout()
        let ov = self.overlay_on()
        let part = not self.full and self.damage != nil and not ov and not self.ov_prev and not self.show_cursor
        self.ov_prev = ov
        if part:
            let d = self.damage
            c.set_clip(d[0], d[1], d[2] - d[0], d[3] - d[1])
            c.fill_rect(d[0], d[1], d[2] - d[0], d[3] - d[1], THEME.bg)
            self.dclip = d
            CLIPS.append(d)
            self.draw_root(c)
            CLIPS.pop()
            self.dclip = nil
            c.clear_clip()
            self.last_rect = [d[0], d[1], d[2] - d[0], d[3] - d[1]]
            self.needs_draw = false
            self.damage = nil
            self.frames += 1
            return c
        c.clear_clip()
        c.clear(THEME.bg)
        self.draw_root(c)
        self.last_rect = [0, 0, self.width, self.height]
        if self.modal != nil:
            c.set_alpha(110)
            c.fill_rect(0, 0, self.width, self.height, THEME.shadow)
            c.set_alpha(255)
            self.modal.draw(c)
        if self.popup != nil:
            self.popup.draw_popup(c)
        if self.ctx != nil:
            self.ctx.draw(c)
        self.draw_tip(c)
        self.draw_toasts(c)
        self.draw_drag(c)
        if self.show_cursor:
            draw_cursor(c, self.cursor_name(), self.mx, self.my)
        self.needs_draw = false
        self.full = false
        self.damage = nil
        self.frames += 1
        return c

    func draw_root(self, c):
        STAMP += 1
        let r = self.root
        r.cax = r.x
        r.cay = r.y
        r.cs = STAMP
        r.draw(c)
        STAMP += 1

    # name of the cursor to show now: an explicit override, "grab" while dragging, else the widget under it
    func cursor_name(self):
        if self.cursor_override != nil:
            return self.cursor_override
        if self.drag_on:
            return "grab"
        let w = self.capture if self.capture != nil else self.hover
        return "arrow" if w == nil else w.cursor_kind()

    func draw_tip(self, c):
        let h = self.hover
        if h == nil or h.tip == "" or self.hover_ms < TIP_DELAY or self.popup != nil or self.ctx != nil:
            return nil
        let w = gfx.text_advance(h.tip) + 10
        let x = clamp(self.mx + 8, 0, imax(0, self.width - w))
        let y = clamp(self.my + 16, 0, imax(0, self.height - 16))
        c.fill_rect(x + 2, y + 2, w, 15, THEME.shadow)
        c.fill_rect(x, y, w, 15, THEME.field)
        c.rect(x, y, w, 15, THEME.accent)
        c.text(x + 5, y + 4, h.tip, THEME.text)

    func draw_toasts(self, c):
        let y = self.height - 6
        let i = len(self.toasts) - 1
        while i >= 0:
            let tt = self.toasts[i]
            let w = gfx.text_advance(tt.text) + 16
            y -= 20
            c.set_alpha(255 if tt.left >= 300 else tt.left * 255 // 300)
            c.fill_round_rect(self.width - w - 6, y, w, 18, 3, THEME.panel)
            c.round_rect(self.width - w - 6, y, w, 18, 3, THEME.accent)
            c.text(self.width - w + 2, y + 6, tt.text, THEME.text)
            c.set_alpha(255)
            y -= 4
            i -= 1

    # ---- recording / replay / snapshots ----
    func start_recording(self):
        self.rec = []
        self.recording = true

    func stop_recording(self):
        self.recording = false
        return self.rec

    func replay(self, events, on_frame = nil):
        for ev in events:
            self.dispatch(ev)
            self.pump()
            if on_frame != nil and self.needs_draw:
                self.draw()
                on_frame(self)

    func screenshot(self, path):
        self.draw().save_png(path)

    # hash of the rendered window: equal hashes mean identical pixels
    func snapshot(self):
        return self.draw().hash()

    # ---- scripted input helpers (handy for tests and demos) ----
    func click(self, x, y):
        self.dispatch(mouse_move(x, y))
        self.dispatch(mouse_down(x, y))
        self.dispatch(mouse_up(x, y))
        self.pump()

    # ---- drag and drop: a widget with `drag_data` (a value, or function(widget) -> value) can be dragged onto a
    # widget (or one of its ancestors) with `on_drop(target, data, source)`; `drop_ok(target, data)` may refuse.
    func drag_move(self, x, y):
        let src = self.drag_src
        if not self.drag_on:
            let dx = x - self.drag_from[0]
            let dy = y - self.drag_from[1]
            if (dx if dx > 0 else -dx) + (dy if dy > 0 else -dy) < 6:
                return nil
            self.drag_on = true
            let d = src.drag_data
            self.drag_payload = d(src) if type(d) == "function" else d
        self.drop_hot = self.find_drop(x, y)
        self.redraw()

    func find_drop(self, x, y):
        let w = self.pick_at(x, y)
        while w != nil:
            if w.on_drop != nil and w != self.drag_src and (w.drop_ok == nil or w.drop_ok(w, self.drag_payload)):
                return w
            w = w.parent
        return nil

    func finish_drag(self, x, y):
        let target = self.find_drop(x, y)
        let data = self.drag_payload
        let src = self.drag_src
        self.cancel_drag()
        if target != nil:
            target.on_drop(target, data, src)

    func cancel_drag(self):
        self.drag_on = false
        self.drag_payload = nil
        self.drop_hot = nil
        self.redraw()

    func draw_drag(self, c):
        if not self.drag_on:
            return nil
        let h = self.drop_hot
        if h != nil:
            c.rect(h.abs_x() - 1, h.abs_y() - 1, h.w + 2, h.h + 2, THEME.ok)
            c.rect(h.abs_x(), h.abs_y(), h.w, h.h, THEME.ok)
        let src = self.drag_src
        let lab = src.drag_label if src.drag_label != "" else to_string(self.drag_payload)
        let w = gfx.text_advance(lab) + 10
        let x = clamp(self.mx + 8, 0, imax(0, self.width - w))
        let y = clamp(self.my + 8, 0, imax(0, self.height - 15))
        c.set_alpha(200)
        c.fill_round_rect(x, y, w, 15, 3, THEME.accent_dark)
        c.set_alpha(255)
        c.round_rect(x, y, w, 15, 3, THEME.accent)
        c.text(x + 5, y + 4, lab, THEME.text)

    # run fn(app) when the key combination ("Ctrl+S", "F5", ...) is pressed, before the focused widget sees it
    func bind(self, combo, fn):
        self.keymap[combo] = fn

    func unbind(self, combo):
        self.keymap[combo] = nil

    func double_click(self, x, y):
        self.dispatch(mouse_move(x, y))
        self.dispatch(mouse_down(x, y, 1, "", 2))
        self.dispatch(mouse_up(x, y))
        self.pump()

    func triple_click(self, x, y):
        self.dispatch(mouse_move(x, y))
        self.dispatch(mouse_down(x, y, 1, "", 3))
        self.dispatch(mouse_up(x, y))
        self.pump()

    # click with modifiers held, e.g. app.click_with(x, y, "Shift")
    func click_with(self, x, y, mods):
        self.dispatch(mouse_move(x, y))
        self.dispatch(mouse_down(x, y, 1, mods))
        self.dispatch(mouse_up(x, y, 1, mods))
        self.pump()

    func right_click(self, x, y):
        self.dispatch(mouse_move(x, y))
        self.dispatch(mouse_down(x, y, 3))
        self.dispatch(mouse_up(x, y, 3))
        self.pump()

    func hover_at(self, x, y):
        self.dispatch(mouse_move(x, y))
        self.pump()

    func drag(self, x0, y0, x1, y1):
        self.dispatch(mouse_move(x0, y0))
        self.dispatch(mouse_down(x0, y0))
        self.dispatch(mouse_move(x1, y1))
        self.dispatch(mouse_up(x1, y1))
        self.pump()

    func type_text(self, s):
        self.dispatch(text(s))
        self.pump()

    func press(self, k):
        self.dispatch(key(k))
        self.pump()

    func scroll(self, x, y, dy):
        self.dispatch(wheel(x, y, dy))
        self.pump()
