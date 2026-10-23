/* e2ec.inc - end-to-end encryption in the Message client (the cryptography itself is in e2e.inc).
 *
 * What it does for a 1:1 chat with a server friend:
 *  - makes the account's identity key and weekly prekeys on first use (/messenger/e2e.key; encrypted again by the app lock when it is on),
 *    publishes the public bundle to the server (KEYPUT) and fetches the friend's bundle (KEYGET);
 *  - pins the friend's identity the first time it is seen (trust on first use). A different identity later is NEVER accepted silently:
 *    sending stops, the friend's incoming messages wait, and the chat shows a warning until the user compares the safety number and
 *    chooses T("Trust new key");
 *  - encrypts every text (and the key of every attachment) before it leaves the app; the server only ever sees envelopes.
 *  T("Require encryption") (default on) also refuses to SEND plain text to a friend and refuses to SHOW plain text received from one:
 *  a relay that strips the encryption (a downgrade) therefore cannot get a message through. */

#define E2E_MAXSPK 6
#define E2E_ROTATE (7u * 86400u)                  /* a new prekey every week ... */
#define E2E_KEEP (30u * 86400u)                   /* ... and old ones are deleted after 30 days (forward secrecy) */
#define E2E_REFRESH_MS 600000                     /* a cached friend bundle older than this is fetched again before sending */
#define MF_KEYWAIT 8                              /* our message waits for the friend's key */
enum { E2E_WAIT = 0, E2E_OK = 1, E2E_PLAIN = 2, E2E_FAIL = -1 };
enum { E2E_PARKED = 0, E2E_GOT = 1 };

typedef struct { char magic[8]; e2e_id_t id; uint32_t nspk; e2e_spk_t spk[E2E_MAXSPK]; } e2e_store_t;
static e2e_store_t e2e_st;
static bool e2e_ready;                            /* we have keys and a safe random source */
static bool e2e_req = true;                       /* require encryption */
static char e2e_why[120];
static uint8_t e2e_published_spk_valid; static uint32_t e2e_published_spk;
static bool e2e_bad;                                         /* the last incoming text could not be opened: *out is a placeholder, not content */
static char *e2e_ph(const char *t) { e2e_bad = true; return strdup(t); }

static void vault_random(uint8_t *out, size_t n);
static bool vault_enabled(void);
static void srv_handle(char **f, int n);
static void save_chat(contact_t *k);
static bool srv_tx5(const char *a, const char *b, const char *c, const char *d, const char *e);

static bool e2e_hw_rng(void) {
    uint32_t eax, ebx, ecx, edx; __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (!(ecx & (1u << 30))) return false;
    uint64_t w = 0; unsigned char ok = 0; for (int i = 0; i < 10 && !ok; i++) __asm__ volatile("rdrand %0; setc %1" : "=r"(w), "=qm"(ok));
    return ok != 0;
}
static void e2e_ent(uint8_t *b, size_t n) { vault_random(b, n); }

static const e2e_spk_t *e2e_newest(void) {
    const e2e_spk_t *b = NULL; for (uint32_t i = 0; i < e2e_st.nspk; i++) if (!b || e2e_st.spk[i].id > b->id) b = &e2e_st.spk[i]; return b;
}
static void e2e_save_keys(void) { vwrite(DIR "/e2e.key", &e2e_st, sizeof e2e_st); }
static void e2e_save_mode(void) { vwrite(DIR "/e2e_mode.txt", e2e_req ? "1" : "0", 1); }

/* a new weekly prekey when the newest one is a week old; deletes the ones older than 30 days. Returns true when something changed. */
static bool e2e_maintain(void) {
    uint32_t now = (uint32_t)cos_time_unix(); bool ch = false;
    const e2e_spk_t *nw = e2e_newest();
    if (!nw || now > nw->created + E2E_ROTATE) {
        uint32_t id = nw ? nw->id + 1 : 1;
        if (e2e_st.nspk == E2E_MAXSPK) { int o = 0; for (uint32_t i = 1; i < e2e_st.nspk; i++) if (e2e_st.spk[i].id < e2e_st.spk[o].id) o = (int)i; e2e_st.spk[o] = e2e_st.spk[--e2e_st.nspk]; }
        e2e_new_spk(&e2e_st.spk[e2e_st.nspk++], id, now); ch = true;
    }
    nw = e2e_newest();
    for (uint32_t i = 0; i < e2e_st.nspk;) {
        if (&e2e_st.spk[i] != nw && now > e2e_st.spk[i].created + E2E_KEEP) { e2e_wipe(&e2e_st.spk[i], sizeof e2e_st.spk[i]); e2e_st.spk[i] = e2e_st.spk[--e2e_st.nspk]; nw = e2e_newest(); ch = true; }
        else i++;
    }
    if (ch) e2e_save_keys();
    return ch;
}

static void e2e_init(void) {
    if (e2e_ready) return;
    char v[2] = {0}; cos_stat_t st;
    if (cos_stat(DIR "/e2e_mode.txt", &st) == 0 && vread(DIR "/e2e_mode.txt", v, 1) == 1 && v[0] == '0') e2e_req = false;
    if (!e2e_hw_rng()) { snprintf(e2e_why, sizeof e2e_why, "%s", T("No hardware random number generator: end-to-end encryption is off")); return; }
    e2e_rng_init(e2e_ent);
    memset(&e2e_st, 0, sizeof e2e_st);
    bool have = false;
    if (cos_stat(DIR "/e2e.key", &st) == 0 && st.size > 0) {
        uint8_t *fb = (uint8_t *)malloc((size_t)st.size); if (!fb) return;      /* (an encrypted file is longer than what it holds: read all of it) */
        ssize_t n = vread(DIR "/e2e.key", fb, (size_t)st.size);
        if (n == (ssize_t)sizeof(e2e_store_t) && !memcmp(fb, "C-OSE2E1", 8) && ((e2e_store_t *)fb)->nspk <= E2E_MAXSPK) { memcpy(&e2e_st, fb, sizeof e2e_st); have = true; }
        e2e_wipe(fb, (size_t)st.size); free(fb);
        if (!have) { snprintf(e2e_why, sizeof e2e_why, "%s", T("The encryption key file cannot be read (damaged?) - it is NOT replaced; restore it or delete /messenger/e2e.key to start over")); return; }     /* never overwrite an existing key file */
    }
    if (!have) {
        memcpy(e2e_st.magic, "C-OSE2E1", 8);
        if (!e2e_new_identity(&e2e_st.id)) { snprintf(e2e_why, sizeof e2e_why, "%s", T("Could not make an encryption key")); return; }
        e2e_save_keys(); cos_puts("[MESSENGER] e2e: new identity key made\n");
        if (!vault_enabled()) set_status(T("Encryption keys made. Turn on the App lock (Menu) so they are stored encrypted on this computer"));
    }
    e2e_ready = true; e2e_maintain();
}

static bool e2e_my_bundle(e2e_bundle_t *b) { const e2e_spk_t *s = e2e_newest(); return e2e_ready && s && e2e_make_bundle(&e2e_st.id, s, b); }
static void e2e_my_fp(uint8_t fp[32]) { e2e_fingerprint(e2e_st.id.dh_pub, e2e_st.id.sg_pub, fp); }
static void e2e_publish(void) {
    e2e_bundle_t b; uint8_t pk[E2E_BUNDLE_LEN]; char t[300];
    if (!e2e_ready || srv_state != SV_ONLINE || !e2e_my_bundle(&b)) return;
    e2e_bundle_pack(&b, pk); e2e_b64enc(pk, sizeof pk, t); srv_tx("KEYPUT", t, NULL, NULL); cos_puts("[MESSENGER] e2e: key bundle published\n");
}

