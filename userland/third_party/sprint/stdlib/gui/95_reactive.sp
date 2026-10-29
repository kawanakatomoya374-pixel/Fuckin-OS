# ---- reactive values and one-line windows ---------------------------------------
# A State holds a value; widgets built with *_of(state) stay in sync with it in both directions, so an
# interface is described by its state instead of by event handlers that copy values around.
#
#   let count = gui.state(0)
#   gui.show(gui.column([
#       gui.label_of(count, func(n): "clicked " .. to_string(n) .. " times"),
#       gui.button("+1", func(b): count.update(func(n): n + 1)),
#       gui.slider_of(0, 10, count)
#   ]), "counter")
export class State:
    let value = nil
    let subs = []
    func init(self, v):
        self.value = v
        self.subs = []
    func get(self):
        return self.value
    func set(self, v):
        if v == self.value:
            return nil
        self.value = v
        for f in self.subs.copy():
            f(v)
    func update(self, f):
        self.set(f(self.value))
    # call f now and after every change
    func watch(self, f):
        self.subs.append(f)
        f(self.value)
        return self

export func state(v):
    return State(v)

# a value computed from other states; it follows them
export func derived(sources, f):
    let vals = []
    for s in sources:
        vals.append(s.get())
    let out = State(f(vals))
    for i in range(len(sources)):
        let idx = i
        func upd(v):
            vals[idx] = v
            out.set(f(vals))
        sources[i].subs.append(upd)
    return out

export func label_of(st, fmt = nil, scale = 1):
    let l = Label("", 0, 0, scale)
    func show(v):
        l.set_text(to_string(v) if fmt == nil else fmt(v))
        l.relayout()
    st.watch(show)
    return l

export func checkbox_of(text, st):
    let cb = Checkbox(text, 0, 0, st.get(), nil)
    func push(c):
        st.set(c.checked)
    cb.on_change = push
    func pull(v):
        cb.checked = v
        cb.dirty()
    st.watch(pull)
    return cb

export func slider_of(lo, hi, st):
    let s = Slider(0, 0, 120, lo, hi, st.get(), nil)
    func push(w):
        st.set(w.value)
    s.on_change = push
    func pull(v):
        if s.value != v:
            s.set_value(v)
    st.watch(pull)
    return s

export func input_of(st, placeholder = ""):
    let t = TextInput(0, 0, 140, to_string(st.get()))
    t.placeholder = placeholder
    func push(w):
        st.set(w.text)
    t.on_change = push
    func pull(v):
        if t.text != v:
            t.set_text(v)
    st.watch(pull)
    return t

export func progress_of(st, maximum = 100):
    let p = ProgressBar(0, 0, 120, 10, st.get(), maximum)
    func pull(v):
        p.set_value(v)
    st.watch(pull)
    return p

# show/hide a widget with a boolean state
export func visible_when(widget, st):
    func pull(v):
        widget.set_visible(v)
        widget.relayout()
    st.watch(pull)
    return widget

# a window sized to its content: the interface, laid out at its preferred size
export func show(ui, title = "", min_w = 0, min_h = 0):
    let p = ui.pref()
    let app = App(imax(p[0], min_w), imax(p[1], min_h), title)
    app.fill(ui)
    return app
