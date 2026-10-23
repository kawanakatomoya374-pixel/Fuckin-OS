/* cosmsgd - the C-OS Message relay server as a C-OS program. Several people's C-OS machines can use one C-OS machine as their
 * server; nothing else is needed. TLS 1.2 (BearSSL), certificate made on first start, state kept in /cosmsgd/.
 * Also builds on the host (COSMSGD_HOST, see tools/cosmsgd_host.sh) so the same code can be tested without a virtual machine. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bearssl.h"
#include "core.h"
#include "cert.h"

#ifdef COSMSGD_HOST
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
static const char *DIR = "cosmsgd_data";
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void say(const char *s) { fputs(s, stderr); }
static void entropy(unsigned char *s, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (!f || fread(s, 1, n, f) != n) { say("[COSMSGD] no /dev/urandom\n"); exit(1); } fclose(f); }
static int listen_on(int port) { int s = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) return -1; fcntl(s, F_SETFL, O_NONBLOCK); return s; }
static int host_connect(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL; char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0 || connect(s, res->ai_addr, res->ai_addrlen)) { if (s >= 0) close(s); freeaddrinfo(res); return -1; }
    freeaddrinfo(res); fcntl(s, F_SETFL, O_NONBLOCK); return s;
}
static int do_accept(int ls, uint32_t *ip) { struct sockaddr_in a; socklen_t l = sizeof a; int s = accept(ls, (struct sockaddr *)&a, &l); *ip = s >= 0 ? ntohl(a.sin_addr.s_addr) : 0; if (s >= 0) fcntl(s, F_SETFL, O_NONBLOCK); return s; }
#define EAGAIN_RET (-1)
static long do_recv(int s, void *b, size_t n) { long r = recv(s, b, n, 0); if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2; return r < 0 ? -1 : (r == 0 ? -1 : r); }
static long do_send(int s, const void *b, size_t n) { long r = send(s, b, n, MSG_NOSIGNAL); if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2; return r; }
static void do_close(int s) { close(s); }
static uint64_t unix_now(void) { return (uint64_t)time(NULL); }
static char *file_load(const char *p, size_t *len) { FILE *f = fopen(p, "rb"); if (!f) { char tmp[300]; snprintf(tmp, sizeof tmp, "%s.new", p); f = fopen(tmp, "rb"); } if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc((size_t)n + 1); if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); *len = (size_t)n; b[n] = 0; return b; }
static bool file_save(const char *p, const void *d, size_t n) { char tmp[300]; snprintf(tmp, sizeof tmp, "%s.new", p); FILE *f = fopen(tmp, "wb"); if (!f) return false; bool ok = fwrite(d, 1, n, f) == n; ok &= fclose(f) == 0; return ok && rename(tmp, p) == 0; }
static void make_dir(void) { mkdir(DIR, 0700); char p[200]; snprintf(p, sizeof p, "%s/files", DIR); mkdir(p, 0700); }
static void file_remove(const char *p) { unlink(p); }
#else
#include "cos.h"
static const char *DIR = "/cosmsgd";
static uint64_t now_ms(void) { return cos_time_ms(); }
static void sleep_ms(int ms) { cos_sleep_ms((uint64_t)ms); }
static void say(const char *s) { cos_puts(s); }
static void entropy(unsigned char *s, size_t n) {
    uint32_t eax, ebx, ecx, edx; __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (!(ecx & (1u << 30))) { say("[COSMSGD] this CPU has no RDRAND: refusing to start (no safe random numbers for tokens and keys)\n"); for (;;) cos_sleep_ms(1000); }
    for (size_t i = 0; i < n; i += 8) {
        uint64_t w = 0; unsigned char ok = 0;
        for (int t = 0; t < 10 && !ok; t++) __asm__ volatile("rdrand %0; setc %1" : "=r"(w), "=qm"(ok));
        if (!ok) { say("[COSMSGD] RDRAND keeps failing: refusing to start\n"); for (;;) cos_sleep_ms(1000); }
        for (size_t b = 0; b < 8 && i + b < n; b++) s[i + b] = (unsigned char)(w >> (b * 8));
    }
}
static int listen_on(int port) { return cos_tcp_listen((uint16_t)port, 8); }
static int do_accept(int ls, uint32_t *ip) { cos_sockaddr_t p = {0}; int s = cos_accept(ls, &p); *ip = s >= 0 ? p.ip : 0; return s; }
static long do_recv(int s, void *b, size_t n) { ssize_t r = cos_recv(s, b, n); if (r == -COS_EAGAIN) return -2; return r <= 0 ? -1 : r; }
static long do_send(int s, const void *b, size_t n) { ssize_t r = cos_send(s, b, n); if (r == -COS_EAGAIN) return -2; return r; }
static void do_close(int s) { cos_sock_close(s); }
static uint64_t unix_now(void) { return cos_time_unix(); }
static char *file_load(const char *p, size_t *len) {
    /* file_save() removes the old file before it renames the new one: a crash in between leaves only "<name>.new", which is complete (it is written first) */
    char tmp[300]; cos_stat_t st; if (cos_stat(p, &st) != 0) { snprintf(tmp, sizeof tmp, "%s.new", p); if (cos_stat(tmp, &st) != 0 || st.size <= 0) return NULL; p = tmp; }
    if (st.size <= 0) return NULL;
    char *b = malloc((size_t)st.size + 1); if (!b) return NULL;
    ssize_t n = cos_read_file(p, b, (size_t)st.size); if (n < 0) { free(b); return NULL; }
    b[n] = 0; *len = (size_t)n; return b;
}
static bool file_save(const char *p, const void *d, size_t n) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.new", p);
    if (cos_write_file(tmp, d, n) != (ssize_t)n) return false;
    cos_unlink(p); return cos_rename(tmp, p) == 0;
}
static void make_dir(void) { cos_mkdir(DIR); char p[200]; snprintf(p, sizeof p, "%s/files", DIR); cos_mkdir(p); }
static void file_remove(const char *p) { cos_unlink(p); }
#endif