/* ---------------------------------------------------------------- people's keys (friends and fellow group members) */
static void hex_of(const uint8_t *b, size_t n, char *o) { for (size_t i = 0; i < n; i++) snprintf(o + i * 2, 3, "%02x", b[i]); }
static bool hex_to(const char *s, uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + i * 2, "%2x", &v) != 1) return false; o[i] = (uint8_t)v; } return true; }
#define PEER_LINE (12 + 3 + 65 + E2E_BUNDLE_LEN * 2 + 3)           /* the longest line of e2e_peers.txt */
static void e2e_save_peers(void) {
    char *buf = (char *)malloc((size_t)nkr * PEER_LINE + 1); if (!buf) return; size_t o = 0;
    for (int i = 0; i < nkr; i++) {
        pkey_t *p = &kr[i]; if (!p->ks) continue;
        char fp[65], bh[E2E_BUNDLE_LEN * 2 + 1] = "-"; hex_of(p->kfp, 32, fp); if (p->has_kb) { uint8_t pk[E2E_BUNDLE_LEN]; e2e_bundle_pack(&p->kb, pk); hex_of(pk, sizeof pk, bh); }
        o += (size_t)snprintf(buf + o, PEER_LINE + 1, "%s %d %s %s\n", p->uid, p->ks, fp, bh);       /* at most PEER_LINE bytes per entry: the buffer cannot overflow */
    }
    vwrite(DIR "/e2e_peers.txt", buf, o); free(buf);
}
static void e2e_load_peers(void) {
    cos_stat_t st; if (cos_stat(DIR "/e2e_peers.txt", &st) != 0 || st.size <= 0 || st.size > (int64_t)KR_MAX * (PEER_LINE + 64)) return;
    char *b = (char *)malloc((size_t)st.size + 1); if (!b) return;
    ssize_t n = vread(DIR "/e2e_peers.txt", b, (size_t)st.size); if (n <= 0) { free(b); return; } b[n] = 0;
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char uid[16], fp[80], bh[E2E_BUNDLE_LEN * 2 + 8]; int ks;
        if (sscanf(l, "%15s %d %79s %400s", uid, &ks, fp, bh) == 4 && strlen(uid) < 12) {
            pkey_t *p = pkr(uid);
            if (strlen(fp) == 64 && hex_to(fp, p->kfp, 32)) {
                p->ks = (uint8_t)(ks == 2 ? 2 : 1);
                uint8_t pk[E2E_BUNDLE_LEN];
                if (strlen(bh) == E2E_BUNDLE_LEN * 2 && hex_to(bh, pk, sizeof pk) && e2e_bundle_unpack(pk, sizeof pk, &p->kb)) p->has_kb = 1;
            }
        }
        if (!e) break; l = e + 1;
    }
    free(b);
}

static void e2e_request_uid(const char *uid) {
    pkey_t *p = pkr(uid); if (p->kwait || srv_state != SV_ONLINE) return;
    if (srv_tx("KEYGET", uid, NULL, NULL)) { p->kwait = 1; p->kreq_ms = cos_time_ms(); }
}
static bool e2e_bundle_same(const e2e_bundle_t *a, const e2e_bundle_t *b) { uint8_t x[E2E_BUNDLE_LEN], y[E2E_BUNDLE_LEN]; e2e_bundle_pack(a, x); e2e_bundle_pack(b, y); return !memcmp(x, y, sizeof x); }       /* (compare the packed form: the struct has padding bytes) */
static void e2e_request_key(contact_t *k) {
    if (k->uid[0]) { e2e_request_uid(k->uid); return; }
    pkey_t *p = KP(k); if (p->kwait) return;                              /* a LAN contact: ask over UDP */
    send_pkt(k->ip, T_KEYREQ, 0, 0, NULL); p->kwait = 1; p->kreq_ms = cos_time_ms();
}
static const char *mylabel(const contact_t *k) { static char b[4][16]; static int n; char *o = b[n++ & 3]; if (k->uid[0]) snprintf(o, 16, "%s", acc_id); else snprintf(o, 16, "ip:%08x", my_ip); return o; }
static int find_ckey(const char *key) { if (!strncmp(key, "ip:", 3)) { unsigned v = 0; sscanf(key + 3, "%8x", &v); return find_ip((uint32_t)v); } return find_uid(key); }
static void e2e_system_note(contact_t *k, const char *text) { msg_t *m = add_msg(k, false, (uint32_t)cos_time_unix(), ST_RECV, 0, text); (void)m; save_chat(k); dirty = true; }

/* things that waited for a key: incoming frames (waiting for a person's key, or for a group's key) and our own messages / edits */
typedef struct { char *f[8]; int n; char wait[12]; uint32_t seq; uint64_t t0; } parked_t;
static parked_t parked[24]; static int nparked; static int e2e_refeeding;
static void e2e_park(const char *wait, char **f, int n) {
    if (nparked >= 24) return;                                           /* not acknowledged: the server sends it again later */
    for (int i = 0; i < nparked; i++) if (!strcmp(parked[i].wait, wait) && !strcmp(parked[i].f[0], f[0]) && parked[i].seq == (uint32_t)strtoul(f[1], NULL, 10) && n >= 2) return;      /* the server sends the same frame again at every login: keep one */
    parked_t *p = &parked[nparked]; memset(p, 0, sizeof *p); snprintf(p->wait, sizeof p->wait, "%s", wait); p->seq = (uint32_t)strtoul(f[1], NULL, 10); p->t0 = cos_time_ms();
    for (int i = 0; i < n && i < 8; i++) { p->f[i] = strdup(f[i]); if (!p->f[i]) { for (int j = 0; j < i; j++) free(p->f[j]); return; } }
    p->n = n > 8 ? 8 : n; nparked++;
}
static void srv_send_msg(contact_t *k, msg_t *m);
static void send_event(contact_t *k, msg_t *m, int kind);
static void e2e_flush_wait(const char *wait) {                          /* feed the frames that waited for `wait` (a person or a group) through the normal handler again */
    parked_t mine[24]; int nm = 0, keep = 0;                              /* in arrival order: a group key must be opened before the messages that need it */
    for (int i = 0; i < nparked; i++) { if (!strcmp(parked[i].wait, wait)) mine[nm++] = parked[i]; else parked[keep++] = parked[i]; }
    nparked = keep;
    e2e_refeeding++;
    for (int i = 0; i < nm; i++) { srv_handle(mine[i].f, mine[i].n); for (int j = 0; j < mine[i].n; j++) free(mine[i].f[j]); }
    e2e_refeeding--;
}
static void e2e_resend_waiting(contact_t *k) {
    for (int j = 0; j < k->n; j++) {
        msg_t *m = &k->m[j];
        if (m->mine && (m->flags & MF_KEYWAIT)) { m->flags &= (uint8_t)~MF_KEYWAIT; if (m->pend) send_event(k, m, m->pend); else srv_send_msg(k, m); }
    }
}
static void e2e_flush(contact_t *k) {
    if (KP(k)->kchanged) return;                                           /* everything keeps waiting until the user decides about the new key */
    e2e_resend_waiting(k);
    e2e_flush_wait(k->uid);
}

