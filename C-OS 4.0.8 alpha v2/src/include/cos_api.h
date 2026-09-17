#ifndef COS_API_H
#define COS_API_H

#include "types.h"
#include "module_interface.h"

/* C-OS 4.0.8 alpha Unified API System
 * Provides standardized interface for all OS components
 */

/* API Version */
#define COS_API_VERSION_MAJOR  1
#define COS_API_VERSION_MINOR  0
#define COS_API_VERSION_PATCH  0

/* Standard Return Codes */
#define COS_API_OK           0
#define COS_API_ERROR        -1
#define COS_API_INVALID_ARG  -2
#define COS_API_NOT_FOUND    -3
#define COS_API_NOT_INITIALIZED -4
#define COS_API_ACCESS_DENIED -7
#define COS_API_NO_MEMORY    -5
#define COS_API_TIMEOUT      -6

/* Standard API Structure */
typedef struct {
    const char* name;
    uint64_t    version;
    int        (*init)(void);
    int        (*cleanup)(void);
    int        (*get_status)(void);
    const char* (*get_info)(void);
} cos_api_interface_t;

/* ============================================================
 * STORAGE API
 * ============================================================ */

/* Storage device types */
#define COS_STORAGE_TYPE_RAM     0
#define COS_STORAGE_TYPE_VFS     1
#define COS_STORAGE_TYPE_FAT32   2
#define COS_STORAGE_TYPE_MP3     3

/* Storage operations */
typedef struct {
    uint64_t sector;
    void*     buffer;
    uint64_t size;
} cos_storage_read_t;

typedef struct {
    uint64_t sector;
    const void* buffer;
    uint64_t size;
} cos_storage_write_t;

typedef struct {
    char     name[64];
    uint64_t size;
    uint64_t created_time;
    uint64_t modified_time;
    uint8_t  attributes;
} cos_storage_file_info_t;

/* Storage API functions */
int cos_storage_init(void);
int cos_storage_cleanup(void);
int cos_storage_get_device_count(void);
int cos_storage_get_device_info(int device_id, cos_storage_file_info_t* info);
int cos_storage_set_device(int device_id);
int cos_storage_read(const cos_storage_read_t* request);
int cos_storage_write(const cos_storage_write_t* request);
int cos_storage_format_device(int device_id);
uint64_t cos_storage_get_capacity(void);
uint64_t cos_storage_get_free_space(void);

/* ============================================================
 * KEYBOARD API
 * ============================================================ */

/* Keyboard modes */
#define COS_KB_MODE_NORMAL    0
#define COS_KB_MODE_GAME      1
#define COS_KB_MODE_DIAG       2
#define COS_KB_MODE_RECOVERY   3

/* Key codes */
#define COS_KEY_A      0x1E
#define COS_KEY_B      0x30
#define COS_KEY_C      0x2E
#define COS_KEY_D      0x20
#define COS_KEY_E      0x12
#define COS_KEY_F      0x21
#define COS_KEY_G      0x22
#define COS_KEY_H      0x23
#define COS_KEY_I      0x17
#define COS_KEY_J      0x24
#define COS_KEY_K      0x25
#define COS_KEY_L      0x26
#define COS_KEY_M      0x32
#define COS_KEY_N      0x31
#define COS_KEY_O      0x18
#define COS_KEY_P      0x19
#define COS_KEY_Q      0x10
#define COS_KEY_R      0x13
#define COS_KEY_S      0x1F
#define COS_KEY_T      0x14
#define COS_KEY_U      0x16
#define COS_KEY_V      0x2F
#define COS_KEY_W      0x11
#define COS_KEY_X      0x2D
#define COS_KEY_Y      0x15
#define COS_KEY_Z      0x2C

#define COS_KEY_1      0x02
#define COS_KEY_2      0x03
#define COS_KEY_3      0x04
#define COS_KEY_4      0x05
#define COS_KEY_5      0x06
#define COS_KEY_6      0x07
#define COS_KEY_7      0x08
#define COS_KEY_8      0x09
#define COS_KEY_9      0x0A
#define COS_KEY_0      0x0B

