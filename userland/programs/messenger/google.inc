/* Google sign-in (OAuth device flow). The relay server does all the talking to Google; this side only shows the short code and
 * the address, then asks the server every few seconds whether the person has finished. The Google password is typed on
 * Google's own page (phone or PC) - it never passes through C-OS Message or the relay server. */
static struct { bool open, starting; char code[24], url[72]; uint64_t deadline, next, interval; } gl;
static cui_font *f_big;

static void gl_begin(void) { memset(&gl, 0, sizeof gl); gl.open = gl.starting = true; dirty = true; }

static void gl_cmd(void) {                                       /* /google */
    if (srv_state != SV_ONLINE) { set_status(T("Not connected to a server - open the Menu")); return; }
    if (g_linked) { char b[100]; snprintf(b, sizeof b, T("Signed in with Google as %s"), g_masked); set_status(b); return; }
    if (!g_conf) { set_status(T("This server has no Google sign-in")); return; }
    gl_begin(); srv_tx("GLINK", NULL, NULL, NULL);
}

static bool gl_frame(char **f, int n) {
    const char *cmd = f[0];
    if (strcmp(cmd, "NEED_GOOGLE") == 0) { g_required = g_conf = true; gl_begin(); srv_tx("GREG", my_name, NULL, NULL); return true; }
    if (strcmp(cmd, "GOOGLE_CODE") == 0 && n >= 5) {
        uint64_t now = cos_time_ms();
        scopy(gl.code, sizeof gl.code, f[1]); scopy(gl.url, sizeof gl.url, f[2]);
        gl.deadline = now + (uint64_t)atoi(f[3]) * 1000; gl.interval = (uint64_t)atoi(f[4]) * 1000; gl.next = now + gl.interval;
        gl.open = true; gl.starting = false; return true;
    }
    if (strcmp(cmd, "GOOGLE_PENDING") == 0) return true;
    if (strcmp(cmd, "GOOGLE_OK") == 0 && n >= 2) {
        g_linked = true; scopy(g_masked, sizeof g_masked, f[1]); gl.open = false;
        char b[100]; snprintf(b, sizeof b, T("Signed in with Google as %s"), g_masked); set_status(b); return true;
    }
    if (strcmp(cmd, "GOOGLE_STATUS") == 0 && n >= 5) {
        g_linked = f[1][0] == '1'; scopy(g_masked, sizeof g_masked, f[2]); g_required = f[3][0] == '1'; g_conf = f[4][0] == '1'; return true;
    }
    if (strcmp(cmd, "OK_LOGOUT_ALL") == 0 && n >= 2) {
        snprintf(acc_token, sizeof acc_token, "%s", f[1]); save_account(); set_status(T("Every other device was logged out")); return true;
    }
    return false;
}

static void gl_poll(void) {
    if (!gl.open || gl.starting) return;
    uint64_t now = cos_time_ms();
    if (now > gl.deadline) { gl.open = false; set_status(T("The code expired - type /google to try again")); dirty = true; return; }
    if (now >= gl.next && srv_state >= SV_LOGIN) { gl.next = now + gl.interval; srv_tx("GPOLL", NULL, NULL, NULL); }
    dirty = true;                                                 /* the countdown */
}

#define GL_W 540
#define GL_H 360
static void gl_draw(void) {
    if (!gl.open) return;
    if (!f_big) f_big = mkfont(34, true);
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 170);
    int x = (win_w - GL_W) / 2, y = (win_h - GL_H) / 2;
    cui_round_rect(&c, x, y, GL_W, GL_H, 14, PANEL);
    cui_text(&c, f_h, x + 24, y + 18, "Google でログイン", TEXT);
    if (gl.starting) { cui_text_center(&c, f_t, x, y + 60, GL_W, 200, "Google に接続しています...", MUTED); }
    else {
        cui_text(&c, f_s, x + 24, y + 62, "1. スマホまたはパソコンのブラウザで、次のページを開きます", MUTED);
        cui_text(&c, f_b, x + 40, y + 84, gl.url, ACCENT);
        cui_text(&c, f_s, x + 24, y + 116, "2. 次のコードを入力します", MUTED);
        cui_round_rect(&c, x + 40, y + 138, GL_W - 80, 64, 10, BG);
        cui_text_center(&c, f_big, x + 40, y + 138, GL_W - 80, 64, gl.code, TEXT);
        cui_text(&c, f_s, x + 24, y + 216, "3. Google の画面で Gmail アカウントにログインして「許可」を押します", MUTED);
        int left = (int)((gl.deadline > cos_time_ms() ? gl.deadline - cos_time_ms() : 0) / 1000);
        char tb[64]; snprintf(tb, sizeof tb, "コードの有効期限: あと %d:%02d", left / 60, left % 60);
        cui_text(&c, f_s, x + 24, y + 244, tb, left < 60 ? WARN : MUTED);
        cui_round_rect(&c, x + 24, y + 270, GL_W - 48, 30, 8, 0x1F2B3F);
        cui_text_center(&c, f_s, x + 24, y + 270, GL_W - 48, 30, "Google のパスワードは、この画面にも中継サーバーにも入力・送信されません", 0x8FD0A0);
    }
    cui_round_rect(&c, x + GL_W - 140, y + GL_H - 46, 116, 30, 8, 0x4A5068); cui_text_center(&c, f_s, x + GL_W - 140, y + GL_H - 46, 116, 30, "キャンセル", 0xFFFFFF);
    if (!gl.starting) { cui_round_rect(&c, x + 24, y + GL_H - 46, 140, 30, 8, 0x2B3550); cui_text_center(&c, f_s, x + 24, y + GL_H - 46, 140, 30, "コードをコピー", 0xFFFFFF); }
}

static void gl_click(int mx, int my) {
    int x = (win_w - GL_W) / 2, y = (win_h - GL_H) / 2;
    if (my >= y + GL_H - 46 && my < y + GL_H - 16) {
        if (mx >= x + GL_W - 140 && mx < x + GL_W - 24) { gl.open = false; dirty = true; set_status(T("Sign-in cancelled")); }
        else if (!gl.starting && mx >= x + 24 && mx < x + 164) { cos_clip_set(gl.code, strlen(gl.code)); set_status(T("Code copied")); }
    }
}

/* An error while the sign-in window is open ends it; a server that wants every account linked sends us here from any command. */
static void gl_on_error(const char *msg) {
    if (gl.open) { gl.open = false; dirty = true; }
    if (strstr(msg, T("sign in with Google first")) && g_conf && !gl.open && srv_state == SV_ONLINE) gl_cmd();
}
