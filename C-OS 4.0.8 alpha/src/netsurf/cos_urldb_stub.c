/*
 * cos_urldb_stub.c - minimal implementations of the 4 urldb.h
 * functions content/content.c and content/llcache.c call.
 *
 * content/urldb.c itself (4499 lines: persistent storage for cookies,
 * HTTP auth credentials, per-site certificate exceptions, HSTS
 * policy, and general URL visit history) is not part of this build -
 * a large, low-priority piece for a driver whose only fetcher so far
 * is data: URIs, which involve none of cookies/auth/certs/HSTS at
 * all. Rather than leave content.c/llcache.c's calls into it
 * unresolved, or scatter guards through those two Tier A files for
 * calls that are individually simple, each function gets an honest
 * "nothing stored yet" answer here: no cached certificate exception,
 * no cached auth details, HSTS not known-enabled, and
 * urldb_set_hsts_policy() accepts and discards its input rather than
 * persisting it (there is nothing yet to persist it *in*). None of
 * this silently fabricates a permissive answer (e.g. "yes this
 * insecure cert is fine") - every answer here is the same "we don't
 * know/don't have one" a fresh install with an empty urldb would
 * give.
 *
 * If real cookie/auth/cert/HSTS persistence is needed later, this
 * file's job is to be replaced by a real port of content/urldb.c
 * (itself needing its own dependency triage, the way every other
 * component in PORTING_NOTES.md did) - not to grow further.
 */
#include <stddef.h>

#include "content/urldb.h"

const char *urldb_get_auth_details(struct nsurl *url, const char *realm)
{
    (void)url;
    (void)realm;
    return NULL;
}

bool urldb_get_cert_permissions(struct nsurl *url)
{
    (void)url;
    return false;
}

bool urldb_set_hsts_policy(struct nsurl *url, const char *header)
{
    (void)url;
    (void)header;
    return true;
}

bool urldb_get_hsts_enabled(struct nsurl *url)
{
    (void)url;
    return false;
}

/* Added alongside the http:/https: fetcher (cos_fetch_http.c): once
 * html_init()/nscss_init() were actually being called (see
 * cos_netsurf.c), html.o's own references pulled in content/handlers/
 * css/select.c, whose :visited selector support calls this. Same
 * "nothing stored yet" honesty as the rest of this file - visited
 * status, real title, and visit counts would need content/urldb.c
 * itself, not a fabricated answer here. NULL is the documented
 * "no data known for this URL" return, not an error. */
const struct url_data *urldb_get_url_data(struct nsurl *url)
{
    (void)url;
    return NULL;
}

/* browser_window.c records visit metadata through urldb. C-OS currently has
 * no persistent urldb store, so preserve the success/failure contracts while
 * retaining no credentials, cookies, or certificate exceptions. */
bool urldb_add_url(struct nsurl *url)
{
    return url != NULL;
}

nserror urldb_set_url_title(struct nsurl *url, const char *title)
{
    (void)title;
    return (url != NULL) ? NSERROR_OK : NSERROR_BAD_PARAMETER;
}

nserror urldb_set_url_content_type(struct nsurl *url, content_type type)
{
    (void)type;
    return (url != NULL) ? NSERROR_OK : NSERROR_BAD_PARAMETER;
}

nserror urldb_update_url_visit_data(struct nsurl *url)
{
    return (url != NULL) ? NSERROR_OK : NSERROR_BAD_PARAMETER;
}

struct nsurl *urldb_get_url(struct nsurl *url)
{
    return url;
}

void urldb_set_auth_details(struct nsurl *url, const char *realm, const char *auth)
{
    (void)url; (void)realm; (void)auth;
}

void urldb_set_cert_permissions(struct nsurl *url, bool permit)
{
    (void)url; (void)permit;
}

char *urldb_get_cookie(struct nsurl *url, bool include_http_only)
{
    (void)url; (void)include_http_only;
    return NULL;
}
