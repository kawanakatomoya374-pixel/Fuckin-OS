/* App lock ("quick password"). Optional, switched on and off inside the app with /lock.
 *
 * When it is on, the account token and every conversation file are stored encrypted (AES-256-GCM) with a random master key; that
 * key is kept in vault.bin wrapped by a key derived from the password with scrypt (N=2^15, r=8, p=1 - about 32 MB of memory per guess,
 * which is what makes guessing a password on a stolen disk slow). The app asks for the password when it starts and after
 * the idle time; while it is locked nothing is decrypted, no network is used and the conversations are wiped from memory.
 * Wrong passwords are answered with growing waiting times (3rd failure 5 s ... 7th and later 15 min), kept across restarts.
 * Not covered: attachments you saved to /downloads, and what the operating system or a debugger could read from memory while unlocked. */
#include "vault_crypto.inc"

#define VAULT_FILE  DIR "/vault.bin"
#define LOCKSTATE   DIR "/lockstate"
#define LOCKCFG     DIR "/lockcfg"
#define KDF_LOGN    15
#define KDF_R       8
#define KDF_P       1
#define ENC_MAGIC   "CENC1"
#define ENC_OVER    (5 + 12 + 16)

typedef struct __attribute__((packed)) { uint8_t magic[6], logn, r, p, pad, salt[16], nonce[12], wrapped[32], tag[16]; } vhdr_t;
static struct { bool exists, unlocked; uint8_t key[32]; int idle_min; uint32_t fails; int64_t until_unix; uint64_t last_input; } vlt = { .idle_min = 5 };

/* ---------------------------------------------------------------- randomness: RDRAND when there is one, plus input timing */
static br_sha256_context ent_sha; static bool ent_ready;
static inline uint64_t tsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static void ent_mix(uint64_t v) {
    if (!ent_ready) { br_sha256_init(&ent_sha); ent_ready = true; }
    uint64_t t = tsc(); br_sha256_update(&ent_sha, &v, 8); br_sha256_update(&ent_sha, &t, 8);
}
static void vault_random(uint8_t *out, size_t n) {
    uint8_t seed[32 + 32 + 16]; size_t o = 0;
    uint32_t eax, ebx, ecx, edx; __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    for (int i = 0; i < 4; i++) {
        uint64_t w = 0; unsigned char ok = 0;
        if (ecx & (1u << 30)) for (int tries = 0; tries < 10 && !ok; tries++) __asm__ volatile("rdrand %0; setc %1" : "=r"(w), "=qm"(ok));
        if (!ok) for (int j = 0; j < 64; j++) w = (w << 7 | w >> 57) ^ (tsc() + (uint64_t)j);
        memcpy(seed + o, &w, 8); o += 8;
    }
    if (!ent_ready) ent_mix(0);
    br_sha256_context c = ent_sha; br_sha256_out(&c, seed + o); o += 32;
    uint64_t t[2] = { cos_time_ms(), (uint64_t)cos_time_unix() }; memcpy(seed + o, t, 16); o += 16;
    br_hmac_drbg_context d; br_hmac_drbg_init(&d, &br_sha256_vtable, seed, o); br_hmac_drbg_generate(&d, out, n);
    memset(seed, 0, sizeof seed);
}

/* ---------------------------------------------------------------- encrypted file access */
static ssize_t vread(const char *path, void *buf, size_t cap) {
    ssize_t n = cos_read_file(path, buf, cap);
    if (n < 5 || memcmp(buf, ENC_MAGIC, 5) != 0) return n;
    if (!vlt.unlocked || n < ENC_OVER) return -1;
    uint8_t *b = (uint8_t *)buf; size_t pl = (size_t)n - ENC_OVER; uint8_t tag[16]; memcpy(tag, b + 17 + pl, 16);
    if (!gcm_open(vlt.key, b + 5, path, b + 17, pl, tag)) return -1;
    memmove(b, b + 17, pl);
    return (ssize_t)pl;
}
static ssize_t vwrite(const char *path, const void *buf, size_t n) {
    if (!vlt.exists) return cos_write_file(path, buf, n);
    if (!vlt.unlocked) return -1;                                          /* never write plain text while a vault exists */
    uint8_t *o = (uint8_t *)malloc(n + ENC_OVER); if (!o) return -1;
    memcpy(o, ENC_MAGIC, 5); vault_random(o + 5, 12); memcpy(o + 17, buf, n);
    gcm_seal(vlt.key, o + 5, path, o + 17, n, o + 17 + n);
    ssize_t r = cos_write_file(path, o, n + ENC_OVER); free(o);
    return r == (ssize_t)(n + ENC_OVER) ? (ssize_t)n : -1;
}
static bool vault_locked(void) { return vlt.exists && !vlt.unlocked; }

