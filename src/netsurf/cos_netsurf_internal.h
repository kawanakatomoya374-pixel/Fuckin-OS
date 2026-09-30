/*
 * cos_netsurf_internal.h - declarations shared between cos_netsurf.c
 * and cos_netsurf_render.c ONLY.
 *
 * GUI code (gui_apps_browser.c) should include cos_netsurf.h instead,
 * which exposes cos_netsurf_render_page() using only plain C types.
 * This file exists purely so that header stays free of NetSurf's own
 * headers (content/hlcache.h and everything it drags in) - nothing
 * outside the src/netsurf/ module has any business seeing a raw
 * hlcache_handle.
 */
#ifndef COS_NETSURF_INTERNAL_H
#define COS_NETSURF_INTERNAL_H

#include <stddef.h>
#include "utils/errors.h"
#include "content/hlcache.h"

/* Implemented in cos_netsurf.c. Same synchronous load as
 * cos_netsurf_load_url_sync(), but leaves the resulting hlcache_handle
 * open (via `*out_handle`) for the caller to inspect - e.g. to walk
 * its real DOM tree - instead of releasing it immediately. See the
 * full doc comment in cos_netsurf.c for the exact ownership contract. */
nserror cos_netsurf_load_url_sync_for_render(const char *url_string,
                                              hlcache_handle **out_handle,
                                              char *out_error,
                                              size_t error_sz);

/* Implemented in cos_fetch_http.c. Registers the real http:/https:
 * fetcher - see that file's header comment. */
nserror fetch_http_register(void);


/* Runs callbacks queued through guit->misc->schedule().  The synchronous
 * C-OS NetSurf driver calls this after fetch polling so HTML/CSS conversion
 * steps scheduled with a zero delay can reach CONTENT_MSG_DONE. */
void cos_netsurf_schedule_pump(void);

#endif /* COS_NETSURF_INTERNAL_H */
