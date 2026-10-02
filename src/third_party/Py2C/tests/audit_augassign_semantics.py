log = []

class Box:
    def __init__(self):
        self.value = 1

box = Box()

def get_box():
    log.append("box")
    return box

def get_list():
    log.append("list")
    return [10]

def get_index():
    log.append("index")
    return 0

get_box().value += 4
items = get_list()
items[get_index()] += 3
print(box.value, items, log)