/* ---------------------------------------------------------------- the vault file */
static void vault_zero(void) { memset(vlt.key, 0, sizeof vlt.key); vlt.unlocked = false; }
static int vault_kdf(const char *pw, const uint8_t salt[16], int logn, int r, int p, uint8_t key[32]) {
    return scrypt_kdf((const uint8_t *)pw, strlen(pw), salt, 16, (uint32_t)1 << logn, (uint32_t)r, (uint32_t)p, key, 32);
}
enum { VR_OK = 0, VR_WRONG = 1, VR_ERR = 2 };
static int vault_open(const char *pw) {
    vhdr_t h; cos_stat_t st;
    if (cos_stat(VAULT_FILE, &st) != 0 || st.size != sizeof h || cos_read_file(VAULT_FILE, &h, sizeof h) != (ssize_t)sizeof h || memcmp(h.magic, "CVLT1", 6) != 0) return VR_ERR;
    if (h.logn < 10 || h.logn > 20 || h.r != KDF_R || h.p != KDF_P) return VR_ERR;
    uint8_t kek[32], key[32];
    if (vault_kdf(pw, h.salt, h.logn, h.r, h.p, kek) != 0) return VR_ERR;
    memcpy(key, h.wrapped, 32);
    int ok = gcm_open(kek, h.nonce, "cvlt1", key, 32, h.tag);
    memset(kek, 0, 32);
    if (!ok) { memset(key, 0, 32); return VR_WRONG; }
    memcpy(vlt.key, key, 32); memset(key, 0, 32); vlt.unlocked = true;
    return VR_OK;
}
static int vault_write_header(const char *pw, const uint8_t key[32]) {
    vhdr_t h; memset(&h, 0, sizeof h); memcpy(h.magic, "CVLT1", 6); h.logn = KDF_LOGN; h.r = KDF_R; h.p = KDF_P;
    vault_random(h.salt, 16); vault_random(h.nonce, 12);
    uint8_t kek[32]; if (vault_kdf(pw, h.salt, h.logn, h.r, h.p, kek) != 0) return -1;
    memcpy(h.wrapped, key, 32); gcm_seal(kek, h.nonce, "cvlt1", h.wrapped, 32, h.tag); memset(kek, 0, 32);
    return cos_write_file(VAULT_FILE, &h, sizeof h) == (ssize_t)sizeof h ? 0 : -1;
}

/* the files the lock protects */
static bool vault_file_wanted(const char *n) {
    return !strcmp(n, "account.txt") || !strcmp(n, "contacts.txt") || !strcmp(n, "unread.txt") || (!strncmp(n, "chat_", 5) && strstr(n, ".txt"))
        || !strcmp(n, "e2e.key") || !strcmp(n, "e2e_peers.txt") || !strcmp(n, "e2e_groups.bin") || !strcmp(n, "e2e_roster.bin") || !strcmp(n, "e2e_mode.txt") || !strcmp(n, "prefs.txt") || !strcmp(n, "drafts.txt");
}
static void vault_convert_dir(const char *dir, bool whole_dir, bool encrypt) {            /* whole_dir: every file in it (the cache of received files) */
    int fd = cos_opendir(dir); if (fd < 0) return;
    static char names[256][40]; int nn = 0; cos_dirent_t de;
    while (nn < 256 && cos_readdir(fd, &de) == 1) if (!de.is_dir && strlen(de.name) < 40 && (whole_dir || vault_file_wanted(de.name))) snprintf(names[nn++], 40, "%s", de.name);
    cos_close(fd);
    for (int i = 0; i < nn; i++) {
        char p[96]; snprintf(p, sizeof p, "%s/%s", dir, names[i]); cos_stat_t st;
        if (cos_stat(p, &st) != 0 || st.size == 0 || st.size > (9u << 20)) continue;
        uint8_t *b = (uint8_t *)malloc((size_t)st.size + 1); if (!b) continue;
        ssize_t n = vread(p, b, (size_t)st.size);                          /* plain or encrypted: vread copes with both */
        if (n >= 0) {
            if (encrypt) { vlt.exists = true; vwrite(p, b, (size_t)n); }
            else cos_write_file(p, b, (size_t)n);
        }
        memset(b, 0, (size_t)st.size); free(b);
    }
}
static void vault_convert_all(bool encrypt) { vault_convert_dir(DIR, false, encrypt); vault_convert_dir(DIR "/files", true, encrypt); }

