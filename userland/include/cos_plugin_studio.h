/* cos_plugin_studio.h - what a plugin of C-OS Studio ("studio") can use: commands in the Tools menu that work on the open document. */
#ifndef COS_PLUGIN_STUDIO_H
#define COS_PLUGIN_STUDIO_H
#include "cos_plugin_base.h"

#define STUDIOP_ABI 1u

typedef struct {
    cos_pbase_t base;
    uint32_t abi, size;
    /* A command in the Tools menu (below Settings...). fn runs when it is chosen. */
    int      (*add_command)(const char *label, void (*fn)(void));
    /* the open document; offsets are bytes. Edits are undoable (Ctrl+Z). */
    int      (*has_doc)(void);
    const char *(*doc_path)(void);                                              /* "" for an untitled document */
    size_t   (*doc_length)(void);
    size_t   (*doc_read)(size_t pos, char *out, size_t n);                      /* copies up to n bytes, returns how many */
    void     (*doc_replace)(size_t pos, size_t del, const char *ins, size_t n); /* one undo step */
    size_t   (*caret)(void);
    size_t   (*sel_start)(void);                                                /* == sel_end() when nothing is selected */
    size_t   (*sel_end)(void);
    void     (*set_selection)(size_t from, size_t to);
    size_t   (*line_count)(void);
    size_t   (*line_start)(size_t line);                                        /* 0-based line -> offset */
    size_t   (*line_of)(size_t pos);
    void     (*output)(const char *line);                                       /* a line in the Output panel (opens it) */
    int      (*open_file)(const char *path, int line);
    const char *(*project_root)(void);                                          /* "" when no project is open */
} studio_api_t;
#endif
