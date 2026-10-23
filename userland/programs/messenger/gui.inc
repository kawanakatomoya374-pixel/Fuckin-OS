/* gui.inc - menus and forms: everything C-OS Message can do works with the mouse and plain typing. Every button builds the same
 * "/command" the message box accepts and runs it through run_command(), so the two ways never differ.
 *
 *  - the Menu button (left header)  : server, hosting, Google, account, friends, groups, privacy, app lock, help
 *  - the Actions button (chat header): per-chat things (send a file, block, report, remove / leave, notes, announcement ...)
 *  - members page                   : roles, make owner, remove (groups)
 *  - forms                          : small dialogs with text fields; typing goes through the normal message-box editing, so the
 *                                     Japanese input method works in them too.
 * Included after run_command() and before draw(). */

static void run_command(char *s);
static void gui_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gui_cmd(const char *fmt, ...) {
    char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    run_command(b); dirty = true;
}
static void trim_ends(char *s) { size_t n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0; char *p = s; while (*p == ' ') p++; if (p != s) memmove(s, p, strlen(p) + 1); }

/* ---------------------------------------------------------------- forms */
enum { FK_NONE, FK_SERVER, FK_FRIEND, FK_INVITE, FK_FIND, FK_NAME, FK_HANDLE, FK_BIO, FK_GNEW, FK_GJOIN, FK_GOOGLE, FK_LAN, FK_NOTE, FK_ANN, FK_GRENAME,
       FK_GPASS, FK_REPORT, FK_LOCKTO, FK_CONFIRM, FK_SEARCH, FK_SEARCHALL };
#define FM_W 520
static struct { bool open; int kind, nf, focus; char title[72], hint[200], cmd[120]; struct { char label[48], val[200]; bool secret; } f[3]; char saved_in[MAX_TEXT]; int saved_pos; } fm;

static void form_load_focus(void) { scopy(in, sizeof in, fm.f[fm.focus].val); in_len = (int)strlen(in); in_pos = in_len; }
static void form_store_focus(void) { if (fm.nf) scopy(fm.f[fm.focus].val, sizeof fm.f[fm.focus].val, in); }
static void form_open(int kind, const char *title, const char *hint, int nf, const char *l0, const char *l1, const char *l2, bool secret1) {
    memset(&fm, 0, sizeof fm); fm.open = true; fm.kind = kind; fm.nf = nf;
    scopy(fm.title, sizeof fm.title, title); scopy(fm.hint, sizeof fm.hint, hint);
    const char *ls[3] = { l0, l1, l2 }; for (int i = 0; i < nf; i++) scopy(fm.f[i].label, sizeof fm.f[i].label, ls[i] ? ls[i] : "");
    fm.f[1].secret = secret1 && nf >= 2;
    scopy(fm.saved_in, sizeof fm.saved_in, in); fm.saved_pos = in_pos;                       /* the half-typed message comes back afterwards */
    in_clear(); fm.focus = 0; dirty = true;
}
static void form_close(void) {
    if (!fm.open) return;
    fm.open = false; scopy(in, sizeof in, fm.saved_in); in_len = (int)strlen(in); in_pos = fm.saved_pos <= in_len ? fm.saved_pos : in_len; dirty = true;
}
static void form_confirm(const char *title, const char *text, const char *cmd) { form_open(FK_CONFIRM, title, text, 0, NULL, NULL, NULL, false); scopy(fm.cmd, sizeof fm.cmd, cmd); }
static void form_move(int d) { if (fm.nf < 2) return; form_store_focus(); fm.focus = (fm.focus + d + fm.nf) % fm.nf; form_load_focus(); dirty = true; }

static void form_submit(void) {
    form_store_focus();
    char a[200], b[200], cc[200]; scopy(a, sizeof a, fm.f[0].val); scopy(b, sizeof b, fm.f[1].val); scopy(cc, sizeof cc, fm.f[2].val); trim_ends(a); trim_ends(b); trim_ends(cc);
    int kind = fm.kind; char cmd[120]; scopy(cmd, sizeof cmd, fm.cmd);
    bool need = kind != FK_CONFIRM && kind != FK_ANN && kind != FK_GPASS && kind != FK_BIO && kind != FK_HANDLE && kind != FK_FRIEND && kind != FK_LAN && kind != FK_GNEW;
    if (need && !a[0]) { set_status(T("Fill in the first field")); return; }
    if ((kind == FK_FRIEND || kind == FK_GNEW || kind == FK_LAN) && !a[0]) { set_status(T("Fill in the first field")); return; }
    if ((kind == FK_GJOIN || kind == FK_GOOGLE) && !b[0]) { set_status(T("Fill in both fields")); return; }
    form_close();
    switch (kind) {
    case FK_SERVER: gui_cmd("/server %s", a); break;
    case FK_FRIEND: gui_cmd("/friend %s %s", a, b); break;
    case FK_INVITE: gui_cmd("/join %s", a); break;
    case FK_FIND: gui_cmd("/find %s", a); break;
    case FK_NAME: gui_cmd("/name %s", a); break;
    case FK_HANDLE: gui_cmd("/handle %s", a); break;
    case FK_BIO: gui_cmd("/bio %s", a); break;
    case FK_GNEW: gui_cmd(b[0] ? "/gnew %s %s" : "/gnew %s", a, b); break;
    case FK_GJOIN: gui_cmd("/gjoin %s %s", a, b); break;
    case FK_GOOGLE: gui_cmd("/host google %s %s", a, b); break;
    case FK_LAN: gui_cmd("/add %s %s", a, b); break;
    case FK_NOTE: gui_cmd("/gnote %s", a); break;
    case FK_ANN: gui_cmd("/gannounce %s", a); break;
    case FK_GRENAME: gui_cmd("/grename %s", a); break;
    case FK_GPASS: gui_cmd("/gpass %s", a); break;
    case FK_REPORT: gui_cmd("/report %s", a); break;
    case FK_LOCKTO: gui_cmd("/lock timeout %s", a); break;
    case FK_SEARCH: gui_cmd("/search %s", a); break;
    case FK_SEARCHALL: gui_cmd("/searchall %s", a); break;
    case FK_CONFIRM: gui_cmd("%s", cmd); break;
    }
}

