# A calculator with the Py2C `gui` library (tkinter-like: Tk, Button, Entry, grid, mainloop).
# On C-OS it opens a window: click the keys; close the window to end. Every key and the result are also printed.
import gui

def button_click(value):
    print("key", value)
    if value == "C":
        entry.delete(0, gui.END)
    elif value == "=":
        result = _gui_eval(entry.get())
        print("result", result)
        entry.delete(0, gui.END)
        entry.insert(gui.END, result)
    else:
        entry.insert(gui.END, value)

root = gui.Tk(340, 280)
root.title("Py2C calc")
entry = gui.Entry(root, width=16, justify="right")
entry.grid(row=0, column=0, columnspan=4)

buttons = [["7", "8", "9", "/"], ["4", "5", "6", "*"], ["1", "2", "3", "-"], ["0", "C", "=", "+"]]
for r, row in enumerate(buttons):
    for c, v in enumerate(row):
        gui.Button(root, text=v, command=lambda v=v: button_click(v)).grid(row=r + 1, column=c)

gui.mainloop()
print("closed", entry.get())
