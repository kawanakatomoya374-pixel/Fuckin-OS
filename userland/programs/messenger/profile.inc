/* profile.inc - profiles, avatars, people search and invite codes for C-OS Message (included after attach.inc).
 *
 * Everyone can have an @handle, a bio and a round profile photo; searching for people only finds those who switched
 * "discoverable" on, and an invite code lets anyone start a friend request without being searchable. The server side is
 * described in tools/cosmsg_server.py. The photo is cropped to a square, scaled to 96x96 and uploaded as a small BMP. */

#define AV_PX 96

/* ---------------------------------------------------------------- avatars */
typedef struct { char fid[24]; uint32_t *px; int state; uint64_t t; } av_t;       /* state: 0 new, 1 loading, 2 ready, 3 failed */
static av_t avs[64]; static int av_next;

static void av_changed(void) { dirty = true; }

static av_t *av_get(const char *fid) {
    if (!fid || !fid[0]) return NULL;
    for (int i = 0; i < 64; i++) if (avs[i].fid[0] && !strcmp(avs[i].fid, fid)) return &avs[i];
    av_t *a = &avs[av_next]; av_next = (av_next + 1) % 64;
    free(a->px); memset(a, 0, sizeof *a);
    snprintf(a->fid, sizeof a->fid, "%s", fid);
    return a;
}

static bool av_load(av_t *a) {
    char path[96]; att_path(path, sizeof path, a->fid);
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) return false;
    int w, h; uint32_t *px = img_decode(d, n, &w, &h);
    free(d);
    if (!px) return false;
    int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    uint32_t *sq = (uint32_t *)malloc((size_t)side * side * 4);
    if (!sq) { free(px); return false; }
    for (int y = 0; y < side; y++) memcpy(sq + (size_t)y * side, px + (size_t)(y + oy) * w + ox, (size_t)side * 4);
    free(px);
    a->px = img_scale(sq, side, side, AV_PX, AV_PX, 0x1C2333);
    free(sq);
    return a->px != NULL;
}

static void av_poll(av_t *a) {
    if (a->state == 2 || (a->state == 3 && since(a->t) < 30000)) return;
    if (a->state == 3) a->state = 0;
    char path[96]; cos_stat_t st; att_path(path, sizeof path, a->fid);
    if (a->state == 0) {
        if (cos_stat(path, &st) == 0) { a->state = av_load(a) ? 2 : 3; a->t = cos_time_ms(); return; }
        if (att_fetch_avatar(a->fid)) { a->state = 1; a->t = cos_time_ms(); }
        return;
    }
    if (a->state == 1 && !dl_find(a->fid)) {                       /* the download ended */
        a->state = (cos_stat(path, &st) == 0 && av_load(a)) ? 2 : 3; a->t = cos_time_ms();
    }
}

/* A round photo (or a coloured disc with the first letter) of diameter d at (x,y). */
static void av_draw(const char *fid, const char *name, int x, int y, int d, uint32_t fallback) {
    av_t *a = av_get(fid);
    if (a) av_poll(a);
    if (a && a->state == 2 && a->px) {
        float r = d / 2.0f;
        for (int yy = 0; yy < d; yy++) {
            int py = y + yy; if (py < c.cy0 || py >= c.cy1) continue;
            float dy = yy + 0.5f - r;
            for (int xx = 0; xx < d; xx++) {
                int pxx = x + xx; if (pxx < c.cx0 || pxx >= c.cx1) continue;
                float dx = xx + 0.5f - r, dist2 = dx * dx + dy * dy;
                if (dist2 > r * r) continue;
                int sx0 = xx * AV_PX / d, sx1 = (xx + 1) * AV_PX / d, sy0 = yy * AV_PX / d, sy1 = (yy + 1) * AV_PX / d;
                if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
                unsigned rr = 0, gg = 0, bb = 0, cnt = 0;
                for (int sy = sy0; sy < sy1; sy += (sy1 - sy0 > 3 ? 2 : 1)) for (int sx = sx0; sx < sx1; sx += (sx1 - sx0 > 3 ? 2 : 1)) {
                    uint32_t p = a->px[sy * AV_PX + sx]; rr += (p >> 16) & 255; gg += (p >> 8) & 255; bb += p & 255; cnt++;
                }
                c.px[py * c.stride + pxx] = (rr / cnt) << 16 | (gg / cnt) << 8 | (bb / cnt);
            }
        }
        return;
    }
    cui_circle(&c, x + d / 2.0f, y + d / 2.0f, d / 2.0f, fallback);
    char ini[5] = { 0 };
    if (name && name[0]) { int il = 1; unsigned char u0 = (unsigned char)name[0]; if (u0 >= 0xF0) il = 4; else if (u0 >= 0xE0) il = 3; else if (u0 >= 0xC0) il = 2; memcpy(ini, name, (size_t)il); }
    else ini[0] = '?';
    cui_text_center(&c, d >= 60 ? f_h : f_b, x, y, d, d, ini, 0xFFFFFF);
}