/* ------------------------------------------------------------------ platform services for the core */
br_prng_seeder br_prng_seeder_system(const char **name) { (void)name; return 0; }   /* entropy is injected by hand */
static br_hmac_drbg_context rng; static bool rng_ok; static uint64_t rng_used;
uint64_t plat_unix(void) { return unix_now(); }
uint64_t plat_ms(void) { return now_ms(); }
void plat_random(void *buf, size_t n) {
    if (!rng_ok) { unsigned char s[48]; entropy(s, sizeof s); br_hmac_drbg_init(&rng, &br_sha256_vtable, s, sizeof s); rng_ok = true; }
    if (rng_used > (1u << 20)) { unsigned char s[32]; entropy(s, sizeof s); br_hmac_drbg_update(&rng, s, sizeof s); rng_used = 0; }
    br_hmac_drbg_generate(&rng, buf, n); rng_used += n;
}
void plat_log(const char *line) { say(line); }
static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", DIR, name); }
bool plat_save(const char *data, size_t len) { char p[200]; path(p, sizeof p, "state.txt"); return file_save(p, data, len); }
char *plat_load(size_t *len) { char p[200]; path(p, sizeof p, "state.txt"); return file_load(p, len); }
static void blob_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/files/%s", DIR, name); }
bool plat_blob_save(const char *name, const void *data, size_t len) { char p[220]; blob_path(p, sizeof p, name); return file_save(p, data, len); }
unsigned char *plat_blob_load(const char *name, size_t *len) { char p[220]; blob_path(p, sizeof p, name); return (unsigned char *)file_load(p, len); }
void plat_blob_delete(const char *name) { char p[220]; blob_path(p, sizeof p, name); file_remove(p); }

char *plat_conf_load(size_t *len) { char p[200]; path(p, sizeof p, "google.conf"); return file_load(p, len); }
#include "https.inc"

/* ------------------------------------------------------------------ TLS connections */
#define MAXC 40
#define PER_IP 8             /* connections from one address (loopback is exempt) */
#define HANDSHAKE_MS 10000   /* a client that has not finished the TLS handshake by then is dropped */
#define CLOSE_MS 15000       /* a connection the server wants to close: it gets this long to take what is queued */
typedef struct { int sd; core_conn_t *core; uint32_t ip; uint64_t born, close_since; bool hs_done; br_ssl_server_context sc; unsigned char iobuf[BR_SSL_BUFSIZE_BIDI]; } conn_t;
static conn_t *conns[MAXC];
static br_x509_certificate chain[1]; static br_ec_private_key sk; static unsigned char cert_der[512], key_bytes[32]; static size_t cert_len;

static void drop(int i) {
    conn_t *c = conns[i]; if (!c) return;
    do_close(c->sd); core_close(c->core); free(c); conns[i] = NULL;
}
static bool service(int i) {
    conn_t *c = conns[i]; br_ssl_engine_context *e = &c->sc.eng; bool prog = false;
    for (int guard = 0; guard < 64; guard++) {
        unsigned st = br_ssl_engine_current_state(e); size_t len;
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) c->hs_done = true;       /* the TLS handshake is over */
        if (st & BR_SSL_CLOSED) { drop(i); return true; }
        if (st & BR_SSL_SENDREC) {
            unsigned char *b = br_ssl_engine_sendrec_buf(e, &len); long n = do_send(c->sd, b, len);
            if (n > 0) { br_ssl_engine_sendrec_ack(e, (size_t)n); prog = true; continue; }
            if (n != -2) { drop(i); return true; }
        }
        if (st & BR_SSL_RECVAPP) {                                  /* bytes from the client -> the protocol core */
            unsigned char *r = br_ssl_engine_recvapp_buf(e, &len);
            bool keep = core_feed(c->core, (const char *)r, len); br_ssl_engine_recvapp_ack(e, len); prog = true;
            if (!keep) { br_ssl_engine_close(e); prog = true; continue; }
            continue;
        }
        if (st & BR_SSL_SENDAPP) {                                  /* what the core queued -> the client */
            size_t on; const char *o = core_out(c->core, &on);
            if (on) { size_t sl; unsigned char *s = br_ssl_engine_sendapp_buf(e, &sl); size_t k = on < sl ? on : sl; memcpy(s, o, k); br_ssl_engine_sendapp_ack(e, k); core_out_ack(c->core, k); br_ssl_engine_flush(e, 0); prog = true; continue; }
            if (core_wants_close(c->core)) { br_ssl_engine_close(e); prog = true; continue; }
        }
        if (st & BR_SSL_RECVREC) {
            unsigned char *b = br_ssl_engine_recvrec_buf(e, &len); long n = do_recv(c->sd, b, len);
            if (n > 0) { br_ssl_engine_recvrec_ack(e, (size_t)n); prog = true; continue; }
            if (n != -2) { drop(i); return true; }
        }
        break;
    }
    return prog;
}

