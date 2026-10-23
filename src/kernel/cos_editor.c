/* cos_editor.c - the Text Editor (userland/programs/editor) and the Spreadsheet (userland/programs/sheet) are ring-3
 * programs. Unlike the Files app they are not single-instance: each document opens in its own process, so edits in one
 * window are never replaced by opening another file. */
#include "serial.h"
#include "cos_editor_elf.h"
#include "cos_sheet_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);

void cos_editor_open(const char *path) {
    bool has = path && path[0];
    const char *argv[2] = { "Editor", has ? path : NULL };
    int64_t pid = cos_launch_elf_image_args("Editor.c-os", cos_editor_elf, cos_editor_elf_len, argv, has ? 2 : 1, NULL, 0);
    serial_puts(pid > 0 ? "[EDITOR] launched\n" : "[EDITOR] launch FAILED\n");
}

/* The Spreadsheet follows the same one-process-per-document rule. */
void cos_sheet_open(const char *path) {
    bool has = path && path[0];
    const char *argv[2] = { "Sheet", has ? path : NULL };
    int64_t pid = cos_launch_elf_image_args("Sheet.c-os", cos_sheet_elf, cos_sheet_elf_len, argv, has ? 2 : 1, NULL, 0);
    serial_puts(pid > 0 ? "[SHEET] launched\n" : "[SHEET] launch FAILED\n");
}
