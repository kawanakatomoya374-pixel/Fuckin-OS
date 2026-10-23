/* cosupdate - app updates for C-OS (docs/app_update.md).
 *
 *   cosupdate status                      what updates installed, and from which manifest serial
 *   cosupdate url [URL]                   show / set the update source (a directory URL that holds manifest.cosup)
 *   cosupdate check [URL]                 fetch and verify the manifest, list what is newer than what is installed
 *   cosupdate install [URL] [NAME ...]    install the newer items (or only the named ones)
 *   cosupdate rollback PATH               put back what an update replaced
 *   cosupdate poll                        a quiet check for the background: writes /system/update/available.txt and shows a toast the first time an update appears
 *   cosupdate auto [on|off|MINUTES]       the background check (the taskbar runs `poll` every N minutes)
 *
 * A manifest may carry one "os version=LABEL notes=TEXT" line: it is an OS release (the taskbar then offers it with the C-OS icon and installs it with one click).
 *
 * The manifest is signed (ECDSA P-256 over SHA-256, the public key is compiled into this program). Nothing in it is used before the signature checks.
 * Every file is then downloaded, and its size and SHA-256 must be the ones the manifest names. A replaced file is kept in /system/update/backup first.
 * Where files may go is limited (see path_ok): programs in /bin, language packs, the SDK, /apps - never kernel modules. A manifest whose serial is lower
 * than one already accepted is refused (an old manifest cannot be replayed), and an item is installed only when its version is higher than the installed one.
 * /system/update/state.txt records the installed files; at boot the system leaves those files alone instead of restoring the copy inside the kernel image. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "bearssl.h"
#include "update_pubkey.h"

#define UPD_DIR "/system/update"
#define STATE_PATH UPD_DIR "/state.txt"
#define IMAGE_PATH UPD_DIR "/image.txt"
#define URL_PATH UPD_DIR "/url.txt"
#define BACKUP_DIR UPD_DIR "/backup"
#define AVAIL_PATH UPD_DIR "/available.txt"      /* what the taskbar and Settings show: serial, os label, notes, items, bytes */
#define PROGRESS_PATH UPD_DIR "/progress.txt"    /* an install in progress / its result (the taskbar shows it) */
#define NOTIFIED_PATH UPD_DIR "/notified.txt"    /* the serial a toast was shown for */
#define LAST_PATH UPD_DIR "/lastcheck.txt"       /* when the last check ran and how it went */
#define AUTO_PATH UPD_DIR "/auto.txt"            /* "on|off MINUTES" */
#define MAX_MANIFEST (64u * 1024u)
#define MAX_FILE (9u * 1024u * 1024u + 512u * 1024u)
#define MAX_ITEMS 48
#define MAX_STATE 160

typedef struct { char name[48], path[128], file[96]; long version; char sha[65]; uint32_t size; int cur; } item_t;     /* cur: -1 not looked at, 1 the file on the device already is this one, 0 it is not */
typedef struct { char path[128]; long version; char sha[65]; long prev; } st_t;     /* prev: version before the update, -1: the file did not exist (it was added) */

static char os_label[48], os_notes[120];          /* from the manifest: an OS release */
static char st_os[48];                              /* the OS release the last install brought */
static char image_line[96];                 /* "image <build stamp>" of the running system */
static st_t st[MAX_STATE]; static int nst; static long st_serial;
/* the kernel line of a manifest: `kernel serial=N sha256=H size=BYTES parts=K file=BASE label=TEXT` (BASE.pNN.bin are the parts, each below MAX_FILE) */
#define MAX_KERNEL (64u * 1024u * 1024u)
static struct { bool present; long serial; char sha[65]; uint32_t size; int parts; char file[64], label[32]; } kern;