/* ---------------------------------------------------------------- lock state on disk and the waiting time after wrong passwords */
static void lockstate_save(void) {
    if (!vlt.fails) { cos_unlink(LOCKSTATE); return; }
    char b[64]; int n = snprintf(b, sizeof b, "%u %lld\n", vlt.fails, (long long)vlt.until_unix); cos_write_file(LOCKSTATE, b, (size_t)n);
}
static void lockstate_load(void) {
    char b[64] = {0}; cos_stat_t st; long long u = 0; unsigned f = 0;
    if (cos_stat(LOCKSTATE, &st) == 0 && st.size > 0 && st.size < 60 && cos_read_file(LOCKSTATE, b, (size_t)st.size) > 0 && sscanf(b, "%u %lld", &f, &u) == 2) { vlt.fails = f; vlt.until_unix = u; }
    if (cos_stat(LOCKCFG, &st) == 0 && st.size > 0 && st.size < 8) { char c[8] = {0}; cos_read_file(LOCKCFG, c, (size_t)st.size); vlt.idle_min = atoi(c); if (vlt.idle_min < 0 || vlt.idle_min > 600) vlt.idle_min = 5; }
}
static int fail_delay(uint32_t fails) { static const int d[] = { 0, 0, 0, 5, 15, 60, 300, 900 }; return d[fails > 7 ? 7 : fails]; }

/* ---------------------------------------------------------------- the password window */
enum { PW_UNLOCK = 1, PW_SET, PW_OFF, PW_CHANGE };
static struct { bool open; int mode, nf, cur; char buf[3][72]; int len[3]; char msg[120]; bool busy; } pwp;
static const char *PW_LABEL[4][3] = { {0}, {"パスワード"}, {"新しいパスワード (6文字以上)", "もう一度入力"}, {"今のパスワード"} };
static const char *pw_label(int f) {
    if (pwp.mode == PW_UNLOCK) return "パスワード";
    if (pwp.mode == PW_SET) return PW_LABEL[2][f];
    if (pwp.mode == PW_OFF) return "今のパスワード";
    static const char *c[3] = { "今のパスワード", "新しいパスワード (6文字以上)", "もう一度入力" }; return c[f];
}
static void pw_clear(void) { memset(pwp.buf, 0, sizeof pwp.buf); memset(pwp.len, 0, sizeof pwp.len); pwp.cur = 0; }
static void pw_show(int mode) {
    pw_clear(); pwp.open = true; pwp.mode = mode; pwp.busy = false; pwp.msg[0] = 0;
    pwp.nf = mode == PW_SET ? 2 : mode == PW_CHANGE ? 3 : 1; dirty = true;
}
static void pw_close(void) { pw_clear(); pwp.open = false; pwp.msg[0] = 0; dirty = true; }
static void pw_busy(const char *t) { snprintf(pwp.msg, sizeof pwp.msg, "%s", t); pwp.busy = true; draw(); cos_win2_present(g_win); }