/* returns true when the key was handled here (the rest of the editing is the message box's own) */
static bool form_special(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ESC) { form_close(); return true; }
    if (ev->special == COS_KEY_ENTER) { if (fm.nf > 1 && fm.focus < fm.nf - 1) form_move(1); else form_submit(); return true; }
    if (ev->special == COS_KEY_TAB || ev->special == COS_KEY_DOWN) { form_move(1); return true; }
    if (ev->special == COS_KEY_UP) { form_move(-1); return true; }
    if (ev->special == COS_KEY_PGUP || ev->special == COS_KEY_PGDN) return true;
    return false;
}

static int fm_h(void) { return 118 + fm.nf * 66 + (fm.nf ? 0 : 20); }
static void gbtn(int x, int y, int w, int h, const char *label, uint32_t col) { cui_round_rect(&c, x, y, w, h, 8, col); cui_text_center(&c, f_s, x, y, w, h, label, 0xFFFFFF); }

static void form_draw(void) {
    if (!fm.open) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 170);
    int h = fm_h(), x = (win_w - FM_W) / 2, y = (win_h - h) / 2;
    cui_round_rect(&c, x, y, FM_W, h, 12, PANEL);
    cui_text_fit(&c, f_b, x + 20, y + 14, FM_W - 40, fm.title, TEXT);
    int ty = y + 46;
    if (fm.hint[0]) { int ln = wrap(f_s, fm.hint, FM_W - 40); for (int l = 0; l < ln && l < 3; l++) { char b[160]; int len = ll_[l] > 159 ? 159 : ll_[l]; memcpy(b, ls_[l], (size_t)len); b[len] = 0; cui_text(&c, f_s, x + 20, ty, b, MUTED); ty += 16; } }
    int fy = y + (fm.nf ? 78 : 60) + (fm.hint[0] && fm.nf ? 10 : 0);
    if (fm.nf == 0) fy = ty + 6;
    for (int i = 0; i < fm.nf; i++) {
        int by = fy + i * 56;
        cui_text(&c, f_s, x + 20, by, fm.f[i].label, MUTED);
        cui_round_rect(&c, x + 20, by + 18, FM_W - 40, 32, 8, BG);
        if (i == fm.focus) cui_round_rect_outline(&c, x + 20, by + 18, FM_W - 40, 32, 8, 1.5f, ACCENT);
        const char *val = i == fm.focus ? in : fm.f[i].val; char shown[220];
        if (fm.f[i].secret) { size_t n = 0; for (const char *p = val; *p && n < 60; p++) if (((unsigned char)*p & 0xC0) != 0x80) shown[n++] = '*'; shown[n] = 0; val = shown; }
        cui_clip(&c, x + 28, by + 18, FM_W - 56, 32);
        int tw = cui_text_width(f_t, val); int ox = tw > FM_W - 70 ? tw - (FM_W - 70) : 0;
        cui_text(&c, f_t, x + 30 - ox, by + 25, val, TEXT);
        if (i == fm.focus && (cos_time_ms() / 500) % 2 == 0) cui_fill(&c, x + 30 - ox + tw, by + 24, 2, lh - 2, ACCENT);
        cui_unclip(&c);
    }
    int bw = 100, bx = x + FM_W - 20 - bw * 2 - 10, byy = y + h - 48;
    gbtn(bx, byy, bw, 32, T("Cancel"), 0x4A5068);
    gbtn(bx + bw + 10, byy, bw, 32, fm.kind == FK_CONFIRM ? T("Yes") : "OK", fm.kind == FK_CONFIRM ? 0x7A3A44 : MINE);
}
static void form_click(int mx, int my) {
    int h = fm_h(), x = (win_w - FM_W) / 2, y = (win_h - h) / 2;
    if (mx < x || mx >= x + FM_W || my < y || my >= y + h) return;                  /* a click outside does nothing (Esc cancels) */
    int bw = 100, bx = x + FM_W - 20 - bw * 2 - 10, byy = y + h - 48;
    if (my >= byy && my < byy + 32) { if (mx >= bx && mx < bx + bw) form_close(); else if (mx >= bx + bw + 10 && mx < bx + 2 * bw + 10) form_submit(); return; }
    int ty = y + 46; if (fm.hint[0]) { int ln = wrap(f_s, fm.hint, FM_W - 40); ty += 16 * (ln > 3 ? 3 : ln); }
    int fy = y + (fm.nf ? 78 : 60) + (fm.hint[0] && fm.nf ? 10 : 0); if (fm.nf == 0) fy = ty + 6;
    for (int i = 0; i < fm.nf; i++) { int by = fy + i * 56 + 18; if (my >= by && my < by + 32 && i != fm.focus) { form_store_focus(); fm.focus = i; form_load_focus(); dirty = true; } }
}