/* ---------------------------------------------------------------- small helpers */
static void hex(const unsigned char *b, size_t n, char *out) { static const char *d = "0123456789abcdef"; for (size_t i = 0; i < n; ++i) { out[i * 2] = d[b[i] >> 4]; out[i * 2 + 1] = d[b[i] & 15]; } out[n * 2] = 0; }
static void sha256_hex(const void *p, size_t n, char out[65]) { br_sha256_context c; unsigned char h[32]; br_sha256_init(&c); br_sha256_update(&c, p, n); br_sha256_out(&c, h); hex(h, 32, out); }
static bool read_all(const char *path, char **out, size_t *len, size_t max) {
    cos_stat_t s; if (cos_stat(path, &s) != 0 || s.is_dir || s.size > max) return false;
    char *b = malloc((size_t)s.size + 1); if (!b) return false;
    ssize_t n = s.size ? cos_read_file(path, b, (size_t)s.size) : 0;
    if (n < 0 || (uint64_t)n != s.size) { free(b); return false; }
    b[n] = 0; *out = b; *len = (size_t)n; return true;
}
/* the kernel refuses zero-length writes, and replaces a file only when it is written fresh */
static bool write_new(const char *path, const void *data, size_t n) {
    cos_unlink(path);
    if (n == 0) { int fd = cos_open(path, COS_O_WRONLY_CREATE); if (fd < 0) return false; cos_close(fd); return true; }
    return cos_write_file(path, data, n) == (ssize_t)n;
}
static bool exists(const char *path) { cos_stat_t s; return cos_stat(path, &s) == 0 && !s.is_dir; }

