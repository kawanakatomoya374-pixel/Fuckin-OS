#ifndef MODULE_INTERFACE_H
#define MODULE_INTERFACE_H

#include "types.h"

/* Module interface standards for C-OS 4.0.8 alpha */

/* Module types */
#define MODULE_TYPE_STORAGE    1
#define MODULE_TYPE_KEYBOARD   2
#define MODULE_TYPE_GUI        3
#define MODULE_TYPE_DRIVER     4

/* Module status codes */
#define MODULE_STATUS_OK       0
#define MODULE_STATUS_ERROR    -1
#define MODULE_STATUS_BUSY    -2

/* Standard module interface */
typedef struct {
    const char* name;
    const char* version;
    uint8_t     type;
    uint8_t     status;
    
    /* Required functions */
    int (*init)(void);
    void (*cleanup)(void);
    int (*get_status)(void);
    
    /* Optional functions */
    int (*configure)(const char* config);
    const char* (*get_info)(void);
} module_interface_t;

/* Module registry */
#define MAX_MODULES 64
typedef struct {
    module_interface_t* modules[MAX_MODULES];
    int               count;
} module_registry_t;

/* Module management functions */
int  module_register(module_interface_t* module);
int  module_unregister(const char* name);
module_interface_t* module_find(const char* name);
void module_init_all(void);
void module_cleanup_all(void);

/* Error handling */
typedef struct {
    int    code;
    char    message[128];
    char    file[64];
    int    line;
} module_error_t;

void module_log_error(const module_error_t* error);
void module_log_info(const char* module, const char* message);
void module_log_debug(const char* module, const char* message);

#endif