static const char *e2e_who(const char *uid) {                           /* a name for warnings: a friend's, else a group member's */
    int i = find_ckey(uid); if (i >= 0) return ct[i].name;
    for (int g = 0; g < nct; g++) if (is_group(&ct[g])) for (int m = 0; m < ct[g].ngm; m++) if (!strcmp(ct[g].gm[m].uid, uid)) return ct[g].gm[m].name;
    return uid;
}
static void e2e_group_flush_for(const char *uid);
static void e2e_on_key(const char *uid, const char *b64) {
    pkey_t *p = pkr(uid);
    p->kwait = 0; p->kchecked = 1; p->kfetch = cos_time_ms();
    if (b64[0]) {
        uint8_t pk[E2E_BUNDLE_LEN]; e2e_bundle_t nb; uint8_t fp[32];
        if (e2e_b64dec(b64, strlen(b64), pk, sizeof pk) == E2E_BUNDLE_LEN && e2e_bundle_unpack(pk, sizeof pk, &nb) && e2e_bundle_verify(&nb)) {
            e2e_fingerprint(nb.ik_dh, nb.ik_sg, fp);
            if (!p->ks) { p->kb = nb; p->has_kb = 1; p->ks = 1; memcpy(p->kfp, fp, 32); e2e_save_peers(); cos_puts("[MESSENGER] e2e: pinned a key\n"); }
            else if (!memcmp(fp, p->kfp, 32)) {
                /* the same identity: take a newer prekey, never an older one (the server could otherwise force the oldest we still hold) */
                if (!p->has_kb || (!e2e_bundle_same(&p->kb, &nb) && nb.spk_id >= p->kb.spk_id)) { p->kb = nb; p->has_kb = 1; e2e_save_peers(); }
                p->kchanged = 0;
            }
            else if (!p->kchanged || !e2e_bundle_same(&p->knb, &nb)) {
                p->knb = nb; p->kchanged = 1; set_status(T("Security key of a contact changed - check the safety number"));
                char note[300]; snprintf(note, sizeof note, "[警告] %s さんの暗号鍵(安全番号)が変わりました。新しい端末に変えた可能性もありますが、なりすましの可能性もあります。安全番号を確認し、信頼するまで、この人との送受信は止まります。", e2e_who(uid));
                int fi = find_ckey(uid); if (fi >= 0 && !is_group(&ct[fi])) e2e_system_note(&ct[fi], note);
                for (int g = 0; g < nct; g++) if (is_group(&ct[g])) for (int m = 0; m < ct[g].ngm; m++) if (!strcmp(ct[g].gm[m].uid, uid)) { e2e_system_note(&ct[g], note); break; }
                cos_puts("[MESSENGER] e2e: friend key CHANGED\n");
            }
        } else { cos_puts("[MESSENGER] e2e: a published key bundle was invalid\n"); }
    }
    int fi = find_ckey(uid); if (fi >= 0 && !is_group(&ct[fi])) e2e_flush(&ct[fi]); else if (!p->kchanged) e2e_flush_wait(uid);
    if (!p->kchanged) e2e_group_flush_for(uid);
    dirty = true;
}

/* verify / trust (by C-OS ID: friends and group members alike) */
static bool e2e_safety_uid(const char *uid, char out[80]) {
    pkey_t *p = pkr(uid); if (!e2e_ready || !p->ks) return false; uint8_t me[32]; e2e_my_fp(me);
    uint8_t fp[32]; if (p->kchanged) e2e_fingerprint(p->knb.ik_dh, p->knb.ik_sg, fp); else memcpy(fp, p->kfp, 32);
    e2e_safety_number(me, fp, out); return true;
}
static void e2e_verify_uid(const char *uid) { pkey_t *p = pkr(uid); if (p->ks && !p->kchanged) { p->ks = 2; e2e_save_peers(); set_status(T("Marked as verified")); } dirty = true; }
static void e2e_trust_uid(const char *uid, const char *fp8) {
    pkey_t *p = pkr(uid); if (!p->kchanged) return;
    if (fp8) { uint8_t nf[32]; char h[9]; e2e_fingerprint(p->knb.ik_dh, p->knb.ik_sg, nf); snprintf(h, sizeof h, "%02x%02x%02x%02x", nf[0], nf[1], nf[2], nf[3]);
               if (strcasecmp(h, fp8)) { set_status(T("The key changed again while you were comparing: look at the safety number once more")); return; } }
    e2e_fingerprint(p->knb.ik_dh, p->knb.ik_sg, p->kfp); p->kb = p->knb; p->has_kb = 1; p->kchanged = 0; p->ks = 1; e2e_save_peers();
    cos_puts("[MESSENGER] e2e: new key trusted\n"); set_status(T("New key trusted"));
    int fi = find_ckey(uid); if (fi >= 0 && !is_group(&ct[fi])) { e2e_system_note(&ct[fi], "[お知らせ] 相手の新しい暗号鍵を信頼しました。"); e2e_flush(&ct[fi]); } else e2e_flush_wait(uid);
    e2e_group_flush_for(uid); dirty = true;
}
static bool e2e_safety(contact_t *k, char out[80]) { return e2e_safety_uid(k->uid, out); }
static void e2e_verify(contact_t *k) { e2e_verify_uid(k->uid); }
static void e2e_trust_new(contact_t *k) { e2e_trust_uid(ckey(k), NULL); }

/* ---------------------------------------------------------------- 1:1 sending */
static bool e2e_text_has_filekey(const char *t) { if (t[0] != 1 || t[1] != 'F' || t[2] != '|') return false; int bars = 0; for (const char *p = t + 3; *p; p++) if (*p == '|') bars++; return bars >= 3; }
static void e2e_fid_of(const char *plain, char *fid, size_t fcap) {
    fid[0] = 0;
    if (!strncmp(plain, "\x01" "F|", 3)) { const char *p = plain + 3, *q = strchr(p, '|'); if (q && (size_t)(q - p) < fcap) { memcpy(fid, p, (size_t)(q - p)); fid[q - p] = 0; } }
}
/* An edit or a delete travels as an encrypted EDIT whose text is "E<time>|<new text>" or "D<time>|": the server cannot forge a delete, and
 * an old edit cannot be replayed over a newer one (the receiver only accepts a newer time than the last one it took). */
static void e2e_frame_event(char *out, size_t cap, int kind, const char *text) { static uint32_t last_t; uint32_t t = (uint32_t)cos_time_unix(); if (t <= last_t) t = last_t + 1; last_t = t;      /* strictly increasing: two changes in one second both count */
    snprintf(out, cap, kind == EV_EDIT ? "E%u|%s" : "D%u|", (unsigned)t, kind == EV_EDIT ? text : ""); (void)text; }
static bool e2e_parse_event(const char *p, int *kind, uint32_t *ts, const char **text) {
    if ((p[0] != 'E' && p[0] != 'D') || p[1] < '0' || p[1] > '9') return false;
    char *e; unsigned long v = strtoul(p + 1, &e, 10); if (*e != '|') return false;
    *kind = p[0] == 'E' ? EV_EDIT : EV_DEL; *ts = (uint32_t)v; *text = e + 1; return true;
}
static void e2e_apply_event(contact_t *k, msg_t *m, bool decrypted, const char *plain, const char *legacy_text, bool legacy_del) {
    if (e2e_bad) return;                                                  /* it could not be opened: never overwrite a real message with an error text */
    if (decrypted) { int kind; uint32_t ts; const char *tx; if (!e2e_parse_event(plain, &kind, &ts, &tx) || ts <= m->evts) return; m->evts = ts; apply_event(k, m, kind, kind == EV_EDIT ? tx : ""); }
    else apply_event(k, m, legacy_del ? EV_DEL : EV_EDIT, legacy_text);   /* (plain text: only reachable when encryption is optional and the peer has no key) */
}
#define E2E_MAXPT 2200
static int e2e_wire(contact_t *k, uint32_t id, int kind, const char *plain, char *wire, size_t wcap, char *fid, size_t fcap) {
    fid[0] = 0;
    if (!e2e_ready) { if (!e2e_why[0]) snprintf(e2e_why, sizeof e2e_why, "%s", T("Encryption is not available")); return e2e_req ? E2E_FAIL : E2E_PLAIN; }
    uint64_t age = cos_time_ms() - KP(k)->kfetch;
    if (!KP(k)->kchecked || age > E2E_REFRESH_MS || (!KP(k)->has_kb && age > 15000)) {      /* a friend without a key may have updated meanwhile */ if (!KP(k)->kwait) e2e_request_key(k); if (KP(k)->kwait) return E2E_WAIT; }
    if (KP(k)->kchanged) { snprintf(e2e_why, sizeof e2e_why, T("%s's security key changed - check Actions > Security first"), k->name); return E2E_FAIL; }
    if (!KP(k)->has_kb) {
        if (e2e_req) { snprintf(e2e_why, sizeof e2e_why, T("%s has no encryption key yet (update C-OS Message there, or turn off Require encryption)"), k->name); return E2E_FAIL; }
        return E2E_PLAIN;
    }
    size_t pl = strlen(plain); if (pl > E2E_MAXPT) { snprintf(e2e_why, sizeof e2e_why, "%s", T("Message too long for encryption")); return E2E_FAIL; }
    char ctx[96]; snprintf(ctx, sizeof ctx, "%s|%s|%u|%d", mylabel(k), ckey(k), id, kind);
    static uint8_t env[E2E_MAXPT + 160]; size_t el = e2e_encrypt(&e2e_st.id, &KP(k)->kb, ctx, (const uint8_t *)plain, pl, env, sizeof env);
    if (!el || wcap < el * 4 / 3 + 8) { snprintf(e2e_why, sizeof e2e_why, "%s", T("Could not encrypt (bad key?)")); return E2E_FAIL; }
    wire[0] = 1; wire[1] = 'E'; e2e_b64enc(env, el, wire + 2);
    e2e_fid_of(plain, fid, fcap);
    return E2E_OK;
}

