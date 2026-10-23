/* Outbound HTTPS for the Google sign-in: small non-blocking jobs (resolve -> connect -> TLS -> request -> read to the end). The server
 * certificate is checked against the system trust store (121 Mozilla roots, shared with the kernel's browser TLS) plus, for tests only,
 * one extra root named by "extra_ca=<DER file>" in google.conf. Included by main.c after its platform layer. */
#include "../../../src/kernel/drivers/generated/bearssl_system_trust_anchors.inc"

#define HJOBS 4
#define HRESP_MAX (256 * 1024)
typedef struct {
    int id, state, sd, port; uint64_t t0; char host[64]; char *req; size_t req_len, req_off;
    uint32_t ip; br_ssl_client_context cc; br_x509_minimal_context xc; unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char *resp; size_t resp_len; int dns_id;
#ifndef COSMSGD_HOST
    cos_dns_result_t dns;
#endif
} hjob_t;
enum { HS_FREE = 0, HS_RESOLVE, HS_CONNECT, HS_TLS, HS_DEAD };
static bool ci_prefix(const char *s, const char *p) { for (; *p; s++, p++) { char a = *s, b = *p; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) return false; } return true; }
static hjob_t *hjobs[HJOBS]; static int next_job_id = 1;
static br_x509_trust_anchor *anchors; static size_t n_anchors; static unsigned char extra_dn[600]; static unsigned char *extra_a, *extra_b; static bool anchors_ready;

static size_t dn_len_;
static void dn_append(void *ctx, const void *buf, size_t len) { (void)ctx; if (dn_len_ + len <= sizeof extra_dn) { memcpy(extra_dn + dn_len_, buf, len); dn_len_ += len; } }
static void build_anchors(void) {
    if (anchors_ready) return; anchors_ready = true;
    size_t base = COS_SYSTEM_TRUST_ANCHORS_COUNT; anchors = malloc(sizeof *anchors * (base + 1)); if (!anchors) return;
    memcpy(anchors, COS_SYSTEM_TRUST_ANCHORS, sizeof *anchors * base); n_anchors = base;
    char path[200]; if (!core_conf_get("extra_ca", path, sizeof path)) return;                      /* tests only */
    size_t len; unsigned char *der = (unsigned char *)file_load(path, &len); if (!der) return;
    br_x509_decoder_context d; dn_len_ = 0; br_x509_decoder_init(&d, dn_append, NULL); br_x509_decoder_push(&d, der, len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&d);
    if (pk && pk->key_type == BR_KEYTYPE_RSA) {
        br_x509_trust_anchor *t = &anchors[n_anchors]; memset(t, 0, sizeof *t);
        unsigned char *dn = malloc(dn_len_), *n = malloc(pk->key.rsa.nlen), *e = malloc(pk->key.rsa.elen);
        memcpy(dn, extra_dn, dn_len_); memcpy(n, pk->key.rsa.n, pk->key.rsa.nlen); memcpy(e, pk->key.rsa.e, pk->key.rsa.elen);
        t->dn.data = dn; t->dn.len = dn_len_; t->flags = BR_X509_TA_CA; t->pkey.key_type = BR_KEYTYPE_RSA;
        t->pkey.key.rsa.n = n; t->pkey.key.rsa.nlen = pk->key.rsa.nlen; t->pkey.key.rsa.e = e; t->pkey.key.rsa.elen = pk->key.rsa.elen; n_anchors++;
        say("[COSMSGD] google.conf: one extra trust anchor (test mode)\n");
    }
    free(der);
}