#define COS_KEY_SPACE   0x39
#define COS_KEY_ENTER   0x1C
#define COS_KEY_ESC     0x01
#define COS_KEY_TAB     0x0F
#define COS_KEY_BACKSPACE 0x0E
#define COS_KEY_DELETE   0x53
#define COS_KEY_INSERT   0x52
#define COS_KEY_HOME    0x47
#define COS_KEY_END     0x4F
#define COS_KEY_PGUP    0x49
#define COS_KEY_PGDN    0x51

#define COS_KEY_F1      0x3B
#define COS_KEY_F2      0x3C
#define COS_KEY_F3      0x3D
#define COS_KEY_F4      0x3E
#define COS_KEY_F5      0x3F
#define COS_KEY_F6      0x40
#define COS_KEY_F7      0x41
#define COS_KEY_F8      0x42
#define COS_KEY_F9      0x43
#define COS_KEY_F10     0x44
#define COS_KEY_F11     0x45
#define COS_KEY_F12     0x46

/* Keyboard events */
typedef struct {
    uint8_t  key_code;
    bool     pressed;
    uint8_t  modifiers;  /* Shift, Ctrl, Alt */
    uint64_t timestamp;
} cos_keyboard_event_t;

/* Keyboard API functions */
int cos_keyboard_init(void);
int cos_keyboard_cleanup(void);
int cos_keyboard_set_mode(uint8_t mode);
int cos_keyboard_get_mode(void);
int cos_keyboard_get_event(cos_keyboard_event_t* event);
int cos_keyboard_set_repeat_rate(uint8_t rate, uint8_t delay);
int cos_keyboard_enable_filter(bool enable);
int cos_keyboard_enable_stabilizer(bool enable);

/* ============================================================
 * GUI API
 * ============================================================ */

/* Window types */
#define COS_WINDOW_TYPE_NORMAL     0
#define COS_WINDOW_TYPE_TERMINAL   1
#define COS_WINDOW_TYPE_FILE_MGR   2
#define COS_WINDOW_TYPE_BROWSER    3
#define COS_WINDOW_TYPE_ABOUT      4

/* GUI events */
typedef struct {
    uint8_t  type;      /* Mouse, Keyboard, Window */
    uint8_t  subtype;   /* Click, Move, Resize, etc. */
    uint64_t x, y;
    uint64_t data;
    uint64_t timestamp;
} cos_gui_event_t;

/* Window operations */
typedef struct {
    int       window_id;
    int       type;
    char      title[64];
    uint64_t  x, y;
    uint64_t  width, height;
    bool      visible;
    bool      minimized;
    bool      maximized;
} cos_window_info_t;

/* GUI API functions */
int cos_gui_init(void);
int cos_gui_cleanup(void);
int cos_gui_create_window(const cos_window_info_t* info, int* window_id);
int cos_gui_close_window(int window_id);
int cos_gui_show_window(int window_id);
int cos_gui_hide_window(int window_id);
int cos_gui_minimize_window(int window_id);
int cos_gui_restore_window(int window_id);
int cos_gui_get_window_info(int window_id, cos_window_info_t* info);
int cos_gui_get_event(cos_gui_event_t* event);
int cos_gui_desktop_add_icon(const char* path, const char* name, int is_file);
int cos_gui_desktop_remove_icon(const char* name);
int cos_gui_desktop_sync(void);

/* ============================================================
 * FILE SYSTEM API
 * ============================================================ */

/* File attributes */
#define COS_FILE_ATTR_READ_ONLY   0x01
#define COS_FILE_ATTR_HIDDEN      0x02
#define COS_FILE_ATTR_SYSTEM      0x04
#define COS_FILE_ATTR_DIRECTORY   0x10

/* File operations */
typedef struct {
    char     path[256];
    char     name[64];
    uint8_t  attributes;
    uint64_t size;
    void*    data;
    uint64_t data_size;
} cos_file_op_t;

/* Directory operations */
typedef struct {
    char     path[256];
    cos_file_op_t* files;
    int      max_files;
    int      file_count;
} cos_dir_op_t;