/* ---------------------------------------------------------------- where an update may put files */
static bool ends_with(const char *s, const char *e) { size_t a = strlen(s), b = strlen(e); return a >= b && strcmp(s + a - b, e) == 0; }
static bool path_ok(const char *p) {
    size_t n = strlen(p);
    if (n < 6 || n >= 120 || p[0] != '/') return false;
    for (size_t i = 0; i < n; ++i) { unsigned char c = (unsigned char)p[i]; if (c <= ' ' || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false; }
    if (strstr(p, "..") || strstr(p, "//") || p[n - 1] == '/') return false;
    if (strncmp(p, "/bin/", 5) == 0) return ends_with(p, ".c-os") && !strchr(p + 5, '/');          /* system programs */
    if (strncmp(p, "/system/lang/", 13) == 0) return ends_with(p, ".c-osll") && !strchr(p + 13, '/');   /* language packs */
    if (strncmp(p, "/system/sdk/", 12) == 0) return true;                                           /* headers, libraries, examples */
    if (strncmp(p, "/apps/", 6) == 0) return true;
    return false;       /* never: /system/drivers (kernel modules have their own signing), /system/update, /etc, the user's folders */
}
static bool name_ok(const char *s) { if (!*s || strlen(s) >= 48) return false; for (; *s; ++s) { char c = *s; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false; } return true; }

/* ---------------------------------------------------------------- state */
static void state_load(void) {
    nst = 0; st_serial = 0; st_os[0] = 0;
    char *img; size_t il;
    image_line[0] = 0;
    if (read_all(IMAGE_PATH, &img, &il, 200)) { char *nl = strchr(img, '\n'); if (nl) *nl = 0; snprintf(image_line, sizeof image_line, "%s", img); free(img); }
    char *b; size_t n;
    if (!read_all(STATE_PATH, &b, &n, 64 * 1024)) return;
    bool same = false;
    for (char *line = b; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (strncmp(line, "image ", 6) == 0) same = strcmp(line, image_line) == 0;
        else if (same && strncmp(line, "serial ", 7) == 0) st_serial = atol(line + 7);
        else if (same && strncmp(line, "os ", 3) == 0) snprintf(st_os, sizeof st_os, "%s", line + 3);
        else if (same && strncmp(line, "file ", 5) == 0 && nst < MAX_STATE) {
            st_t *s = &st[nst]; char ver[24], prev[24];
            char sha[80];
            if (sscanf(line + 5, "%127s %23s %79s %23s", s->path, ver, sha, prev) == 4 && strlen(sha) == 64) { s->version = atol(ver); strcpy(s->sha, sha); s->prev = atol(prev); nst++; }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(b);
}
static bool state_save(void) {
    char *b = malloc(64 * 1024); if (!b) return false;
    size_t o = (size_t)snprintf(b, 64 * 1024, "%s\nserial %ld\n", image_line, st_serial);
    if (st_os[0]) o += (size_t)snprintf(b + o, 64 * 1024 - o, "os %s\n", st_os);
    for (int i = 0; i < nst && o + 400 < 64 * 1024; ++i) o += (size_t)snprintf(b + o, 64 * 1024 - o, "file %s %ld %s %ld\n", st[i].path, st[i].version, st[i].sha, st[i].prev);
    bool ok = write_new(STATE_PATH, b, o);
    free(b); return ok;
}
static st_t *state_find(const char *path) { for (int i = 0; i < nst; ++i) if (!strcmp(st[i].path, path)) return &st[i]; return NULL; }

/* ---------------------------------------------------------------- network */
static uint8_t *http_fetch(const char *url, size_t *len, size_t max, char *why, size_t whycap) {
    int job = cos_net_http_start(url);
    if (job < 0) { snprintf(why, whycap, "no free network job"); return NULL; }
    cos_http_result_t r; uint64_t t0 = cos_time_ms();
    for (;;) {
        int p = cos_net_http_poll(job, &r);
        if (p == 1) break;
        if (p < 0) { snprintf(why, whycap, "the transfer disappeared"); return NULL; }
        if (cos_time_ms() - t0 > 180000) { cos_net_http_cancel(job); snprintf(why, whycap, "timeout"); return NULL; }
        cos_sleep_ms(50);
    }
    if (r.error || r.status != 200) { cos_net_http_cancel(job); if (r.status) snprintf(why, whycap, "HTTP %d", (int)r.status); else snprintf(why, whycap, "no response (network, DNS or TLS)"); return NULL; }
    if (r.body_len > max) { cos_net_http_cancel(job); snprintf(why, whycap, "too large (%llu bytes)", (unsigned long long)r.body_len); return NULL; }
    uint8_t *b = malloc((size_t)r.body_len + 1);
    if (!b) { cos_net_http_cancel(job); snprintf(why, whycap, "out of memory"); return NULL; }
    ssize_t n = cos_net_http_take(job, b, (size_t)r.body_len);
    if (n < 0 || (uint64_t)n != r.body_len) { free(b); snprintf(why, whycap, "short read"); return NULL; }
    b[n] = 0; *len = (size_t)n; return b;
}

/* ---------------------------------------------------------------- manifest */
static char base_url[300];
static char last_err[200];
static item_t items[MAX_ITEMS]; static int nitems; static long man_serial;

static bool hexval(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) { char c = s[i * 2 + k]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return false; v = v * 16 + d; }
        out[i] = (unsigned char)v;
    }
    return true;
}
/* verifies the signature, then parses; false (with a reason) for anything wrong */
static bool manifest_parse(char *m, size_t len, char *why, size_t whycap) {
    const char *marker = "\n--SIG--\n"; char *sg = NULL;
    for (size_t i = 0; i + 9 <= len; ++i) if (memcmp(m + i, marker, 9) == 0) { sg = m + i; break; }
    if (!sg) { snprintf(why, whycap, "no signature"); return false; }
    size_t body_len = (size_t)(sg - m) + 1;                       /* the body includes its last newline */
    const char *hx = sg + 9; unsigned char sig[64];
    if (strlen(hx) < 128 || !hexval(hx, sig, 64)) { snprintf(why, whycap, "the signature is malformed"); return false; }
    unsigned char dg[32]; br_sha256_context c; br_sha256_init(&c); br_sha256_update(&c, m, body_len); br_sha256_out(&c, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)cos_update_pubkey, sizeof cos_update_pubkey };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, sig, 64) != 1) { snprintf(why, whycap, "the signature does not match (not made with this system's update key)"); return false; }
    m[body_len] = 0;                                              /* from here the body is trusted */
    nitems = 0; man_serial = -1; bool header = false; os_label[0] = 0; os_notes[0] = 0; memset(&kern, 0, sizeof kern);
    for (char *line = m; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!strcmp(line, "COSUP 1")) header = true;
        else if (!strncmp(line, "serial ", 7)) man_serial = atol(line + 7);
        else if (!strncmp(line, "os ", 3)) {
            for (char *tok = strtok(line + 3, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "version")) snprintf(os_label, sizeof os_label, "%s", v);
                else if (!strcmp(tok, "notes")) { snprintf(os_notes, sizeof os_notes, "%s", v); for (char *q = os_notes; *q; ++q) if (*q == '_') *q = ' '; }
            }
            for (char *q = os_label; *q; ++q) if (*q <= ' ' || *q == 0x7f) *q = '?';
        }
        else if (!strncmp(line, "kernel ", 7)) {
            for (char *tok = strtok(line + 7, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "serial")) kern.serial = atol(v);
                else if (!strcmp(tok, "sha256")) snprintf(kern.sha, sizeof kern.sha, "%s", v);
                else if (!strcmp(tok, "size")) kern.size = (uint32_t)strtoul(v, NULL, 10);
                else if (!strcmp(tok, "parts")) kern.parts = atoi(v);
                else if (!strcmp(tok, "file")) snprintf(kern.file, sizeof kern.file, "%s", v);
                else if (!strcmp(tok, "label")) snprintf(kern.label, sizeof kern.label, "%s", v);
            }
            if (kern.serial <= 0 || strlen(kern.sha) != 64 || kern.size < 1024 || kern.size > MAX_KERNEL || kern.parts < 1 || kern.parts > 16 || !name_ok(kern.file)
                || (uint64_t)kern.parts * MAX_FILE < kern.size) { snprintf(why, whycap, "the kernel entry is malformed"); return false; }
            kern.present = true;
        }
        else if (!strncmp(line, "item ", 5)) {
            if (nitems >= MAX_ITEMS) { snprintf(why, whycap, "too many items"); return false; }
            item_t *it = &items[nitems]; memset(it, 0, sizeof *it); it->version = -1; it->cur = -1;
            for (char *tok = strtok(line + 5, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "name")) snprintf(it->name, sizeof it->name, "%s", v);
                else if (!strcmp(tok, "version")) it->version = atol(v);
                else if (!strcmp(tok, "path")) snprintf(it->path, sizeof it->path, "%s", v);
                else if (!strcmp(tok, "sha256")) snprintf(it->sha, sizeof it->sha, "%s", v);
                else if (!strcmp(tok, "size")) it->size = (uint32_t)strtoul(v, NULL, 10);
                else if (!strcmp(tok, "file")) snprintf(it->file, sizeof it->file, "%s", v);
            }
            if (!name_ok(it->name) || it->version < 0 || strlen(it->sha) != 64 || it->size == 0 || it->size > MAX_FILE || !it->file[0] || strchr(it->file, '/') || strstr(it->file, "..")) { snprintf(why, whycap, "item %d is malformed", nitems + 1); return false; }
            if (!path_ok(it->path)) { snprintf(why, whycap, "item %s: %s is not a place updates may write to", it->name, it->path); return false; }
            nitems++;
        }
        line = nl ? nl + 1 : NULL;
        if (nl && !line[0]) break;
    }
    if (!header || man_serial < 0) { snprintf(why, whycap, "not an update manifest"); return false; }
    return true;
}
static bool fetch_manifest(const char *arg_url) {
    char url[300] = "";
    if (arg_url && *arg_url) snprintf(url, sizeof url, "%s", arg_url);
    else { char *b; size_t n; if (read_all(URL_PATH, &b, &n, 280)) { char *nl = strchr(b, '\n'); if (nl) *nl = 0; snprintf(url, sizeof url, "%s", b); free(b); } }
    if (!url[0]) { snprintf(last_err, sizeof last_err, "no update source set"); printf("No update source: cosupdate url http://HOST/PATH/\n"); return false; }
    size_t l = strlen(url); if (url[l - 1] != '/') { if (l + 1 < sizeof url) { url[l] = '/'; url[l + 1] = 0; } }
    snprintf(base_url, sizeof base_url, "%s", url);
    char full[340]; snprintf(full, sizeof full, "%smanifest.cosup", base_url);
    char why[160]; size_t n = 0;
    uint8_t *m = http_fetch(full, &n, MAX_MANIFEST, why, sizeof why);
    if (!m) { snprintf(last_err, sizeof last_err, "cannot reach the update server (%s)", why); printf("[UPDATE] cannot fetch %s: %s\n", full, why); return false; }
    bool ok = manifest_parse((char *)m, n, why, sizeof why);
    if (!ok) { snprintf(last_err, sizeof last_err, "manifest refused: %s", why); printf("[UPDATE] manifest refused: %s\n", why); free(m); return false; }
    free(m);
    if (man_serial < st_serial) { snprintf(last_err, sizeof last_err, "an older manifest than the one accepted"); printf("[UPDATE] manifest refused: serial %ld is older than the %ld already accepted (an old manifest is no update)\n", man_serial, st_serial); return false; }
    printf("[UPDATE] manifest ok: serial %ld, %d item(s)\n", man_serial, nitems);
    return true;
}

