/**
 * efm_tree.c - the file manager's hierarchical directory tree sidebar.
 *
 * Replaces a flat list of fixed shortcuts (Root/Desktop/Documents/...)
 * that could not express nesting at all, with a real expandable tree that
 * shows where the current directory actually sits in the filesystem -
 * the structure a modern file manager's navigation pane has.
 *
 * The tree is rebuilt from disk on every refresh (directories change),
 * with expansion state held separately in efm_state_t::expanded so the
 * user's expanded folders survive that rebuild.
 */
#include "enhanced_file_manager.h"
#include "efm_internal.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "gui.h"

extern void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);
extern void vga_fill_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern bool gui_is_japanese(void);

/* ---- expansion state ---------------------------------------------------- */

bool efm_tree_is_expanded(efm_state_t* state, const char* path) {
    if (!state || !path) return false;
    for (int i = 0; i < state->expanded_count; ++i) {
        if (strcmp(state->expanded[i], path) == 0) return true;
    }
    return false;
}

static void efm_tree_set_expanded(efm_state_t* state, const char* path, bool on) {
    if (!state || !path) return;
    for (int i = 0; i < state->expanded_count; ++i) {
        if (strcmp(state->expanded[i], path) != 0) continue;
        if (!on) {
            /* Remove by moving the last entry into this slot - order in
             * this list carries no meaning (it is only ever searched by
             * strcmp), so an O(1) swap-remove is correct and avoids
             * shifting the whole array. */
            state->expanded[i][0] = '\0';
            strncpy(state->expanded[i], state->expanded[state->expanded_count - 1],
                    EFM_MAX_PATH - 1);
            state->expanded[i][EFM_MAX_PATH - 1] = '\0';
            state->expanded_count--;
        }
        return;   /* already recorded; nothing to add */
    }
    if (on && state->expanded_count < EFM_MAX_TREE_EXPANDED) {
        strncpy(state->expanded[state->expanded_count], path, EFM_MAX_PATH - 1);
        state->expanded[state->expanded_count][EFM_MAX_PATH - 1] = '\0';
        state->expanded_count++;
    }
}

void efm_tree_toggle(efm_state_t* state, const char* path) {
    if (!state || !path) return;
    efm_tree_set_expanded(state, path, !efm_tree_is_expanded(state, path));
    efm_tree_rebuild(state);
}

/* ---- building ------------------------------------------------------------ */

/* Joins a parent directory and a child name into `out`, avoiding the
 * "//child" that naive concatenation produces when the parent is "/". */
static void tree_join(char* out, size_t out_sz, const char* dir, const char* name) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    size_t dl = strlen(dir);
    if (dl == 1 && dir[0] == '/') {
        snprintf(out, out_sz, "/%s", name);
    } else {
        snprintf(out, out_sz, "%s/%s", dir, name);
    }
}

/* True if `path` contains at least one subdirectory - used only to decide
 * whether to draw a disclosure triangle, so an empty folder does not get
 * a control that would do nothing when clicked. */
static bool tree_has_subdir(const char* path) {
    fs_entry_t* entries = fs_list_dir(path);
    int count = fs_entry_count();
    if (!entries) return false;
    for (int i = 0; i < count; ++i) {
        if (!entries[i].is_dir) continue;
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        return true;
    }
    return false;
}

static void tree_add_dir(efm_state_t* state, const char* path, const char* name, int depth) {
    if (state->tree_count >= EFM_MAX_TREE_NODES) return;

    efm_tree_node_t* n = &state->tree[state->tree_count++];
    strncpy(n->path, path, EFM_MAX_PATH - 1);
    n->path[EFM_MAX_PATH - 1] = '\0';
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    n->depth = depth;
    n->has_children = tree_has_subdir(path);
    n->is_expanded = n->has_children && efm_tree_is_expanded(state, path);

    if (!n->is_expanded) return;

    /* Recurse into children. fs_list_dir() returns a pointer to a SHARED
     * static buffer that the next call overwrites, so the child names must
     * be copied out BEFORE recursing - reading entries[i].name after a
     * nested call would read whichever directory that call listed instead.
     * This is the same shared-buffer hazard fs_read_file_at() has, and it
     * is why the names are snapshotted into a local array here rather than
     * iterated in place. */
    fs_entry_t* entries = fs_list_dir(path);
    int count = fs_entry_count();
    if (!entries || count <= 0) return;

    #define TREE_MAX_CHILDREN 64
    char child_names[TREE_MAX_CHILDREN][128];
    int  child_count = 0;
    for (int i = 0; i < count && child_count < TREE_MAX_CHILDREN; ++i) {
        if (!entries[i].is_dir) continue;
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        strncpy(child_names[child_count], entries[i].name, 127);
        child_names[child_count][127] = '\0';
        child_count++;
    }

    for (int i = 0; i < child_count; ++i) {
        char child_path[EFM_MAX_PATH];
        tree_join(child_path, sizeof(child_path), path, child_names[i]);
        tree_add_dir(state, child_path, child_names[i], depth + 1);
    }
}