/* ---------------------------------------------------------------- groups: keys, distribution, rekeying */
typedef struct { char gid[12], by[12]; uint32_t epoch, created; uint8_t key[32], mine; } gkey_t;       /* one epoch's group key; mine: we made it; created: the maker's clock (it came wrapped, so it is authentic) */
typedef struct { char gid[12]; uint32_t epoch; char uid[12]; } gsent_t;                       /* the key of this epoch was handed to this member (the server said OK) */
typedef struct { char gid[12]; uint32_t epoch; } gtomb_t;                                    /* an epoch we have seen once: it can never be added again (no rollback to an old key) */
typedef struct { char gid[12], uid[12]; uint32_t epoch; uint64_t t; } ginf_t;                /* handed to the server, no answer yet */
#define E2E_MAXGK 96
#define E2E_MAXSENT 1024
#define E2E_MAXTOMB 1024
static gkey_t gks[E2E_MAXGK]; static int ngks; static gsent_t gsent[E2E_MAXSENT]; static int ngsent; static gtomb_t gtomb[E2E_MAXTOMB]; static int ntomb; static ginf_t ginf[32]; static int nginf;
typedef struct { char magic[8]; uint32_t n1, n2, n3; gkey_t a[E2E_MAXGK]; gsent_t b[E2E_MAXSENT]; gtomb_t c[E2E_MAXTOMB]; } gblob_t;
static void gk_save(void) {
    gblob_t *bl = (gblob_t *)malloc(sizeof *bl); if (!bl) return;
    memset(bl, 0, sizeof *bl); memcpy(bl->magic, "C-OSGK03", 8); bl->n1 = (uint32_t)ngks; bl->n2 = (uint32_t)ngsent; bl->n3 = (uint32_t)ntomb;
    memcpy(bl->a, gks, sizeof(gkey_t) * (size_t)ngks); memcpy(bl->b, gsent, sizeof(gsent_t) * (size_t)ngsent); memcpy(bl->c, gtomb, sizeof(gtomb_t) * (size_t)ntomb);
    vwrite(DIR "/e2e_groups.bin", bl, sizeof *bl); e2e_wipe(bl, sizeof *bl); free(bl);
}
static void gk_load(void) {
    cos_stat_t st; if (cos_stat(DIR "/e2e_groups.bin", &st) != 0 || st.size <= 0) return;
    uint8_t *fb = (uint8_t *)malloc((size_t)st.size); if (!fb) return;
    ssize_t n = vread(DIR "/e2e_groups.bin", fb, (size_t)st.size); gblob_t *bl = (gblob_t *)fb;      /* (an encrypted file is longer than what it holds: read all of it) */
    if (n == (ssize_t)sizeof *bl && !memcmp(bl->magic, "C-OSGK03", 8) && bl->n1 <= E2E_MAXGK && bl->n2 <= E2E_MAXSENT && bl->n3 <= E2E_MAXTOMB) {
        ngks = (int)bl->n1; ngsent = (int)bl->n2; ntomb = (int)bl->n3;
        memcpy(gks, bl->a, sizeof(gkey_t) * (size_t)ngks); memcpy(gsent, bl->b, sizeof(gsent_t) * (size_t)ngsent); memcpy(gtomb, bl->c, sizeof(gtomb_t) * (size_t)ntomb);
    }
    e2e_wipe(fb, (size_t)st.size); free(fb);
}
static gkey_t *gk_find(const char *gid, uint32_t epoch) { for (int i = 0; i < ngks; i++) if (gks[i].epoch == epoch && !strcmp(gks[i].gid, gid)) return &gks[i]; return NULL; }
static gkey_t *gk_newest(const char *gid) { gkey_t *b = NULL; for (int i = 0; i < ngks; i++) if (!strcmp(gks[i].gid, gid) && (!b || gks[i].created > b->created || (gks[i].created == b->created && gks[i].epoch > b->epoch))) b = &gks[i]; return b; }
static bool gk_tomb_has(const char *gid, uint32_t epoch) { for (int i = 0; i < ntomb; i++) if (gtomb[i].epoch == epoch && !strcmp(gtomb[i].gid, gid)) return true; return false; }
static void gk_tomb_add(const char *gid, uint32_t epoch) {
    if (gk_tomb_has(gid, epoch)) return;
    if (ntomb >= E2E_MAXTOMB) { memmove(&gtomb[0], &gtomb[1], sizeof(gtomb_t) * (size_t)(E2E_MAXTOMB - 1)); ntomb--; }
    snprintf(gtomb[ntomb].gid, sizeof gtomb[0].gid, "%s", gid); gtomb[ntomb++].epoch = epoch;
}
static bool gk_sent(const char *gid, uint32_t epoch, const char *uid) {
    for (int i = 0; i < ngsent; i++) if (gsent[i].epoch == epoch && !strcmp(gsent[i].gid, gid) && !strcmp(gsent[i].uid, uid)) return true;
    uint64_t now = cos_time_ms(); for (int i = 0; i < nginf; i++) if (ginf[i].epoch == epoch && !strcmp(ginf[i].gid, gid) && !strcmp(ginf[i].uid, uid) && now - ginf[i].t < 20000) return true;   /* sent a moment ago: wait for the answer */
    return false;
}
static void e2e_gkey_ok(const char *gid, const char *uid, uint32_t epoch) {                    /* OK_GKEY: only now is the key recorded as handed out */
    for (int i = 0; i < nginf;) if (ginf[i].epoch == epoch && !strcmp(ginf[i].gid, gid) && !strcmp(ginf[i].uid, uid)) ginf[i] = ginf[--nginf]; else i++;
    if (gk_sent(gid, epoch, uid) || strlen(gid) >= 12 || strlen(uid) >= 12) return;
    if (ngsent >= E2E_MAXSENT) { memmove(&gsent[0], &gsent[1], sizeof(gsent_t) * (size_t)(E2E_MAXSENT - 1)); ngsent--; }
    gsent_t *s = &gsent[ngsent++]; snprintf(s->gid, sizeof s->gid, "%s", gid); s->epoch = epoch; snprintf(s->uid, sizeof s->uid, "%s", uid); gk_save();
}
static void gk_drop_index(int i) { gks[i] = gks[--ngks]; }
static gkey_t *gk_add(const char *gid, uint32_t epoch, const uint8_t key[32], bool mine, const char *by, uint32_t created) {
    /* one member cannot push the others' keys out: at most 3 epochs per sender and group (the oldest of that sender goes first) */
    { int sc = 0, so = -1; for (int i = 0; i < ngks; i++) if (!strcmp(gks[i].gid, gid) && !strcmp(gks[i].by, by)) { sc++; if (so < 0 || gks[i].created < gks[so].created) so = i; } if (sc >= 3 && so >= 0) gk_drop_index(so); }
    int cnt = 0, oldest = -1;                                              /* at most 8 epochs per group; the oldest goes first */
    for (int i = 0; i < ngks; i++) if (!strcmp(gks[i].gid, gid)) { cnt++; if (oldest < 0 || gks[i].created < gks[oldest].created) oldest = i; }
    if (cnt >= 8 && oldest >= 0) gk_drop_index(oldest);
    if (ngks >= E2E_MAXGK) { int o = 0; for (int i = 1; i < ngks; i++) if (gks[i].created < gks[o].created) o = i; gk_drop_index(o); }
    gkey_t *g = &gks[ngks++]; memset(g, 0, sizeof *g); snprintf(g->gid, sizeof g->gid, "%s", gid); g->epoch = epoch; g->created = created; memcpy(g->key, key, 32); g->mine = mine; snprintf(g->by, sizeof g->by, "%s", by);
    gk_tomb_add(gid, epoch); gk_save(); return g;
}
static gkey_t *gk_create(const char *gid) {
    uint8_t k[32]; uint32_t ep; do { e2e_rand(&ep, 4); } while (!ep || gk_find(gid, ep) || gk_tomb_has(gid, ep));
    e2e_rand(k, 32); gkey_t *g = gk_add(gid, ep, k, true, acc_id, (uint32_t)cos_time_unix()); e2e_wipe(k, 32);
    cos_puts("[MESSENGER] e2e: made a group key\n"); return g;
}
static void gk_forget(const char *gid) {
    for (int i = 0; i < ngks;) if (!strcmp(gks[i].gid, gid)) gk_drop_index(i); else i++;
    for (int i = 0; i < ngsent;) if (!strcmp(gsent[i].gid, gid)) gsent[i] = gsent[--ngsent]; else i++;
    for (int i = 0; i < ntomb;) if (!strcmp(gtomb[i].gid, gid)) gtomb[i] = gtomb[--ntomb]; else i++;
    for (int i = 0; i < nginf;) if (!strcmp(ginf[i].gid, gid)) ginf[i] = ginf[--nginf]; else i++;
    gk_save();
}
static int my_group_role(contact_t *g);
static bool gk_is_member(contact_t *g, const char *uid) { for (int i = 0; i < g->ngm; i++) if (!strcmp(g->gm[i].uid, uid)) return true; return false; }
static int gk_role_of(contact_t *g, const char *uid) { for (int i = 0; i < g->ngm; i++) if (!strcmp(g->gm[i].uid, uid)) return g->gm[i].role; return -1; }

