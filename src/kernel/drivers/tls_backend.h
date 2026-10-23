/**
 * tls_backend.h - TLS backend abstraction for HTTPS support
 *
 * This OS source tree is now wired for HTTPS-capable browser integration.
 * A concrete TLS implementation (for example BearSSL or mbedTLS) should
 * provide these functions on top of the socket layer.
 */

#ifndef TLS_BACKEND_H
#define TLS_BACKEND_H

#include "types.h"

typedef struct tls_session tls_session_t;

int tls_backend_available(void);
tls_session_t* tls_connect(const char* host, uint64_t port);
/* Proposes h2 first and HTTP/1.1 second. Callers must inspect the selected
 * protocol before writing application bytes. */
tls_session_t* tls_connect_prefer_http2(const char* host, uint64_t port);
/* ALPN selected by the peer, or NULL when a legacy peer did not negotiate it.
 * Existing HTTP/1.1 callers keep using tls_connect(); future HTTP/2 code must
 * only send an h2 preface after this function reports "h2". */
const char* tls_get_selected_protocol(tls_session_t* session);
int tls_send(tls_session_t* session, const char* data, size_t len);
int tls_recv(tls_session_t* session, char* buffer, size_t max_len);
void tls_close(tls_session_t* session);

#endif /* TLS_BACKEND_H */
