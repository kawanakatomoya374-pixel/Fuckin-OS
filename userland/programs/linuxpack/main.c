/* linuxpack - installs a Linux app pack (Firefox ...) into /linux (docs/LINUX_COMPAT.md, "App packs").
 *
 *   LinuxInstall [URL]        download the pack and install it (continues where an interrupted install stopped)
 *   LinuxInstall status       what is installed, and how far an install got
 *
 * The pack lives on a web server as a signed manifest (linuxpack.manifest) and chunks (NAME.pNNNN.bin, zlib streams of file records, see tools/linux_pack.py).
 * The manifest is signed with the same key as the app updates (ECDSA P-256 over SHA-256); every chunk must have the size and SHA-256 the manifest names before
 * anything of it is used. Files go only below /linux (no ".." and no absolute paths). Progress is written to /system/linuxpack/progress.txt for the Launcher:
 *   "<running|done|error> <chunks done> <chunks> <bytes done> <bytes> <message>" */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "bearssl.h"
#include "update_pubkey.h"
#include "linuxpack_url.h"
#include "inflate.h"

#define PACK_DIR "/system/linuxpack"
#define STATE_PATH PACK_DIR "/state.txt"
#define PROGRESS_PATH PACK_DIR "/progress.txt"
#define URL_PATH PACK_DIR "/url.txt"
#define ROOT "/linux"
#define MAX_MANIFEST (64u * 1024u)
#define MAX_CHUNKS 512
#define MAX_DL (9u * 1024u * 1024u + 512u * 1024u)
#define MAX_RAW (48u * 1024u * 1024u)

typedef struct { char file[64]; uint32_t size, raw; char sha[65]; } chunk_t;
static chunk_t chunks[MAX_CHUNKS]; static int nchunks;
static char pk_name[32], pk_title[48], pk_version[32]; static long pk_serial; static uint64_t pk_bytes, pk_download;
static uint64_t bytes_done; static int chunks_done;

