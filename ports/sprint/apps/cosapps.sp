# C-OS Apps, the store, in Sprint. The screens and the logic are here; the parts that need speed or care are the C natives of `_cos` (ports/sprint/cosapi.c):
# network transfers, the signature check of the catalog, image decoding, starting the installer. Files go through Sprint's own sandboxed `fs`.
#
#   sprint run --allow-ffi --allow-read=/apps --allow-read=/system --allow-read=/desktop --allow-write=/apps --allow-write=/desktop cosapps.sp
import gui
import gfx
import window
import cosgui
import fs
import str
import _cos

const BASE_DEFAULT = "https://kawanakatomoya374-pixel.github.io/C-OS-APPS/apps/"
const INSTALLER = "/apps/linux/LinuxInstall.c-os"
const ICON_DIR = "/system/apps/icons"
const JA = (_cos.prefs() & 2) != 0

func tr(en, ja):
    return ja if JA else en

# fs.exists() raises when even the folder above is missing: here that simply means "no"
func exists(path):
    try:
        return fs.exists(path)
    catch:
        return false

func read_text(path):
    if exists(path):
        return fs.read_file(path)
    return nil

func base_url():
    let u = BASE_DEFAULT
    let t = read_text("/system/apps/url.txt")
    if t != nil and t.starts_with("http"):
        u = t.split("\n")[0].trim()
    if not u.ends_with("/"):
        u = u .. "/"
    return u

func warnings():
    let t = read_text("/system/apps/uninstall_warnings")
    if t != nil and len(t) > 0 and t[0] >= 48 and t[0] <= 51:
        return t[0] - 48
    return 3

