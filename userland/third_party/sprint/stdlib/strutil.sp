# strutil: string helpers (written in Sprint, bundled). Widths count code points.
import str

export func pad_left(s, n, ch = " "):
    let k = n - s.char_count()
    return ch.repeat(k) .. s if k > 0 else s

export func pad_right(s, n, ch = " "):
    let k = n - s.char_count()
    return s .. ch.repeat(k) if k > 0 else s

export func center(s, n, ch = " "):
    let k = n - s.char_count()
    if k <= 0:
        return s
    let l = k // 2
    return ch.repeat(l) .. s .. ch.repeat(k - l)

export func capitalize(s):
    if len(s) == 0:
        return s
    return s.slice(0, 1).upper() .. s.slice(1).lower()

export func title(s):
    return words(s).map(capitalize).join(" ")

export func words(s):
    return s.split(" ").filter(w => len(w) > 0)

export func is_digits(s):
    if len(s) == 0:
        return false
    for i in range(len(s)):
        if s[i] < 48 or s[i] > 57:
            return false
    return true

export func is_alpha(s):
    if len(s) == 0:
        return false
    for i in range(len(s)):
        let c = s[i] | 32
        if c < 97 or c > 122:
            return false
    return true

export func is_space(s):
    if len(s) == 0:
        return false
    for i in range(len(s)):
        let c = s[i]
        if c != 32 and (c < 9 or c > 13):
            return false
    return true

export func truncate(s, n, ellipsis = "..."):
    if s.char_count() <= n:
        return s
    let keep = n - ellipsis.char_count()
    if keep < 0:
        keep = 0
    let cut = 0
    let seen = 0
    while cut < len(s) and seen < keep:
        cut += 1
        while cut < len(s) and not s.is_boundary(cut):
            cut += 1
        seen += 1
    return s.slice(0, cut) .. ellipsis

export func indent(s, prefix = "    "):
    return s.split("\n").map(l => prefix .. l if len(l) > 0 else l).join("\n")

export func wrap(s, width):
    let lines = []
    let cur = ""
    for w in words(s):
        if len(cur) == 0:
            cur = w
        elif cur.char_count() + 1 + w.char_count() <= width:
            cur = cur .. " " .. w
        else:
            lines.append(cur)
            cur = w
    if len(cur) > 0:
        lines.append(cur)
    return lines

export func reverse(s):
    let cps = s.codepoints()
    let out = []
    for cp in cps:
        out.append(str.from_codepoint(cp))
    out.reverse()
    return out.join("")

export func lines(s):
    return s.split_lines()