/* ---------------------------------------------------------------- helpers */
static void hex(const unsigned char *b, size_t n, char *out) { static const char *d = "0123456789abcdef"; for (size_t i = 0; i < n; ++i) { out[i * 2] = d[b[i] >> 4]; out[i * 2 + 1] = d[b[i] & 15]; } out[n * 2] = 0; }
static void sha256_hex(const void *p, size_t n, char out[65]) { br_sha256_context c; unsigned char h[32]; br_sha256_init(&c); br_sha256_update(&c, p, n); br_sha256_out(&c, h); hex(h, 32, out); }
static bool hexval(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) { char c = s[i * 2 + k]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return false; v = v * 16 + d; }
        out[i] = (unsigned char)v;
    }
    return true;
}
static void progress(const char *state, const char *msg) {
    char b[320]; int n = snprintf(b, sizeof b, "%s %d %d %llu %llu %s\n", state, chunks_done, nchunks, (unsigned long long)bytes_done, (unsigned long long)pk_bytes, msg);
    cos_mkdir("/system"); cos_mkdir(PACK_DIR); cos_write_file(PROGRESS_PATH, b, (size_t)n);
}
static bool name_ok(const char *s) { if (!*s || strlen(s) > 40) return false; for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) return false; return true; }
/* a path inside the pack: relative, every part a plain name (FAT-safe), no "." or ".." */
static bool rel_ok(const char *p, size_t n) {
    if (n == 0 || n > 200) return false;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || p[i] == '/') {
            size_t l = i - start; if (l == 0 || l > 120) return false;
            if ((l == 1 && p[start] == '.') || (l == 2 && p[start] == '.' && p[start + 1] == '.')) return false;
            start = i + 1;
        } else { unsigned char c = (unsigned char)p[i]; if (c < 0x20 || c == 0x7f || strchr("\"*:<>?\\|", c)) return false; }
    }
    return true;
}
static uint8_t *http_fetch(const char *url, size_t *len, size_t max, char *why, size_t whycap) {
    int job = cos_net_http_start(url);
    if (job < 0) { snprintf(why, whycap, "no free network job"); return NULL; }
    cos_http_result_t r; uint64_t t0 = cos_time_ms();
    for (;;) {
        int p = cos_net_http_poll(job, &r);
        if (p == 1) break;
        if (p < 0) { snprintf(why, whycap, "the transfer disappeared"); return NULL; }
        if (cos_time_ms() - t0 > 600000) { cos_net_http_cancel(job); snprintf(why, whycap, "timeout"); return NULL; }
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
static bool manifest_parse(char *m, size_t len, char *why, size_t whycap) {
    const char *marker = "\n--SIG--\n"; char *sg = NULL;
    for (size_t i = 0; i + 9 <= len; ++i) if (memcmp(m + i, marker, 9) == 0) { sg = m + i; break; }
    if (!sg) { snprintf(why, whycap, "no signature"); return false; }
    size_t body_len = (size_t)(sg - m) + 1; const char *hx = sg + 9; unsigned char sig[64];
    if (strlen(hx) < 128 || !hexval(hx, sig, 64)) { snprintf(why, whycap, "the signature is malformed"); return false; }
    unsigned char dg[32]; br_sha256_context c; br_sha256_init(&c); br_sha256_update(&c, m, body_len); br_sha256_out(&c, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)cos_update_pubkey, sizeof cos_update_pubkey };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, sig, 64) != 1) { snprintf(why, whycap, "the signature does not match (not made with this system's key)"); return false; }
    m[body_len] = 0;                                                  /* from here the body is trusted */
    nchunks = 0; pk_name[0] = pk_title[0] = pk_version[0] = 0; pk_serial = -1; pk_bytes = pk_download = 0; bool header = false, pack = false;
    for (char *line = m; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!strcmp(line, "COSLP 1")) header = true;
        else if (!strncmp(line, "pack ", 5)) {
            for (char *tok = strtok(line + 5, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "name")) snprintf(pk_name, sizeof pk_name, "%s", v);
                else if (!strcmp(tok, "title")) { snprintf(pk_title, sizeof pk_title, "%s", v); for (char *q = pk_title; *q; ++q) if (*q == '_') *q = ' '; }
                else if (!strcmp(tok, "version")) snprintf(pk_version, sizeof pk_version, "%s", v);
                else if (!strcmp(tok, "serial")) pk_serial = atol(v);
                else if (!strcmp(tok, "bytes")) pk_bytes = strtoull(v, NULL, 10);
                else if (!strcmp(tok, "download")) pk_download = strtoull(v, NULL, 10);
            }
            pack = true;
        } else if (!strncmp(line, "chunk ", 6)) {
            if (nchunks >= MAX_CHUNKS) { snprintf(why, whycap, "too many chunks"); return false; }
            chunk_t *k = &chunks[nchunks]; memset(k, 0, sizeof *k);
            for (char *tok = strtok(line + 6, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(tok, "file")) snprintf(k->file, sizeof k->file, "%s", v);
                else if (!strcmp(tok, "size")) k->size = (uint32_t)strtoul(v, NULL, 10);
                else if (!strcmp(tok, "sha256")) snprintf(k->sha, sizeof k->sha, "%s", v);
                else if (!strcmp(tok, "raw")) k->raw = (uint32_t)strtoul(v, NULL, 10);
            }
            if (!name_ok(k->file) || strlen(k->sha) != 64 || k->size == 0 || k->size > MAX_DL || k->raw == 0 || k->raw > MAX_RAW) { snprintf(why, whycap, "chunk %d is malformed", nchunks + 1); return false; }
            nchunks++;
        }
        line = nl ? nl + 1 : NULL;
        if (nl && !line[0]) break;
    }
    if (!header || !pack || pk_serial < 0 || !name_ok(pk_name) || nchunks == 0) { snprintf(why, whycap, "not a pack manifest"); return false; }
    return true;
}