func fmt_size(b):
    if b >= 1048576:
        return to_string(b // 1048576) .. "." .. to_string((b * 10 // 1048576) % 10) .. " MB"
    return to_string((b + 1023) // 1024) .. " KB"

# ---- the catalog: "app id=... title=... version=... download=N installed=N icon=... iconsha=... desc=..." lines of the verified body ----
func parse_catalog(body):
    let out = []
    for line in body.split("\n"):
        if not line.starts_with("app "):
            continue
        let a = {id: "", title: "", version: "", desc: "", icon: "", iconsha: "", download: 0, installed: 0}
        for tok in line.slice(4).split(" "):
            let eq = tok.find("=")
            if eq < 0:
                continue
            let k = tok.slice(0, eq)
            let v = tok.slice(eq + 1)
            if k == "download" or k == "installed":
                a[k] = to_int(v)
            elif k == "title" or k == "desc":
                a[k] = v.replace("_", " ")
            else:
                a[k] = v
        if a.id != "" and a.title != "":
            out.append(a)
    return out

func installed_version(a):
    let t = read_text("/apps/" .. a.id .. "/pack.txt")
    if t == nil:
        return nil
    for l in t.split("\n"):
        if l.starts_with("version "):
            return l.slice(8)
    return ""

func rm_tree(path):
    if fs.is_dir(path):
        for name in fs.list_dir(path):
            rm_tree(path .. "/" .. name)
    fs.remove(path)

# ---- widgets ----
class Icon extends gui.Widget:
    let cv = nil
    override func init(self, size):
        super(0, 0, size, size)
    func load(self, path):
        let cv = gfx.canvas(self.w, self.h, gui.THEME.bg)
        if _cos.image_blit(cv, path, 0, 0, self.w):
            self.cv = cv
            self.dirty()
    override func draw_self(self, c, ax, ay):
        if self.cv != nil:
            c.blit(self.cv, ax, ay)
        else:
            c.fill_round_rect(ax, ay, self.w, self.h, 8, gui.THEME.border)

class IconButton extends gui.Widget:
    let kind = "download"
    let on_click = nil
    let state = "on"            # on | off | armed
    override func init(self, kind, on_click):
        super(0, 0, 40, 40)
        self.kind = kind
        self.on_click = on_click
    override func cursor_kind(self):
        return "hand"
    override func on_mouse_up(self, px, py, button, inside):
        if inside and self.state != "off":
            self.on_click(self)
    override func draw_self(self, c, ax, ay):
        let on = self.state != "off"
        let bg = 0x2e323c
        if self.kind == "download" and on:
            bg = 0x4cc38a if self.hover else 0x3a8c4c
        elif self.kind == "trash" and on:
            bg = 0x6e4246 if self.hover else 0x46323a
        c.fill_round_rect(ax, ay, 40, 40, 10, bg)
        let fg = 0xffffff if on else 0x5a5e68
        let cx = ax + 20
        let cy = ay + 20
        if self.kind == "download":
            c.fill_rect(cx - 2, cy - 11, 5, 12, fg)
            c.fill_triangle(cx - 8, cy, cx + 8, cy, cx, cy + 10, fg)
            c.fill_rect(cx - 11, cy + 12, 23, 3, fg)
            c.fill_rect(cx - 11, cy + 8, 3, 5, fg)
            c.fill_rect(cx + 9, cy + 8, 3, 5, fg)
        else:
            c.fill_rect(cx - 10, cy - 9, 21, 3, fg)
            c.fill_rect(cx - 3, cy - 13, 7, 3, fg)
            c.fill_rect(cx - 8, cy - 5, 17, 18, fg)
            c.fill_rect(cx - 4, cy - 2, 2, 11, bg)
            c.fill_rect(cx + 1, cy - 2, 2, 11, bg)

class Bar extends gui.Widget:
    let pct = 0
    override func init(self):
        super(0, 0, 300, 10)
    func set_pct(self, p):
        if p != self.pct:
            self.pct = p
            self.dirty()
    override func draw_self(self, c, ax, ay):
        c.fill_round_rect(ax, ay, self.w, 10, 5, 0x2c303a)
        if self.pct > 0:
            c.fill_round_rect(ax, ay, imax(10, self.w * self.pct // 100), 10, 5, 0x3a8c4c)

func imax(a, b):
    return a if a > b else b

# ---- the store ----
let apps = []
let rows = {}            # id -> {row, desc, dl, tr, bar, icon}
let app = nil
let status = nil
let col = nil
let base = base_url()
let phase = "catalog"    # catalog | icons | idle
let job = -1
let icon_i = 0
let install_id = ""
let busy = false

func set_status(s):
    status.set_text(s)

func refresh_row(a):
    let r = rows[a.id]
    let ver = installed_version(a)
    let sz = fmt_size(a.installed)
    let state = ""
    if ver != nil:
        state = tr("Update available   ", "更新があります   ") if ver != a.version else tr("Installed   ", "インストール済み   ")
    r.desc.set_text(a.desc .. "   " .. state .. "(" .. sz .. tr(" on disk)", " ディスク使用)"))
    r.desc.color = 0x82c88c if ver != nil else gui.THEME.dim
    let can_dl = (ver == nil or ver != a.version) and not busy
    r.dl.state = "on" if can_dl else "off"
    r.dl.tip = tr("Download size: ", "ダウンロードサイズ: ") .. fmt_size(a.download) if (ver == nil or ver != a.version) else tr("Installed", "インストール済み")
    r.tr.state = "on" if (ver != nil and not busy) else "off"
    r.dl.dirty()
    r.tr.dirty()

func refresh_all():
    for a in apps:
        refresh_row(a)

func apply_filter(text):
    let q = text.lower()
    for a in apps:
        let hit = q == "" or a.title.lower().contains(q) or a.desc.lower().contains(q) or a.id.lower().contains(q)
        rows[a.id].row.set_visible(hit)
    app.relayout()

func start_install(a):
    if busy:
        return
    let url = base .. a.id .. "/"
    if exists("/system/linuxpack"):
        fs.write_file("/system/linuxpack/progress.txt", "running 0 0 0 0 starting\n")
    let pid = _cos.spawn(INSTALLER, [url])
    if pid <= 0:
        set_status(tr("Could not start the installer.", "インストーラーを起動できません。"))
        return
    busy = true
    install_id = a.id
    set_status(tr("Installing ", "インストール中: ") .. a.title .. "...")
    refresh_all()

func do_remove(a):
    let sc = "/desktop/" .. a.title .. ".app"
    if exists(sc):
        fs.remove(sc)
    rm_tree("/apps/" .. a.id)
    refresh_row(a)
    set_status(a.title .. tr(" was removed.", " を削除しました。"))

# the warnings before a removal (0..3, Settings > Removing apps): each one is a dialog, the last "Yes" removes
func ask(a, step, total):
    if step >= total:
        do_remove(a)
        return
    let texts = [tr("Remove this app?", "このアプリを削除しますか?"), tr("Do you really want to remove it?", "本当に削除しますか?"),
                 tr("Its data will be removed too. Do you really want to remove it?", "データも一緒に消えます。本当に削除しますか?")]
    func chosen(i):
        if i == 1:
            ask(a, step + 1, total)
    gui.dialog(app, a.title .. "  (" .. to_string(step + 1) .. "/" .. to_string(total) .. ")", texts[step], [tr("Cancel", "キャンセル"), tr("Remove", "削除")], chosen)

func on_trash(a):
    let n = warnings()
    ask(a, 0, n)

func make_row(a):
    let ic = Icon(48)
    let title = gui.label(a.title, 2)
    let desc = gui.label(a.desc, 1, 520)
    let bar = Bar()
    bar.visible = false
    let dl = IconButton("download", w => start_install(a))
    let tr_b = IconButton("trash", w => on_trash(a))
    let text = gui.column([title, desc, bar], 3, 0)
    text.flex = 1
    let row = gui.row([ic, text, dl, tr_b], 12, 8)
    row.framed = true
    rows[a.id] = {row: row, desc: desc, dl: dl, tr: tr_b, bar: bar, icon: ic}
    col.add(row)
    return row

# ---- loading: one transfer at a time, polled from the tick ----
func load_catalog_done(text):
    let body = _cos.catalog_verify(text)
    if body == nil:
        set_status(tr("The list of apps is not trusted (signature mismatch).", "アプリ一覧の署名が一致しません。"))
        phase = "idle"
        return
    apps = parse_catalog(body)
    for a in apps:
        make_row(a)
    refresh_all()
    app.relayout()
    set_status(to_string(len(apps)) .. tr(" app(s)", " 個のアプリ"))
    phase = "icons"
    icon_i = 0

func step_loading():
    if phase == "idle":
        return
    if job < 0:
        let url = ""
        if phase == "catalog":
            url = base .. "catalog.manifest"
        else:
            if icon_i >= len(apps):
                phase = "idle"
                return
            let a = apps[icon_i]
            url = base .. a.id .. "/" .. a.icon
        job = _cos.http_start(url)
        if job < 0:
            set_status(tr("No network.", "ネットワークに接続できません。"))
            phase = "idle"
        return
    let p = _cos.http_poll(job)
    if p == 0:
        return
    if phase == "catalog":
        if p < 0:
            set_status(tr("Could not reach the app server (check the network).", "アプリのサーバーに接続できません(ネットワークを確認してください)。"))
            job = -1
            phase = "idle"
            return
        let text = _cos.http_text(job)
        job = -1
        load_catalog_done(text if text != nil else "")
    else:
        let a = apps[icon_i]
        if p > 0:
            if not exists("/system/apps"):
                fs.make_dir("/system/apps")
            if not exists(ICON_DIR):
                fs.make_dir(ICON_DIR)
            let path = ICON_DIR .. "/" .. a.id .. ".png"
            if _cos.http_save(job, path) and _cos.sha256_file(path) == a.iconsha:
                rows[a.id].icon.load(path)
        job = -1
        icon_i += 1

# the installer reports in /system/linuxpack/progress.txt: "<running|done|error> <chunks> <total> <bytes done> <bytes> <message>"
func step_install():
    if not busy:
        return
    let t = read_text("/system/linuxpack/progress.txt")
    if t == nil:
        return
    let f = t.trim().split(" ")
    if len(f) < 5:
        return
    let r = rows[install_id]
    let bytes = to_int(f[4])
    let pct = to_int(f[3]) * 100 // bytes if bytes > 0 else 0
    r.bar.visible = true
    r.bar.set_pct(imax(0, pct if pct < 100 else 100))
    if f[0] == "done" or f[0] == "error":
        busy = false
        r.bar.visible = false
        let a = apps[0]
        for x in apps:
            if x.id == install_id:
                a = x
        refresh_all()
        set_status(a.title .. tr(" is installed. Its icon is on the desktop.", " をインストールしました。アイコンはデスクトップにあります。") if f[0] == "done" else tr("Stopped.", "中断しました。"))
        app.relayout()

class Pump extends gui.Widget:
    override func init(self):
        super(0, 0, 1, 1)
    override func on_tick(self, ms):
        step_loading()
        step_install()

# ---- window ----
app = gui.App(760, 520, "C-OS Apps")
gui.set_theme("dark")
let title = gui.label("C-OS Apps", 3)
let search = gui.input("", tr("Search apps", "アプリを検索"))
search.set_size(260, 20)
func on_search(w):
    apply_filter(search.text)
search.on_change = on_search
col = gui.column([], 6, 8)
col.auto = false
let list = gui.scroll(col, 760, 400)
status = gui.label("")
status.dim = true
set_status(tr("Loading the list of apps...", "アプリ一覧を読み込んでいます..."))
let head = gui.row([title, gui.spacer(), search], 8, 10)
let foot = gui.row([status, Pump()], 8, 6)
list.flex = 1
app.fill(gui.column([head, list, foot], 0, 0))
app.relayout()
let win = window.open(760, 520, "C-OS Apps", "cos")
window.run(app, win, -1, (a, w) => true)