/* ---------------------------------------------------------------- the signed roster
 * The server decides who is "in" a group, so on its word alone a stranger could be added and would be handed the group key. Who may get the key
 * is therefore decided by the group's admins: a ROSTER (member IDs and roles) is posted by an admin as an ordinary encrypted, signed group message
 * ("\x01R|<time>|id:role,id:role,...") and only a roster signed by somebody the previous roster trusts is taken. Until the first roster arrives
 * only the owner named when the group was first seen counts as an admin. A member the server lists but the roster does not is "unapproved":
 * an admin has to approve them (/gapprove) before they get the key, and they are never believed to be an admin. */
#define ROST_MAX 128
#define ROST_GROUPS 32
typedef struct { char gid[12]; uint32_t t; int n; bool post; struct { char uid[12]; uint8_t role; } m[ROST_MAX]; } rost_t;
typedef struct { char magic[8]; uint32_t n; rost_t r[ROST_GROUPS]; } rblob_t;
static rost_t rosts[ROST_GROUPS]; static int nrosts;
static rost_t *rost_find(const char *gid) { for (int i = 0; i < nrosts; i++) if (!strcmp(rosts[i].gid, gid)) return &rosts[i]; return NULL; }
static void rost_save(void) {
    rblob_t *bl = (rblob_t *)calloc(1, sizeof *bl); if (!bl) return;
    memcpy(bl->magic, "C-OSRS01", 8); bl->n = (uint32_t)nrosts; memcpy(bl->r, rosts, sizeof(rost_t) * (size_t)nrosts);
    vwrite(DIR "/e2e_roster.bin", bl, sizeof *bl); free(bl);
}
static void rost_load(void) {
    cos_stat_t st; if (cos_stat(DIR "/e2e_roster.bin", &st) != 0 || st.size <= 0) return;
    uint8_t *fb = (uint8_t *)malloc((size_t)st.size); if (!fb) return;
    ssize_t n = vread(DIR "/e2e_roster.bin", fb, (size_t)st.size); rblob_t *bl = (rblob_t *)fb;
    if (n == (ssize_t)sizeof *bl && !memcmp(bl->magic, "C-OSRS01", 8) && bl->n <= ROST_GROUPS) { nrosts = (int)bl->n; memcpy(rosts, bl->r, sizeof(rost_t) * (size_t)nrosts); }
    free(fb);
}
/* The first time a group is seen, the owner the server names is pinned (trust on first use, like a friend's key): the server cannot change it later. */
static void rost_pin_owner(const char *gid, const char *owner) {
    if (rost_find(gid) || nrosts >= ROST_GROUPS || strlen(gid) >= 12 || strlen(owner) >= 12 || !owner[0]) return;
    rost_t *r = &rosts[nrosts++]; memset(r, 0, sizeof *r); snprintf(r->gid, sizeof r->gid, "%s", gid); r->n = 1; snprintf(r->m[0].uid, sizeof r->m[0].uid, "%s", owner); r->m[0].role = 2; rost_save();
}
static int rost_role(contact_t *g, const char *uid) {               /* the trusted role: -1 = not approved */
    rost_t *r = rost_find(g->uid); if (!r) return -1;
    for (int i = 0; i < r->n; i++) if (!strcmp(r->m[i].uid, uid)) return r->m[i].role;
    return -1;
}
static bool rost_has(contact_t *g, const char *uid) { return rost_role(g, uid) >= 0; }
static int rost_pending(contact_t *g) { int c = 0; for (int i = 0; i < g->ngm; i++) if (!rost_has(g, g->gm[i].uid)) c++; return c; }
static void rost_forget(const char *gid) { for (int i = 0; i < nrosts; i++) if (!strcmp(rosts[i].gid, gid)) { rosts[i] = rosts[--nrosts]; break; } rost_save(); }
static void rost_touch(rost_t *r) { uint32_t now = (uint32_t)cos_time_unix(); r->t = now > r->t ? now : r->t + 1; r->post = true; rost_save(); }
/* local changes by an admin (never from a server frame) */
static bool rost_set(contact_t *g, const char *uid, int role) {       /* role < 0: remove */
    rost_t *r = rost_find(g->uid); if (!r || rost_role(g, acc_id) < 1 || strlen(uid) >= 12) return false;
    for (int i = 0; i < r->n; i++) if (!strcmp(r->m[i].uid, uid)) {
        if (role < 0) { r->m[i] = r->m[--r->n]; } else r->m[i].role = (uint8_t)role;
        rost_touch(r); return true;
    }
    if (role < 0 || r->n >= ROST_MAX) return false;
    snprintf(r->m[r->n].uid, sizeof r->m[0].uid, "%s", uid); r->m[r->n++].role = (uint8_t)role; rost_touch(r); return true;
}
static int rost_approve_all(contact_t *g) { int c = 0; for (int i = 0; i < g->ngm; i++) if (!rost_has(g, g->gm[i].uid) && rost_set(g, g->gm[i].uid, 0)) c++; return c; }
/* a roster that arrived as a signed group message (text after the "\x01R|"). Returns true when it was taken. */
static bool rost_apply(contact_t *g, const char *from, const char *p) {
    rost_t *r = rost_find(g->uid); if (!r) return false;
    if (rost_role(g, from) < 1) { cos_puts("[MESSENGER] e2e: a roster from somebody who is not an admin was ignored\n"); return false; }
    char *e; unsigned long t = strtoul(p, &e, 10); if (*e != '|') return false;
    if (t <= r->t || t > (unsigned long)cos_time_unix() + 86400) return false;                      /* only a newer roster; not one from the future */
    rost_t nr; memset(&nr, 0, sizeof nr); snprintf(nr.gid, sizeof nr.gid, "%s", g->uid); nr.t = (uint32_t)t;
    for (const char *q = e + 1; *q && nr.n < ROST_MAX;) {
        const char *c = strchr(q, ':'); if (!c || c - q <= 0 || c - q >= 12 || (c[1] < '0' || c[1] > '2')) return false;
        memcpy(nr.m[nr.n].uid, q, (size_t)(c - q)); nr.m[nr.n].role = (uint8_t)(c[1] - '0'); nr.n++;
        q = c + 2; if (*q == ',') q++; else if (*q) return false;
    }
    if (nr.n == 0) return false;
    for (int i = 0; i < r->n; i++) if (r->m[i].role == 2 && strcmp(from, r->m[i].uid)) {            /* only the owner can hand the group on or demote itself */
        bool kept = false; for (int j = 0; j < nr.n; j++) if (!strcmp(nr.m[j].uid, r->m[i].uid) && nr.m[j].role == 2) kept = true;
        if (!kept) { cos_puts("[MESSENGER] e2e: a roster that removes the owner was ignored\n"); return false; }
    }
    *r = nr; r->post = false; rost_save(); cos_puts("[MESSENGER] e2e: roster accepted\n"); return true;
}
static bool rost_post(contact_t *g, rost_t *r);
/* Somebody left or was removed: an admin makes a new epoch and hands it to everybody who is still in (the removed person never gets it). */
static void gk_rekey_check(contact_t *g) {
    if (!e2e_ready || g->ngm == 0 || rost_role(g, acc_id) < 1) return;
    bool gone = false; for (int i = 0; i < ngsent; i++) if (!strcmp(gsent[i].gid, g->uid) && !gk_is_member(g, gsent[i].uid)) { gone = true; break; }
    if (!gone) return;
    for (int i = 0; i < ngsent;) if (!strcmp(gsent[i].gid, g->uid) && !gk_is_member(g, gsent[i].uid)) gsent[i] = gsent[--ngsent]; else i++;
    gk_create(g->uid); cos_puts("[MESSENGER] e2e: rekeyed a group after a member left\n");
}
/* hands keys to the members that lack them: one pass, a few at a time (called about once a second) */
static void gk_distribute(void) {
    if (!e2e_ready || srv_state != SV_ONLINE) return;
    int budget = 3;                                                        /* frames to the server per pass, key lookups included (the server drops floods) */
    for (int gi = 0; gi < nct && budget > 0; gi++) {
        contact_t *g = &ct[gi]; if (!is_group(g) || g->ngm == 0) continue;
        int role = rost_role(g, acc_id); gkey_t *nw = gk_newest(g->uid);
        for (int ki = 0; ki < ngks && budget > 0; ki++) {
            gkey_t *k = &gks[ki]; if (strcmp(k->gid, g->uid)) continue;
            if (!k->mine && !(role >= 1 && k == nw)) continue;                      /* keys are handed out by their maker, and by admins for the current epoch */
            for (int m = 0; m < g->ngm && budget > 0; m++) {
                const char *uid = g->gm[m].uid; if (!strcmp(uid, acc_id) || !rost_has(g, uid) || gk_sent(g->uid, k->epoch, uid)) continue;      /* only people an admin approved get a key */
                pkey_t *p = pkr(uid);
                if (!p->kchecked || cos_time_ms() - p->kfetch > E2E_REFRESH_MS) { if (!p->kwait) { e2e_request_uid(uid); budget--; } continue; }
                if (!p->has_kb || p->kchanged) continue;
                char ctx[120], ep[16]; snprintf(ctx, sizeof ctx, "%s|%s|%s:%u|K", acc_id, uid, g->uid, k->epoch); snprintf(ep, sizeof ep, "%u", k->epoch);
                uint8_t body[36], env[256]; memcpy(body, k->key, 32); body[32] = (uint8_t)(k->created >> 24); body[33] = (uint8_t)(k->created >> 16); body[34] = (uint8_t)(k->created >> 8); body[35] = (uint8_t)k->created;
                size_t el = e2e_encrypt(&e2e_st.id, &p->kb, ctx, body, sizeof body, env, sizeof env); e2e_wipe(body, sizeof body); if (!el) continue;
                char wire[400]; wire[0] = 1; wire[1] = 'E'; e2e_b64enc(env, el, wire + 2);
                if (!srv_tx5("GKEY", g->uid, uid, ep, wire)) return;
                if (nginf >= 32) { memmove(&ginf[0], &ginf[1], sizeof(ginf_t) * 31); nginf--; }
                ginf_t *f = &ginf[nginf++]; snprintf(f->gid, sizeof f->gid, "%s", g->uid); snprintf(f->uid, sizeof f->uid, "%s", uid); f->epoch = k->epoch; f->t = cos_time_ms();
                budget--;
            }
        }
    }
}
static void e2e_group_flush_for(const char *uid) { (void)uid; }          /* group frames wait under the sender's ID and are released by e2e_flush_wait() */