static void load_all(void);                 /* in main.c */
static bool vault_enabled(void) { return vlt.exists; }
static void e2e_wipe_memory(void);
static void vault_wipe_memory(void) {
    e2e_wipe_memory(); up_wipe(); view_close(); pk.open = false; sr.open = pf.open = gl.open = false;
    for (int i = 0; i < nct; i++) {
        for (int j = 0; j < ct[i].n; j++) { thumb_free(ct[i].m[j].thumb); if (ct[i].m[j].text) { memset(ct[i].m[j].text, 0, strlen(ct[i].m[j].text)); free(ct[i].m[j].text); } }
        free(ct[i].m); free(ct[i].gm);
    }
    memset(ct, 0, sizeof ct); nct = 0; cur = -1; nreq = 0; memset(reqs, 0, sizeof reqs); note_clear();
    memset(acc_token, 0, sizeof acc_token); acc_id[0] = 0; srv_host[0] = 0; last_seq = 0; g_linked = false; g_masked[0] = 0;
    in_clear(); edit_msg = -1; sel_msg = -1; scroll_px = list_scroll = 0;
}
static void vault_lock_now(void) {
    if (!vlt.exists || !vlt.unlocked) return;
    if (srv_sd >= 0) srv_drop(NULL);
    vault_wipe_memory(); vault_zero();
    pw_show(PW_UNLOCK);
}
static void vault_unlocked_ok(void) {
    vlt.fails = 0; vlt.until_unix = 0; lockstate_save(); pw_close(); cos_puts("[MESSENGER] lock: unlocked\n");
    load_all(); srv_next_try = 0; vlt.last_input = cos_time_ms();
}

static bool pw_weak(const char *p) {
    size_t n = strlen(p); bool same = true, digits = true;
    for (size_t i = 0; i < n; i++) { if (p[i] != p[0]) same = false; if (p[i] < '0' || p[i] > '9') digits = false; }
    static const char *bad[] = { "123456", "1234567", "12345678", "123456789", "password", "qwerty", "abcdef", "654321", "111111", "000000", "letmein", "iloveyou" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) if (!strcmp(p, bad[i])) return true;
    (void)digits; return same;
}