static void hjob_end(int i, int status) {
    hjob_t *j = hjobs[i]; int id = j->id; char *body = (char *)""; size_t blen = 0;
    if (!status) { char lb[160]; snprintf(lb, sizeof lb, "[COSMSGD] https request to %s failed (state %d, tls error %d)\n", j->host, j->state, j->state == HS_TLS ? br_ssl_engine_last_error(&j->cc.eng) : 0); say(lb); }
    if (status && j->resp) {                                         /* HTTP/1.x status line, headers, body (chunked bodies are decoded) */
        j->resp[j->resp_len] = 0; char *he = strstr(j->resp, "\r\n\r\n");
        if (!he) status = 0; else {
            status = atoi(j->resp + (strncmp(j->resp, "HTTP/", 5) == 0 ? 9 : 0)); *he = 0; body = he + 4; blen = j->resp_len - (size_t)(body - j->resp);
            bool chunked = false; for (char *p = j->resp; *p; p++) if (ci_prefix(p, "Transfer-Encoding: chunked")) chunked = true;
            if (chunked) {
                char *r = body, *w = body; size_t out = 0;
                while (r < body + blen) { char *e; unsigned long cl = strtoul(r, &e, 16); if (e == r) break; r = strstr(e, "\r\n"); if (!r) break; r += 2; if (!cl) break; if (cl > (unsigned long)(body + blen - r)) cl = (unsigned long)(body + blen - r);   /* (not r + cl: a huge chunk size wraps) */ memmove(w, r, cl); w += cl; out += cl; r += cl + 2; }
                blen = out; body[blen] = 0;
            }
        }
    }
    if (j->sd >= 0) do_close(j->sd);
    char *copy = malloc(blen + 1); if (copy) { memcpy(copy, body, blen); copy[blen] = 0; }
    free(j->req); free(j->resp); free(j); hjobs[i] = NULL;
    core_http_done(id, status, copy ? copy : "", blen); free(copy);
}

/* true once the whole HTTP answer has arrived (Content-Length bytes, or the last chunk), so we need not wait for the peer to hang up */
static bool resp_complete(hjob_t *j) {
    if (!j->resp) return false; j->resp[j->resp_len] = 0; char *he = strstr(j->resp, "\r\n\r\n"); if (!he) return false;
    size_t blen = j->resp_len - (size_t)(he + 4 - j->resp);
    for (char *p = j->resp; p < he; p++) if (ci_prefix(p, "Content-Length:")) return blen >= (size_t)strtoul(p + 15, NULL, 10);
    for (char *p = j->resp; p < he; p++) if (ci_prefix(p, "Transfer-Encoding: chunked")) return blen >= 5 && !memcmp(j->resp + j->resp_len - 5, "0\r\n\r\n", 5);
    return false;
}
static void hjob_tls_start(hjob_t *j) {
    br_ssl_client_init_full(&j->cc, &j->xc, anchors, n_anchors);
    uint64_t t = plat_unix(); br_x509_minimal_set_time(&j->xc, (uint32_t)(t / 86400 + 719528), (uint32_t)(t % 86400));
    br_ssl_engine_set_buffer(&j->cc.eng, j->iobuf, sizeof j->iobuf, 1);
    unsigned char s[32]; plat_random(s, sizeof s); br_ssl_engine_inject_entropy(&j->cc.eng, s, sizeof s);
    br_ssl_client_reset(&j->cc, j->host, 0); j->state = HS_TLS;
}

