# ---- terse constructors (for use with the layout containers) ---------------------
export func label(text, scale = 1, wrap = 0):
    let l = Label(text, 0, 0, scale)
    if wrap > 0:
        l.set_wrap(wrap)
    return l
export func button(text, on_click = nil):
    return Button(text, 0, 0, 0, 0, on_click)
export func checkbox(text, checked = false, on_change = nil):
    return Checkbox(text, 0, 0, checked, on_change)
export func slider(lo, hi, value, on_change = nil):
    return Slider(0, 0, 120, lo, hi, value, on_change)
export func input(text = "", placeholder = ""):
    let t = TextInput(0, 0, 140, text)
    t.placeholder = placeholder
    return t
export func progress(value = 0, maximum = 100):
    return ProgressBar(0, 0, 120, 10, value, maximum)
export func listbox(items, on_select = nil, rows = 4):
    return ListBox(0, 0, 140, rows * 10 + 4, items, on_select)
export func separator(vertical = false):
    return Separator(vertical)
export func image(canvas, zoom = 1):
    return ImageView(canvas, zoom)
export func menubar(menus):
    return MenuBar(menus)
export func tree(roots, w = 160, h = 100, on_select = nil):
    return Tree(roots, w, h, on_select)
export func table(columns, rows = [], w = 240, h = 120, on_select = nil):
    return Table(columns, rows, w, h, on_select)
export func splitter(a, b, vertical = false, pos = 100):
    return Splitter(a, b, vertical, pos)
export func spinner(value = 0, lo = 0, hi = 100, step = 1, on_change = nil):
    return Spinner(value, lo, hi, step, on_change)
export func radio(items, selected = 0, on_change = nil):
    return RadioGroup(items, selected, on_change)
export func flow(children, hgap = 6, vgap = 6, pad = 6):
    return Flow(children, hgap, vgap, pad)
export func stack(children):
    return Stack(children)
export func expander(title, content, is_open = true):
    return Expander(title, content, is_open)
export func tabs(labels, pages, on_change = nil):
    return Tabs(labels, pages, on_change)
export func dropdown(items, selected = 0, on_change = nil):
    return Dropdown(items, selected, on_change)
export func textarea(text = "", rows = 5):
    return TextArea(0, 0, 200, rows * 10 + 6, text)
export func spacer(flex = 1):
    let w = Widget(0, 0, 0, 0)
    w.flex = flex
    return w
export func column(children, gap = 6, pad = 6):
    return Box(true, children, gap, pad)
export func row(children, gap = 6, pad = 6):
    return Box(false, children, gap, pad)
export func grid(cols, children, gap = 6, pad = 6):
    return Grid(cols, children, gap, pad)
export func scroll(content, w, h):
    return ScrollView(content, w, h)
# a titled panel around one child
export func panel(title, child, pad = 8):
    let t = Label(title)
    t.dim = true
    let p = Box(true, [t, child], 4, pad)
    p.framed = true
    return p

# ---- layout helpers ------------------------------------------------------------
# Place widgets in a column / row starting at (x, y); returns the end coordinate.
export func vstack(widgets, x, y, gap = 6):
    for w in widgets:
        w.x = x
        w.y = y
        y += w.h + gap
    return y

export func hstack(widgets, x, y, gap = 6):
    for w in widgets:
        w.x = x
        w.y = y
        x += w.w + gap
    return x
