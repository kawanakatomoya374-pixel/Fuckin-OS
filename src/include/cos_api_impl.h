#ifndef COS_API_IMPL_H
#define COS_API_IMPL_H

#include "cos_api.h"
#include "serial.h"
#include "keyboard.h"
#include <string.h>

/* Forward declarations for missing functions */
const void* unified_storage_get_device(int device_id);
int unified_storage_switch_device(int device_id);
int unified_storage_read(int sector, void* buffer);
int unified_storage_write(int sector, const void* buffer);
int unified_storage_get_device_count(void);

bool keyboard_has_event(void);
keyboard_event_t keyboard_get_event(void);
void gui_init(void);
bool gui_is_initialized(void);
void keyboard_poll(void);

/* Module structures */
struct {
    int (*init)(void);
    int (*cleanup)(void);
} unified_storage_module;

struct {
    int (*init)(void);
    int (*cleanup)(void);
    int (*configure)(int mode);
} unified_keyboard_module;

int unified_keyboard_module_init(void);
int unified_keyboard_module_cleanup(void);
int unified_keyboard_configure(int mode);
int gui_add_desktop_icon(const char* path, const char* name, int is_file);
void gui_sync_desktop_with_fs(void);
void fs_init(void);
bool fs_create_file_at(const char* path, const char* name);
bool fs_create_dir_at(const char* path, const char* name);

/* ============================================================
 * API IMPLEMENTATION HELPERS
 * ============================================================ */

/* Error logging macros */
#define API_LOG_ERROR(module, func, code) \
    cos_api_log_error(module, func, code)

#define API_LOG_INFO(module, msg) \
    cos_api_log_info(module, msg)

#define API_LOG_DEBUG(module, msg) \
    cos_api_log_debug(module, msg)

/* Parameter checking macros */
#define API_CHECK_INITIALIZED(initialized) \
    if (!(initialized)) { \
        API_LOG_ERROR("API", __func__, COS_API_NOT_INITIALIZED); \
        return COS_API_NOT_INITIALIZED; \
    }

/* Parameter validation macros */
#define API_CHECK_PARAM(param)     if (!(param)) {         API_LOG_ERROR("API", __func__, COS_API_INVALID_ARG);         return COS_API_INVALID_ARG;     }

static inline bool api_range_invalid_u64(uint64_t value, uint64_t min, uint64_t max) {
    if (value > max) return true;
    if (min != 0 && value < min) return true;
    return false;
}

#define API_CHECK_RANGE(value, min, max)     if (api_range_invalid_u64((uint64_t)(value), (uint64_t)(min), (uint64_t)(max))) {         API_LOG_ERROR("API", __func__, COS_API_INVALID_ARG);         return COS_API_INVALID_ARG;     }

/* Memory management helpers */
#define API_ALLOC(size) cos_mem_alloc(size, COS_MEM_TYPE_USER)
#define API_FREE(ptr) cos_mem_free(ptr)

/* String helpers */
#define API_STR_COPY(dest, src, max_len) \
    do { \
        strncpy(dest, src, max_len - 1); \
        dest[max_len - 1] = '\0'; \
    } while(0)

#define API_STR_EQUAL(a, b) (strcmp(a, b) == 0)

/* ============================================================
 * STORAGE API IMPLEMENTATION
 * ============================================================ */

/* Storage device registry */
static struct {
    int initialized;
    int current_device;
    int device_count;
} storage_state = {0, 0, 0};

/* Storage API implementation */
int cos_storage_init(void) {
    API_LOG_INFO("STORAGE_API", "Initializing storage API");

    if (!unified_storage_module.init || !unified_storage_module.cleanup) {
        API_LOG_ERROR("STORAGE_API", "module table", COS_API_ERROR);
        return COS_API_ERROR;
    }

    /* Initialize unified storage system */
    if (unified_storage_module.init() != MODULE_STATUS_OK) {
        API_LOG_ERROR("STORAGE_API", "init", COS_API_ERROR);
        return COS_API_ERROR;
    }

    storage_state.initialized = 1;
    storage_state.current_device = 0;
    storage_state.device_count = unified_storage_get_device_count();
    if (storage_state.device_count < 0) {
        API_LOG_ERROR("STORAGE_API", "device_count", COS_API_ERROR);
        unified_storage_module.cleanup();
        storage_state.initialized = 0;
        return COS_API_ERROR;
    }

    API_LOG_INFO("STORAGE_API", "Storage API initialized successfully");
    return COS_API_OK;
}