/* File system API functions */
int cos_fs_init(void);
int cos_fs_cleanup(void);
int cos_fs_create_file(const cos_file_op_t* file);
int cos_fs_create_directory(const cos_file_op_t* dir);
int cos_fs_read_file(const char* path, void* buffer, uint64_t size);
int cos_fs_write_file(const char* path, const void* data, uint64_t size);
int cos_fs_delete_file(const char* path);
int cos_fs_list_directory(const cos_dir_op_t* dir);
int cos_fs_get_file_info(const char* path, cos_file_op_t* info);
int cos_fs_set_file_attributes(const char* path, uint8_t attributes);
int cos_fs_copy_file(const char* src, const char* dst);
int cos_fs_move_file(const char* src, const char* dst);

/* ============================================================
 * SYSTEM API
 * ============================================================ */

/* System information */
typedef struct {
    char     os_name[32];
    char     version[16];
    char     build_date[16];
    uint64_t uptime;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t cpu_usage;
} cos_system_info_t;

/* Process information */
typedef struct {
    int       pid;
    char      name[64];
    uint64_t memory_usage;
    uint64_t cpu_usage;
    uint8_t  priority;
    uint8_t  status;  /* Running, Sleeping, Zombie */
} cos_process_info_t;

/* System API functions */
int cos_system_init(void);
int cos_system_cleanup(void);
int cos_system_get_info(cos_system_info_t* info);
int cos_system_get_process_list(cos_process_info_t* processes, int max_count);
int cos_system_kill_process(int pid);
int cos_system_reboot(void);
int cos_system_shutdown(void);
uint64_t cos_system_get_time(void);
int cos_system_set_time(uint64_t time);

/* ============================================================
 * NETWORK API
 * ============================================================ */

/* Network interface types */
#define COS_NET_TYPE_ETHERNET  0
#define COS_NET_TYPE_WIFI      1
#define COS_NET_TYPE_LOOPBACK  2

/* Network operations */
typedef struct {
    char     interface_name[16];
    uint8_t  type;
    char     ip_address[16];
    char     mac_address[18];
    bool      connected;
    uint64_t bytes_sent;
    uint64_t bytes_received;
} cos_net_interface_t;

/* Network API functions */
int cos_net_init(void);
int cos_net_cleanup(void);
int cos_net_get_interface_count(void);
int cos_net_get_interface_info(int interface_id, cos_net_interface_t* info);
int cos_net_connect(int interface_id);
int cos_net_disconnect(int interface_id);
int cos_net_send_packet(int interface_id, const void* data, uint64_t size);
int cos_net_receive_packet(int interface_id, void* buffer, uint64_t max_size);

/* ============================================================
 * MEMORY API
 * ============================================================ */

/* Memory allocation types */
#define COS_MEM_TYPE_KERNEL    0
#define COS_MEM_TYPE_USER      1
#define COS_MEM_TYPE_DMA       2
#define COS_MEM_TYPE_SHARED    3

/* Memory API functions */
int cos_mem_init(void);
int cos_mem_cleanup(void);
void* cos_mem_alloc(uint64_t size, uint8_t type);
void* cos_mem_realloc(void* ptr, uint64_t new_size, uint8_t type);
void cos_mem_free(void* ptr);
uint64_t cos_mem_get_total(void);
uint64_t cos_mem_get_free(void);
uint64_t cos_mem_get_used(void);
int cos_mem_get_usage_stats(uint64_t* total, uint64_t* used, uint64_t* free);

/* ============================================================
 * MAIN API INITIALIZATION
 * ============================================================ */

/* Initialize all API subsystems */
int cos_api_init(void);
int cos_api_cleanup(void);

/* ============================================================
 * EXPANDED SYSTEM HOOK API
 * ============================================================ */
int cos_widget_create(const char* type, const char* title, int x, int y, int w, int h);
int cos_widget_destroy(int widget_id);
int cos_plugin_load(const char* path);
int cos_plugin_unload(const char* name);
int cos_package_install(const char* package_name);
int cos_package_remove(const char* package_name);
int cos_service_start(const char* name);
int cos_service_stop(const char* name);
int cos_log_write(const char* channel, const char* message);
int cos_log_read(char* out, uint64_t size);
int cos_vmm_get_stats(uint64_t* total, uint64_t* used, uint64_t* free);
int cos_fs_read(const char* path, char* out, uint64_t size);
int cos_fs_write(const char* path, const char* data, uint64_t size);
int cos_process_spawn(const char* app);
int cos_process_list(char* out, uint64_t size);
int cos_task_yield(void);
int cos_task_get_count(uint64_t* count);
int cos_network_set_mode(const char* mode);
int cos_power_request(const char* action);
void cos_power_init(void);
uint64_t cos_power_get_battery_percent(void);
uint64_t cos_power_get_estimated_minutes_remaining(void);
bool cos_power_is_charging(void);
int cos_settings_set(const char* key, const char* value);
int cos_settings_get(const char* key, char* out, uint64_t size);