/* ---------------------------------------------------------------- my profile */
static void prof_avatar_uploaded(const char *fid) { snprintf(my_avatar, sizeof my_avatar, "%s", fid); dirty = true; }

/* Crops the image to a square, scales it to 96x96 and uploads it as a 24-bit BMP. */
static void prof_set_photo(const char *path) {
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) { set_status(T("Cannot read that file")); return; }
    int w, h; uint32_t *px = img_decode(d, n, &w, &h);
    free(d);
    if (!px) { set_status(T("That is not a picture C-OS Message can read (PNG, JPEG, BMP, GIF, WebP)")); return; }
    int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    uint32_t *sq = (uint32_t *)malloc((size_t)side * side * 4);
    if (!sq) { free(px); return; }
    for (int y = 0; y < side; y++) memcpy(sq + (size_t)y * side, px + (size_t)(y + oy) * w + ox, (size_t)side * 4);
    free(px);
    uint32_t *sc = img_scale(sq, side, side, AV_PX, AV_PX, 0x1C2333);
    free(sq);
    if (!sc) return;
    size_t rowb = AV_PX * 3, total = 54 + rowb * AV_PX;
    uint8_t *bmp = (uint8_t *)calloc(1, total);
    if (!bmp) { free(sc); return; }
    bmp[0] = 'B'; bmp[1] = 'M'; bmp[2] = (uint8_t)total; bmp[3] = (uint8_t)(total >> 8); bmp[4] = (uint8_t)(total >> 16);
    bmp[10] = 54; bmp[14] = 40; bmp[18] = AV_PX; bmp[22] = AV_PX; bmp[26] = 1; bmp[28] = 24;
    for (int y = 0; y < AV_PX; y++) for (int x = 0; x < AV_PX; x++) {
        uint32_t p = sc[(AV_PX - 1 - y) * AV_PX + x]; uint8_t *o = bmp + 54 + (size_t)y * rowb + (size_t)x * 3;
        o[0] = (uint8_t)p; o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)(p >> 16);
    }
    free(sc);
    if (att_upload_avatar(bmp, total)) set_status(T("Uploading your photo..."));
}

static void prof_cmd_photo(const char *arg) { if (arg && *arg) prof_set_photo(arg); else { pk.avatar = true; pk_show(); } }

/* ---------------------------------------------------------------- people search overlay */
typedef struct { char id[12], name[NAME_LEN], handle[24], bio[128], avatar[24]; bool requested; } sres_t;
static struct { bool open, pending; char query[40]; sres_t r[20]; int n, scroll; } sr;
static struct { bool open; } pf;
#define OV_W 560
#define OV_H 460
#define SR_ROW 68

static void sr_start(const char *q) {
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to a server - open the Menu")); return; }
    snprintf(sr.query, sizeof sr.query, "%s", q);
    sr.n = 0; sr.scroll = 0; sr.pending = true; sr.open = true; pf.open = false;
    srv_tx("SEARCH", q, NULL, NULL);
    dirty = true;
}

