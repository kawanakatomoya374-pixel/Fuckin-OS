/*
 * cos_fetch.c - native implementation of the fetcher registry/dispatch
 * contract declared in content/fetch.h and content/fetchers.h.
 *
 * Upstream's own implementation of this contract is content/fetch.c,
 * which isn't part of this build (see PORTING_NOTES.md): it's tied
 * to utils/nsoption.c (proxy settings etc. - not ported, needs
 * desktop/options.h) and desktop/gui_internal.h (a whole GUI
 * abstraction this kernel doesn't have), and drives its poll loop
 * through a frontend's `guit->misc->schedule()` timer callback. None
 * of that exists here yet, so rather than untangle an 825-line file
 * from all of it, this is a from-scratch implementation of the same
 * public contract: a small fixed-size scheme registry, a `struct
 * fetch` (upstream leaves this opaque - forward-declared in
 * content/fetch.h, defined however the implementation likes), and
 * synchronous dispatch. "Synchronous" is the key simplification: real
 * NetSurf frontends integrate fetcher polling into an event loop so
 * fetches make progress in the background while everything else
 * keeps running; this one instead expects its caller (see
 * cos_netsurf.c) to drive fetch_poll_all() in a tight loop until a
 * fetch completes, which is exactly right for a synchronous "load
 * this URL and wait" driver (and is genuinely all that's needed for
 * data: URIs, which resolve in a single poll - see fetchers/data.c).
 * A real network fetcher (backed by kernel/api/net_api.c's
 * http_fetch) integrating with a real event loop is future work, once
 * there's an event loop to integrate with (PORTING_NOTES.md item 1).
 */
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#include "cos_netsurf.h"
#include "memory.h"
#include "serial.h"

#define COS_MAX_FETCHER_SCHEMES 8

struct cos_fetcher_entry {
    lwc_string *scheme;
    struct fetcher_operation_table ops;
    bool in_use;
};

static struct cos_fetcher_entry g_fetchers[COS_MAX_FETCHER_SCHEMES];
static int g_fetcher_count = 0;

/* The real definition of the `struct fetch` upstream's content/fetch.h
 * forward-declares and otherwise treats as opaque. One of these
 * exists per in-flight fetch, created by fetch_start() and torn down
 * by fetch_free(). */
struct fetch {
    struct cos_fetcher_entry *entry;
    void *fetcher_handle;      /* from entry->ops.setup() */
    fetch_callback callback;
    void *callback_pw;
    long http_code;
    bool aborted;
};

static struct cos_fetcher_entry *cos_find_fetcher(lwc_string *scheme)
{
    for (int i = 0; i < g_fetcher_count; i++) {
        if (!g_fetchers[i].in_use) continue;
        bool same = false;
        if (lwc_string_isequal(g_fetchers[i].scheme, scheme, &same) == lwc_error_ok && same) {
            return &g_fetchers[i];
        }
    }
    return NULL;
}