void efm_tree_rebuild(efm_state_t* state) {
    if (!state) return;
    state->tree_count = 0;
    /* Root is always present and always expanded - collapsing it would
     * leave the sidebar empty with no way to get it back. */
    efm_tree_set_expanded(state, "/", true);
    tree_add_dir(state, "/", gui_is_japanese() ? "ルート" : "Root", 0);
}

/* ---- rendering ------------------------------------------------------------ */

/* Disclosure triangle: right-pointing when collapsed, down when expanded -
 * the standard affordance, drawn with filled rects rather than a font
 * glyph so it renders identically regardless of the active font. */
static void tree_draw_arrow(int x, int y, bool expanded, uint64_t color) {
    if (expanded) {
        for (int r = 0; r < 4; ++r) {
            vga_fill_rect(x + r, y + 2 + r, 8 - 2 * r, 1, color);
        }
    } else {
        for (int r = 0; r < 4; ++r) {
            vga_fill_rect(x + 2 + r, y + r, 1, 8 - 2 * r, color);
        }
    }
}

void efm_draw_tree(efm_state_t* state, int x, int y, int w, int h) {
    if (!state) return;

    vga_fill_rect(x, y, w, h, EFM_C_SIDEBAR);
    vga_fill_rect(x + w - 1, y, 1, h, EFM_C_BORDER);

    vga_fill_rect(x, y, w, 28, EFM_C_TOOLBAR);
    vga_draw_string(x + 8, y + 8,
                    gui_is_japanese() ? "フォルダ" : "Folders",
                    EFM_C_MUTED, 0xFFFFFFFF);

    if (state->tree_count == 0) efm_tree_rebuild(state);

    int iy = y + 32;
    for (int i = 0; i < state->tree_count; ++i) {
        if (iy + EFM_TREE_ROW_H > y + h - 2) break;   /* clip to the pane */
        efm_tree_node_t* n = &state->tree[i];

        bool is_current = (strcmp(n->path, state->current_path) == 0);
        if (is_current) {
            vga_fill_rounded_rect(x + 2, iy, w - 6, EFM_TREE_ROW_H, 4, EFM_C_SELECT);
        }

        int indent = n->depth * EFM_TREE_INDENT;
        int ax = x + 6 + indent;

        if (n->has_children) {
            tree_draw_arrow(ax, iy + 7, n->is_expanded, EFM_C_MUTED);
        }

        efm_draw_file_icon_pub(ax + 12, iy + 3, 14, EFM_TYPE_FOLDER, true);

        /* Truncate to the room actually left after the indent, so a deeply
         * nested name is cut rather than overflowing into the file list -
         * the same class of overflow that made the column headers bleed
         * into this very sidebar before it was fixed. */
        int text_x = ax + 30;
        int avail = (x + w - 6) - text_x;
        if (avail > 0) {
            char shown[64];
            int max_chars = avail / FONT_W;
            if (max_chars > 63) max_chars = 63;
            if (max_chars > 0) {
                efm_utf8_truncate(n->name, shown, sizeof(shown), max_chars);
                vga_draw_string(text_x, iy + 5, shown,
                                is_current ? EFM_C_ACCENT : EFM_C_TEXT, 0xFFFFFFFF);
            }
        }
        iy += EFM_TREE_ROW_H;
    }
}

/* Maps a click in the sidebar to a tree row. Returns the node index, or
 * -1 if the click was on the header or past the last row. `hit_arrow`
 * reports whether the click landed on the disclosure triangle (toggle)
 * rather than the name (navigate) - two different actions on one row,
 * which is why the caller needs both pieces of information. */
int efm_tree_hit_test(efm_state_t* state, int x, int y, int tree_x, int tree_y,
                      bool* hit_arrow) {
    if (hit_arrow) *hit_arrow = false;
    if (!state) return -1;

    int rel_y = y - (tree_y + 32);
    if (rel_y < 0) return -1;
    int idx = rel_y / EFM_TREE_ROW_H;
    if (idx < 0 || idx >= state->tree_count) return -1;

    efm_tree_node_t* n = &state->tree[idx];
    int ax = tree_x + 6 + n->depth * EFM_TREE_INDENT;
    if (hit_arrow && n->has_children && x >= ax && x < ax + 12) {
        *hit_arrow = true;
    }
    return idx;
}
