# ---- mouse cursor ---------------------------------------------------------------------
# Software cursor sprites. '#' is black (with a white halo), '.' white, anything else transparent.
# Each entry is [hot_x, hot_y, rows]. The cursor is drawn by App.draw() when App.show_cursor is set.
func build_cursors():
    return {
        arrow: [0, 0, ["#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#", "#.......#",
                       "#....####", "#..#..#", "#.# #..#", "##  #..#", "#    #..#", "     ##"]],
        ibeam: [3, 5, ["###.###", "  .#.  ", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ",
                       "   #   ", "  .#.  ", "###.###"]],
        hand: [4, 0, ["  #", " #.#", " #.#", " #.#", " #.###", " #.#.#.#", "###.#.#.##", "#.#.....#", "#.......#",
                      " #......#", " #.....#", "  #....#", "  #...#", "  #####"]],
        grab: [5, 5, ["", "  ## ## ##", " #..#..#..#", " #........##", "##.........#", "#..........#", "#.........#",
                      " #........#", "  #......#", "   ######"]],
        move: [6, 6, ["      #", "     #.#", "    #...#", "   #######", "     #.#", "  #  #.#  #", " #.# #.# #.#", "#..#######..#",
                      " #.# #.# #.#", "  #  #.#  #", "     #.#", "   #######", "    #...#", "     #.#", "      #"]],
        resize_h: [7, 3, ["   #     #", "  ##     ##", " #.#######.#", "#...........#", " #.#######.#", "  ##     ##", "   #     #"]],
        resize_v: [3, 7, ["   #", "  #.#", " #...#", "#######", "  #.#", "  #.#", "  #.#", "  #.#", "  #.#", "#######", " #...#", "  #.#", "   #"]],
        crosshair: [5, 5, ["     #", "     #", "     #", "     #", "     #", "#### ####", "     #", "     #", "     #", "     #", "     #"]],
        wait: [5, 6, ["###########", " #.......# ", "  #.....#  ", "   #...#   ", "    #.#    ", "     #     ", "    #.#    ", "   #...#   ",
                      "  #.....#  ", " #.......# ", "###########"]],
        no: [5, 5, ["   #####", "  #.....#", " #...#..#", "#...#...#", "#..#....#", "#.#.....#", "##......#", " #.....#", "  #####"]]
    }

let cursor_cache = nil

# name -> [hot_x, hot_y, rows]; built on first use so that importing gui stays cheap
export func cursors():
    if cursor_cache == nil:
        cursor_cache = build_cursors()
    return cursor_cache

func put_sprite(c, sp, x, y, ch, color, grow):
    let rows = sp[2]
    for j in range(len(rows)):
        let row = rows[j]
        for i in range(len(row)):
            if row[i] == ch:
                let px = x - sp[0] + i
                let py = y - sp[1] + j
                if grow:
                    c.pixel(px - 1, py, color)
                    c.pixel(px + 1, py, color)
                    c.pixel(px, py - 1, color)
                    c.pixel(px, py + 1, color)
                else:
                    c.pixel(px, py, color)

# draw cursor `name` with its hot spot at (x, y); unknown names draw "arrow"
export func draw_cursor(c, name, x, y):
    let all = cursors()
    let sp = all[name] if name in all else all.arrow
    put_sprite(c, sp, x, y, 35, 0xffffff, true)
    put_sprite(c, sp, x, y, 46, 0xffffff, false)
    put_sprite(c, sp, x, y, 35, 0x000000, false)
