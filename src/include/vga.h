/* This used to be a second, independently-maintained copy of vga.h
 * that had drifted out of sync with src/drivers/video/vga.h (missing
 * vga_draw_bmp/vga_set_pixel/vga_reserve_physical_regions, which
 * meant any file outside drivers/video/ including this header - most
 * of the GUI - couldn't see those declarations, since
 * -Isrc/include comes before -Isrc/drivers/video in the Makefile's
 * INCLUDES). It's now just a redirect, so there's exactly one
 * declaration set for vga.c to keep in sync with.
 *
 * Deliberately no #ifndef/#define guard of its own here: the
 * canonical header below already has one (VGA_H), and this file
 * defining the *same* macro name before including it would make that
 * header's own guard see VGA_H as already-defined and skip its
 * entire body - i.e. this redirect would silently expand to nothing.
 * A single #include, relying entirely on the real header's guard, is
 * also naturally idempotent if this file itself gets included more
 * than once. */
#include "../drivers/video/vga.h"


