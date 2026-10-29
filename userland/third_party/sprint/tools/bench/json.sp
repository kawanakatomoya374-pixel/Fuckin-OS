import json
let doc = {name: "x", items: [], meta: {a: 1, b: [1, 2, 3], c: "text"}}
for i in range(2000):
    doc.items.append({id: i, tags: ["a", "b", "c"], v: i * 1.5})
let total = 0
for r in range(20):
    let s = json.stringify(doc)
    let d = json.parse(s)
    total += len(d.items) + len(s)
print(total)
