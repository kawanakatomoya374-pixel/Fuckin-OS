/*
 * tls_backend_openssl.c - Optional OpenSSL TLS backend
 *
 * This backend is disabled by default in the freestanding kernel build.
 * Enable it only for hosted builds where OpenSSL and socket-style I/O are available.
 */

#if defined(COS_ENABLE_OPENSSL_BACKEND)

#include "tls_backend.h"
#include "net.h"
#include "dns.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

struct tls_session {
    socket_t* socket;
    SSL_CTX* ctx;
    SSL* ssl;
};

static BIO_METHOD* s_bio_method = NULL;

static int openssl_bio_create(BIO* bio) {
    BIO_set_init(bio, 0);
    BIO_set_data(bio, NULL);
    BIO_set_shutdown(bio, 1);
    return 1;
}

static int openssl_bio_destroy(BIO* bio) {
    if (!bio) return 0;
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int openssl_bio_read(BIO* bio, char* out, int outl) {
    socket_t* sock = (socket_t*)BIO_get_data(bio);
    if (!sock || !out || outl <= 0) return 0;
    int r = socket_recv(sock, out, (size_t)outl, 0);
    if (r <= 0) {
        BIO_set_retry_read(bio);
        return -1;
    }
    return r;
}

static int openssl_bio_write(BIO* bio, const char* in, int inl) {
    socket_t* sock = (socket_t*)BIO_get_data(bio);
    if (!sock || !in || inl <= 0) return 0;
    size_t sent = 0;
    while (sent < (size_t)inl) {
        int r = socket_send(sock, (void*)(in + sent), (size_t)(inl - (int)sent), 0);
        if (r <= 0) {
            BIO_set_retry_write(bio);
            return -1;
        }
        sent += (size_t)r;
    }
    return (int)sent;
}

static long openssl_bio_ctrl(BIO* bio, int cmd, long num, void* ptr) {
    (void)num; (void)ptr;
    switch (cmd) {
        case BIO_CTRL_FLUSH:
            return 1;
        default:
            return 1;
    }
}

static BIO_METHOD* openssl_get_bio_method(void) {
    if (s_bio_method) return s_bio_method;
    s_bio_method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "cos-socket-bio");
    if (!s_bio_method) return NULL;
    BIO_meth_set_write(s_bio_method, openssl_bio_write);
    BIO_meth_set_read(s_bio_method, openssl_bio_read);
    BIO_meth_set_ctrl(s_bio_method, openssl_bio_ctrl);
    BIO_meth_set_create(s_bio_method, openssl_bio_create);
    BIO_meth_set_destroy(s_bio_method, openssl_bio_destroy);
    return s_bio_method;
}

static void tls_session_destroy(tls_session_t* session) {
    if (!session) return;
    if (session->ssl) {
        SSL_free(session->ssl);
        session->ssl = NULL;
    }
    if (session->ctx) {
        SSL_CTX_free(session->ctx);
        session->ctx = NULL;
    }
    if (session->socket) {
        socket_close(session->socket);
        session->socket = NULL;
    }
    kfree(session);
}

int tls_backend_available(void) {
    return 1;
}

tls_session_t* tls_connect(const char* host, uint64_t port) {
    if (!host || !host[0]) return NULL;

    ip_addr_t server_ip;
    if (dns_resolve(host, &server_ip) < 0) {
        serial_puts("[TLS/OPENSSL] DNS lookup failed\n");
        return NULL;
    }

    socket_t* sock = socket_create(AF_INET, SOCK_STREAM, 0);
    if (!sock) {
        serial_puts("[TLS/OPENSSL] socket_create failed\n");
        return NULL;
    }

    sock_addr_t addr;
    addr.family = AF_INET;
    addr.port = (uint16_t)port;
    addr.addr = server_ip;
    if (socket_connect(sock, &addr) < 0) {
        serial_puts("[TLS/OPENSSL] socket_connect failed\n");
        socket_close(sock);
        return NULL;
    }

    OPENSSL_init_ssl(0, NULL);

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        serial_puts("[TLS/OPENSSL] SSL_CTX_new failed\n");
        socket_close(sock);
        return NULL;
    }
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        socket_close(sock);
        return NULL;
    }

#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_set1_host(ssl, host);
#endif
    SSL_set_tlsext_host_name(ssl, host);

    BIO* bio = BIO_new(openssl_get_bio_method());
    if (!bio) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        socket_close(sock);
        return NULL;
    }
    BIO_set_data(bio, sock);
    BIO_set_init(bio, 1);
    BIO_set_shutdown(bio, 0);

    SSL_set_bio(ssl, bio, bio);

    if (SSL_connect(ssl) <= 0) {
        serial_puts("[TLS/OPENSSL] SSL_connect failed\n");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        socket_close(sock);
        return NULL;
    }

    tls_session_t* session = (tls_session_t*)kmalloc(sizeof(tls_session_t));
    if (!session) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        socket_close(sock);
        return NULL;
    }
    memset(session, 0, sizeof(*session));
    session->socket = sock;
    session->ctx = ctx;
    session->ssl = ssl;

    serial_puts("[TLS/OPENSSL] connected\n");
    return session;
}

int tls_send(tls_session_t* session, const char* data, size_t len) {
    if (!session || !session->ssl || !data || len == 0) return -1;
    int r = SSL_write(session->ssl, data, (int)len);
    return (r > 0) ? r : -1;
}

int tls_recv(tls_session_t* session, char* buffer, size_t max_len) {
    if (!session || !session->ssl || !buffer || max_len == 0) return -1;
    int r = SSL_read(session->ssl, buffer, (int)max_len);
    return (r > 0) ? r : -1;
}

void tls_close(tls_session_t* session) {
    if (!session) return;
    if (session->ssl) {
        SSL_shutdown(session->ssl);
    }
    tls_session_destroy(session);
}

#endif /* COS_ENABLE_OPENSSL_BACKEND */