static void prof_open_profile(void);
static void sr_open_empty(void) { sr.open = true; sr.pending = false; sr.n = 0; sr.query[0] = 0; pf.open = false; dirty = true; }
/* frames from the server; true when handled */
static bool prof_srv_frame(char **f, int n) {
    const char *cmd = f[0];
    if (!strcmp(cmd, "ME") && n >= 8) {
        scopy(my_handle, sizeof my_handle, f[3]); scopy(my_bio, sizeof my_bio, f[4]);
        my_disc = f[5][0] == '1'; scopy(my_avatar, sizeof my_avatar, f[6]); scopy(my_invite, sizeof my_invite, f[7]);
        if (f[2][0] && strcmp(my_name, f[2]) != 0) { scopy(my_name, sizeof my_name, f[2]); save_contacts(); }
        return true;
    }
    if (!strcmp(cmd, "OK_HANDLE")) { scopy(my_handle, sizeof my_handle, n >= 2 ? f[1] : ""); set_status(my_handle[0] ? T("Handle saved") : T("Handle removed")); return true; }
    if (!strcmp(cmd, "OK_BIO")) { set_status(T("Bio saved")); return true; }
    if (!strcmp(cmd, "OK_DISC")) { my_disc = n >= 2 && f[1][0] == '1'; set_status(my_disc ? T("People can find you in search") : T("You are hidden from search")); return true; }
    if (!strcmp(cmd, "OK_AVATAR")) { set_status(T("Profile photo updated")); return true; }
    if (!strcmp(cmd, "INVITE") && n >= 2) { scopy(my_invite, sizeof my_invite, f[1]); set_status(T("New invite code created")); return true; }
    if (!strcmp(cmd, "RESULT") && n >= 6) {
        if (sr.n < 20) { sres_t *r = &sr.r[sr.n++]; memset(r, 0, sizeof *r); scopy(r->id, sizeof r->id, f[1]); scopy(r->name, sizeof r->name, f[2]); scopy(r->handle, sizeof r->handle, f[3]); scopy(r->bio, sizeof r->bio, f[4]); scopy(r->avatar, sizeof r->avatar, f[5]); }
        return true;
    }
    if (!strcmp(cmd, "RESULT_END")) { sr.pending = false; return true; }
    return false;
}

static void prof_open_profile(void) { if (srv_state == SV_ONLINE) srv_tx("ME", NULL, NULL, NULL); pf.open = true; sr.open = false; dirty = true; }

#define OVX ((win_w - OV_W) / 2)
#define OVY ((win_h - OV_H) / 2)

