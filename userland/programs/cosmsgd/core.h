/* cosmsgd core - the C-OS Message relay server's protocol and state, with no operating-system calls of its own so that the
 * same code runs inside C-OS (userland/programs/cosmsgd/main.c) and on the host (tools/cosmsgd_host.sh, for tests).
 * It speaks exactly the wire format of tools/cosmsg_server.py (see the header of that file): one line per frame, fields
 * separated by TAB, "\\" "\t" "\n" escaped. Not in this stage: groups, attachments, Google sign-in (these answer with an error). */
#ifndef COSMSGD_CORE_H
#define COSMSGD_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct core_conn core_conn_t;

/* what the platform has to give the core */
uint64_t plat_unix(void);                               /* seconds since 1970 */
uint64_t plat_ms(void);                                 /* monotonic milliseconds */
void     plat_random(void *buf, size_t n);              /* cryptographically strong bytes */
void     plat_log(const char *line);
bool     plat_save(const char *data, size_t len);       /* replace the persistent state file; false on failure */
char    *plat_load(size_t *len);
bool     plat_blob_save(const char *name, const void *data, size_t len);   /* attachment files, by file id */
unsigned char *plat_blob_load(const char *name, size_t *len);               /* malloc'd, or NULL */
void     plat_blob_delete(const char *name);                        /* the state file's content (malloc'd) or NULL */

/* outbound HTTPS for Google sign-in: the platform runs the request in the background and reports with core_http_done() */
int  plat_http(bool post, const char *host, int port, const char *path, const char *body);   /* a job id, or 0 when none can be started */
void core_http_done(int job, int status, const char *body, size_t len);                      /* status 0: the request failed */
char *plat_conf_load(size_t *len);                      /* google.conf (malloc'd) or NULL */
bool core_conf_get(const char *key, char *out, size_t cap);   /* one "key=value" line of google.conf */

void core_init(void);                                   /* loads the saved state */
void core_tick(void);                                   /* call about every 100 ms: saves changed state, expires idle connections */
core_conn_t *core_open(void);                           /* a new connection (NULL when there are too many) */
void core_close(core_conn_t *c);
void core_set_peer(core_conn_t *c, uint32_t ip);       /* the peer's IPv4 address (host order), for per-address limits */
bool core_conn_closing(core_conn_t *c);                 /* the core has decided to hang up (close_after) */
uint64_t core_conn_age_ms(core_conn_t *c);
bool core_conn_loggedin(core_conn_t *c);
/* Feeds bytes received from the client. Returns false when the connection has to be closed (after sending what is queued). */
bool core_feed(core_conn_t *c, const char *data, size_t n);
/* Bytes waiting to be sent to the client. */
const char *core_out(core_conn_t *c, size_t *n);
void core_out_ack(core_conn_t *c, size_t n);
bool core_wants_close(core_conn_t *c);                  /* set when the server decided to hang up: close once the output is drained */
#endif