static void setup_identity(void) {
    char pc[200], pk2[200]; path(pc, sizeof pc, "cert.der"); path(pk2, sizeof pk2, "key.bin");
    size_t cl = 0, kl = 0; char *c = file_load(pc, &cl), *k = file_load(pk2, &kl);
    if (c && k && cl <= sizeof cert_der && kl == 32) { memcpy(cert_der, c, cl); cert_len = cl; memcpy(key_bytes, k, 32); say("[COSMSGD] using the saved certificate\n"); }
    else {
        if (!cert_make(cert_der, &cert_len, key_bytes)) { say("[COSMSGD] cannot make a certificate\n"); for (;;) sleep_ms(1000); }
        file_save(pc, cert_der, cert_len); file_save(pk2, key_bytes, 32);
        say("[COSMSGD] made a new self-signed certificate\n");
    }
    free(c); free(k);
    unsigned char fp[32]; br_sha256_context h; br_sha256_init(&h); br_sha256_update(&h, cert_der, cert_len); br_sha256_out(&h, fp);
    char lb[120], hx[65]; for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", fp[i]);
    snprintf(lb, sizeof lb, "[COSMSGD] certificate fingerprint (SHA-256) %s\n", hx); say(lb);
    chain[0].data = cert_der; chain[0].data_len = cert_len; sk.curve = BR_EC_secp256r1; sk.x = key_bytes; sk.xlen = 32;
}

#ifdef COSMSGD_HOST
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 5151; if (argc > 2) DIR = argv[2];
#else
int main(void) {
    int port = 5151;
    cos_netinfo_t ni; for (int i = 0; i < 300; i++) { if (cos_net_info(&ni) == 0 && ni.has_lease) break; cos_sleep_ms(100); }
#endif
    make_dir(); setup_identity(); core_init();
    int ls = listen_on(port);
    if (ls < 0) { say("[COSMSGD] cannot listen\n"); return 1; }
    char lb[64]; snprintf(lb, sizeof lb, "[COSMSGD] listening on tcp %d\n", port); say(lb);
    uint64_t last_tick = 0;
    for (;;) {
        bool prog = false;
        uint32_t pip = 0; int nsd = do_accept(ls, &pip);
        if (nsd >= 0) {
            int slot = -1, same = 0; for (int i = 0; i < MAXC; i++) { if (!conns[i]) { if (slot < 0) slot = i; } else if (pip && conns[i]->ip == pip) same++; }
            if (pip && (pip >> 24) != 127 && same >= PER_IP) slot = -1;       /* one address cannot take every slot */
            conn_t *c = slot >= 0 ? calloc(1, sizeof *c) : NULL;
            core_conn_t *cc = c ? core_open() : NULL;
            if (!c || !cc) { free(c); do_close(nsd); say("[COSMSGD] refused a connection: too many\n"); }
            else {
                c->sd = nsd; c->core = cc; c->ip = pip; c->born = now_ms(); core_set_peer(cc, pip); conns[slot] = c;
                br_ssl_server_init_full_ec(&c->sc, chain, 1, BR_KEYTYPE_EC, &sk);
                br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
                unsigned char s[32]; plat_random(s, sizeof s); br_ssl_engine_inject_entropy(&c->sc.eng, s, sizeof s);
                br_ssl_server_reset(&c->sc); prog = true;
            }
        }
        for (int i = 0; i < MAXC; i++) if (conns[i] && service(i)) prog = true;
        for (int i = 0; i < MAXC; i++) if (conns[i]) {                    /* connections the TLS or protocol state machine will never finish */
            conn_t *c = conns[i]; uint64_t t = now_ms();
            if (!c->hs_done && t - c->born > HANDSHAKE_MS) { drop(i); continue; }
            if (core_conn_closing(c->core)) { if (!c->close_since) c->close_since = t; if (t - c->close_since > CLOSE_MS) drop(i); }
        }
        if (hjobs_pump()) prog = true;
        uint64_t t = now_ms(); if (t - last_tick >= 100) { last_tick = t; core_tick(); }
        if (!prog) sleep_ms(1);
    }
}
