import gui

let rows = []
for i in range(60):
    rows.append(["item" .. to_string(i), i * 37 % 101, "k" .. to_string(i % 5)])
let nodes = [gui.node("root", [gui.node("a", [gui.node("a1"), gui.node("a2")]), gui.node("b")])]
nodes[0].open = true
let table = gui.table([["name", 80], ["n", 50], "kind"], rows, 220, 150)
let tree = gui.tree(nodes, 100, 150)
let ui = gui.column([
    gui.menubar([["File", [["Open", nil]]], ["Edit", [["Copy", nil]]]]),
    gui.splitter(gui.row([tree, gui.column([gui.label("hello"), gui.spinner(3), gui.radio(["a", "b", "c"])])]), table, false, 200)
], 0, 0)
let app = gui.App(420, 220)
app.fill(ui)
let acc = 0
for i in range(400):
    table.sort_by(i % 3, i % 2 == 0)
    app.click(tree.abs_x() + 20, tree.abs_y() + 4 + (i % 4) * 12)
    table.select_pos(i % 50)
    acc += app.snapshot() % 1000
print(acc)
