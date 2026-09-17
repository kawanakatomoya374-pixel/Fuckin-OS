/* Host-test stub: only the parts of NetSurf's plot_style.h that
 * cos_ns_font.c actually uses, with the same values as upstream. */
#ifndef STUB_PLOT_STYLE_H
#define STUB_PLOT_STYLE_H
#include <stdint.h>

#define PLOT_STYLE_RADIX (10)
#define PLOT_STYLE_SCALE (1 << PLOT_STYLE_RADIX)
#define plot_style_int_to_fixed(v) ((v) << PLOT_STYLE_RADIX)
#define plot_style_fixed_to_int(v) ((v) >> PLOT_STYLE_RADIX)

typedef int plot_style_fixed;
typedef uint32_t colour;

typedef enum {
    PLOT_FONT_FAMILY_SANS_SERIF = 0,
    PLOT_FONT_FAMILY_SERIF,
    PLOT_FONT_FAMILY_MONOSPACE,
    PLOT_FONT_FAMILY_CURSIVE,
    PLOT_FONT_FAMILY_FANTASY,
    PLOT_FONT_FAMILY_COUNT
} plot_font_generic_family_t;

typedef enum {
    FONTF_NONE = 0,
    FONTF_ITALIC = 1,
    FONTF_OBLIQUE = 2,
    FONTF_SMALLCAPS = 4
} plot_font_flags_t;

struct lwc_string_s;
typedef struct plot_font_style {
    struct lwc_string_s * const * families;
    plot_font_generic_family_t family;
    plot_style_fixed size;
    int weight;
    plot_font_flags_t flags;
    colour background;
    colour foreground;
} plot_font_style_t;
#endif