/* one step; returns true when it made progress */
static bool hjob_step(int i) {
    hjob_t *j = hjobs[i]; bool prog = false;
    if (plat_ms() - j->t0 > 30000 || j->state == HS_DEAD) { hjob_end(i, 0); return true; }
    if (j->state == HS_RESOLVE) {
#ifdef COSMSGD_HOST
        hjob_end(i, 0); return true;
#else
        if (j->dns.state == 0) { if (j->dns_id == 0) { if (cos_dns_start(j->host, &j->dns) < 0) { hjob_end(i, 0); return true; } j->dns_id = 1; } else cos_dns_poll(&j->dns); }
        if (j->dns.state < 0) { hjob_end(i, 0); return true; }
        if (j->dns.state == 1) { j->ip = COS_IP(j->dns.ip[0], j->dns.ip[1], j->dns.ip[2], j->dns.ip[3]); j->sd = cos_socket(COS_SOCK_STREAM); if (j->sd < 0) { hjob_end(i, 0); return true; } j->state = HS_CONNECT; prog = true; }
        return prog;
#endif
    }
    if (j->state == HS_CONNECT) {
#ifndef COSMSGD_HOST
        int r = cos_connect(j->sd, j->ip, (uint16_t)j->port);
        if (r == 0 || r == -COS_EISCONN) { hjob_tls_start(j); return true; }
        if (r != -COS_EINPROGRESS && r != -COS_EAGAIN) { hjob_end(i, 0); return true; }
#endif
        return false;
    }
    br_ssl_engine_context *e = &j->cc.eng;
    for (int guard = 0; guard < 64; guard++) {
        unsigned st = br_ssl_engine_current_state(e); size_t len; 
        if (st & BR_SSL_CLOSED) { hjob_end(i, br_ssl_engine_last_error(e) == BR_ERR_OK || br_ssl_engine_last_error(e) == 0 ? 1 : 0); return true; }
        if (st & BR_SSL_SENDREC) { unsigned char *b = br_ssl_engine_sendrec_buf(e, &len); long n = do_send(j->sd, b, len); if (n > 0) { br_ssl_engine_sendrec_ack(e, (size_t)n); prog = true; continue; } if (n != -2) { hjob_end(i, j->resp_len ? 1 : 0); return true; } }
        if (st & BR_SSL_SENDAPP) {
            if (j->req_off < j->req_len) { size_t sl; unsigned char *s = br_ssl_engine_sendapp_buf(e, &sl); size_t k = j->req_len - j->req_off < sl ? j->req_len - j->req_off : sl; memcpy(s, j->req + j->req_off, k); br_ssl_engine_sendapp_ack(e, k); j->req_off += k; if (j->req_off == j->req_len) br_ssl_engine_flush(e, 0); prog = true; continue; }
        }
        if (st & BR_SSL_RECVAPP) {
            unsigned char *r = br_ssl_engine_recvapp_buf(e, &len);
            if (j->resp_len + len > HRESP_MAX) { hjob_end(i, 0); return true; }
            char *nr = realloc(j->resp, j->resp_len + len + 1); if (!nr) { hjob_end(i, 0); return true; }
            j->resp = nr; memcpy(j->resp + j->resp_len, r, len); j->resp_len += len; br_ssl_engine_recvapp_ack(e, len); prog = true;
            if (resp_complete(j)) { hjob_end(i, 1); return true; }
            continue;
        }
        if (st & BR_SSL_RECVREC) { unsigned char *b = br_ssl_engine_recvrec_buf(e, &len); long n = do_recv(j->sd, b, len); if (n > 0) { br_ssl_engine_recvrec_ack(e, (size_t)n); prog = true; continue; } if (n != -2) { /* the peer closed: whatever we have is the answer */ br_ssl_engine_close(e); if (br_ssl_engine_current_state(e) & BR_SSL_CLOSED) { hjob_end(i, j->resp_len ? 1 : 0); return true; } continue; } }
        break;
    }
    return prog;
}

int plat_http(bool post, const char *host, int port, const char *path, const char *body) {
    int slot = -1; for (int i = 0; i < HJOBS; i++) if (!hjobs[i]) { slot = i; break; }
    if (slot < 0) return 0;
    build_anchors(); if (!anchors) return 0;
    hjob_t *j = calloc(1, sizeof *j); if (!j) return 0;
    j->id = next_job_id++; j->sd = -1; j->port = port; j->t0 = plat_ms(); snprintf(j->host, sizeof j->host, "%s", host);
    size_t bl = body ? strlen(body) : 0; j->req = malloc(bl + 400 + strlen(path)); if (!j->req) { free(j); return 0; }
    j->req_len = (size_t)snprintf(j->req, bl + 400 + strlen(path), "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: cosmsgd\r\nAccept: application/json\r\n%sConnection: close\r\n\r\n%s",
                                  post ? "POST" : "GET", path, host, post ? "Content-Type: application/x-www-form-urlencoded\r\n" : "", body ? body : "");
    if (post) { char *p = strstr(j->req, "Connection: close"); char cl[48]; snprintf(cl, sizeof cl, "Content-Length: %u\r\n", (unsigned)bl); memmove(p + strlen(cl), p, strlen(p) + 1); memcpy(p, cl, strlen(cl)); j->req_len = strlen(j->req); }
    hjobs[slot] = j;
#ifdef COSMSGD_HOST
    j->sd = host_connect(host, port);
    if (j->sd < 0) j->state = HS_DEAD; else hjob_tls_start(j);
#else
    j->state = HS_RESOLVE;
#endif
    return j->id;
}
static bool hjobs_pump(void) { bool prog = false; for (int i = 0; i < HJOBS; i++) if (hjobs[i] && hjob_step(i)) prog = true; return prog; }
