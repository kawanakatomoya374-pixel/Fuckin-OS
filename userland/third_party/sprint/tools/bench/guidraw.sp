import gui
# redraw cost of a busy window: 120 labels/buttons/checkboxes, a table and a text area, drawn repeatedly
let kids = []
for i in range(40):
    kids.append(gui.row([gui.label("row " .. to_string(i)), gui.button("Go " .. to_string(i)), gui.checkbox("opt", i % 2 == 0), gui.slider(0, 100, i * 2)], 4, 2))
let rows = []
for i in range(80):
    rows.append(["item" .. to_string(i), i * 37 % 101, "k" .. to_string(i % 5)])
let ui = gui.row([gui.scroll(gui.column(kids, 2, 4), 360, 380), gui.column([gui.table([["name", 80], ["n", 50], "kind"], rows, 240, 200), gui.textarea("hello world\nsecond line with some more text\nthird", 6)])], 8, 8)
let app = gui.App(640, 400)
app.fill(ui)
let acc = 0
for i in range(300):
    let c = app.draw()
    acc += c.get(i % 640, 7)
print(acc % 1000)
