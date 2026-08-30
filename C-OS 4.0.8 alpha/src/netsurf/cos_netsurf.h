/*
 * cos_netsurf.h - public entry points for the C-OS NetSurf frontend
 * (cos_netsurf.c, cos_fetch.c). See cos_netsurf.c's file header for
 * what this does and doesn't do yet.
 *
 * C-OS 4.0.8 alpha additions:
 *   cos_netsurf_is_ready()             - query engine state (no side effects)
 *   cos_netsurf_load_url_sync_nowait() - fire-and-forget GUI variant
 *   cos_netsurf_eval_script()          - direct JS evaluation (javascript: URLs)
 */
#ifndef COS_NETSURF_H
#define COS_NETSURF_H

#include "utils/errors.h"
#include <stdbool.h>

/* One-time engine init (idempotent - safe to call more than once).
 * cos_netsurf_load_url_sync() also calls this itself if needed, so
 * callers that just want to load a URL don't have to call this
 * first. */
nserror cos_netsurf_init(void);

/* Returns true if cos_netsurf_init() has completed successfully.
 * Safe to call from any GUI code without side effects. */
bool cos_netsurf_is_ready(void);

/* Loads a URL through the real content pipeline synchronously,
 * logging progress (including any console.log()/script output) to
 * the serial console. See the full doc comment in cos_netsurf.c. */
nserror cos_netsurf_load_url_sync(const char *url_string);

/* Fire-and-forget variant for GUI callers (e.g. gui_apps_browser.c)
 * that want to kick off the NetSurf pipeline without blocking the
 * render loop. Initialises the engine if needed. Errors are logged
 * to the serial console but not returned. */
void cos_netsurf_load_url_sync_nowait(const char *url_string);

/* Evaluates `script` through the shared QuickJS runtime and logs the
 * result (or exception) to the serial console. Intended for the
 * browser address-bar "javascript:" URL scheme and the shell "js"
 * command. Initialises the engine if needed. */
void cos_netsurf_eval_script(const char *script);

/* Implemented in cos_fetch.c: polls every registered fetcher scheme
 * once. Exposed here because cos_netsurf.c's synchronous load loop
 * drives it directly. */
void cos_fetch_poll_all(void);

/* Implemented in cos_gui_table.c: finishes wiring up the `guit`
 * global's llcache table (see that file's header comment for why
 * this is a separate step rather than a static initializer).
 * cos_netsurf_init() calls this itself. */
void cos_gui_table_init(void);

/* Real-DOM-backed page rendering for GUI callers lives in
 * cos_netsurf_render.h, NOT here - this file needs
 * netsurf/utils/errors.h (for nserror, above), which pulls in
 * NetSurf's own include paths; cos_netsurf_render.h deliberately
 * doesn't, so plain kernel/GUI code (gui_apps_browser.c) can include
 * it without needing COS_NETSURF_INCLUDES. See that file. */

#endif /* COS_NETSURF_H */