/* a GKEY_IN frame: seq gid from ts epoch wrapped. Returns true when it was handled (acknowledge it), false when it waits for a key. */
static bool e2e_gkey_in(char **f, int n) {
    int gi = find_uid(f[2]); const char *from = f[3];
    if (gi < 0) { e2e_park(f[2], f, n); return false; }                    /* the group is not known yet: wait for its GROUP frame */
    contact_t *g = &ct[gi];
    if (g->ngm > 0 && !gk_is_member(g, from)) return true;                 /* from somebody who is not in the group: ignore */
    uint32_t epoch = (uint32_t)strtoul(f[5], NULL, 10); if (!epoch || gk_find(g->uid, epoch) || gk_tomb_has(g->uid, epoch)) return true;     /* known, or one that was dropped: never again */
    if (rost_role(g, from) < 1) { cos_puts("[MESSENGER] e2e: a group key from somebody who is not an admin was refused\n"); return true; }      /* only the group's admins (by the signed roster) hand out keys */
    pkey_t *p = pkr(from);
    if (!p->kchecked && !e2e_refeeding) { e2e_request_uid(from); if (p->kwait) { e2e_park(from, f, n); return false; } }
    if (p->kchanged) { if (!e2e_refeeding) { e2e_park(from, f, n); return false; } return true; }
    if (!e2e_ready || !p->has_kb || f[6][0] != 1 || f[6][1] != 'E') return true;
    static uint8_t env[600], pt[600]; long el = e2e_b64dec(f[6] + 2, strlen(f[6] + 2), env, sizeof env); size_t pl = 0;
    char ctx[120]; snprintf(ctx, sizeof ctx, "%s|%s|%s:%u|K", from, acc_id, g->uid, epoch);
    int r = el > 0 ? e2e_decrypt(&e2e_st.id, e2e_st.spk, (int)e2e_st.nspk, p->kb.ik_dh, ctx, env, (size_t)el, pt, sizeof pt - 1, &pl) : -1;
    if (r == -3 && !e2e_refeeding && cos_time_ms() - p->kfetch > 20000) { p->kchecked = 0; e2e_request_uid(from); if (p->kwait) { e2e_park(from, f, n); return false; } }
    if (r != 0 || pl != 36) { e2e_wipe(pt, sizeof pt); cos_puts("[MESSENGER] e2e: a group key could not be opened\n"); return true; }
    uint32_t created = (uint32_t)pt[32] << 24 | (uint32_t)pt[33] << 16 | (uint32_t)pt[34] << 8 | pt[35], now = (uint32_t)cos_time_unix();
    if (created > now) created = now;                                      /* nobody can make a key "newer than everything" by lying about the time */
    gk_add(g->uid, epoch, pt, false, from, created); e2e_wipe(pt, sizeof pt); cos_puts("[MESSENGER] e2e: received a group key\n");
    e2e_flush_wait(g->uid);                                                 /* the group messages that waited for it */
    e2e_resend_waiting(g);                                                  /* ... and our own that waited to be sent */
    return true;
}

