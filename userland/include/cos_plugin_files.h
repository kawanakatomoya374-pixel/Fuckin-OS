/* cos_plugin_files.h - what a plugin of the File Manager ("files") can use. Pass this table's address to nothing: the program hands it to init as host->api. */
#ifndef COS_PLUGIN_FILES_H
#define COS_PLUGIN_FILES_H
#include <stdint.h>
#include <stddef.h>

#define FMP_ABI 1u
#define FMP_FILES 1                      /* add_action: offered for a selected file */
#define FMP_DIRS  2                      /*             ... for a selected folder */

typedef struct {
    uint32_t abi, size;
    /* A command in the right-click menu of a single selected item. fn(path, out, cap): do the work, write a message into out; the program shows it in a dialog.
     * Returns 0 on success (the folder is reloaded afterwards, so new files appear), nonzero on failure. */
    int  (*add_action)(const char *label_en, const char *label_ja, int flags, int (*fn)(const char *path, char *out, size_t cap));
    /* A viewer for files whose names end in one of exts (".bin;.dat"): fn fills the preview pane's text. Returns 0 when it handled the file. */
    int  (*add_viewer)(const char *exts, int (*fn)(const char *path, char *out, size_t cap));
    void (*status)(const char *msg);
    const char *(*cwd)(void);
    long (*read_file)(const char *path, uint64_t off, void *buf, size_t cap);          /* bytes read, -1 on error */
    int  (*write_file)(const char *path, const void *buf, size_t n);                   /* creates or truncates; 0 on success */
    int64_t (*file_size)(const char *path);                                            /* -1 when missing */
    int  (*snprintf)(char *buf, size_t cap, const char *fmt, ...);
    uint64_t (*now_ms)(void);
} fmp_api_t;
#endif
