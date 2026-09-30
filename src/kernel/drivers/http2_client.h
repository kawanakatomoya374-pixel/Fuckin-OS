#ifndef COS_HTTP2_CLIENT_H
#define COS_HTTP2_CLIENT_H

/* The HTTP/2 adapter is intentionally a narrow, synchronous transport for a
 * single idempotent request.  It is selected only after TLS ALPN confirms h2;
 * callers must retain HTTP/1.1 fallback for peers that do not negotiate it. */
#include "http.h"

/* Returns 0 after a complete, decoded h2 response is stored in http; returns
 * -1 for protocol, transport, bounds, or peer errors. */
int http2_get_response(http_client_t* http);

#endif