/* sending to a group: the newest key (made on the spot when the group has none) */
static int e2e_gwire(contact_t *g, uint32_t id, int kind, const char *plain, char *wire, size_t wcap, char *fid, size_t fcap) {
    fid[0] = 0;
    if (!e2e_ready) { if (!e2e_why[0]) snprintf(e2e_why, sizeof e2e_why, "%s", T("Encryption is not available")); return e2e_req ? E2E_FAIL : E2E_PLAIN; }
    if (g->ngm == 0) return E2E_WAIT;                                        /* the member list has not arrived yet */
    size_t pl = strlen(plain); if (pl > E2E_MAXPT) { snprintf(e2e_why, sizeof e2e_why, "%s", T("Message too long for encryption")); return E2E_FAIL; }
    gkey_t *k = gk_newest(g->uid); if (!k) { if (rost_role(g, acc_id) < 1) return E2E_WAIT; k = gk_create(g->uid); }      /* (members wait for an admin's key) */
    char ctx[96]; snprintf(ctx, sizeof ctx, "%s|%s|%u|%d", acc_id, g->uid, id, kind);
    static uint8_t env[E2E_MAXPT + 220]; size_t el = e2e_group_encrypt(&e2e_st.id, k->key, k->epoch, ctx, (const uint8_t *)plain, pl, env, sizeof env);
    if (!el || wcap < el * 4 / 3 + 8) { snprintf(e2e_why, sizeof e2e_why, "%s", T("Could not encrypt")); return E2E_FAIL; }
    wire[0] = 1; wire[1] = 'G'; e2e_b64enc(env, el, wire + 2);
    e2e_fid_of(plain, fid, fcap);
    return E2E_OK;
}


static bool rost_post(contact_t *g, rost_t *r) {                      /* true: done (or nothing can be done), false: try again later */
    if (srv_state != SV_ONLINE || !e2e_ready || g->ngm == 0) return false;
    static char pl[ROST_MAX * 16 + 40], wire[3400]; char fid[24]; size_t o = (size_t)snprintf(pl, sizeof pl, "\x01R|%u|", (unsigned)r->t);
    for (int i = 0; i < r->n; i++) o += (size_t)snprintf(pl + o, sizeof pl - o, "%s%s:%d", i ? "," : "", r->m[i].uid, r->m[i].role);
    uint32_t id = ++next_id; char ids[16]; snprintf(ids, sizeof ids, "%u", id);
    int rr = e2e_gwire(g, id, 0, pl, wire, sizeof wire, fid, sizeof fid);
    if (rr == E2E_WAIT) return false;
    if (rr != E2E_OK) return true;                                    /* a roster is never sent unencrypted */
    srv_tx("GSEND", ids, g->uid, wire); cos_puts("[MESSENGER] e2e: roster posted\n"); return true;
}
static void rost_tick(void) {
    if (!e2e_ready || srv_state != SV_ONLINE) return;
    for (int gi = 0; gi < nct; gi++) {
        contact_t *g = &ct[gi]; if (!is_group(g) || g->ngm == 0) continue;
        rost_t *r = rost_find(g->uid); if (!r) continue;
        if (r->t == 0 && r->n == 1 && !strcmp(r->m[0].uid, acc_id)) {   /* our own group (or one made before rosters existed): the people who already hold the key are approved */
            for (int i = 0; i < ngsent; i++) if (!strcmp(gsent[i].gid, g->uid) && gk_is_member(g, gsent[i].uid) && !rost_has(g, gsent[i].uid) && r->n < ROST_MAX) { snprintf(r->m[r->n].uid, sizeof r->m[0].uid, "%s", gsent[i].uid); r->m[r->n++].role = (uint8_t)(gk_role_of(g, gsent[i].uid) > 0 ? 1 : 0); }
            r->t = (uint32_t)cos_time_unix(); r->post = r->n > 1; rost_save();
        }
        if (r->post && rost_role(g, acc_id) >= 1) { if (rost_post(g, r)) { r->post = false; rost_save(); } return; }      /* one frame per pass */
    }
}

/* ---------------------------------------------------------------- receiving */
/* f[ti] is the text of an MSG (ti 4) or EDIT_IN (ti 5) frame from friend k. Returns E2E_GOT with *out = a new string (or NULL: use f[ti] as it is), or E2E_PARKED. */
static int e2e_in(contact_t *k, char **f, int n, int ti, uint32_t rid, int kind, char **out) {
    *out = NULL; e2e_bad = false; const char *t = f[ti];
    bool enc = t[0] == 1 && t[1] == 'E';
    if (!KP(k)->kchecked && !e2e_refeeding) { e2e_request_key(k); if (KP(k)->kwait) { e2e_park(k->uid, f, n); return E2E_PARKED; } }
    if (KP(k)->kchanged) { if (!e2e_refeeding) { e2e_park(k->uid, f, n); return E2E_PARKED; } *out = e2e_ph("[相手の鍵が変わったため、このメッセージは読めません]"); return E2E_GOT; }
    if (!enc) {
        if (e2e_req || KP(k)->has_kb) { *out = e2e_ph("[暗号化されていないメッセージを受け取りましたが、設定により破棄しました]"); return E2E_GOT; }
        return E2E_GOT;
    }
    if (!e2e_ready || !KP(k)->has_kb) { *out = e2e_ph("[暗号化されたメッセージ: 復号用の鍵がありません]"); return E2E_GOT; }
    static uint8_t env[3300], pt[3300]; long el = e2e_b64dec(t + 2, strlen(t + 2), env, sizeof env); size_t pl = 0;
    char ctx[96]; snprintf(ctx, sizeof ctx, "%s|%s|%u|%d", ckey(k), mylabel(k), rid, kind);
    int r = el > 0 ? e2e_decrypt(&e2e_st.id, e2e_st.spk, (int)e2e_st.nspk, KP(k)->kb.ik_dh, ctx, env, (size_t)el, pt, sizeof pt - 1, &pl) : -1;
    if (r == -3 && !e2e_refeeding && cos_time_ms() - KP(k)->kfetch > 20000) {          /* maybe the friend's key changed: look again once */
        KP(k)->kchecked = 0; e2e_request_key(k); if (KP(k)->kwait) { e2e_park(k->uid, f, n); return E2E_PARKED; }
    }
    if (r == 0) { pt[pl] = 0; *out = strdup((char *)pt); e2e_wipe(pt, sizeof pt); return E2E_GOT; }
    e2e_wipe(pt, sizeof pt);
    *out = strdup(r == -2 ? "[復号できません: 暗号鍵が古すぎます]" : r == -3 ? "[復号できません: 改ざんされたか、鍵が一致しません]" : "[復号できません: 壊れたメッセージ]");
    cos_puts("[MESSENGER] e2e: could not decrypt a message\n");
    return E2E_GOT;
}
/* group text: GMSG seq gid from ts text msgid (ti 5, rid index 6) / GEDIT_IN seq gid from ts msgid text (ti 6, rid index 5). */
static int e2e_gin(contact_t *g, char **f, int n, int ti, uint32_t rid, int kind, char **out) {
    *out = NULL; e2e_bad = false; const char *t = f[ti], *from = f[3]; bool enc = t[0] == 1 && t[1] == 'G';
    if (!enc) {
        if (e2e_req || gk_newest(g->uid)) { *out = e2e_ph("[暗号化されていないメッセージを受け取りましたが、設定により破棄しました]"); return E2E_GOT; }       /* (a group that has a key does not take plain text) */
        return E2E_GOT;
    }
    pkey_t *p = pkr(from);
    if (!p->kchecked && !e2e_refeeding) { e2e_request_uid(from); if (p->kwait) { e2e_park(from, f, n); return E2E_PARKED; } }
    if (p->kchanged) { if (!e2e_refeeding) { e2e_park(from, f, n); return E2E_PARKED; } *out = e2e_ph("[送信者の鍵が変わったため、このメッセージは読めません]"); return E2E_GOT; }
    if (!e2e_ready || !p->has_kb) { *out = e2e_ph("[暗号化されたメッセージ: 送信者の鍵がありません]"); return E2E_GOT; }
    static uint8_t env[3300], pt[3300]; long el = e2e_b64dec(t + 2, strlen(t + 2), env, sizeof env); size_t pl = 0;
    if (el < E2E_GFIXED + E2E_PAD) { *out = e2e_ph("[復号できません: 壊れたメッセージ]"); return E2E_GOT; }
    uint32_t epoch = e2e_group_epoch(env, (size_t)el); gkey_t *k = gk_find(g->uid, epoch);
    char ctx[96]; snprintf(ctx, sizeof ctx, "%s|%s|%u|%d", from, g->uid, rid, kind);
    if (!k) {
        if (e2e_group_verify(p->kb.ik_sg, ctx, env, (size_t)el) != 0) { *out = e2e_ph("[復号できません: 署名が合わないメッセージ]"); return E2E_GOT; }       /* not signed by the sender: it cannot even wait */
        e2e_park(g->uid, f, n); return E2E_PARKED;                         /* the group key has not arrived yet (it is handed out by the others); the server keeps the message too */
    }
    int r = e2e_group_decrypt(p->kb.ik_sg, k->key, ctx, env, (size_t)el, pt, sizeof pt - 1, &pl);
    if (r == 0) { pt[pl] = 0; *out = strdup((char *)pt); e2e_wipe(pt, sizeof pt); return E2E_GOT; }
    e2e_wipe(pt, sizeof pt);
    *out = strdup(r == -3 ? "[復号できません: 署名が合わないか、改ざんされています]" : "[復号できません: 壊れたメッセージ]");
    cos_puts("[MESSENGER] e2e: could not decrypt a group message\n");
    return E2E_GOT;
}