/* API version checking */
int cos_api_check_version(uint64_t major, uint64_t minor, uint64_t patch);

/* Error handling */
void cos_api_log_error(const char* module, const char* function, int error_code);
void cos_api_log_info(const char* module, const char* message);
void cos_api_log_debug(const char* module, const char* message);
/* ============================================================
 * OS-WIDE BRIDGE API
 * ============================================================ */

#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "mouse_minimal.h"
#include "calc_school_math.h"
#include "cos_version.h"

typedef enum {
    COS_OSWIDE_APP_NONE = 0,
    COS_OSWIDE_APP_FILE_MANAGER,
    COS_OSWIDE_APP_TEXT_EDITOR,
    COS_OSWIDE_APP_TERMINAL,
    COS_OSWIDE_APP_SETTINGS,
    COS_OSWIDE_APP_ABOUT,
    COS_OSWIDE_APP_CALC,
    COS_OSWIDE_APP_STORAGE,
    COS_OSWIDE_APP_BROWSER,
    COS_OSWIDE_APP_TASK_MANAGER,
    COS_OSWIDE_APP_PAINT,
    COS_OSWIDE_APP_MUSIC,
    COS_OSWIDE_APP_CLOCK,
    COS_OSWIDE_APP_SYSINFO,
    COS_OSWIDE_APP_PYTHON_IDE,
    COS_OSWIDE_APP_SHEET
} cos_oswide_app_id_t;

typedef calc_school_result_t cos_oswide_calc_result_t;

typedef struct {
    uint64_t uptime_ticks;
    uint64_t uptime_ms;
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t heap_total;
    const char* version;
} cos_oswide_system_info_t;

bool cos_oswide_init(void);
void cos_oswide_refresh(void);
const char* cos_oswide_version_string(void);
void cos_oswide_get_system_info(cos_oswide_system_info_t* out);
uint64_t cos_oswide_uptime_ticks(void);
uint64_t cos_oswide_uptime_ms(void);

void* cos_oswide_malloc(size_t size);
void* cos_oswide_calloc(size_t count, size_t size);
void* cos_oswide_realloc(void* ptr, size_t size);
void  cos_oswide_free(void* ptr);

bool cos_oswide_fs_exists(const char* full_path);
const char* cos_oswide_fs_read_text(const char* full_path);
bool cos_oswide_fs_read_text_into(const char* full_path, char* out, size_t out_size);
bool cos_oswide_fs_create_file_at(const char* path, const char* name);
bool cos_oswide_fs_create_dir_at(const char* path, const char* name);
bool cos_oswide_fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size);
bool cos_oswide_fs_create_file(const char* full_path);
bool cos_oswide_fs_create_dir(const char* full_path);
bool cos_oswide_fs_write_text(const char* full_path, const char* data);
bool cos_oswide_fs_delete(const char* full_path);
bool cos_oswide_fs_rename(const char* old_path, const char* new_path);
bool cos_oswide_fs_copy(const char* src_path, const char* dst_path);
bool cos_oswide_fs_move(const char* src_path, const char* dst_path);

window_t* cos_oswide_open_window(int kind, const char* title, int x, int y, int w, int h);
window_t* cos_oswide_open_app(cos_oswide_app_id_t app, const char* title, int x, int y, int w, int h);
bool      cos_oswide_launch_app_by_name(const char* name);
void      cos_oswide_notify(const char* message, int type);
void      cos_oswide_request_redraw(void);

bool cos_oswide_input_read_key(keyboard_event_t* out);
keyboard_event_t cos_oswide_input_last_key(void);
const minimal_mouse_t* cos_oswide_input_mouse_state(void);
void cos_oswide_input_clear_clicks(void);

bool cos_oswide_calc_eval(const char* expr, bool degree_mode, cos_oswide_calc_result_t* out);

bool cos_oswide_system_reboot(void);
bool cos_oswide_system_shutdown(void);

#endif