/* ---------------------------------------------------------------- the menu pages */
enum { PG_MAIN = 1, PG_CHAT, PG_MEMBERS };
typedef struct { int kind; char label[80], sub[120]; int nb; struct { char t[20]; int act; int arg; uint32_t col; } b[4]; } mrow_t;     /* kind 0 section title, 1 row */
#define MAXROWS 48
static struct { bool open; int page, scroll; } mn;
#define MN_W 600
#define MN_H 500
#define MN_ROW 46
#define MN_HEAD 28

enum { A_NONE, A_SERVER, A_REPIN, A_HOST_ON, A_HOST_OFF, A_GOOGLE_SETUP, A_GOOGLE_REQ, A_GOOGLE_OPT, A_PROFILE, A_NAME, A_HANDLE, A_BIO, A_GOOGLE, A_LOGOUTALL, A_LOGOUT,
       A_LOCK_ON, A_LOCK_OFF, A_LOCK_CHANGE, A_LOCK_NOW, A_LOCK_TO, A_FIND, A_FRIEND, A_INVITE, A_MYINVITE, A_BLOCKS, A_LAN, A_SCAN, A_GNEW, A_GJOIN, A_RR, A_PRES, A_SOUND,
       A_HELP, A_SEARCH, A_SEARCHALL, A_PIN, A_MUTE, A_SEND, A_BLOCK, A_REPORT, A_DEL, A_MEMBERS, A_NOTE, A_NOTES, A_ANN, A_ANN_CLEAR, A_GRENAME, A_GPASS, A_GLEAVE, A_M_ADMIN, A_M_UNADMIN, A_M_OWNER, A_M_KICK, A_E2E_REQ, A_E2E_SAFETY, A_M_SAFETY, A_M_APPROVE, A_HOST_E2E_REQ, A_HOST_E2E_OPT, A_CLOSE };

static mrow_t *rowp(mrow_t *r, int *n, int kind, const char *label, const char *sub) {
    if (*n >= MAXROWS) return &r[MAXROWS - 1];
    mrow_t *w = &r[(*n)++]; memset(w, 0, sizeof *w); w->kind = kind; scopy(w->label, sizeof w->label, label); scopy(w->sub, sizeof w->sub, sub ? sub : ""); return w;
}
static void addb(mrow_t *w, const char *t, int act, int arg, uint32_t col) { if (w->nb < 4) { scopy(w->b[w->nb].t, sizeof w->b[0].t, t); w->b[w->nb].act = act; w->b[w->nb].arg = arg; w->b[w->nb].col = col; w->nb++; } }
#define BTN_BLUE 0x2F5C9E
#define BTN_RED 0x7A3A44
#define BTN_GREY 0x3A4A70

static int my_group_role(contact_t *g) { for (int i = 0; i < g->ngm; i++) if (!strcmp(g->gm[i].uid, acc_id)) return g->gm[i].role; return 0; }