static void pw_submit(void) {
    pwp.msg[0] = 0;
    if (pwp.mode == PW_UNLOCK) {
        int64_t now = cos_time_unix();
        if (now < vlt.until_unix) { snprintf(pwp.msg, sizeof pwp.msg, "続けて間違えました。あと %d 秒お待ちください", (int)(vlt.until_unix - now)); pw_clear(); cos_puts("[MESSENGER] lock: refused during the waiting time\n"); dirty = true; return; }
        pw_busy("確認しています... (数秒かかります)");
        int r = vault_open(pwp.buf[0]);
        pwp.busy = false;
        if (r == VR_OK) { vault_unlocked_ok(); return; }
        if (r == VR_ERR) { snprintf(pwp.msg, sizeof pwp.msg, "ロックの情報を読み込めませんでした (メモリ不足またはファイル破損)"); pw_clear(); dirty = true; return; }
        vlt.fails++; int d = fail_delay(vlt.fails); vlt.until_unix = cos_time_unix() + d; lockstate_save(); pw_clear();
        { char lb[80]; snprintf(lb, sizeof lb, "[MESSENGER] lock: wrong password #%u, wait %d s\n", vlt.fails, d); cos_puts(lb); }
        if (d) snprintf(pwp.msg, sizeof pwp.msg, "パスワードが違います。%d 秒待ってからもう一度どうぞ (失敗 %u 回)", d, vlt.fails);
        else snprintf(pwp.msg, sizeof pwp.msg, "パスワードが違います (失敗 %u 回)", vlt.fails);
        dirty = true; return;
    }
    if (pwp.mode == PW_SET || pwp.mode == PW_CHANGE) {
        int nf = pwp.mode == PW_SET ? 0 : 1;                                   /* index of the new password */
        const char *np = pwp.buf[nf];
        if (pwp.len[nf] < 6) { snprintf(pwp.msg, sizeof pwp.msg, "6文字以上にしてください"); pwp.busy = false; dirty = true; return; }
        if (pw_weak(np)) { snprintf(pwp.msg, sizeof pwp.msg, "よくある推測されやすいパスワードです。別のものにしてください"); pwp.busy = false; dirty = true; return; }
        if (strcmp(np, pwp.buf[nf + 1]) != 0) { snprintf(pwp.msg, sizeof pwp.msg, "2回の入力が一致しません"); pwp.busy = false; dirty = true; return; }
        if (pwp.mode == PW_CHANGE) {
            uint8_t keep[32]; memcpy(keep, vlt.key, 32);
            pw_busy("今のパスワードを確認しています...");
            vault_zero(); int r = vault_open(pwp.buf[0]);                       /* the old password must be right */
            if (r != VR_OK) { memcpy(vlt.key, keep, 32); vlt.unlocked = true; memset(keep, 0, 32); snprintf(pwp.msg, sizeof pwp.msg, "今のパスワードが違います"); pwp.busy = false; pw_clear(); dirty = true; return; }
            pw_busy("新しいパスワードで保存しています...");
            int w = vault_write_header(np, vlt.key); memset(keep, 0, 32); pwp.busy = false;
            if (w != 0) { snprintf(pwp.msg, sizeof pwp.msg, "保存できませんでした"); dirty = true; return; }
            pw_close(); set_status("パスワードを変更しました"); return;
        }
        pw_busy("暗号化の鍵を作っています... (数秒かかります)");
        uint8_t key[32]; vault_random(key, 32);
        if (vault_write_header(np, key) != 0) { memset(key, 0, 32); snprintf(pwp.msg, sizeof pwp.msg, "メモリ不足または保存失敗のため有効にできませんでした"); pwp.busy = false; dirty = true; return; }
        memcpy(vlt.key, key, 32); memset(key, 0, 32); vlt.exists = vlt.unlocked = true; vlt.last_input = cos_time_ms();
        vault_convert_all(true);                                               /* encrypt what is already on disk */
        pwp.busy = false;
        bool digits = true; for (int i = 0; i < pwp.len[0]; i++) if (pwp.buf[0][i] < '0' || pwp.buf[0][i] > '9') digits = false;
        char nb[640]; snprintf(nb, sizeof nb, "アプリのロックを有効にしました\n> アカウントと会話の履歴を暗号化しました。起動時と、%d 分操作しなかったときにパスワードを聞きます。(/lock timeout 分 で変更)%s\n> パスワードを忘れると履歴は戻せません。Google 連携済みなら /google でアカウント自体は復旧できます。",
                              vlt.idle_min, (digits && pwp.len[0] < 10) ? "\n> 数字だけの短いパスワードは、ディスクを盗まれた場合に破られやすくなります。8文字以上で英字も混ぜるのがおすすめです (/lock change)。" : "");
        pw_close(); note_add(nb); return;
    }
    if (pwp.mode == PW_OFF) {
        pw_busy("確認しています...");
        uint8_t keep[32]; memcpy(keep, vlt.key, 32); vault_zero();
        int r = vault_open(pwp.buf[0]); pwp.busy = false;
        if (r != VR_OK) { memcpy(vlt.key, keep, 32); vlt.unlocked = true; memset(keep, 0, 32); snprintf(pwp.msg, sizeof pwp.msg, "パスワードが違います"); pw_clear(); dirty = true; return; }
        memset(keep, 0, 32);
        vault_convert_all(false); cos_unlink(VAULT_FILE); vlt.exists = false; vault_zero();
        pw_close(); set_status("アプリのロックをオフにしました (保存データは暗号化されません)"); return;
    }
}