static void sr_draw(void) {
    if (!sr.open) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 160);
    int x = OVX, y = OVY;
    cui_round_rect(&c, x, y, OV_W, OV_H, 12, PANEL);
    char t[80]; snprintf(t, sizeof t, T("Find people: %s"), sr.query);
    cui_text_fit(&c, f_b, x + 16, y + 12, OV_W - 100, t, TEXT);
    cui_round_rect(&c, x + OV_W - 72, y + 8, 60, 26, 8, 0x4A5068); cui_text_center(&c, f_s, x + OV_W - 72, y + 8, 60, 26, T("Close"), 0xFFFFFF);
    int rows = (OV_H - 90) / SR_ROW;
    cui_clip(&c, x, y + 44, OV_W, rows * SR_ROW);
    for (int i = 0; i < rows && sr.scroll + i < sr.n; i++) {
        sres_t *r = &sr.r[sr.scroll + i]; int ry = y + 44 + i * SR_ROW;
        if (i % 2) cui_fill(&c, x + 8, ry, OV_W - 16, SR_ROW, 0x20293E);
        av_draw(r->avatar, r->name, x + 18, ry + 10, 48, cui_mix(ACCENT, 0x7040C0, ((sr.scroll + i) * 53) & 255));
        cui_text_fit(&c, f_b, x + 78, ry + 8, OV_W - 78 - 110, r->name, TEXT);
        if (r->handle[0]) { char hb[40]; snprintf(hb, sizeof hb, "@%s", r->handle); cui_text(&c, f_s, x + 78 + cui_text_width(f_b, r->name) + 8, ry + 11, hb, ACCENT); }
        cui_text_fit(&c, f_s, x + 78, ry + 32, OV_W - 78 - 110, r->bio[0] ? r->bio : T("(no bio)"), MUTED);
        bool fr = find_uid(r->id) >= 0;
        uint32_t bc = (fr || r->requested) ? 0x3A4156 : MINE;
        cui_round_rect(&c, x + OV_W - 100, ry + 20, 84, 28, 8, bc);
        cui_text_center(&c, f_s, x + OV_W - 100, ry + 20, 84, 28, fr ? T("Friends") : r->requested ? T("Requested") : T("Add"), 0xFFFFFF);
    }
    cui_unclip(&c);
    if (sr.pending) cui_text_center(&c, f_s, x, y + 44, OV_W, 60, T("Searching..."), MUTED);
    else if (sr.n == 0) cui_text_center(&c, f_s, x, y + 44, OV_W, 100, sr.query[0] ? T("Nobody found - people only appear if they allowed it (/discover on).") : T("Search by name or @handle:  /find <text>"), MUTED);
    cui_text(&c, f_s, x + 16, y + OV_H - 28, T("Search again with  /find <name or @handle>   -   or use an invite code:  /join <code>"), MUTED);
}
static void sr_click(int mx, int my) {
    int x = OVX, y = OVY;
    if (mx < x || mx >= x + OV_W || my < y || my >= y + OV_H) { sr.open = false; dirty = true; return; }
    if (my < y + 40 && mx >= x + OV_W - 72) { sr.open = false; dirty = true; return; }
    int rows = (OV_H - 90) / SR_ROW;
    if (my < y + 44 || my >= y + 44 + rows * SR_ROW) return;
    int i = sr.scroll + (my - y - 44) / SR_ROW;
    if (i >= sr.n || mx < x + OV_W - 100 || mx >= x + OV_W - 16) return;
    sres_t *r = &sr.r[i];
    if (find_uid(r->id) >= 0 || r->requested) return;
    srv_tx("FREQ", r->id, T("Hi, I found you on C-OS Message"), NULL);
    r->requested = true; dirty = true;
}