/* ---------------------------------------------------------------- files */
static void mkdirs(const char *full) {                                  /* every directory above the file */
    char p[400]; snprintf(p, sizeof p, "%s", full);
    for (char *s = p + 1; *s; ++s) if (*s == '/') { *s = 0; cos_mkdir(p); *s = '/'; }
}
static bool write_all(int fd, const uint8_t *d, size_t n) {
    while (n) { ssize_t w = cos_fd_write(fd, d, n > 65536 ? 65536 : n); if (w <= 0) return false; d += w; n -= (size_t)w; }
    return true;
}
/* one chunk: records of  u16 path length | path | u8 kind | u64 size | u64 offset | u32 length | data */
static bool apply_chunk(const uint8_t *raw, size_t n, char *why, size_t whycap) {
    size_t pos = 0; char last_dir[260]; last_dir[0] = 0;
    while (pos < n) {
        if (pos + 2 > n) goto bad;
        size_t pl = raw[pos] | (size_t)raw[pos + 1] << 8; pos += 2;
        if (pos + pl + 21 > n) goto bad;
        const char *path = (const char *)raw + pos; pos += pl;
        if (!rel_ok(path, pl)) { snprintf(why, whycap, "a path in the pack is not allowed"); return false; }
        uint8_t kind = raw[pos]; uint64_t size, off; uint32_t len; memcpy(&size, raw + pos + 1, 8); memcpy(&off, raw + pos + 9, 8); memcpy(&len, raw + pos + 17, 4); pos += 21;
        if (kind != 0 || pos + len > n || off > size || (uint64_t)len > size - off) goto bad;
        char full[400]; snprintf(full, sizeof full, "%s/%.*s", ROOT, (int)pl, path);
        const char *slash = strrchr(full, '/'); size_t dl = (size_t)(slash - full);
        if (dl != strlen(last_dir) || strncmp(full, last_dir, dl)) { mkdirs(full); snprintf(last_dir, sizeof last_dir, "%.*s", (int)dl, full); }
        int fd = off == 0 ? cos_open(full, COS_O_WRONLY_CREATE) : cos_open(full, COS_O_RDWR_CREATE);      /* the first piece starts the file, later ones are put at their offset */
        if (fd < 0) { snprintf(why, whycap, "cannot write %s (disk full?)", full); return false; }
        if (off && cos_lseek(fd, (int64_t)off) < 0) { cos_close(fd); snprintf(why, whycap, "cannot seek in %s", full); return false; }
        bool ok = write_all(fd, raw + pos, len); cos_close(fd);
        if (!ok) { snprintf(why, whycap, "cannot write %s (disk full?)", full); return false; }
        pos += len;
    }
    return true;
bad:
    snprintf(why, whycap, "a chunk is damaged"); return false;
}

/* ---------------------------------------------------------------- state */
static int state_next(void) {                                         /* which chunk to continue with (0 = start over) */
    char b[160]; ssize_t n = cos_read_file(STATE_PATH, b, sizeof b - 1); if (n <= 0) return 0; b[n] = 0;
    char nm[48]; long ser = -1, next = 0;
    char *l = b; while (l && *l) { char *nl = strchr(l, '\n'); if (nl) *nl = 0; if (!strncmp(l, "name ", 5)) snprintf(nm, sizeof nm, "%s", l + 5); else if (!strncmp(l, "serial ", 7)) ser = atol(l + 7); else if (!strncmp(l, "next ", 5)) next = atol(l + 5); l = nl ? nl + 1 : NULL; }
    if (ser != pk_serial || strcmp(nm, pk_name) != 0 || next < 0 || next > nchunks) return 0;
    return (int)next;
}
static void state_save(int next) { char b[160]; int n = snprintf(b, sizeof b, "name %s\nserial %ld\nnext %d\nversion %s\n", pk_name, pk_serial, next, pk_version); cos_mkdir("/system"); cos_mkdir(PACK_DIR); cos_write_file(STATE_PATH, b, (size_t)n); }