static void pw_key(const cos_win_event_t *ev) {
    if (pwp.busy) return;
    if (ev->special == COS_KEY_ESC) { if (pwp.mode != PW_UNLOCK) pw_close(); return; }
    if (ev->special == COS_KEY_ENTER) { if (pwp.cur < pwp.nf - 1) pwp.cur++; else pw_submit(); dirty = true; return; }
    if (ev->special == COS_KEY_TAB || ev->special == COS_KEY_DOWN) { pwp.cur = (pwp.cur + 1) % pwp.nf; dirty = true; return; }
    if (ev->special == COS_KEY_UP) { pwp.cur = (pwp.cur + pwp.nf - 1) % pwp.nf; dirty = true; return; }
    int f = pwp.cur;
    if (ev->special == COS_KEY_BACKSPACE) { if (pwp.len[f]) pwp.buf[f][--pwp.len[f]] = 0; dirty = true; return; }
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == 'v' || ev->ascii == 'V')) {
        char tmp[72]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1); if (n < 0) n = 0; tmp[n] = 0;
        for (int i = 0; tmp[i] && tmp[i] != '\n' && tmp[i] != '\r' && pwp.len[f] < 64; i++) if ((unsigned char)tmp[i] >= 32 && tmp[i] < 127) pwp.buf[f][pwp.len[f]++] = tmp[i];
        memset(tmp, 0, sizeof tmp); dirty = true; return;
    }
    if ((unsigned char)ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & COS_MOD_CTRL) && pwp.len[f] < 64) { pwp.buf[f][pwp.len[f]++] = ev->ascii; pwp.msg[0] = 0; dirty = true; }
}

#define PW_W 440
static void pw_geom(int *x, int *y, int *h) { *h = pwp.mode == PW_UNLOCK ? 300 : 120 + pwp.nf * 70; *x = (win_w - PW_W) / 2; *y = (win_h - *h) / 2; }
static void pw_draw(void) {
    if (!pwp.open) return;
    int x, y, h; pw_geom(&x, &y, &h);
    if (pwp.mode == PW_UNLOCK) cui_fill(&c, 0, 0, win_w, win_h, BG); else cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x000000, 170);
    cui_round_rect(&c, x, y, PW_W, h, 14, PANEL);
    int ty = y + 20;
    if (pwp.mode == PW_UNLOCK) {                                           /* a padlock */
        cui_round_rect_outline(&c, x + PW_W / 2 - 16, ty, 32, 30, 14, 3.0f, ACCENT);
        cui_round_rect(&c, x + PW_W / 2 - 24, ty + 20, 48, 34, 7, ACCENT);
        cui_text_center(&c, f_h, x, ty + 64, PW_W, 26, "C-OS Message はロックされています", TEXT); ty += 100;
    } else {
        const char *t = pwp.mode == PW_SET ? "アプリのロックを有効にする" : pwp.mode == PW_OFF ? "アプリのロックをオフにする" : "パスワードを変更する";
        cui_text(&c, f_h, x + 24, ty, t, TEXT); ty += 42;
    }
    for (int f = 0; f < pwp.nf; f++) {
        cui_text(&c, f_s, x + 24, ty, pw_label(f), MUTED);
        cui_round_rect(&c, x + 24, ty + 18, PW_W - 48, 34, 8, BG);
        if (f == pwp.cur) cui_round_rect_outline(&c, x + 24, ty + 18, PW_W - 48, 34, 8, 1.5f, ACCENT);
        for (int i = 0; i < pwp.len[f] && i < 30; i++) cui_round_rect(&c, x + 36 + i * 12, ty + 31, 8, 8, 4, TEXT);
        if (f == pwp.cur && ((cos_time_ms() / 500) & 1)) cui_fill(&c, x + 36 + (pwp.len[f] > 30 ? 30 : pwp.len[f]) * 12, ty + 27, 2, 16, ACCENT);
        ty += 66;
    }
    int64_t wait = vlt.until_unix - cos_time_unix(); char wm[100];
    if (pwp.mode == PW_UNLOCK && wait > 0) { snprintf(wm, sizeof wm, "続けて間違えました。あと %d 秒お待ちください", (int)wait); cui_text_fit(&c, f_s, x + 24, ty - 4, PW_W - 48, wm, WARN); }
    else if (pwp.msg[0]) cui_text_fit(&c, f_s, x + 24, ty - 4, PW_W - 48, pwp.msg, pwp.busy ? ACCENT : WARN);
    int by = y + h - 50;
    cui_round_rect(&c, x + PW_W - 24 - 120, by, 120, 32, 8, ACCENT); cui_text_center(&c, f_b, x + PW_W - 24 - 120, by, 120, 32, pwp.mode == PW_UNLOCK ? "ロック解除" : "OK", 0xFFFFFF);
    if (pwp.mode != PW_UNLOCK) { cui_round_rect(&c, x + PW_W - 24 - 120 - 100, by, 90, 32, 8, 0x4A5068); cui_text_center(&c, f_s, x + PW_W - 24 - 120 - 100, by, 90, 32, "キャンセル", 0xFFFFFF); }
}
static void pw_click(int mx, int my) {
    if (pwp.busy) return;
    int x, y, h; pw_geom(&x, &y, &h); int by = y + h - 50;
    if (my >= by && my < by + 32) {
        if (mx >= x + PW_W - 144 && mx < x + PW_W - 24) { pw_submit(); return; }
        if (pwp.mode != PW_UNLOCK && mx >= x + PW_W - 244 && mx < x + PW_W - 154) { pw_close(); return; }
    }
    int ty = y + (pwp.mode == PW_UNLOCK ? 120 : 62);
    for (int f = 0; f < pwp.nf; f++) { if (my >= ty + 18 && my < ty + 52 && mx >= x + 24 && mx < x + PW_W - 24) { pwp.cur = f; dirty = true; } ty += 66; }
}

