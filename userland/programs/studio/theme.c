/* theme.c - colour tokens (design doc 8.2). Dark is the default.
 *
 * Values that the design doc leaves open (button fills, the activity bar,
 * the gutter, the panel body) were measured from cos_studio_gui_mockup.png. */
#include "studio.h"

const Theme THEME_DARK = {
    .bg = 0x1B1E26, .side = 0x161920, .bar = 0x20242E, .border = 0x292E3A,
    .act_bg = 0x12141A, .gutter = 0x181B22, .panel_bg = 0x171B22,
    .text = 0xD7DAE0, .dim = 0x7C8496, .faint = 0x4B5262,
    .accent = 0x5B8CFF, .on_accent = 0xFFFFFF,
    .curline = 0x232734, .errline = 0x2C1F26, .sel = 0x2F4A7D,
    .err = 0xF0616D, .warn = 0xE5C07B, .ok = 0x2E9E6B,
    .btn = 0x2B303D, .btn_hi = 0x363C4C, .field = 0x161920, .tip_bg = 0x2A2F3C,
    .tab_active = 0x1B1E26, .row_sel = 0x252F48, .bar2 = 0x21252F, .folder = 0xE2B96F, .thumb = 0x333949,
    .kw = 0x569CD6, .type = 0x4EC9B0, .fn = 0xDCDCAA, .str = 0xCE9178,
    .num = 0xB5CEA8, .cmt = 0x6A9955, .pp = 0xC586C0,
};

const Theme THEME_LIGHT = {
    .bg = 0xFFFFFF, .side = 0xF3F4F7, .bar = 0xE9EBF0, .border = 0xD5D8E0,
    .act_bg = 0xDDE0E8, .gutter = 0xF6F7FA, .panel_bg = 0xFAFBFC,
    .text = 0x1E222C, .dim = 0x6B7385, .faint = 0xA9AFBD,
    .accent = 0x3A6EE6, .on_accent = 0xFFFFFF,
    .curline = 0xF1F4FA, .errline = 0xFCEBEC, .sel = 0xCFE0FF,
    .err = 0xD12F3D, .warn = 0xB7791F, .ok = 0x2E9E6B,
    .btn = 0xDDE0E8, .btn_hi = 0xCED3DE, .field = 0xFFFFFF, .tip_bg = 0xFFFFFF,
    .tab_active = 0xFFFFFF, .row_sel = 0xD9E5FB, .bar2 = 0xEBEDF2, .folder = 0xB07A1F, .thumb = 0xC4C9D6,
    .kw = 0x0000FF, .type = 0x267F99, .fn = 0x795E26, .str = 0xA31515,
    .num = 0x098658, .cmt = 0x008000, .pp = 0xAF00DB,
};

const Theme *T = &THEME_DARK;

void app_apply_theme(bool dark) {
    T = dark ? &THEME_DARK : &THEME_LIGHT;
    G.dark = dark;
    G.dirty_frame = true;
}