/* exported interface documented in content/fetchers.h */
nserror fetcher_add(lwc_string *scheme, const struct fetcher_operation_table *ops)
{
    if (scheme == NULL || ops == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    if (g_fetcher_count >= COS_MAX_FETCHER_SCHEMES) {
        return NSERROR_NOSPACE;
    }
    if (cos_find_fetcher(scheme) != NULL) {
        /* Already registered - not an error, matches upstream's
         * "adding twice is harmless" tolerance. */
        return NSERROR_OK;
    }

    struct cos_fetcher_entry *e = &g_fetchers[g_fetcher_count];
    e->scheme = lwc_string_ref(scheme);
    e->ops = *ops;
    e->in_use = true;
    g_fetcher_count++;

    if (e->ops.initialise != NULL && !e->ops.initialise(scheme)) {
        e->in_use = false;
        lwc_string_unref(e->scheme);
        g_fetcher_count--;
        return NSERROR_INIT_FAILED;
    }

    return NSERROR_OK;
}

/* exported interface documented in content/fetchers.h */
nserror fetcher_init(void)
{
    /* Nothing to do beyond zeroing the registry, which static storage
     * already guarantees - each individual fetcher's own initialise()
     * runs from fetcher_add() above, at the point it's registered
     * (see cos_netsurf.c). */
    return NSERROR_OK;
}

/* exported interface documented in content/fetchers.h */
void fetcher_quit(void)
{
    for (int i = 0; i < g_fetcher_count; i++) {
        if (!g_fetchers[i].in_use) continue;
        if (g_fetchers[i].ops.finalise != NULL) {
            g_fetchers[i].ops.finalise(g_fetchers[i].scheme);
        }
        lwc_string_unref(g_fetchers[i].scheme);
        g_fetchers[i].in_use = false;
    }
    g_fetcher_count = 0;
}

/* exported interface documented in content/fetch.h */
bool fetch_can_fetch(const nsurl *url)
{
    lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
    if (scheme == NULL) return false;
    struct cos_fetcher_entry *e = cos_find_fetcher(scheme);
    lwc_string_unref(scheme);
    if (e == NULL) return false;
    if (e->ops.acceptable != NULL) {
        return e->ops.acceptable(url);
    }
    return true;
}

/* exported interface documented in content/fetch.h */
nserror fetch_start(nsurl *url, nsurl *referer, fetch_callback callback,
                     void *p, bool only_2xx, const char *post_urlenc,
                     const struct fetch_multipart_data *post_multipart,
                     bool verifiable, bool downgrade_tls,
                     const char *headers[], struct fetch **fetch_out)
{
    (void)referer;
    (void)verifiable;
    /* Upstream fetchers may iterate headers until a NULL terminator.  The
     * C-OS caller legitimately supplies NULL for an empty header set, so
     * adapt it to a real zero-length, terminated array before dispatch. */
    static const char *empty_headers[] = { NULL };
    const char **request_headers = (headers != NULL) ? headers : empty_headers;

    if (url == NULL || callback == NULL || fetch_out == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
    if (scheme == NULL) {
        return NSERROR_BAD_URL;
    }
    struct cos_fetcher_entry *e = cos_find_fetcher(scheme);
    lwc_string_unref(scheme);
    if (e == NULL) {
        return NSERROR_NO_FETCH_HANDLER;
    }

    struct fetch *f = (struct fetch *)kmalloc(sizeof(struct fetch));
    if (f == NULL) {
        return NSERROR_NOMEM;
    }
    f->entry = e;
    f->callback = callback;
    f->callback_pw = p;
    f->http_code = 0;
    f->aborted = false;

    f->fetcher_handle = e->ops.setup(f, url, only_2xx, downgrade_tls,
                                      post_urlenc, post_multipart, request_headers);
    if (f->fetcher_handle == NULL) {
        kfree(f);
        return NSERROR_INVALID;
    }

    if (!e->ops.start(f->fetcher_handle)) {
        if (e->ops.free != NULL) e->ops.free(f->fetcher_handle);
        kfree(f);
        return NSERROR_INVALID;
    }

    *fetch_out = f;
    return NSERROR_OK;
}

/* exported interface documented in content/fetch.h */
void fetch_abort(struct fetch *f)
{
    if (f == NULL || f->aborted) return;
    f->aborted = true;
    if (f->entry->ops.abort != NULL) {
        f->entry->ops.abort(f->fetcher_handle);
    }
}

/* exported interface documented in content/fetch.h */
void fetch_change_callback(struct fetch *fetch, fetch_callback callback, void *p)
{
    if (fetch == NULL) return;
    fetch->callback = callback;
    fetch->callback_pw = p;
}

/* exported interface documented in content/fetch.h */
long fetch_http_code(struct fetch *fetch)
{
    return fetch != NULL ? fetch->http_code : 0;
}

/* exported interface documented in content/fetch.h */
void fetch_set_http_code(struct fetch *fetch, long http_code)
{
    if (fetch != NULL) fetch->http_code = http_code;
}

/* exported interface documented in content/fetch.h */
void fetch_set_cookie(struct fetch *fetch, const char *data)
{
    /* No cookie jar yet - nothing in this build sends or persists
     * cookies (see PORTING_NOTES.md; this is a small, well-contained
     * future addition, not a structural gap). Accepting and
     * discarding is correct behaviour for a fetcher that doesn't
     * support cookies, same as if none had been set. */
    (void)fetch;
    (void)data;
}

/* exported interface documented in content/fetch.h */
void fetch_send_callback(const fetch_msg *msg, struct fetch *fetch)
{
    if (fetch == NULL || fetch->callback == NULL) return;
    fetch->callback(msg, fetch->callback_pw);
}

/* exported interface documented in content/fetch.h */
void fetch_remove_from_queues(struct fetch *fetch)
{
    /* Every fetcher here (currently just data:) resolves synchronously
     * within a single poll rather than sitting in a real queue across
     * multiple polls, so there's no separate queue structure to pull
     * `fetch` out of - this is a deliberate no-op, not a missing
     * implementation. */
    (void)fetch;
}

/* exported interface documented in content/fetch.h */
void fetch_free(struct fetch *f)
{
    if (f == NULL) return;
    fetch_remove_from_queues(f);
    if (f->entry->ops.free != NULL) {
        f->entry->ops.free(f->fetcher_handle);
    }
    kfree(f);
}

/* Not part of upstream's contract (upstream drives this through a
 * frontend's scheduled-timer callback via guit->misc->schedule() -
 * see this file's header comment) - this is what cos_netsurf.c's
 * synchronous page-load driver calls in a loop instead. Polls every
 * registered fetcher scheme once. */
void cos_fetch_poll_all(void)
{
    for (int i = 0; i < g_fetcher_count; i++) {
        if (!g_fetchers[i].in_use) continue;
        if (g_fetchers[i].ops.poll != NULL) {
            g_fetchers[i].ops.poll(g_fetchers[i].scheme);
        }
    }
}

/* exported interface documented in content/fetch.h - a plain linked
 * list of {name, value, rawfile, file} nodes, so clone/destroy are
 * genuinely simple to implement correctly rather than needing a stub. */
void fetch_multipart_data_destroy(struct fetch_multipart_data *list)
{
    while (list != NULL) {
        struct fetch_multipart_data *next = list->next;
        kfree(list->name);
        kfree(list->value);
        kfree(list->rawfile);
        kfree(list);
        list = next;
    }
}

struct fetch_multipart_data *fetch_multipart_data_clone(const struct fetch_multipart_data *list)
{
    struct fetch_multipart_data *head = NULL;
    struct fetch_multipart_data **tail = &head;

    while (list != NULL) {
        struct fetch_multipart_data *copy =
            (struct fetch_multipart_data *)kmalloc(sizeof(*copy));
        if (copy == NULL) {
            fetch_multipart_data_destroy(head);
            return NULL;
        }
        copy->next = NULL;
        copy->name = list->name ? strdup(list->name) : NULL;
        copy->value = list->value ? strdup(list->value) : NULL;
        copy->rawfile = list->rawfile ? strdup(list->rawfile) : NULL;
        copy->file = list->file;

        *tail = copy;
        tail = &copy->next;
        list = list->next;
    }

    return head;
}