/* ---------------------------------------------------------------- install / rollback */
static void flat_backup(const char *path, char *out, size_t cap) { size_t o = 0; snprintf(out, cap, "%s/", BACKUP_DIR); o = strlen(out); for (const char *p = path + 1; *p && o + 1 < cap; ++p) out[o++] = *p == '/' ? '_' : *p; out[o] = 0; }
/* A file that already is byte for byte what the manifest names needs no download (the first release equals the image it was built from). */
static bool item_current(item_t *it) {
    if (it->cur >= 0) return it->cur == 1;
    it->cur = 0;
    cos_stat_t s2; if (cos_stat(it->path, &s2) != 0 || s2.is_dir || s2.size != it->size) return false;
    char *b; size_t n; if (!read_all(it->path, &b, &n, MAX_FILE + 1024 * 1024)) return false;
    char sha[65]; sha256_hex(b, n, sha); free(b);
    it->cur = strcmp(sha, it->sha) == 0;
    return it->cur == 1;
}
static bool needs(item_t *it) {
    if (item_current(it)) return false; st_t *s = state_find(it->path); if (s) return it->version > s->version; return !exists(it->path) || it->version > 0; }
static int do_install(const item_t *it) {
    st_t *old = state_find(it->path);
    char why[160]; size_t n = 0; char url[420]; snprintf(url, sizeof url, "%s%s", base_url, it->file);
    printf("[UPDATE] %s %ld -> %s ...\n", it->name, it->version, it->path);
    uint8_t *data = http_fetch(url, &n, MAX_FILE, why, sizeof why);
    if (!data) { printf("[UPDATE] %s: download failed: %s\n", it->name, why); return 1; }
    char sha[65]; sha256_hex(data, n, sha);
    if (n != it->size || strcmp(sha, it->sha) != 0) { printf("[UPDATE] %s: refused: size or SHA-256 differs from the manifest (the download is not what was signed)\n", it->name); free(data); return 1; }
    /* keep what is replaced */
    bool had = exists(it->path); char bk[200]; flat_backup(it->path, bk, sizeof bk);
    if (had) {
        char *oldb; size_t ol;
        if (!read_all(it->path, &oldb, &ol, MAX_FILE + 1024 * 1024) || !write_new(bk, oldb, ol)) { printf("[UPDATE] %s: cannot keep a backup of %s, not replacing it\n", it->name, it->path); free(data); return 1; }
        free(oldb);
    } else cos_unlink(bk);
    /* write beside it, check, then swap in */
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s.upd", it->path);
    bool ok = write_new(tmp, data, n);
    if (ok) { char *chk; size_t cl; ok = read_all(tmp, &chk, &cl, MAX_FILE + 1024) && cl == n && memcmp(chk, data, n) == 0; if (chk) free(chk); }
    free(data);
    if (!ok) { cos_unlink(tmp); printf("[UPDATE] %s: writing failed, %s is untouched\n", it->name, it->path); return 1; }
    cos_unlink(it->path);
    if (cos_rename(tmp, it->path) != 0) {                         /* put the backup back if the swap failed */
        if (had) { char *oldb; size_t ol; if (read_all(bk, &oldb, &ol, MAX_FILE + 1024 * 1024)) { write_new(it->path, oldb, ol); free(oldb); } }
        cos_unlink(tmp); printf("[UPDATE] %s: could not put the new file in place (the old one was restored)\n", it->name); return 1;
    }
    st_t *s = old; if (!s) { if (nst >= MAX_STATE) { printf("[UPDATE] too many updated files\n"); return 1; } s = &st[nst++]; memset(s, 0, sizeof *s); snprintf(s->path, sizeof s->path, "%s", it->path); s->prev = had ? 0 : -1; }
    else s->prev = old->version;
    s->version = it->version; strcpy(s->sha, sha);
    if (!old && had) s->prev = 0;                                   /* what the image shipped counts as version 0 */
    printf("[UPDATE] installed %s version %ld (%s)\n", it->name, it->version, it->path);
    return 0;
}
static void progress(const char *state, int step, int total, const char *name, int pct, const char *msg);
/* ---- the kernel itself (A/B slots on the boot disk; the kernel verifies the signature of the blob again before it writes anything) */
static bool kernel_needs(void) {
    if (!kern.present) return false;
    cos_kupd_status_t ks; if (cos_kernel_update_status(&ks) != 0 || !ks.supported) return false;
    uint64_t have = ks.running_serial; if (ks.trial_slot && ks.trial_serial > have) have = ks.trial_serial; if (ks.bad_serial > have) have = ks.bad_serial;
    return (uint64_t)kern.serial > have;
}
static int kernel_install(void) {
    cos_kupd_status_t ks; if (cos_kernel_update_status(&ks) != 0 || !ks.supported) { printf("[UPDATE] kernel: this boot medium cannot take a kernel update (%s)\n", cos_kernel_update_status(&ks) == 0 ? ks.why : "?"); return 1; }
    uint8_t *all = malloc(kern.size); if (!all) { printf("[UPDATE] kernel: out of memory for %u bytes\n", (unsigned)kern.size); return 1; }
    size_t got = 0;
    for (int i = 0; i < kern.parts; ++i) {
        char url[420], why[160]; snprintf(url, sizeof url, "%s%s.p%02d.bin", base_url, kern.file, i);
        size_t n = 0; printf("[UPDATE] kernel: part %d/%d ...\n", i + 1, kern.parts);
        char msg[60]; snprintf(msg, sizeof msg, "kernel %d/%d", i + 1, kern.parts); progress("running", i + 1, kern.parts, "Kernel", i * 100 / kern.parts, msg);
        uint8_t *d = http_fetch(url, &n, MAX_FILE, why, sizeof why);
        if (!d) { printf("[UPDATE] kernel: download failed: %s\n", why); free(all); return 1; }
        if (got + n > kern.size) { printf("[UPDATE] kernel: refused: more data than the manifest names\n"); free(d); free(all); return 1; }
        memcpy(all + got, d, n); got += n; free(d);
    }
    char sha[65]; sha256_hex(all, got, sha);
    if (got != kern.size || strcmp(sha, kern.sha) != 0) { printf("[UPDATE] kernel: refused: size or SHA-256 differs from the manifest (the download is not what was signed)\n"); free(all); return 1; }
    int rc = cos_kernel_update_install(all, got); free(all);
    if (rc != 0) { char w[96] = ""; cos_kernel_update_why(w); printf("[UPDATE] kernel: refused by the kernel: %s\n", w); return 1; }
    printf("[UPDATE] kernel: installed serial %ld (%s) in the other slot; it starts at the next restart and confirms itself\n", kern.serial, kern.label[0] ? kern.label : "-");
    return 0;
}
static int cmd_status(void) {
    state_load();
    printf("[UPDATE] image: %s, accepted manifest serial: %ld, updated files: %d%s%s\n", image_line, st_serial, nst, st_os[0] ? ", OS release: " : "", st_os);
    { cos_kupd_status_t ks; if (cos_kernel_update_status(&ks) == 0 && ks.supported)
        printf("[UPDATE] kernel: serial %llu in slot %c (active %c)%s trial %c serial %llu%s; last failed serial %llu\n", (unsigned long long)ks.running_serial, ks.running_slot, ks.active_slot, "", ks.trial_slot ? ks.trial_slot : '-', (unsigned long long)ks.trial_serial, ks.trial_slot ? " (waiting for a restart)" : "", (unsigned long long)ks.bad_serial);
      else printf("[UPDATE] kernel: not updatable here (%s)\n", ks.why); }
    for (int i = 0; i < nst; ++i) printf("%s version %ld sha256 %.12s... (before: %s)\n", st[i].path, st[i].version, st[i].sha, st[i].prev < 0 ? "not there" : "version 0");
    return 0;
}
static int cmd_check(const char *url) {
    state_load();
    if (!fetch_manifest(url)) return 1;
    int avail = 0;
    for (int i = 0; i < nitems; ++i) {
        st_t *s = state_find(items[i].path);
        bool n = needs(&items[i]);
        printf("%s %s: version %ld (installed %ld) %s\n", n ? "UPDATE" : "ok", items[i].name, items[i].version, s ? s->version : 0L, items[i].path);
        avail += n;
    }
    if (kern.present) { bool n = kernel_needs(); printf("%s Kernel: serial %ld %s\n", n ? "UPDATE" : "ok", kern.serial, kern.label); avail += n; }
    printf("[UPDATE] %d update(s) available\n", avail);
    return 0;
}
static void progress(const char *state, int step, int total, const char *name, int pct, const char *msg) {
    char b[400];
    snprintf(b, sizeof b, "state %s\nstep %d %d\nname %s\npercent %d\nos %s\nmessage %s\n", state, step, total, name ? name : "", pct, os_label, msg ? msg : "");
    write_new(PROGRESS_PATH, b, strlen(b));
}
static void now_text(char *out, size_t cap) { cos_datetime_t t; if (cos_rtc_now(&t) == 0) snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", (int)t.year, (int)t.month, (int)t.day, (int)t.hour, (int)t.minute); else snprintf(out, cap, "unknown"); }
static int cmd_install(const char *url, char **names, int nn) {
    state_load();
    progress("running", 0, 0, "", 0, "checking for updates");
    if (!fetch_manifest(url)) { progress("failed", 0, 0, "", 0, last_err); cos_notify("C-OS", "アップデートに失敗しました", 2); return 1; }
    int want[MAX_ITEMS], nw = 0;
    for (int i = 0; i < nitems; ++i) {
        if (nn) { bool w = false; for (int k = 0; k < nn; ++k) if (!strcmp(names[k], items[i].name)) w = true; if (!w) continue; }
        if (needs(&items[i])) want[nw++] = i;
    }
    bool dokernel = kern.present && kernel_needs() && (!nn || ({ bool w = false; for (int k = 0; k < nn; ++k) if (!strcmp(names[k], "Kernel")) w = true; w; }));
    int total = nw + (dokernel ? 1 : 0);
    int done = 0, failed = 0;
    for (int k = 0; k < nw; ++k) {
        progress("running", k + 1, nw, items[want[k]].name, k * 100 / nw, "installing");
        if (do_install(&items[want[k]]) == 0) done++; else failed++;
        if (!state_save()) printf("[UPDATE] cannot write %s\n", STATE_PATH);          /* after every file: a stop in the middle keeps what is done */
    }
    bool kernel_done = false;
    if (dokernel) { if (kernel_install() == 0) { done++; kernel_done = true; } else failed++; }
    nw = total;
    if (man_serial > st_serial) st_serial = man_serial;
    if (!failed && os_label[0] && nw > 0) snprintf(st_os, sizeof st_os, "%s", os_label);
    if (!state_save()) { printf("[UPDATE] cannot write %s\n", STATE_PATH); progress("failed", nw, nw, "", 100, "cannot write the update state"); return 1; }
    printf("[UPDATE] done: %d installed, %d failed\n", done, failed);
    if (failed) { progress("failed", nw, nw, "", 100, "some files could not be installed"); cos_notify("C-OS", "アップデートに失敗したファイルがあります", 2); return 1; }
    progress("done", nw, nw, "", 100, os_label[0] ? "the update is installed" : "installed");
    cos_unlink(AVAIL_PATH);
    if (kernel_done) { progress("done", nw, nw, "", 100, "restart to start the new kernel"); cos_notify("C-OS", "カーネルを更新しました。再起動すると新しいカーネルで起動します", 3); }
    else if (os_label[0] && nw > 0) cos_notify("C-OS", "アップデートが完了しました。再起動すると完全に反映されます", 3);
    return 0;
}
/* the background check: quiet, and the only thing that shows anything is the toast the first time an update appears */
static int cmd_poll(const char *url) {
    state_load();
    char when[40]; now_text(when, sizeof when);
    last_err[0] = 0;
    if (!fetch_manifest(url)) {
        char b[300]; snprintf(b, sizeof b, "time %s\nresult error %s\n", when, last_err); write_new(LAST_PATH, b, strlen(b));
        return 1;
    }
    int avail = 0; unsigned long bytes = 0;
    for (int i = 0; i < nitems; ++i) if (needs(&items[i])) { avail++; bytes += items[i].size; }
    bool kn = kernel_needs(); if (kn) { avail++; bytes += kern.size; }
    char b[500];
    snprintf(b, sizeof b, "time %s\nresult ok %d\n", when, avail); write_new(LAST_PATH, b, strlen(b));
    if (avail == 0) { cos_unlink(AVAIL_PATH); return 0; }
    snprintf(b, sizeof b, "serial %ld\n%s%s%snotes %s\nitems %d\nbytes %lu\n%s", man_serial, os_label[0] ? "os " : "", os_label, os_label[0] ? "\n" : "", os_notes, avail, bytes, kn ? "kernel 1\n" : "");
    write_new(AVAIL_PATH, b, strlen(b));
    char *nb; size_t nl; long notified = -1;
    if (read_all(NOTIFIED_PATH, &nb, &nl, 64)) { notified = atol(nb); free(nb); }
    if (notified != man_serial) {
        char ns[40]; snprintf(ns, sizeof ns, "%ld", man_serial); write_new(NOTIFIED_PATH, ns, strlen(ns));
        if (os_label[0]) cos_notify("C-OS", "C-OSのアップデートが配信されました", 0);
        else cos_notify("C-OS", "アプリの更新があります", 0);
        printf("[UPDATE] announced: serial %ld%s%s\n", man_serial, os_label[0] ? " os " : "", os_label);
    }
    return 0;
}
static int cmd_auto(const char *arg) {
    char *b; size_t n; bool on = true; long minutes = 30;
    if (read_all(AUTO_PATH, &b, &n, 64)) { if (!strncmp(b, "off", 3)) on = false; char *sp = strchr(b, ' '); if (sp) minutes = atol(sp + 1); free(b); }
    if (arg) {
        if (!strcmp(arg, "on")) on = true; else if (!strcmp(arg, "off")) on = false;
        else if (atol(arg) > 0) minutes = atol(arg);
        else { printf("usage: cosupdate auto [on|off|MINUTES]\n"); return 1; }
        char o[40]; snprintf(o, sizeof o, "%s %ld", on ? "on" : "off", minutes);
        if (!write_new(AUTO_PATH, o, strlen(o))) return 1;
    }
    printf("background check: %s, every %ld minute(s)\n", on ? "on" : "off", minutes);
    return 0;
}
static int cmd_rollback(const char *path) {
    state_load();
    st_t *s = state_find(path);
    if (!s) { printf("[UPDATE] %s was not changed by an update\n", path); return 1; }
    char bk[200]; flat_backup(path, bk, sizeof bk);
    if (s->prev < 0) { cos_unlink(path); printf("[UPDATE] removed %s (it was added by an update)\n", path); }
    else {
        char *b; size_t n;
        if (!read_all(bk, &b, &n, MAX_FILE + 1024 * 1024)) { printf("[UPDATE] no backup of %s\n", path); return 1; }
        bool ok = write_new(path, b, n); free(b);
        if (!ok) { printf("[UPDATE] cannot restore %s\n", path); return 1; }
        s->version = s->prev; s->prev = s->version;
        sha256_hex("", 0, s->sha);
        printf("[UPDATE] restored %s (version %ld)\n", path, s->version);
        /* the restored file stays listed so the boot does not touch it; its recorded sha is no longer meaningful */
        if (!state_save()) return 1;
        return 0;
    }
    int k = (int)(s - st); memmove(&st[k], &st[k + 1], (size_t)(nst - k - 1) * sizeof st[0]); nst--;
    return state_save() ? 0 : 1;
}

int main(int argc, char **argv) {
    cos_mkdir(UPD_DIR); cos_mkdir(BACKUP_DIR);
    const char *c = argc > 1 ? argv[1] : "status";
    if (!strcmp(c, "status")) return cmd_status();
    if (!strcmp(c, "url")) {
        if (argc > 2) { if (!write_new(URL_PATH, argv[2], strlen(argv[2]))) { printf("cannot write %s\n", URL_PATH); return 1; } printf("update source: %s\n", argv[2]); return 0; }
        char *b; size_t n; if (read_all(URL_PATH, &b, &n, 300)) { printf("update source: %s\n", b); free(b); } else printf("no update source set\n");
        return 0;
    }
    if (!strcmp(c, "check")) return cmd_check(argc > 2 ? argv[2] : NULL);
    if (!strcmp(c, "poll")) return cmd_poll(argc > 2 && strstr(argv[2], "://") ? argv[2] : NULL);
    if (!strcmp(c, "auto")) return cmd_auto(argc > 2 ? argv[2] : NULL);
    if (!strcmp(c, "install")) {
        const char *url = NULL; int first = 2;
        if (argc > 2 && strstr(argv[2], "://")) { url = argv[2]; first = 3; }
        return cmd_install(url, argv + first, argc > first ? argc - first : 0);
    }
    if (!strcmp(c, "rollback") && argc > 2) return cmd_rollback(argv[2]);
    printf("usage: cosupdate status | url [URL] | check [URL] | poll | auto [on|off|MINUTES] | install [URL] [NAME...] | rollback PATH\n");
    return 1;
}
