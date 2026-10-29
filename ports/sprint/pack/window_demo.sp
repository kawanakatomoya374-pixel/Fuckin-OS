import gui
import window
import cosgui

let count = 0
let lbl = gui.label("Hello from Sprint on C-OS")
let btn = gui.button("Click me", a => lbl.set_text("clicked"))
let app = gui.App(420, 180, "demo")
app.fill(gui.column([lbl, btn], 8, 12))
app.relayout()
let win = window.open(420, 180, "Sprint window", "cos")
window.run(app, win, -1, (a, w) => true)