int cos_storage_cleanup(void) {
    API_LOG_INFO("STORAGE_API", "Cleaning up storage API");

    if (storage_state.initialized) {
        if (unified_storage_module.cleanup) {
            unified_storage_module.cleanup();
        }
        storage_state.initialized = 0;
    }

    return COS_API_OK;
}

int cos_storage_get_device_count(void) {
    API_CHECK_INITIALIZED(storage_state.initialized);
    return storage_state.device_count;
}

int cos_storage_get_device_info(int device_id, cos_storage_file_info_t* info) {
    API_CHECK_INITIALIZED(storage_state.initialized);
    API_CHECK_PARAM(info);
    API_CHECK_RANGE(device_id, 0, storage_state.device_count - 1);
    
    const void* dev = unified_storage_get_device(device_id);
    if (!dev) {
        return COS_API_NOT_FOUND;
    }
    
    API_STR_COPY(info->name, "Storage Device", sizeof(info->name));
    info->size = 1024 * 1024 * 1024;  /* 1GB default */
    info->created_time = cos_system_get_time();
    info->modified_time = info->created_time;
    info->attributes = 0;  /* Read-write by default */
    
    return COS_API_OK;
}

int cos_storage_set_device(int device_id) {
    API_CHECK_INITIALIZED(storage_state.initialized);
    API_CHECK_RANGE(device_id, 0, storage_state.device_count - 1);
    
    if (unified_storage_switch_device(device_id) != MODULE_STATUS_OK) {
        return COS_API_ERROR;
    }
    
    storage_state.current_device = device_id;
    API_LOG_INFO("STORAGE_API", "Device switched successfully");
    return COS_API_OK;
}

int cos_storage_read(const cos_storage_read_t* request) {
    API_CHECK_INITIALIZED(storage_state.initialized);
    API_CHECK_PARAM(request);
    API_CHECK_PARAM(request->buffer);
    
    if (unified_storage_read(request->sector, request->buffer) != 0) {
        return COS_API_ERROR;
    }
    
    return COS_API_OK;
}

int cos_storage_write(const cos_storage_write_t* request) {
    API_CHECK_INITIALIZED(storage_state.initialized);
    API_CHECK_PARAM(request);
    API_CHECK_PARAM(request->buffer);
    
    if (unified_storage_write(request->sector, request->buffer) != 0) {
        return COS_API_ERROR;
    }
    
    return COS_API_OK;
}

/* ============================================================
 * KEYBOARD API IMPLEMENTATION
 * ============================================================ */

/* Keyboard state */
static struct {
    int initialized;
    int current_mode;
    int event_queue_size;
} keyboard_state = {0, 0, 0};

/* Keyboard API implementation */
int cos_keyboard_init(void) {
    API_LOG_INFO("KEYBOARD_API", "Initializing keyboard API");

    if (!unified_keyboard_module.init || !unified_keyboard_module.cleanup) {
        API_LOG_ERROR("KEYBOARD_API", "module table", COS_API_ERROR);
        return COS_API_ERROR;
    }

    if (unified_keyboard_module.init() != MODULE_STATUS_OK) {
        API_LOG_ERROR("KEYBOARD_API", "init", COS_API_ERROR);
        return COS_API_ERROR;
    }

    keyboard_state.initialized = 1;
    keyboard_state.current_mode = COS_KB_MODE_NORMAL;
    keyboard_state.event_queue_size = 0;

    API_LOG_INFO("KEYBOARD_API", "Keyboard API initialized successfully");
    return COS_API_OK;
}

