/*
 * cos_parserutils_filter_stub.c - passthrough replacement for
 * libparserutils/src/input/filter.c, which this build's Makefile
 * deliberately excludes (LPARSERUTILS_SRCS filters it out) because it
 * depends on <iconv.h> - real charset conversion, not available in a
 * freestanding kernel without porting a full iconv implementation
 * (conversion tables for dozens of charsets), which is well outside
 * the scope of what got this build working.
 *
 * libparserutils/src/input/inputstream.c - which IS compiled - calls
 * parserutils__filter_create("UTF-8", ...) unconditionally to build
 * every input stream (see parserutils_inputstream_create()), so
 * these 4 functions are on the critical path for parsing ANY page,
 * not an edge case: without something defining them, nothing could
 * ever be parsed at all, real network fetch or not.
 *
 * What this provides instead of real charset conversion: an honest
 * passthrough. Bytes are copied from input to output unchanged,
 * regardless of what source encoding parserutils__filter_setopt()
 * is told about. This is actually *correct*, not just a safe
 * placeholder, for the very common case where a page's declared or
 * detected charset already is UTF-8 (or plain ASCII, a UTF-8
 * subset) - which is most of the modern web. For a genuinely
 * non-UTF-8 page (legacy Shift-JIS, ISO-8859-1, etc.), this will
 * pass the raw bytes through unconverted, which typically renders as
 * mojibake rather than the correct text - a real, known limitation,
 * not a silent correctness bug: nothing here pretends a conversion
 * happened that didn't. Never crashes or hangs either way.
 *
 * parserutils__filter_process_chunk()'s contract was read directly
 * from its one real caller, parserutils_inputstream_refill_buffer()
 * (inputstream.c), to get the in/out pointer-and-length-advancing
 * semantics and the "NOMEM means 'ran out of output space, caller
 * already handles this as expected/non-fatal'" contract exactly
 * right - not guessed.
 */
#include <stdlib.h>
#include <string.h>

#include <parserutils/errors.h>
#include <parserutils/functypes.h>
#include "input/filter.h"

struct parserutils_filter {
    int unused; /* no real state needed for a passthrough */
};

parserutils_error parserutils__filter_create(const char *int_enc,
                                              parserutils_filter **filter)
{
    (void)int_enc; /* always "UTF-8" in practice - see file header
                     * comment - and irrelevant to a passthrough
                     * either way. */
    if (filter == NULL) {
        return PARSERUTILS_BADPARM;
    }
    *filter = malloc(sizeof(parserutils_filter));
    if (*filter == NULL) {
        return PARSERUTILS_NOMEM;
    }
    return PARSERUTILS_OK;
}

parserutils_error parserutils__filter_destroy(parserutils_filter *input)
{
    free(input);
    return PARSERUTILS_OK;
}

parserutils_error parserutils__filter_setopt(
        parserutils_filter *input,
        parserutils_filter_opttype type,
        parserutils_filter_optparams *params)
{
    /* Source encoding is accepted and discarded - see file header
     * comment for why (this filter never actually converts). */
    (void)input; (void)type; (void)params;
    return PARSERUTILS_OK;
}

parserutils_error parserutils__filter_process_chunk(
        parserutils_filter *input,
        const uint8_t **data, size_t *len,
        uint8_t **output, size_t *outlen)
{
    (void)input;

    size_t n = (*len < *outlen) ? *len : *outlen;
    if (n > 0) {
        memcpy(*output, *data, n);
    }
    *data += n;
    *len -= n;
    *output += n;
    *outlen -= n;

    if (*len > 0) {
        /* Ran out of output space before consuming all input - the
         * caller (parserutils_inputstream_refill_buffer) already
         * treats this as expected/non-fatal and simply calls again
         * with a bigger buffer, not an error condition. */
        return PARSERUTILS_NOMEM;
    }
    return PARSERUTILS_OK;
}

parserutils_error parserutils__filter_reset(parserutils_filter *input)
{
    (void)input; /* no state to reset in a passthrough */
    return PARSERUTILS_OK;
}