static uint64_t e2e_dist_t;
static void e2e_tick(void) {
    uint64_t now = cos_time_ms();
    for (int i = 0; i < nkr; i++) if (kr[i].kwait && now - kr[i].kreq_ms > 8000) e2e_on_key(kr[i].uid, "");      /* a server without key support */
    for (int i = 0; i < nparked;) if (now - parked[i].t0 > 300000) { for (int j = 0; j < parked[i].n; j++) free(parked[i].f[j]); parked[i] = parked[--nparked]; } else i++;   /* the server sends them again at the next login */
    if (now - e2e_dist_t > 1000) { e2e_dist_t = now; rost_tick(); gk_distribute(); }
}

/* the app lock closes: nothing secret stays in memory (identity and prekeys, group keys, pinned keys, waiting frames, the random generator) */
static void e2e_wipe_memory(void) {
    e2e_wipe(&e2e_st, sizeof e2e_st); e2e_ready = false; e2e_wipe(&e2e_drbg, sizeof e2e_drbg); e2e_seeded = false;
    e2e_wipe(gks, sizeof gks); ngks = 0; e2e_wipe(gsent, sizeof gsent); ngsent = 0; e2e_wipe(gtomb, sizeof gtomb); ntomb = 0; e2e_wipe(ginf, sizeof ginf); nginf = 0; e2e_wipe(kr, sizeof kr); nkr = 0;
    for (int i = 0; i < nparked; i++) for (int j = 0; j < parked[i].n; j++) { if (parked[i].f[j]) { memset(parked[i].f[j], 0, strlen(parked[i].f[j])); free(parked[i].f[j]); } }
    e2e_wipe(rosts, sizeof rosts); nrosts = 0;
    nparked = 0; e2e_published_spk_valid = 0;
}

/* ---------------------------------------------------------------- LAN contacts (UDP): the same encryption, keyed by IP address */
static void lan_key_request_reply(uint32_t ip) {                        /* somebody asks for our public bundle */
    e2e_bundle_t b; uint8_t pk[E2E_BUNDLE_LEN]; char t[300];
    if (!e2e_ready || !e2e_my_bundle(&b)) return;
    e2e_bundle_pack(&b, pk); e2e_b64enc(pk, sizeof pk, t); send_pkt(ip, T_KEY, 0, 0, t);
}
static void lan_key_arrived(uint32_t ip, const char *b64) {            /* only an answer to our own question counts: an unasked "key" cannot change a pinned one */
    int i = find_ip(ip); if (i < 0 || !pkr(ckey(&ct[i]))->kwait) return;
    e2e_on_key(ckey(&ct[i]), b64);
}
static void transmit(uint32_t ip, msg_t *m) {
    m->sent_ms = cos_time_ms(); m->tries++;
    int i = find_ip(ip); if (i < 0) return; contact_t *k = &ct[i];
    static char wire[3400]; char fid[24]; int r = e2e_wire(k, m->id, 0, m->text, wire, sizeof wire, fid, sizeof fid);
    if (r == E2E_WAIT) return;                                          /* the retry in net_poll sends it once the key is here */
    if (r == E2E_FAIL) { m->state = ST_FAILED; set_status(e2e_why); save_chat(k); return; }
    send_pkt(ip, T_MSG, m->id, m->ts, r == E2E_OK ? wire : m->text);
}
static bool lan_plain_del_refused(contact_t *k) { return e2e_req || KP(k)->has_kb; }          /* a plain delete from somebody who has a key is spoofed */
static void lan_event(contact_t *k, msg_t *m) {                         /* send (or resend) our pending edit or delete */
    static char wire[3400], evt[1600]; char fid[24]; e2e_frame_event(evt, sizeof evt, m->pend, m->text);
    int r = e2e_wire(k, m->id, 1, evt, wire, sizeof wire, fid, sizeof fid); e2e_wipe(evt, sizeof evt);
    if (r == E2E_WAIT) return;
    if (r == E2E_FAIL) { m->pend = 0; set_status(e2e_why); return; }
    if (r == E2E_OK) send_pkt(k->ip, T_EDIT, m->id, 0, wire);
    else if (m->pend == EV_EDIT) send_pkt(k->ip, T_EDIT, m->id, 0, m->text); else send_pkt(k->ip, T_DEL, m->id, 0, NULL);
}
/* incoming text of a LAN packet. 1: ready (*out is a new string, or NULL to use `text` as it is), 0: wait - do not acknowledge, the sender repeats it */
static int lan_in(contact_t *k, const char *text, uint32_t rid, int kind, char **out) {
    *out = NULL; e2e_bad = false; bool enc = text[0] == 1 && text[1] == 'E'; pkey_t *p = KP(k);
    if (!p->kchecked) { e2e_request_key(k); return 0; }
    if (p->kchanged) return 0;                                           /* held until the user decides about the new key */
    if (!enc) { if (e2e_req || p->has_kb) { set_status(T("Refused a message that was not encrypted (the sender sees it fail)")); return 0; } return 1; }     /* not acknowledged: nothing is shown, the sender is told it failed */
    if (!e2e_ready || !p->has_kb) { *out = e2e_ph("[暗号化されたメッセージ: 復号用の鍵がありません]"); return 1; }
    static uint8_t env[1500], pt[1500]; long el = e2e_b64dec(text + 2, strlen(text + 2), env, sizeof env); size_t pl = 0;
    char ctx[96]; snprintf(ctx, sizeof ctx, "%s|%s|%u|%d", ckey(k), mylabel(k), rid, kind);
    int r = el > 0 ? e2e_decrypt(&e2e_st.id, e2e_st.spk, (int)e2e_st.nspk, p->kb.ik_dh, ctx, env, (size_t)el, pt, sizeof pt - 1, &pl) : -1;
    if (r == -3 && cos_time_ms() - p->kfetch > 20000) { p->kchecked = 0; e2e_request_key(k); return 0; }         /* maybe the key changed: look again once */
    if (r == 0) { pt[pl] = 0; *out = strdup((char *)pt); e2e_wipe(pt, sizeof pt); return 1; }
    e2e_wipe(pt, sizeof pt);
    *out = strdup(r == -2 ? "[復号できません: 暗号鍵が古すぎます]" : r == -3 ? "[復号できません: 改ざんされたか、鍵が一致しません]" : "[復号できません: 壊れたメッセージ]");
    cos_puts("[MESSENGER] e2e: could not decrypt a LAN message\n"); return 1;
}