int cos_keyboard_cleanup(void) {
    API_LOG_INFO("KEYBOARD_API", "Cleaning up keyboard API");

    if (keyboard_state.initialized) {
        if (unified_keyboard_module.cleanup) {
            unified_keyboard_module.cleanup();
        }
        keyboard_state.initialized = 0;
    }

    return COS_API_OK;
}

int cos_keyboard_set_mode(uint8_t mode) {
    API_CHECK_INITIALIZED(keyboard_state.initialized);
    API_CHECK_RANGE(mode, COS_KB_MODE_NORMAL, COS_KB_MODE_RECOVERY);
    
    if (unified_keyboard_configure(mode) != MODULE_STATUS_OK) {
        return COS_API_ERROR;
    }
    
    keyboard_state.current_mode = mode;
    API_LOG_INFO("KEYBOARD_API", "Keyboard mode changed successfully");
    return COS_API_OK;
}

int cos_keyboard_get_mode(void) {
    API_CHECK_INITIALIZED(keyboard_state.initialized);
    return keyboard_state.current_mode;
}

int cos_keyboard_get_event(cos_keyboard_event_t* event) {
    static uint64_t event_counter = 0;

    API_CHECK_INITIALIZED(keyboard_state.initialized);
    API_CHECK_PARAM(event);

    if (!keyboard_has_event()) {
        return COS_API_TIMEOUT;
    }

    keyboard_event_t raw = keyboard_get_event();
    event->key_code = raw.scancode;
    event->pressed = raw.pressed != 0;
    event->modifiers = raw.modifiers;
    event->timestamp = ++event_counter;
    return COS_API_OK;
}

/* ============================================================
 * GUI API IMPLEMENTATION
 * ============================================================ */


/* GUI state */
static struct {
    int initialized;
    int window_count;
} gui_state = {0, 0};

/* GUI API implementation */
int cos_gui_init(void) {
    API_LOG_INFO("GUI_API", "Initializing GUI API");

    if (!gui_is_initialized()) {
        gui_init();
    }

    if (!gui_is_initialized()) {
        API_LOG_ERROR("GUI_API", "gui_init", COS_API_ERROR);
        return COS_API_ERROR;
    }

    gui_state.initialized = 1;
    gui_state.window_count = 0;

    API_LOG_INFO("GUI_API", "GUI API initialized successfully");
    return COS_API_OK;
}

int cos_gui_cleanup(void) {
    API_LOG_INFO("GUI_API", "Cleaning up GUI API");

    if (gui_state.initialized) {
        gui_state.initialized = 0;
    }

    return COS_API_OK;
}

int cos_gui_desktop_add_icon(const char* path, const char* name, int is_file) {
    API_CHECK_INITIALIZED(gui_state.initialized);
    API_CHECK_PARAM(path);
    API_CHECK_PARAM(name);
    
    /* Call existing GUI function */
    if (gui_add_desktop_icon(path, name, is_file) != 0) {
        return COS_API_ERROR;
    }
    
    API_LOG_INFO("GUI_API", "Desktop icon added successfully");
    return COS_API_OK;
}

int cos_gui_desktop_sync(void) {
    API_CHECK_INITIALIZED(gui_state.initialized);
    
    /* Call existing GUI sync function */
    gui_sync_desktop_with_fs();
    
    API_LOG_INFO("GUI_API", "Desktop synchronized successfully");
    return COS_API_OK;
}

/* ============================================================
 * FILE SYSTEM API IMPLEMENTATION
 * ============================================================ */

/* File system state */
static struct {
    int initialized;
} fs_state = {0};

/* File system API implementation */
int cos_fs_init(void) {
    API_LOG_INFO("FS_API", "Initializing file system API");

    /* Initialize existing file system */
    fs_init();

    fs_state.initialized = 1;
    API_LOG_INFO("FS_API", "File system API initialized successfully");
    return COS_API_OK;
}