static int menu_rows(mrow_t *r) {
    int n = 0; char b[160]; mrow_t *w;
    if (mn.page == PG_MAIN) {
        bool hosting = host_pid() != 0;
        rowp(r, &n, 0, T("Server"), NULL);
        snprintf(b, sizeof b, "%s%s%s", srv_label(), srv_host[0] ? "   -   " : "", srv_host);
        w = rowp(r, &n, 1, T("Connect to a server"), b); addb(w, T("Change..."), A_SERVER, 0, BTN_BLUE); addb(w, T("Reconnect"), A_REPIN + 100, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("Trust a changed server certificate"), T("Only if you know the server's certificate was renewed")); addb(w, T("Trust"), A_REPIN, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("Be the server for other C-OS machines"), hosting ? T("This C-OS is serving Message now") : T("Others then use  Connect to a server  with this machine's address"));
        addb(w, hosting ? T("Stop") : T("Start"), hosting ? A_HOST_OFF : A_HOST_ON, 0, hosting ? BTN_RED : MINE);
        if (hosting) { w = rowp(r, &n, 1, T("Refuse unencrypted messages"), host_e2e_state() ? T("On: only end-to-end encrypted messages pass through this server") : T("Off: the server also relays plain text (not recommended)")); addb(w, T("Refuse"), A_HOST_E2E_REQ, 0, host_e2e_state() ? MINE : BTN_GREY); addb(w, T("Allow"), A_HOST_E2E_OPT, 0, host_e2e_state() ? BTN_GREY : BTN_RED); }
        if (hosting) { w = rowp(r, &n, 1, T("Google sign-in for this server"), T("Needs a Google OAuth client (docs/messenger_selfhost.md)")); addb(w, T("Set up..."), A_GOOGLE_SETUP, 0, BTN_BLUE); addb(w, T("Required"), A_GOOGLE_REQ, 0, BTN_GREY); addb(w, T("Optional"), A_GOOGLE_OPT, 0, BTN_GREY); }
        rowp(r, &n, 0, T("Friends and groups"), NULL);
        w = rowp(r, &n, 1, T("Find people"), T("By name or @handle (only people who allowed it appear)")); addb(w, T("Search..."), A_FIND, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Search all messages"), T("Looks in every conversation on this computer")); addb(w, T("Search..."), A_SEARCHALL, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Add a friend by C-OS ID"), T("The ID is shown on their profile")); addb(w, T("Add..."), A_FRIEND, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Use an invite code"), T("Anybody can give you one from their profile")); addb(w, T("Enter..."), A_INVITE, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("My invite code"), my_invite[0] ? T("Copy it and send it to a friend") : T("Connect to a server first")); addb(w, T("Copy"), A_MYINVITE, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("Create a group"), T("Joining needs a 6-digit password (made for you if left empty)")); addb(w, T("Create..."), A_GNEW, 0, MINE);
        w = rowp(r, &n, 1, T("Join a group"), T("You need the group's ID and its 6-digit password")); addb(w, T("Join..."), A_GJOIN, 0, MINE);
        w = rowp(r, &n, 1, T("People you blocked"), T("Shows the list in the chat area")); addb(w, T("Show"), A_BLOCKS, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("Friends on the same network (no server)"), T("Add by IP address, or look for them")); addb(w, T("Add..."), A_LAN, 0, BTN_GREY); addb(w, T("Scan"), A_SCAN, 0, BTN_GREY);
        rowp(r, &n, 0, T("Account"), NULL);
        w = rowp(r, &n, 1, T("Profile and photo"), my_name); addb(w, T("Open"), A_PROFILE, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Display name"), my_name); addb(w, T("Change..."), A_NAME, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("@handle"), my_handle[0] ? my_handle : T("none yet")); addb(w, T("Change..."), A_HANDLE, 0, BTN_GREY);
        w = rowp(r, &n, 1, T("About me"), my_bio[0] ? my_bio : T("empty")); addb(w, T("Change..."), A_BIO, 0, BTN_GREY);
        snprintf(b, sizeof b, g_linked ? T("Linked: %s") : T("Not linked to a Google account"), g_masked);
        w = rowp(r, &n, 1, T("Google (Gmail) account"), b); addb(w, g_linked ? T("Check") : T("Link account"), A_GOOGLE, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Sign out the other devices"), T("Makes a new secret key; other devices must sign in again")); addb(w, T("Do it..."), A_LOGOUTALL, 0, BTN_RED);
        w = rowp(r, &n, 1, T("Sign out of this device"), g_linked ? T("You can sign in again with Google") : T("Link Google first, or this account is lost")); addb(w, T("Sign out..."), A_LOGOUT, 0, BTN_RED);
        w = rowp(r, &n, 1, T("App lock"), vlt.exists ? T("On: the password protects the app and encrypts the history") : T("Off: anybody at this computer can read your chats AND your encryption keys (turn it on!)"));
        if (vlt.exists) { addb(w, T("Lock now"), A_LOCK_NOW, 0, BTN_GREY); addb(w, T("Password"), A_LOCK_CHANGE, 0, BTN_GREY); addb(w, T("Off"), A_LOCK_OFF, 0, BTN_RED); } else addb(w, T("Turn on"), A_LOCK_ON, 0, MINE);
        if (vlt.exists) { w = rowp(r, &n, 1, T("Lock automatically after"), T("minutes without use (0: never)")); addb(w, T("Set..."), A_LOCK_TO, 0, BTN_GREY); }
        rowp(r, &n, 0, T("Privacy and sound"), NULL);
        w = rowp(r, &n, 1, T("Read receipts"), rr_on ? T("Others see when you read their messages") : T("Others do not see when you read their messages")); addb(w, rr_on ? T("On") : T("Off"), A_RR, 0, rr_on ? MINE : BTN_GREY);
        w = rowp(r, &n, 1, T("Online status and \"typing\""), pres_on ? T("Friends see when you are online and typing") : T("Hidden from your friends")); addb(w, pres_on ? T("On") : T("Off"), A_PRES, 0, pres_on ? MINE : BTN_GREY);
        { uint8_t mf[32]; char fs[40]; snprintf(fs, sizeof fs, "%s", T("no key")); if (e2e_ready) { e2e_my_fp(mf); snprintf(fs, sizeof fs, T("your key %02x%02x-%02x%02x-%02x%02x"), mf[0], mf[1], mf[2], mf[3], mf[4], mf[5]); }
          snprintf(b, sizeof b, "%s; %s", e2e_ready ? fs : e2e_why, e2e_req ? T("plain text is refused") : T("plain text allowed (not recommended)"));
          w = rowp(r, &n, 1, T("Require end-to-end encryption (1:1 chats)"), b); addb(w, e2e_req ? T("On") : T("Off"), A_E2E_REQ, 0, e2e_req ? MINE : BTN_RED); }
        w = rowp(r, &n, 1, T("Message sound"), snd_on ? T("A chime plays when a message arrives") : T("Silent")); addb(w, snd_on ? T("On") : T("Off"), A_SOUND, 0, snd_on ? MINE : BTN_GREY);
        rowp(r, &n, 0, T("Help"), NULL);
        w = rowp(r, &n, 1, T("Every command"), T("Typing /command in the message box does the same as these buttons")); addb(w, T("Show"), A_HELP, 0, BTN_BLUE);
    } else if (mn.page == PG_CHAT && cur >= 0) {
        contact_t *k = &ct[cur]; bool grp = is_group(k); int role = grp ? my_group_role(k) : 0;
        snprintf(b, sizeof b, "%s", k->name); rowp(r, &n, 0, b, NULL);
        w = rowp(r, &n, 1, T("Send a file or picture"), T("Or drop it onto the chat")); addb(w, T("Choose..."), A_SEND, 0, BTN_BLUE);
        w = rowp(r, &n, 1, T("Pin to the top of the list"), k->pinned ? T("Pinned") : T("Not pinned")); addb(w, k->pinned ? T("On") : T("Off"), A_PIN, 0, k->pinned ? MINE : BTN_GREY);
        w = rowp(r, &n, 1, T("Mute"), k->muted ? T("No sound for new messages (an @mention still rings)") : T("A chime plays for new messages")); addb(w, k->muted ? T("On") : T("Off"), A_MUTE, 0, k->muted ? MINE : BTN_GREY);
        w = rowp(r, &n, 1, T("Search in this chat"), T("Shows the newest matches")); addb(w, T("Search..."), A_SEARCH, 0, BTN_BLUE);
        if (grp) {
            { gkey_t *gk = gk_newest(k->uid); int chg = 0; for (int q = 0; q < k->ngm; q++) if (pkr(k->gm[q].uid)->kchanged) chg++;
              snprintf(b, sizeof b, "%s%s", !e2e_ready ? e2e_why : gk ? T("End-to-end encrypted: every message is signed by its sender") : T("Not encrypted yet: the key is made when somebody writes"), chg ? T("  -  A member's key changed: open Members") : "");
              rowp(r, &n, 1, T("Security (end-to-end)"), b); }
            w = rowp(r, &n, 1, T("Members"), T("Roles, hand over the group, remove people")); addb(w, T("Open"), A_MEMBERS, 0, BTN_BLUE);
            w = rowp(r, &n, 1, T("Notes"), T("Everybody can post notes for the group")); addb(w, T("Write..."), A_NOTE, 0, MINE); addb(w, T("Show all"), A_NOTES, 0, BTN_GREY);
            if (role >= 1) { w = rowp(r, &n, 1, T("Announcement"), k->ann[0] ? k->ann : T("none")); addb(w, T("Set..."), A_ANN, 0, BTN_BLUE); addb(w, T("Clear"), A_ANN_CLEAR, 0, BTN_GREY); w = rowp(r, &n, 1, T("Group name"), k->name); addb(w, T("Rename..."), A_GRENAME, 0, BTN_GREY); }
            if (role >= 2) { w = rowp(r, &n, 1, T("Group password"), T("Joining needs it; a new one locks out the old")); addb(w, T("Change..."), A_GPASS, 0, BTN_GREY); }
            w = rowp(r, &n, 1, T("Leave this group"), T("You can come back with the group ID and password")); addb(w, T("Leave..."), A_GLEAVE, 0, BTN_RED);
        } else {
            { const char *st = KP(k)->kchanged ? T("WARNING: their key changed - compare the safety number") : KP(k)->ks == 2 ? T("End-to-end encrypted, verified") : KP(k)->ks ? T("End-to-end encrypted (not verified yet)") : T("No encryption key: not end-to-end encrypted");
              w = rowp(r, &n, 1, T("Security (end-to-end)"), st); addb(w, KP(k)->kchanged ? T("Check new key") : T("Safety number"), A_E2E_SAFETY, 0, KP(k)->kchanged ? BTN_RED : BTN_BLUE); }
            w = rowp(r, &n, 1, T("Block"), T("They can no longer write to you or send requests")); addb(w, T("Block..."), A_BLOCK, 0, BTN_RED);
            w = rowp(r, &n, 1, T("Report"), T("Sends the last messages as evidence and blocks them")); addb(w, T("Report..."), A_REPORT, 0, BTN_RED);
            w = rowp(r, &n, 1, T("Remove this friend"), T("Deletes the conversation from this list")); addb(w, T("Remove..."), A_DEL, 0, BTN_RED);
        }
    } else if (mn.page == PG_MEMBERS && cur >= 0 && is_group(&ct[cur])) {
        contact_t *g = &ct[cur]; int role = my_group_role(g);
        snprintf(b, sizeof b, T("%s  -  %d members"), g->name, g->ngm); rowp(r, &n, 0, b, NULL);
        for (int i = 0; i < g->ngm && n < MAXROWS; i++) {
            int tr = rost_role(g, g->gm[i].uid); bool pend = tr < 0;                      /* the roster (signed by the admins) decides, not the server's list */
            const char *rn = pend ? T("Waiting for approval") : tr == 2 ? T("Owner") : tr == 1 ? T("Sub-admin") : T("Member"); bool me = !strcmp(g->gm[i].uid, acc_id);
            snprintf(b, sizeof b, "%s%s", g->gm[i].name, me ? T("  (you)") : ""); char sub[80]; snprintf(sub, sizeof sub, "%s   ID %s", rn, g->gm[i].uid);
            w = rowp(r, &n, 1, b, sub);
            if (!me && pkr(g->gm[i].uid)->ks) addb(w, pkr(g->gm[i].uid)->kchanged ? T("New key!") : pkr(g->gm[i].uid)->ks == 2 ? T("Verified") : T("Safety"), A_M_SAFETY, i, pkr(g->gm[i].uid)->kchanged ? BTN_RED : BTN_GREY);
            if (pend && rost_role(g, acc_id) >= 1) addb(w, T("Approve"), A_M_APPROVE, i, BTN_BLUE);
            if (me || g->gm[i].role == 2) continue;
            if (role >= 2) { addb(w, g->gm[i].role == 1 ? T("Demote") : T("Admin"), g->gm[i].role == 1 ? A_M_UNADMIN : A_M_ADMIN, i, BTN_GREY); addb(w, T("Owner"), A_M_OWNER, i, BTN_GREY); addb(w, T("Remove"), A_M_KICK, i, BTN_RED); }
            else if (role == 1 && g->gm[i].role == 0) addb(w, T("Remove"), A_M_KICK, i, BTN_RED);
        }
    }
    return n;
}
static int menu_total_h(mrow_t *r, int n) { int h = 0; for (int i = 0; i < n; i++) h += r[i].kind == 0 ? MN_HEAD : MN_ROW; return h; }

static void menu_close(void) { mn.open = false; dirty = true; }
static void menu_open(int page) { { char lb[40]; snprintf(lb, sizeof lb, "[MESSENGER] ui menu %d\n", page); cos_puts(lb); } mn.open = true; mn.page = page; mn.scroll = 0; dirty = true; }

static void menu_do(int act, int arg) {
    { char lb[40]; snprintf(lb, sizeof lb, "[MESSENGER] ui act %d\n", act); cos_puts(lb); }
    contact_t *g = (cur >= 0 && is_group(&ct[cur])) ? &ct[cur] : NULL;
    switch (act) {
    case A_SERVER: menu_close(); form_open(FK_SERVER, T("Connect to a Message server"), T("The address of the C-OS (or other) machine that serves Message. Example: 192.168.1.20  or  chat.example.com:5151"), 1, T("Server address"), NULL, NULL, false); scopy(fm.f[0].val, sizeof fm.f[0].val, srv_host); form_load_focus(); break;
    case A_REPIN + 100: gui_cmd("/server"); break;
    case A_REPIN: gui_cmd("/repin"); break;
    case A_HOST_ON: gui_cmd("/host on"); menu_close(); break;
    case A_HOST_OFF: gui_cmd("/host off"); break;
    case A_GOOGLE_SETUP: menu_close(); form_open(FK_GOOGLE, T("Google sign-in for this server"), T("Create an OAuth client of type \"TV and Limited Input devices\" in Google Cloud Console, then paste its ID and secret."), 2, T("Client ID (ends with .apps.googleusercontent.com)"), T("Client secret"), NULL, true); break;
    case A_HOST_E2E_REQ: gui_cmd("/host e2e require"); break;
    case A_HOST_E2E_OPT: gui_cmd("/host e2e optional"); break;
    case A_GOOGLE_REQ: gui_cmd("/host google require"); break;
    case A_GOOGLE_OPT: gui_cmd("/host google optional"); break;
    case A_PROFILE: menu_close(); prof_open_profile(); break;
    case A_NAME: menu_close(); form_open(FK_NAME, T("Display name"), T("What your friends see."), 1, T("Name"), NULL, NULL, false); scopy(fm.f[0].val, sizeof fm.f[0].val, my_name); form_load_focus(); break;
    case A_HANDLE: menu_close(); form_open(FK_HANDLE, T("@handle"), T("3-20 letters, digits, _ or . Leave empty to remove it."), 1, T("Handle"), NULL, NULL, false); scopy(fm.f[0].val, sizeof fm.f[0].val, my_handle); form_load_focus(); break;
    case A_BIO: menu_close(); form_open(FK_BIO, T("About me"), T("A short line people see in search and on your profile."), 1, T("Text"), NULL, NULL, false); scopy(fm.f[0].val, sizeof fm.f[0].val, my_bio); form_load_focus(); break;
    case A_GOOGLE: menu_close(); gui_cmd("/google"); break;
    case A_LOGOUTALL: menu_close(); form_confirm(T("Sign out the other devices?"), T("A new secret key is made. Every other device using this account has to sign in again."), "/logoutall"); break;
    case A_LOGOUT: menu_close(); form_confirm(T("Sign out of this device?"), g_linked ? T("You can sign in again with your Google account.") : T("This account is NOT linked to Google: signing out would lose it for good."), g_linked ? "/logout" : "/logout force"); break;
    case A_LOCK_ON: menu_close(); gui_cmd("/lock on"); break;
    case A_LOCK_OFF: menu_close(); gui_cmd("/lock off"); break;
    case A_LOCK_CHANGE: menu_close(); gui_cmd("/lock change"); break;
    case A_LOCK_NOW: menu_close(); gui_cmd("/lock now"); break;
    case A_LOCK_TO: menu_close(); form_open(FK_LOCKTO, T("Automatic lock"), T("Minutes without use before the app locks itself. 0 turns it off."), 1, T("Minutes"), NULL, NULL, false); break;
    case A_FIND: menu_close(); form_open(FK_FIND, T("Find people"), T("A name or @handle. Only people who allowed it are listed."), 1, T("Name or @handle"), NULL, NULL, false); break;
    case A_FRIEND: menu_close(); form_open(FK_FRIEND, T("Add a friend"), T("Their C-OS ID (a number). The greeting is optional."), 2, "C-OS ID", T("Greeting"), NULL, false); break;
    case A_INVITE: menu_close(); form_open(FK_INVITE, T("Use an invite code"), T("The 8-character code a friend gave you (dashes do not matter)."), 1, T("Invite code"), NULL, NULL, false); break;
    case A_MYINVITE: gui_cmd("/invite"); break;
    case A_BLOCKS: menu_close(); gui_cmd("/blocks"); break;
    case A_LAN: menu_close(); form_open(FK_LAN, T("Friend on the same network"), T("Their IP address (see Message on their computer). The name is optional."), 2, T("IP address"), T("Name"), NULL, false); break;
    case A_SCAN: menu_close(); gui_cmd("/scan"); break;
    case A_GNEW: menu_close(); form_open(FK_GNEW, T("Create a group"), T("Joining needs a 6-digit password. Leave it empty and one is made for you (shown once)."), 2, T("Group name"), T("6-digit password (optional)"), NULL, true); break;
    case A_GJOIN: menu_close(); form_open(FK_GJOIN, T("Join a group"), T("Ask a member for the group ID (starts with g) and the 6-digit password."), 2, T("Group ID"), T("6-digit password"), NULL, true); break;
    case A_RR: gui_cmd("/readreceipts %s", rr_on ? "off" : "on"); break;
    case A_PRES: gui_cmd("/presence %s", pres_on ? "off" : "on"); break;
    case A_SOUND: gui_cmd("/sound %s", snd_on ? "off" : "on"); break;
    case A_HELP: menu_close(); gui_cmd("/help"); break;
    case A_SEARCH: menu_close(); form_open(FK_SEARCH, T("Search in this chat"), T("Text to look for (upper and lower case do not matter)."), 1, T("Text"), NULL, NULL, false); break;
    case A_SEARCHALL: menu_close(); form_open(FK_SEARCHALL, T("Search all messages"), T("Looks in every conversation. Only you see the result."), 1, T("Text"), NULL, NULL, false); break;
    case A_E2E_REQ: gui_cmd("/e2e %s", e2e_req ? "off" : "on"); break;
    case A_E2E_SAFETY: menu_close(); gui_cmd("/e2e safety"); break;
    case A_PIN: gui_cmd(ct[cur].pinned ? "/unpin" : "/pin"); break;
    case A_MUTE: gui_cmd(ct[cur].muted ? "/unmute" : "/mute"); break;
    case A_SEND: menu_close(); pk.avatar = false; pk_show(); break;
    case A_BLOCK: menu_close(); form_confirm(T("Block this person?"), T("They can no longer write to you or send you requests. You can unblock them in the menu."), "/block"); break;
    case A_REPORT: menu_close(); form_open(FK_REPORT, T("Report this person"), T("The last messages are sent as evidence, and they are blocked. Say briefly what is wrong."), 1, T("Reason"), NULL, NULL, false); break;
    case A_DEL: menu_close(); form_confirm(T("Remove this conversation?"), T("The friendship ends and the history disappears from this list."), "/del"); break;
    case A_MEMBERS: mn.page = PG_MEMBERS; mn.scroll = 0; dirty = true; break;
    case A_NOTE: menu_close(); form_open(FK_NOTE, T("Post a note"), T("Everybody in the group can read it."), 1, T("Note"), NULL, NULL, false); break;
    case A_NOTES: menu_close(); gui_cmd("/gnotes"); break;
    case A_ANN: menu_close(); form_open(FK_ANN, T("Announcement"), T("Pinned above the chat for everybody."), 1, T("Text"), NULL, NULL, false); if (g) { scopy(fm.f[0].val, sizeof fm.f[0].val, g->ann); form_load_focus(); } break;
    case A_ANN_CLEAR: menu_close(); gui_cmd("/gannounce"); break;
    case A_GRENAME: menu_close(); form_open(FK_GRENAME, T("Rename the group"), "", 1, T("New name"), NULL, NULL, false); if (g) { scopy(fm.f[0].val, sizeof fm.f[0].val, g->name); form_load_focus(); } break;
    case A_GPASS: menu_close(); form_open(FK_GPASS, T("Group password"), T("6 digits. Leave empty to make a new one (shown once)."), 1, T("New password"), NULL, NULL, true); break;
    case A_GLEAVE: menu_close(); form_confirm(T("Leave this group?"), T("You can come back later with the group ID and password."), "/gleave"); break;
    case A_M_APPROVE: if (g && arg < g->ngm) gui_cmd("/gapprove %s", g->gm[arg].uid); break;
    case A_M_SAFETY: if (g && arg < g->ngm) { menu_close(); gui_cmd("/e2e safety %s", g->gm[arg].uid); } break;
    case A_M_ADMIN: if (g && arg < g->ngm) gui_cmd("/gadmin %s", g->gm[arg].uid); break;
    case A_M_UNADMIN: if (g && arg < g->ngm) gui_cmd("/gunadmin %s", g->gm[arg].uid); break;
    case A_M_OWNER: if (g && arg < g->ngm) { char t[160]; snprintf(t, sizeof t, "/gowner %s", g->gm[arg].uid); menu_close(); form_confirm(T("Hand the group over?"), T("You become a sub-admin; the new owner can change the password and remove admins."), t); } break;
    case A_M_KICK: if (g && arg < g->ngm) gui_cmd("/gkick %s", g->gm[arg].uid); break;
    }
}

static void menu_draw(void) {
    if (!mn.open) return;
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 160);
    int x = (win_w - MN_W) / 2, y = (win_h - MN_H) / 2;
    cui_round_rect(&c, x, y, MN_W, MN_H, 12, PANEL);
    const char *title = mn.page == PG_MAIN ? T("Menu") : mn.page == PG_CHAT ? T("This conversation") : T("Group members");
    cui_text(&c, f_b, x + 20, y + 14, title, TEXT);
    if (mn.page == PG_MEMBERS) gbtn(x + MN_W - 170, y + 8, 80, 26, T("Back"), 0x4A5068);
    gbtn(x + MN_W - 80, y + 8, 64, 26, T("Close"), 0x4A5068);
    mrow_t rows[MAXROWS]; int n = menu_rows(rows), th = menu_total_h(rows, n), vh = MN_H - 52;
    if (mn.scroll > th - vh) mn.scroll = th - vh; if (mn.scroll < 0) mn.scroll = 0;
    cui_clip(&c, x, y + 44, MN_W, vh);
    int ry = y + 44 - mn.scroll;
    for (int i = 0; i < n; i++) {
        mrow_t *w = &rows[i];
        if (w->kind == 0) { if (ry + MN_HEAD > y + 44 && ry < y + 44 + vh) { cui_text_fit(&c, f_b, x + 20, ry + 8, MN_W - 40, w->label, ACCENT); cui_fill(&c, x + 20, ry + MN_HEAD - 3, MN_W - 40, 1, LINE); } ry += MN_HEAD; continue; }
        if (ry + MN_ROW > y + 44 && ry < y + 44 + vh) {
            int bw = 0; for (int k = 0; k < w->nb; k++) bw += 12 + (cui_text_width(f_s, w->b[k].t) + 22);
            cui_text_fit(&c, f_t, x + 20, ry + 5, MN_W - 40 - bw, w->label, TEXT);
            cui_text_fit(&c, f_s, x + 20, ry + 25, MN_W - 40 - bw, w->sub, MUTED);
            int bx = x + MN_W - 20;
            for (int k = w->nb - 1; k >= 0; k--) { int tw = cui_text_width(f_s, w->b[k].t) + 22; bx -= tw; gbtn(bx, ry + 9, tw, 28, w->b[k].t, w->b[k].col); bx -= 12; }
        }
        ry += MN_ROW;
    }
    cui_unclip(&c);
    if (th > vh) { int bh = vh * vh / th; if (bh < 24) bh = 24; cui_round_rect(&c, x + MN_W - 8, y + 44 + (vh - bh) * mn.scroll / (th - vh), 4, bh, 2, 0x4A5068); }
}
static void menu_click(int mx, int my) {
    int x = (win_w - MN_W) / 2, y = (win_h - MN_H) / 2;
    if (mx < x || mx >= x + MN_W || my < y || my >= y + MN_H) { menu_close(); return; }
    if (my < y + 40) {
        if (mx >= x + MN_W - 80) menu_close(); else if (mn.page == PG_MEMBERS && mx >= x + MN_W - 170 && mx < x + MN_W - 90) { mn.page = PG_CHAT; mn.scroll = 0; dirty = true; }
        return;
    }
    mrow_t rows[MAXROWS]; int n = menu_rows(rows), vh = MN_H - 52; (void)vh;
    int ry = y + 44 - mn.scroll;
    for (int i = 0; i < n; i++) {
        int h = rows[i].kind == 0 ? MN_HEAD : MN_ROW;
        if (my >= ry && my < ry + h && rows[i].kind == 1 && my >= y + 44) {
            int bx = x + MN_W - 20;
            for (int k = rows[i].nb - 1; k >= 0; k--) { int tw = cui_text_width(f_s, rows[i].b[k].t) + 22; bx -= tw; if (mx >= bx && mx < bx + tw && my >= ry + 9 && my < ry + 37) { menu_do(rows[i].b[k].act, rows[i].b[k].arg); return; } bx -= 12; }
            return;
        }
        ry += h;
    }
}
static void menu_wheel(int w) { mn.scroll -= w * 60; dirty = true; }
static void gui_edit(int what) { menu_do(what == 0 ? A_NAME : what == 1 ? A_HANDLE : A_BIO, 0); }

static void drafts_tick(void) {
    uint64_t now = cos_time_ms();
    if (cur >= 0 && edit_msg < 0 && !fm.open && !pwp.open) {
        const char *d = ct[cur].draft ? ct[cur].draft : "";
        if (strcmp(d, in) != 0) { free(ct[cur].draft); ct[cur].draft = in_len ? strdup(in) : NULL; drafts_dirty = true; drafts_t = now; }
    }
    if (drafts_dirty && now - drafts_t > 2000) drafts_save();
}
