/* cos_plugin_py2c.h - what a plugin of Py2C Studio ("py2c") can use: toolbar buttons, filters before and after the conversion, access to the texts. */
#ifndef COS_PLUGIN_PY2C_H
#define COS_PLUGIN_PY2C_H
#include "cos_plugin_base.h"

#define PY2CP_ABI 1u

typedef struct {
    cos_pbase_t base;
    uint32_t abi, size;
    int  (*add_button)(const char *label, void (*fn)(void));                     /* a button in the toolbar */
    /* filters get the text and its capacity; edit it in place and return nonzero when it changed */
    int  (*add_source_filter)(int (*fn)(char *py, size_t cap));                  /* Python, before it is converted */
    int  (*add_output_filter)(int (*fn)(char *c, size_t cap));                   /* the generated C, after the conversion */
    const char *(*source)(void);                                                 /* the Python text */
    size_t (*source_len)(void);
    void (*set_source)(const char *text, size_t n);
    void (*insert)(const char *text);                                            /* at the cursor */
    const char *(*output)(void);                                                 /* the generated C ("" before the first conversion) */
    const char *(*path)(void);                                                   /* the Python file's path */
    void (*convert)(void);                                                       /* same as the Convert button */
} py2c_api_t;
#endif
