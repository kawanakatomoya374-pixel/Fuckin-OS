/*
 * cos_nsoptions.c - provides the nsoptions/nsoptions_default globals
 * that utils/nsoption.h's accessor macros (nsoption_bool/int/charp/
 * colour) reach through.
 *
 * The mechanism is exactly what utils/nsoption.c does (reimplemented
 * here rather than porting that file, which is entangled with real
 * file I/O for loading user prefs and an options-UI framework that
 * doesn't exist in this build): redefine the NSOPTION_* macros to
 * emit struct initialisers, then #include desktop/options.h to
 * expand the option list. The resulting compile-time table becomes
 * the live table and the default table simultaneously (there are no
 * user prefs to load from disk).
 *
 * nsmonkey is defined because nsoption.h's enum generation includes
 * a frontend-specific options header for every known frontend name -
 * "monkey" has the simplest/emptiest extra option list and is the
 * closest to "headless driver with no windowing system", which is
 * exactly what this C-OS build is for now.
 */
/* Three compile-time constants desktop/options.h references directly
 * in its NSOPTION_* expansion - they live in utils/config.h upstream
 * when built properly with full configure output, but without that we
 * provide them as source-level defines matching upstream's built-in
 * values (see config.h.in in the netsurf source for the defaults). */
#ifndef PLOT_FONT_FAMILY_SANS_SERIF
#define PLOT_FONT_FAMILY_SANS_SERIF 1
#endif
#ifndef NETSURF_BUILTIN_LOG_FILTER
#define NETSURF_BUILTIN_LOG_FILTER "warning"
#endif
#ifndef NETSURF_BUILTIN_VERBOSE_FILTER
#define NETSURF_BUILTIN_VERBOSE_FILTER "deepdebug"
#endif

#define nsmonkey 1

#include <stdbool.h>
#include "utils/nsoption.h"

/* Redefine macros to emit struct initialisers, same as nsoption.c */
#undef NSOPTION_BOOL
#undef NSOPTION_STRING
#undef NSOPTION_INTEGER
#undef NSOPTION_UINT
#undef NSOPTION_COLOUR

#define NSOPTION_BOOL(NAME, DEFAULT) \
    { #NAME, sizeof(#NAME) - 1, OPTION_BOOL, { .b = DEFAULT } },

#define NSOPTION_STRING(NAME, DEFAULT) \
    { #NAME, sizeof(#NAME) - 1, OPTION_STRING, { .cs = DEFAULT } },

#define NSOPTION_INTEGER(NAME, DEFAULT) \
    { #NAME, sizeof(#NAME) - 1, OPTION_INTEGER, { .i = DEFAULT } },

#define NSOPTION_UINT(NAME, DEFAULT) \
    { #NAME, sizeof(#NAME) - 1, OPTION_UINT, { .u = DEFAULT } },

#define NSOPTION_COLOUR(NAME, DEFAULT) \
    { #NAME, sizeof(#NAME) - 1, OPTION_COLOUR, { .c = DEFAULT } },

static struct nsoption_s cos_nsoption_table[] = {
#include "desktop/options.h"
    { NULL, 0, OPTION_BOOL, { .b = false } } /* sentinel */
};

struct nsoption_s *nsoptions         = cos_nsoption_table;
struct nsoption_s *nsoptions_default = cos_nsoption_table;

/* C-OS ships QuickJS and its NetSurf script backend together. Enable the
 * standard option explicitly because desktop/options.h defaults to false. */
void cos_netsurf_enable_javascript(void)
{
    nsoptions[NSOPTION_enable_javascript].value.b = true;
}

/* Foreground <img> objects are part of ordinary Web content and must use
 * the standard NetSurf content pipeline.  Background-image downloads remain
 * disabled for now because a cooperative single-request transport would let
 * decorative CSS assets delay interactive page content. */
void cos_netsurf_configure_constrained_browser_profile(void)
{
    nsoptions[NSOPTION_foreground_images].value.b = true;
    nsoptions[NSOPTION_background_images].value.b = false;

    /* This compact C-OS resource bundle supplies default.css and user.css,
     * but has no resource:adblock.css.  Leaving the upstream default enabled
     * increments html_content::base.active for a sheet that can never finish,
     * which prevents html_begin_conversion() and leaves real pages blank.
     * Disable only this unavailable optional sheet. */
    nsoptions[NSOPTION_block_advertisements].value.b = false;
}