static void gui_edit(int what);        /* gui.inc: forms for name / @handle / about me */
/* ---------------------------------------------------------------- my profile overlay */
#define PF_H 400
static void pf_draw(void) {
    if (!pf.open) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 160);
    int x = OVX, y = (win_h - PF_H) / 2;
    cui_round_rect(&c, x, y, OV_W, PF_H, 12, PANEL);
    cui_text(&c, f_b, x + 16, y + 12, T("Your profile"), TEXT);
    cui_round_rect(&c, x + OV_W - 72, y + 8, 60, 26, 8, 0x4A5068); cui_text_center(&c, f_s, x + OV_W - 72, y + 8, 60, 26, T("Close"), 0xFFFFFF);
    av_draw(my_avatar, my_name, x + 24, y + 54, 96, ACCENT);
    cui_round_rect(&c, x + 24, y + 158, 96, 26, 8, 0x2B3550); cui_text_center(&c, f_s, x + 24, y + 158, 96, 26, T("Change photo"), 0xFFFFFF);
    cui_text(&c, f_s, x + 24, y + 188, T("or drop a picture here"), MUTED);
    cui_text_fit(&c, f_h, x + 144, y + 58, OV_W - 160, my_name, TEXT);
    char hb[48]; snprintf(hb, sizeof hb, my_handle[0] ? "@%s" : T("No @handle yet  -  /handle yourname"), my_handle);
    cui_text(&c, f_t, x + 144, y + 88, hb, my_handle[0] ? ACCENT : MUTED);
    cui_text_fit(&c, f_s, x + 144, y + 116, OV_W - 160, my_bio[0] ? my_bio : T("No bio yet  -  /bio something about you"), MUTED);
    char idb[40]; snprintf(idb, sizeof idb, "C-OS ID %s", acc_id);
    cui_text(&c, f_s, x + 144, y + 140, idb, MUTED);
    /* discoverable switch */
    cui_text(&c, f_t, x + 24, y + 214, T("Show me in people search"), TEXT);
    cui_round_rect(&c, x + OV_W - 80, y + 210, 52, 26, 13, my_disc ? MINE : 0x3A4156);
    cui_circle(&c, (float)(x + OV_W - (my_disc ? 41 : 67)), (float)(y + 223), 10.0f, 0xFFFFFF);
    cui_text(&c, f_s, x + 24, y + 238, my_disc ? T("Anyone can find you by name or @handle.") : T("Hidden. People can still reach you with your invite code."), MUTED);
    /* invite code */
    cui_text(&c, f_t, x + 24, y + 280, T("Invite code"), TEXT);
    char ic[24]; snprintf(ic, sizeof ic, "%.4s - %s", my_invite, my_invite[0] && strlen(my_invite) > 4 ? my_invite + 4 : "");
    cui_round_rect(&c, x + 24, y + 306, 200, 34, 8, BG);
    cui_text_center(&c, f_h, x + 24, y + 306, 200, 34, my_invite[0] ? ic : "...", TEXT);
    cui_round_rect(&c, x + 236, y + 306, 70, 34, 8, 0x2F5C9E); cui_text_center(&c, f_s, x + 236, y + 306, 70, 34, T("Copy"), 0xFFFFFF);
    cui_round_rect(&c, x + 316, y + 306, 100, 34, 8, 0x3A4A70); cui_text_center(&c, f_s, x + 316, y + 306, 100, 34, T("New code"), 0xFFFFFF);
    cui_round_rect(&c, x + 24, y + PF_H - 46, 110, 30, 8, 0x3A4A70); cui_text_center(&c, f_s, x + 24, y + PF_H - 46, 110, 30, T("Edit name"), 0xFFFFFF);
    cui_round_rect(&c, x + 144, y + PF_H - 46, 110, 30, 8, 0x3A4A70); cui_text_center(&c, f_s, x + 144, y + PF_H - 46, 110, 30, T("Edit @handle"), 0xFFFFFF);
    cui_round_rect(&c, x + 264, y + PF_H - 46, 110, 30, 8, 0x3A4A70); cui_text_center(&c, f_s, x + 264, y + PF_H - 46, 110, 30, T("Edit about me"), 0xFFFFFF);
}
static void pf_click(int mx, int my) {
    int x = OVX, y = (win_h - PF_H) / 2;
    if (mx < x || mx >= x + OV_W || my < y || my >= y + PF_H) { pf.open = false; dirty = true; return; }
    if (my < y + 40 && mx >= x + OV_W - 72) { pf.open = false; dirty = true; return; }
    if (mx >= x + 24 && mx < x + 120 && my >= y + 158 && my < y + 184) { pf.open = false; pk.avatar = true; pk_show(); return; }
    if (mx >= x + OV_W - 80 && mx < x + OV_W - 28 && my >= y + 210 && my < y + 236) { srv_tx("DISC", my_disc ? "0" : "1", NULL, NULL); return; }
    if (my >= y + PF_H - 46 && my < y + PF_H - 16) {
        pf.open = false; if (mx >= x + 24 && mx < x + 134) gui_edit(0); else if (mx >= x + 144 && mx < x + 254) gui_edit(1); else if (mx >= x + 264 && mx < x + 374) gui_edit(2); else pf.open = true;
        return;
    }
    if (my >= y + 306 && my < y + 340) {
        if (mx >= x + 236 && mx < x + 306 && my_invite[0]) { cos_clip_set(my_invite, strlen(my_invite)); set_status(T("Invite code copied")); }
        else if (mx >= x + 316 && mx < x + 416) srv_tx("INVITE_NEW", NULL, NULL, NULL);
    }
}
