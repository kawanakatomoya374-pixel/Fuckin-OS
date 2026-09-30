#ifndef GUI_UTILS_H
#define GUI_UTILS_H

#include <stddef.h>
#include <stdbool.h>

void gui_split_path(const char* full, char* parent, size_t parent_size, char* leaf, size_t leaf_size);
void gui_copy_cstr(char* dst, size_t dst_size, const char* src);
void gui_append_cstr(char* dst, size_t dst_size, const char* src);
void gui_make_unique_desktop_name(const char* base, const char* ext, char* out, size_t out_size);
int gui_parse_int_or_default(const char* s, int fallback);
void gui_format_int(int value, char* out, size_t out_size);
int gui_find_text(const char* haystack, int hay_len, const char* needle);
bool gui_replace_all_text(char* buf, int buf_cap, int* size_io, const char* needle, const char* replacement);
void gui_clamp_window_geometry(int* x, int* y, int* w, int* h);

#endif