static int cmd_status(void) {
    char b[320]; ssize_t n = cos_read_file(PROGRESS_PATH, b, sizeof b - 1);
    if (n > 0) { b[n] = 0; printf("[LINUXPACK] last: %s", b); } else printf("[LINUXPACK] nothing has been installed by this program yet\n");
    cos_stat_t st; printf("[LINUXPACK] Firefox under /linux: %s\n", cos_stat(ROOT "/usr/lib/firefox/firefox", &st) == 0 ? "present" : "not present");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "status")) return cmd_status();
    char url[300] = COS_LINUXPACK_URL;
    { char b[300]; ssize_t n = cos_read_file(URL_PATH, b, sizeof b - 1); if (n > 0) { b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; if (strncmp(b, "http", 4) == 0) snprintf(url, sizeof url, "%s", b); } }
    if (argc > 1 && !strncmp(argv[1], "http", 4)) snprintf(url, sizeof url, "%s", argv[1]);
    if (!url[0]) { printf("[LINUXPACK] no download address set (LinuxInstall http://HOST/PATH/)\n"); progress("error", "no download address"); return 1; }
    size_t l = strlen(url); if (url[l - 1] != '/' && l + 1 < sizeof url) { url[l] = '/'; url[l + 1] = 0; }
    char why[200], full[360]; size_t mlen = 0;
    progress("running", "reading the list of files");
    snprintf(full, sizeof full, "%slinuxpack.manifest", url);
    uint8_t *m = http_fetch(full, &mlen, MAX_MANIFEST, why, sizeof why);
    if (!m) { printf("[LINUXPACK] cannot fetch %s: %s\n", full, why); progress("error", "cannot reach the server"); return 1; }
    bool ok = manifest_parse((char *)m, mlen, why, sizeof why); free(m);
    if (!ok) { printf("[LINUXPACK] manifest refused: %s\n", why); progress("error", "the list of files is not trusted"); return 1; }
    printf("[LINUXPACK] %s %s: %d chunk(s), download %llu MiB, installed %llu MiB\n", pk_title, pk_version, nchunks, (unsigned long long)(pk_download >> 20), (unsigned long long)(pk_bytes >> 20));
    int start = state_next(); chunks_done = start;
    for (int i = 0; i < start; ++i) bytes_done += chunks[i].raw;
    if (start) printf("[LINUXPACK] continuing with chunk %d\n", start + 1);
    cos_mkdir(ROOT);
    for (int i = start; i < nchunks; ++i) {
        chunk_t *k = &chunks[i]; char msg[120]; snprintf(msg, sizeof msg, "downloading %s", k->file); progress("running", msg);
        snprintf(full, sizeof full, "%s%s", url, k->file);
        size_t n = 0; uint8_t *d = http_fetch(full, &n, MAX_DL, why, sizeof why);
        if (!d) { printf("[LINUXPACK] chunk %d: download failed: %s\n", i + 1, why); progress("error", "download failed (run it again to continue)"); return 1; }
        char sha[65]; sha256_hex(d, n, sha);
        if (n != k->size || strcmp(sha, k->sha) != 0) { printf("[LINUXPACK] chunk %d: refused: size or SHA-256 differs from the list (not what was signed)\n", i + 1); free(d); progress("error", "a download does not match the signed list"); return 1; }
        uint8_t *raw = malloc((size_t)k->raw + 16); size_t rn = 0;
        if (!raw) { free(d); printf("[LINUXPACK] out of memory\n"); progress("error", "out of memory"); return 1; }
        int rc = cos_inflate_zlib(d, n, raw, k->raw + 16, &rn); free(d);
        if (rc || rn != k->raw) { printf("[LINUXPACK] chunk %d: cannot unpack (%d)\n", i + 1, rc); free(raw); progress("error", "a download is damaged"); return 1; }
        snprintf(msg, sizeof msg, "installing %s", k->file); progress("running", msg);
        bool wrote = apply_chunk(raw, rn, why, sizeof why); free(raw);
        if (!wrote) { printf("[LINUXPACK] chunk %d: %s\n", i + 1, why); progress("error", why); return 1; }
        bytes_done += k->raw; chunks_done = i + 1; state_save(i + 1);
        printf("[LINUXPACK] chunk %d/%d done (%llu MiB)\n", i + 1, nchunks, (unsigned long long)(bytes_done >> 20));
    }
    progress("done", "installed");
    printf("[LINUXPACK] %s is installed\n", pk_title);
    return 0;
}
