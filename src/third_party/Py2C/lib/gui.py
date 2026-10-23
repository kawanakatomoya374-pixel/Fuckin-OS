# gui: Py2C 同梱の小さな GUI ライブラリ（tkinter の一部と同じ名前）。
# import gui と書くと、変換器がこの内容を先頭に加える。gui.Name は Name として扱われる。
# 描画は _gui_* の関数（ランタイム）で行う。レイアウトは格子（grid）だけ。
END = "end"
CELL_W = 80
CELL_H = 48
GAP = 4
ORIGIN = 8
COLOR_BG = 0x101010
COLOR_KEY = 0x303840
COLOR_TEXT = 0xF0F0F0
COLOR_BORDER = 0x9AA5B1

_windows = []

class Tk:
    def __init__(self, width=320, height=240):
        self.title_text = ""
        self.width = width
        self.height = height
        self.widgets = []
        _gui_open(width, height)
        _windows.append(self)

    def title(self, value):
        self.title_text = value

class Button:
    def __init__(self, master, text="", command=None):
        self.kind = "button"
        self.text = text
        self.command = command
        self.row = 0
        self.column = 0
        self.span = 1
        master.widgets.append(self)

    def grid(self, row=0, column=0, columnspan=1):
        self.row = row
        self.column = column
        self.span = columnspan

class Entry:
    def __init__(self, master, width=16, justify="left"):
        self.kind = "entry"
        self.value = ""
        self.row = 0
        self.column = 0
        self.span = 1
        master.widgets.append(self)

    def grid(self, row=0, column=0, columnspan=1):
        self.row = row
        self.column = column
        self.span = columnspan

    def get(self):
        return self.value

    def insert(self, index, text):
        self.value = self.value + str(text)

    def delete(self, first, last=None):
        self.value = ""

def _place(widget):
    widget.x = ORIGIN + widget.column * (CELL_W + GAP)
    widget.y = ORIGIN + widget.row * (CELL_H + GAP)
    widget.w = widget.span * CELL_W + (widget.span - 1) * GAP
    widget.h = CELL_H

def _draw_box(x, y, w, h, color):
    _gui_fill(x, y, w, h, color)
    _gui_fill(x, y, w, 1, COLOR_BORDER)
    _gui_fill(x, y + h - 1, w, 1, COLOR_BORDER)
    _gui_fill(x, y, 1, h, COLOR_BORDER)
    _gui_fill(x + w - 1, y, 1, h, COLOR_BORDER)

def _draw(root):
    _gui_clear(COLOR_BG)
    for widget in root.widgets:
        _place(widget)
        if widget.kind == "entry":
            _draw_box(widget.x, widget.y, widget.w, widget.h, COLOR_KEY)
            _gui_text(widget.x + 4, widget.y + (widget.h - 16) // 2, widget.value, COLOR_TEXT)
        else:
            _draw_box(widget.x, widget.y, widget.w, widget.h, COLOR_KEY)
            text_x = widget.x + (widget.w - len(widget.text) * 8) // 2
            _gui_text(text_x, widget.y + (widget.h - 16) // 2, widget.text, COLOR_TEXT)

def _inside(widget, px, py):
    return px >= widget.x and px < widget.x + widget.w and py >= widget.y and py < widget.y + widget.h

def _dispatch(root, px, py):
    index = len(root.widgets) - 1
    while index >= 0:
        widget = root.widgets[index]
        if widget.kind == "button" and _inside(widget, px, py):
            if widget.command is not None:
                widget.command()
            return
        index = index - 1

def mainloop():
    root = _windows[0]
    _draw(root)
    while True:
        event = _gui_next_event()
        if event is None:
            break
        _dispatch(root, event[0], event[1])
        _draw(root)

def update():
    mainloop()