int cos_fs_cleanup(void) {
    API_LOG_INFO("FS_API", "Cleaning up file system API");
    fs_state.initialized = 0;
    return COS_API_OK;
}

int cos_fs_create_file(const cos_file_op_t* file) {
    API_CHECK_INITIALIZED(fs_state.initialized);
    API_CHECK_PARAM(file);
    API_CHECK_PARAM(file->path);
    API_CHECK_PARAM(file->name);
    
    /* Call existing file system function */
    if (!fs_create_file_at(file->path, file->name)) {
        return COS_API_ERROR;
    }
    
    API_LOG_INFO("FS_API", "File created successfully");
    return COS_API_OK;
}

int cos_fs_create_directory(const cos_file_op_t* dir) {
    API_CHECK_INITIALIZED(fs_state.initialized);
    API_CHECK_PARAM(dir);
    API_CHECK_PARAM(dir->path);
    API_CHECK_PARAM(dir->name);

    /* Call existing file system function */
    if (!fs_create_dir_at(dir->path, dir->name)) {
        return COS_API_ERROR;
    }

    API_LOG_INFO("FS_API", "Directory created successfully");
    return COS_API_OK;
}


int cos_fs_get_file_info(const char* path, cos_file_op_t* info) {
    API_CHECK_INITIALIZED(fs_state.initialized);
    API_CHECK_PARAM(path);
    API_CHECK_PARAM(info);

    memset(info, 0, sizeof(*info));

    char tmp[1];
    int rc = cos_fs_read_file(path, tmp, 1);

    if (rc < 0) {
        return COS_API_NOT_FOUND;
    }

    API_STR_COPY(info->path, path, sizeof(info->path));
    info->size = 1;

    return COS_API_OK;
}

/* ============================================================
 * ERROR HANDLING IMPLEMENTATION
 * ============================================================ */

void cos_api_log_error(const char* module, const char* function, int error_code) {
    serial_puts("[ERROR] ");
    serial_puts(module);
    serial_puts("::");
    serial_puts(function);
    serial_puts(" - Code: ");
    serial_putdec((uint64_t)error_code);
    serial_puts("\n");
}

void cos_api_log_info(const char* module, const char* message) {
    serial_puts("[INFO] ");
    serial_puts(module);
    serial_puts(": ");
    serial_puts(message);
    serial_puts("\n");
}

void cos_api_log_debug(const char* module, const char* message) {
    serial_puts("[DEBUG] ");
    serial_puts(module);
    serial_puts(": ");
    serial_puts(message);
    serial_puts("\n");
}

/* Version checking */
int cos_api_check_version(uint64_t major, uint64_t minor, uint64_t patch) {
    if (major != COS_API_VERSION_MAJOR || 
        minor > COS_API_VERSION_MINOR ||
        (minor == COS_API_VERSION_MINOR && patch > COS_API_VERSION_PATCH)) {
        return COS_API_ERROR;
    }
    return COS_API_OK;
}

/* Main API initialization */
int cos_api_init(void) {
    API_LOG_INFO("COS_API", "Initializing C-OS 4.0.8 alpha Unified API System");
    API_LOG_INFO("COS_API", "API Version 1.0.0");

    /* Initialize all subsystems */
    if (cos_storage_init() != COS_API_OK) return COS_API_ERROR;
    if (cos_keyboard_init() != COS_API_OK) return COS_API_ERROR;
    if (cos_gui_init() != COS_API_OK) return COS_API_ERROR;
    if (cos_fs_init() != COS_API_OK) return COS_API_ERROR;

    API_LOG_INFO("COS_API", "All API subsystems initialized successfully");
    return COS_API_OK;
}

int cos_api_cleanup(void) {
    API_LOG_INFO("COS_API", "Cleaning up C-OS 4.0.8 alpha Unified API System");
    
    cos_storage_cleanup();
    cos_keyboard_cleanup();
    cos_gui_cleanup();
    cos_fs_cleanup();
    
    API_LOG_INFO("COS_API", "All API subsystems cleaned up successfully");
    return COS_API_OK;
}

#endif