/* ---------------------------------------------------------------- called from main.c */
static void vault_startup(void) {                                          /* before anything is loaded */
    cos_stat_t st; vlt.exists = cos_stat(VAULT_FILE, &st) == 0; lockstate_load(); vlt.last_input = cos_time_ms();
    if (vlt.exists) pw_show(PW_UNLOCK);
}
static void vault_note_input(const cos_win_event_t *ev) {
    vlt.last_input = cos_time_ms(); ent_mix(((uint64_t)(uint32_t)ev->x << 32) ^ (uint32_t)ev->y ^ ((uint64_t)ev->type << 56) ^ ((uint64_t)(uint8_t)ev->ascii << 48));
}
static void vault_tick(void) {                                             /* every pass of the main loop */
    static char locked_title; bool lk = vault_locked();
    if (lk != (bool)locked_title) { locked_title = lk; cos_win2_set_title(g_win, lk ? T("C-OS Message (locked)") : "C-OS Message"); }
    if (lk) { if (vlt.until_unix > cos_time_unix() || pwp.msg[0]) dirty = true; return; }
    if (vlt.exists && vlt.idle_min > 0 && !up.active && cos_time_ms() - vlt.last_input > (uint64_t)vlt.idle_min * 60000) vault_lock_now();
}

static void vault_cmd(char *arg) {
    if (!arg || !*arg) {
        char b[260]; snprintf(b, sizeof b, "アプリのロック\n状態\t%s\n自動ロック\t%s\n> /lock on  /lock off  /lock change  /lock now  /lock timeout <分>  (0 で自動ロックなし)",
                              vlt.exists ? "オン (保存データは暗号化されています)" : "オフ", vlt.idle_min ? "操作がなければ自動でロック" : "なし");
        note_add(b); return;
    }
    if (!strcmp(arg, "on")) { if (vlt.exists) set_status("アプリのロックはすでにオンです (/lock change でパスワード変更)"); else pw_show(PW_SET); }
    else if (!strcmp(arg, "off")) { if (!vlt.exists) set_status("アプリのロックはオフです"); else pw_show(PW_OFF); }
    else if (!strcmp(arg, "change")) { if (!vlt.exists) set_status("先に /lock on でロックを有効にしてください"); else pw_show(PW_CHANGE); }
    else if (!strcmp(arg, "now")) { if (!vlt.exists) set_status("ロックがオフです。/lock on でパスワードを設定してください"); else vault_lock_now(); }
    else if (!strncmp(arg, "timeout", 7)) {
        int m = atoi(arg + 7); if (m < 0 || m > 600 || !arg[7]) { set_status("使い方: /lock timeout <分 0-600>"); return; }
        vlt.idle_min = m; char b[8]; int n = snprintf(b, sizeof b, "%d", m); cos_write_file(LOCKCFG, b, (size_t)n);
        char s[80]; if (m) snprintf(s, sizeof s, "%d 分操作がなければ自動でロックします", m); else snprintf(s, sizeof s, "自動ロックをなしにしました"); set_status(s);
    } else set_status("使い方: /lock on|off|change|now|timeout <分>");
}
