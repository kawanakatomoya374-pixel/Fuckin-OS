import gui

def button_click(value):
    if value == "C":
        entry.delete(0, gui.END)
    elif value == "=":
        result = _gui_eval(entry.get())
        entry.delete(0, gui.END)
        entry.insert(gui.END, result)
    else:
        entry.insert(gui.END, value)

root = gui.Tk(340, 280)
root.title("MY CALC")
entry = gui.Entry(root, width=16, justify="right")
entry.grid(row=0, column=0, columnspan=4)

buttons = [["7", "8", "9", "/"], ["4", "5", "6", "*"], ["1", "2", "3", "-"], ["0", "C", "=", "+"]]
for r, row in enumerate(buttons):
    for c, v in enumerate(row):
        gui.Button(root, text=v, command=lambda v=v: button_click(v)).grid(row=r + 1, column=c)

_gui_click(48, 84)
_gui_click(300, 240)
_gui_click(132, 84)
_gui_click(216, 240)
gui.mainloop()
_gui_save("build/calc_gui.ppm")
print(entry.get())
