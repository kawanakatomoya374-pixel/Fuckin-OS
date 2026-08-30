/**
 * cos_api.c - OS-wide bridge API
 *
 * Compatibility layer that exposes a compact, app-friendly API on top of
 * the existing C-OS subsystems without colliding with the legacy cos_api.h
 * entry points already used across the tree.
 */

#include "cos_api.h"
#include "hal_api.h"
#include "string.h"
#include "calc_engine.h"

#include <stddef.h>

extern uint64_t get_timer_ticks(void);

static bool g_oswide_ready = false;
static uint64_t g_oswide_allocated_bytes = 0;
static char g_text_ring[4][FS_MAX_DATA + 1];
static unsigned g_text_ring_slot = 0;

extern bool gui_is_initialized(void);
extern void gui_init(void);
extern void gui_update(void);
extern void gui_request_redraw(void);
extern void gui_notify(const char* msg, int type);
extern window_t* gui_open_window(int kind, const char* title, int x, int y, int w, int h);
extern void cos_power_init(void);
extern void acpi_power_init(void);
extern bool acpi_power_is_available(void);
extern uint64_t acpi_power_get_battery_percent(void);
extern uint64_t acpi_power_get_estimated_minutes_remaining(void);
extern bool acpi_power_is_charging(void);
extern void acpi_power_force_refresh(void);
extern bool acpi_power_suspend_to_state(uint8_t sleep_state);
extern bool acpi_power_resume_from_state(uint8_t sleep_state);
extern const char* acpi_power_get_source_label(void);
extern bool keyboard_has_event(void);
extern keyboard_event_t keyboard_get_event(void);
extern key_event_t keyboard_get_key(void);
extern void keyboard_init(void);
extern void keyboard_poll(void);
extern void minimal_mouse_init(void);
extern void minimal_mouse_poll(void);
extern minimal_mouse_t* minimal_mouse_get_state(void);
extern void minimal_mouse_clear_clicks(void);
extern void fs_init(void);
extern fs_entry_t* fs_list_dir(const char* path);
extern int fs_entry_count_for_path(const char* path);
extern const char* fs_read_file_at(const char* path, const char* name);
extern bool fs_create_file_at(const char* path, const char* name);
extern bool fs_create_dir_at(const char* path, const char* name);
extern bool fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size);
extern bool fs_copy_path(const char* src_full_path, const char* dst_full_path);
extern bool fs_move_path(const char* src_full_path, const char* dst_full_path);
extern bool fs_delete_file(const char* path);
extern int fs_rename(const char* old_name, const char* new_name);

extern int mk_kernel_reboot(void) __attribute__((weak));
extern int mk_kernel_shutdown(void) __attribute__((weak));
extern void bios_shutdown(void) __attribute__((weak));
extern void reboot(void) __attribute__((weak));
extern void shutdown(void) __attribute__((weak));

static size_t oswide_strlen(const char* s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static size_t oswide_strnlen(const char* s, size_t max_len) {
    size_t n = 0;
    if (!s) return 0;
    while (n < max_len && s[n] != '\0') n++;
    return n;
}

static void oswide_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void oswide_split_path(const char* full, char* parent, size_t parent_size, char* leaf, size_t leaf_size) {
    if (!parent || !leaf || parent_size == 0 || leaf_size == 0) return;
    parent[0] = '\0';
    leaf[0] = '\0';
    if (!full || full[0] == '\0') return;

    const char* end = full + oswide_strlen(full);
    while (end > full + 1 && *(end - 1) == '/') --end;

    const char* last = NULL;
    for (const char* p = full; p < end; ++p) {
        if (*p == '/') last = p;
    }

    if (!last) {
        oswide_copy_cstr(parent, parent_size, "/");
        size_t leaf_len = (size_t)(end - full);
        if (leaf_len >= leaf_size) leaf_len = leaf_size - 1;
        memcpy(leaf, full, leaf_len);
        leaf[leaf_len] = '\0';
        return;
    }

    if (last == full) {
        oswide_copy_cstr(parent, parent_size, "/");
        size_t leaf_len = (size_t)(end - (full + 1));
        if (leaf_len >= leaf_size) leaf_len = leaf_size - 1;
        memcpy(leaf, full + 1, leaf_len);
        leaf[leaf_len] = '\0';
        return;
    }

    size_t parent_len = (size_t)(last - full);
    if (parent_len >= parent_size) parent_len = parent_size - 1;
    memcpy(parent, full, parent_len);
    parent[parent_len] = '\0';
    if (parent[0] == '\0') oswide_copy_cstr(parent, parent_size, "/");

    size_t leaf_len = (size_t)(end - (last + 1));
    if (leaf_len >= leaf_size) leaf_len = leaf_size - 1;
    memcpy(leaf, last + 1, leaf_len);
    leaf[leaf_len] = '\0';
}

static bool oswide_full_path_exists(const char* full_path) {
    if (!full_path || !full_path[0]) return false;
    if (strcmp(full_path, "/") == 0) return true;

    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    oswide_split_path(full_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (leaf[0] != '\0') {
        const char* file = fs_read_file_at(parent, leaf);
        if (file) return true;
    }

    fs_entry_t* dir_entries = fs_list_dir(full_path);
    if (!dir_entries) return false;
    return strcmp(dir_entries[0].path, full_path) == 0;
}

static cos_oswide_app_id_t oswide_app_from_name(const char* name) {
    if (!name) return COS_OSWIDE_APP_NONE;
    if (strcmp(name, "file") == 0 || strcmp(name, "files") == 0 ||
        strcmp(name, "filemanager") == 0 || strcmp(name, "file-manager") == 0 ||
        strcmp(name, "fm") == 0) return COS_OSWIDE_APP_FILE_MANAGER;
    if (strcmp(name, "text") == 0 || strcmp(name, "editor") == 0 || strcmp(name, "notepad") == 0) return COS_OSWIDE_APP_TEXT_EDITOR;
    if (strcmp(name, "term") == 0 || strcmp(name, "terminal") == 0) return COS_OSWIDE_APP_TERMINAL;
    if (strcmp(name, "settings") == 0) return COS_OSWIDE_APP_SETTINGS;
    if (strcmp(name, "about") == 0) return COS_OSWIDE_APP_ABOUT;
    if (strcmp(name, "calc") == 0 || strcmp(name, "calculator") == 0) return COS_OSWIDE_APP_CALC;
    if (strcmp(name, "storage") == 0) return COS_OSWIDE_APP_STORAGE;
    if (strcmp(name, "browser") == 0 || strcmp(name, "web") == 0 || strcmp(name, "netsurf") == 0) return COS_OSWIDE_APP_BROWSER;
    if (strcmp(name, "task") == 0 || strcmp(name, "taskmgr") == 0 || strcmp(name, "task-manager") == 0) return COS_OSWIDE_APP_TASK_MANAGER;
    if (strcmp(name, "paint") == 0) return COS_OSWIDE_APP_PAINT;
    if (strcmp(name, "music") == 0) return COS_OSWIDE_APP_MUSIC;
    if (strcmp(name, "clock") == 0) return COS_OSWIDE_APP_CLOCK;
    if (strcmp(name, "sysinfo") == 0 || strcmp(name, "system") == 0 || strcmp(name, "info") == 0) return COS_OSWIDE_APP_SYSINFO;
    if (strcmp(name, "python") == 0 || strcmp(name, "pyide") == 0 || strcmp(name, "python-ide") == 0) return COS_OSWIDE_APP_PYTHON_IDE;
    if (strcmp(name, "sheet") == 0 || strcmp(name, "spreadsheet") == 0) return COS_OSWIDE_APP_SHEET;
    return COS_OSWIDE_APP_NONE;
}

static int oswide_window_kind_for_app(cos_oswide_app_id_t app) {
    switch (app) {
        case COS_OSWIDE_APP_FILE_MANAGER: return WIN_FILE_MGR;
        case COS_OSWIDE_APP_TEXT_EDITOR:  return WIN_TEXT_EDITOR;
        case COS_OSWIDE_APP_TERMINAL:      return WIN_TERMINAL;
        case COS_OSWIDE_APP_SETTINGS:      return WIN_SETTINGS;
        case COS_OSWIDE_APP_ABOUT:         return WIN_ABOUT;
        case COS_OSWIDE_APP_CALC:          return WIN_CALC;
        case COS_OSWIDE_APP_STORAGE:       return WIN_STORAGE;
        case COS_OSWIDE_APP_BROWSER:       return WIN_BROWSER;
        case COS_OSWIDE_APP_TASK_MANAGER:  return WIN_TASK_MGR;
        case COS_OSWIDE_APP_PAINT:         return WIN_PAINT;
        case COS_OSWIDE_APP_MUSIC:         return WIN_MUSIC;
        case COS_OSWIDE_APP_CLOCK:         return WIN_CLOCK;
        case COS_OSWIDE_APP_SYSINFO:       return WIN_SYSINFO;
        case COS_OSWIDE_APP_PYTHON_IDE:    return WIN_PYTHON_IDE;
        case COS_OSWIDE_APP_SHEET:         return WIN_SHEET;
        default:                           return WIN_NONE;
    }
}

static const char* oswide_default_title_for_app(cos_oswide_app_id_t app) {
    switch (app) {
        case COS_OSWIDE_APP_FILE_MANAGER: return "File Manager";
        case COS_OSWIDE_APP_TEXT_EDITOR:  return "Text Editor";
        case COS_OSWIDE_APP_TERMINAL:      return "Terminal";
        case COS_OSWIDE_APP_SETTINGS:      return "Settings";
        case COS_OSWIDE_APP_ABOUT:         return "About C-OS 4.0.8 alpha";
        case COS_OSWIDE_APP_CALC:          return "Calculator";
        case COS_OSWIDE_APP_STORAGE:       return "Storage";
        case COS_OSWIDE_APP_BROWSER:       return "NetSurf";
        case COS_OSWIDE_APP_TASK_MANAGER:  return "Task Manager";
        case COS_OSWIDE_APP_PAINT:         return "Paint";
        case COS_OSWIDE_APP_MUSIC:         return "Music";
        case COS_OSWIDE_APP_CLOCK:         return "Clock";
        case COS_OSWIDE_APP_SYSINFO:       return "System Info";
        case COS_OSWIDE_APP_PYTHON_IDE:    return "Python IDE";
        case COS_OSWIDE_APP_SHEET:         return "Spreadsheet";
        default:                           return "Window";
    }
}

static void* oswide_raw_to_user(void* raw) {
    if (!raw) return NULL;
    return (void*)((uint8_t*)raw + sizeof(size_t));
}

static void* oswide_user_to_raw(void* user) {
    if (!user) return NULL;
    return (void*)((uint8_t*)user - sizeof(size_t));
}

bool cos_oswide_init(void) {
    if (g_oswide_ready) return true;
    keyboard_init();
    minimal_mouse_init();
    fs_init();
    if (!gui_is_initialized()) {
        gui_init();
    }
    cos_power_init();
    g_oswide_ready = true;
    return true;
}

void cos_oswide_refresh(void) {
    if (gui_is_initialized()) {
        gui_update();
    }
}

const char* cos_oswide_version_string(void) {
    return COS_FULL_NAME;
}

void cos_oswide_get_system_info(cos_oswide_system_info_t* out) {
    if (!out) return;
    out->uptime_ticks = cos_oswide_uptime_ticks();
    out->uptime_ms = cos_oswide_uptime_ms();
    out->heap_used = g_oswide_allocated_bytes;
    out->heap_free = 0;
    out->heap_total = 0;
    out->version = COS_FULL_NAME;
}

uint64_t cos_oswide_uptime_ticks(void) {
    return hal_timer_get_ticks();
}

uint64_t cos_oswide_uptime_ms(void) {
    return hal_timer_get_ms();
}

void* cos_oswide_malloc(size_t size) {
    size_t total = sizeof(size_t) + (size == 0 ? 1 : size);
    uint8_t* raw = (uint8_t*)cos_mem_alloc(total, COS_MEM_TYPE_USER);
    if (!raw) return NULL;
    *((size_t*)raw) = size;
    g_oswide_allocated_bytes += size;
    return oswide_raw_to_user(raw);
}

void* cos_oswide_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void* ptr = cos_oswide_malloc(total);
    if (ptr && total > 0) memset(ptr, 0, total);
    return ptr;
}

void* cos_oswide_realloc(void* ptr, size_t size) {
    if (!ptr) return cos_oswide_malloc(size);
    if (size == 0) {
        cos_oswide_free(ptr);
        return NULL;
    }

    uint8_t* raw = (uint8_t*)oswide_user_to_raw(ptr);
    size_t old_size = *((size_t*)raw);
    void* next = cos_oswide_malloc(size);
    if (!next) return NULL;

    size_t copy_size = old_size < size ? old_size : size;
    if (copy_size > 0) memcpy(next, ptr, copy_size);
    cos_oswide_free(ptr);
    return next;
}

void cos_oswide_free(void* ptr) {
    if (!ptr) return;
    uint8_t* raw = (uint8_t*)oswide_user_to_raw(ptr);
    size_t old_size = *((size_t*)raw);
    if (g_oswide_allocated_bytes >= old_size) g_oswide_allocated_bytes -= old_size;
    else g_oswide_allocated_bytes = 0;
    cos_mem_free(raw);
}

bool cos_oswide_fs_exists(const char* full_path) {
    return oswide_full_path_exists(full_path);
}

const char* cos_oswide_fs_read_text(const char* full_path) {
    if (!full_path || !full_path[0]) return NULL;

    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    oswide_split_path(full_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (!leaf[0]) return NULL;

    const char* data = fs_read_file_at(parent, leaf);
    if (!data) return NULL;

    char* slot = g_text_ring[g_text_ring_slot++ & 3u];
    size_t len = oswide_strnlen(data, FS_MAX_DATA);
    memcpy(slot, data, len);
    slot[len] = '\0';
    return slot;
}

bool cos_oswide_fs_read_text_into(const char* full_path, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    const char* text = cos_oswide_fs_read_text(full_path);
    if (!text) {
        out[0] = '\0';
        return false;
    }
    oswide_copy_cstr(out, out_size, text);
    return true;
}

bool cos_oswide_fs_create_file_at(const char* path, const char* name) {
    return fs_create_file_at(path, name);
}

bool cos_oswide_fs_create_dir_at(const char* path, const char* name) {
    return fs_create_dir_at(path, name);
}

bool cos_oswide_fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size) {
    return fs_write_file_at(path, name, data, size);
}

bool cos_oswide_fs_create_file(const char* full_path) {
    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    oswide_split_path(full_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (!leaf[0]) return false;
    return fs_create_file_at(parent, leaf);
}

bool cos_oswide_fs_create_dir(const char* full_path) {
    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    oswide_split_path(full_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (!leaf[0]) return false;
    return fs_create_dir_at(parent, leaf);
}

bool cos_oswide_fs_write_text(const char* full_path, const char* data) {
    if (!full_path || !data) return false;
    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    oswide_split_path(full_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (!leaf[0]) return false;
    return fs_write_file_at(parent, leaf, data, (uint64_t)oswide_strlen(data));
}

bool cos_oswide_fs_delete(const char* full_path) {
    return fs_delete_file(full_path);
}

bool cos_oswide_fs_rename(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) return false;
    return fs_rename(old_path, new_path) == 0;
}

bool cos_oswide_fs_copy(const char* src_path, const char* dst_path) {
    return fs_copy_path(src_path, dst_path);
}

bool cos_oswide_fs_move(const char* src_path, const char* dst_path) {
    return fs_move_path(src_path, dst_path);
}

window_t* cos_oswide_open_window(int kind, const char* title, int x, int y, int w, int h) {
    if (!gui_is_initialized()) gui_init();
    const char* chosen = (title && title[0]) ? title : "Window";
    return gui_open_window(kind, chosen, x, y, w, h);
}

window_t* cos_oswide_open_app(cos_oswide_app_id_t app, const char* title, int x, int y, int w, int h) {
    int kind = oswide_window_kind_for_app(app);
    if (kind == WIN_NONE) return NULL;
    const char* chosen = (title && title[0]) ? title : oswide_default_title_for_app(app);
    return cos_oswide_open_window(kind, chosen, x, y, w, h);
}

bool cos_oswide_launch_app_by_name(const char* name) {
    cos_oswide_app_id_t app = oswide_app_from_name(name);
    if (app == COS_OSWIDE_APP_NONE) return false;
    return cos_oswide_open_app(app, NULL, 72, 56, 960, 640) != NULL;
}

void cos_oswide_notify(const char* message, int type) {
    if (!message) return;
    gui_notify(message, type);
}

void cos_oswide_request_redraw(void) {
    gui_request_redraw();
}

bool cos_oswide_input_read_key(keyboard_event_t* out) {
    if (!out) return false;
    if (!keyboard_has_event()) return false;
    *out = keyboard_get_event();
    return true;
}

keyboard_event_t cos_oswide_input_last_key(void) {
    return keyboard_get_key();
}

const minimal_mouse_t* cos_oswide_input_mouse_state(void) {
    return minimal_mouse_get_state();
}

void cos_oswide_input_clear_clicks(void) {
    minimal_mouse_clear_clicks();
}

bool cos_oswide_calc_eval(const char* expr, bool degree_mode, cos_oswide_calc_result_t* out) {
    if (!expr || !out) return false;
    return calc_engine_evaluate(expr, degree_mode, out);
}

static bool oswide_try_reboot(void) {
    if (mk_kernel_reboot) return mk_kernel_reboot() == 0;
    if (reboot) {
        reboot();
        return true;
    }
    return false;
}

static bool oswide_try_shutdown(void) {
    if (mk_kernel_shutdown) return mk_kernel_shutdown() == 0;
    if (bios_shutdown) {
        bios_shutdown();
        return true;
    }
    if (shutdown) {
        shutdown();
        return true;
    }
    return false;
}

bool cos_oswide_system_reboot(void) {
    return oswide_try_reboot();
}

bool cos_oswide_system_shutdown(void) {
    return oswide_try_shutdown();
}


/* ============================================================
 * POWER / BATTERY MODEL
 * ============================================================ */
static bool g_power_ready = false;
static uint64_t g_power_boot_ticks = 0;
static uint64_t g_power_last_sample = 0;
static uint64_t g_power_battery_percent = 100;
static bool g_power_charging = false;
static bool g_power_low_power_hint = true;

static void cos_power_sample(void) {
    uint64_t now = get_timer_ticks();
    if (g_power_last_sample == 0) {
        g_power_last_sample = now;
    }

    if (acpi_power_is_available()) {
        acpi_power_force_refresh();
        g_power_battery_percent = acpi_power_get_battery_percent();
        g_power_charging = acpi_power_is_charging();
        g_power_last_sample = now;
        return;
    }

    if (now <= g_power_last_sample) return;
    uint64_t elapsed_ticks = now - g_power_last_sample;
    uint64_t elapsed_seconds = elapsed_ticks / 1000;
    if (elapsed_seconds == 0) return;

    /* Simulate a conservative mobile battery model: drain slowly when idle,
     * slightly faster when the system is in normal/high performance mode. */
    uint64_t drain_period = g_power_low_power_hint ? 45 : 25;
    if (g_power_charging) {
        uint64_t charge_gain = (elapsed_seconds + (drain_period / 2)) / drain_period;
        if (charge_gain == 0) charge_gain = 1;
        g_power_battery_percent = (g_power_battery_percent + charge_gain > 100) ? 100 : (g_power_battery_percent + charge_gain);
    } else {
        uint64_t drain = elapsed_seconds / drain_period;
        if (drain == 0 && elapsed_seconds >= 5) drain = 1;
        if (drain >= g_power_battery_percent) g_power_battery_percent = 0;
        else g_power_battery_percent -= drain;
    }

    g_power_last_sample = now;
}

void cos_power_init(void) {
    g_power_ready = true;
    g_power_boot_ticks = get_timer_ticks();
    g_power_last_sample = g_power_boot_ticks;
    g_power_battery_percent = 100;
    g_power_charging = false;
    g_power_low_power_hint = true;
    acpi_power_init();
    if (acpi_power_is_available()) {
        acpi_power_force_refresh();
        g_power_battery_percent = acpi_power_get_battery_percent();
        g_power_charging = acpi_power_is_charging();
    }
}

uint64_t cos_power_get_battery_percent(void) {
    if (!g_power_ready) cos_power_init();
    cos_power_sample();
    if (acpi_power_is_available()) return acpi_power_get_battery_percent();
    return g_power_battery_percent;
}

uint64_t cos_power_get_estimated_minutes_remaining(void) {
    if (!g_power_ready) cos_power_init();
    cos_power_sample();
    if (acpi_power_is_available()) return acpi_power_get_estimated_minutes_remaining();
    if (g_power_charging) return 0;

    uint64_t drain_period = g_power_low_power_hint ? 45 : 25;
    uint64_t seconds = (g_power_battery_percent * drain_period);
    return seconds / 60;
}

bool cos_power_is_charging(void) {
    if (!g_power_ready) cos_power_init();
    cos_power_sample();
    if (acpi_power_is_available()) return acpi_power_is_charging();
    return g_power_charging;
}

/* ============================================================
 * EXPANDED SYSTEM HOOK STUBS
 * ============================================================ */
static char g_oswide_log[4096];
static size_t g_oswide_log_len = 0;
static int g_widget_seq = 1;

static void oswide_log_append(const char* s) {
    if (!s) return;
    while (*s && g_oswide_log_len + 1 < sizeof(g_oswide_log)) {
        g_oswide_log[g_oswide_log_len++] = *s++;
    }
    g_oswide_log[g_oswide_log_len] = '\0';
}

int cos_widget_create(const char* type, const char* title, int x, int y, int w, int h) {
    (void)type; (void)title; (void)x; (void)y; (void)w; (void)h;
    return g_widget_seq++;
}

int cos_widget_destroy(int widget_id) { (void)widget_id; return COS_API_OK; }
int cos_plugin_load(const char* path) { if (!path) return COS_API_INVALID_ARG; oswide_log_append("[plugin-load] "); oswide_log_append(path); oswide_log_append("\n"); return COS_API_OK; }
int cos_plugin_unload(const char* name) { if (!name) return COS_API_INVALID_ARG; oswide_log_append("[plugin-unload] "); oswide_log_append(name); oswide_log_append("\n"); return COS_API_OK; }
int cos_package_install(const char* package_name) { if (!package_name) return COS_API_INVALID_ARG; oswide_log_append("[pkg-install] "); oswide_log_append(package_name); oswide_log_append("\n"); return COS_API_OK; }
int cos_package_remove(const char* package_name) { if (!package_name) return COS_API_INVALID_ARG; oswide_log_append("[pkg-remove] "); oswide_log_append(package_name); oswide_log_append("\n"); return COS_API_OK; }
int cos_service_start(const char* name) { if (!name) return COS_API_INVALID_ARG; oswide_log_append("[service-start] "); oswide_log_append(name); oswide_log_append("\n"); return COS_API_OK; }
int cos_service_stop(const char* name) { if (!name) return COS_API_INVALID_ARG; oswide_log_append("[service-stop] "); oswide_log_append(name); oswide_log_append("\n"); return COS_API_OK; }
int cos_log_write(const char* channel, const char* message) { if (!channel || !message) return COS_API_INVALID_ARG; oswide_log_append("["); oswide_log_append(channel); oswide_log_append("] "); oswide_log_append(message); oswide_log_append("\n"); return COS_API_OK; }
int cos_log_read(char* out, uint64_t size) { if (!out || size == 0) return COS_API_INVALID_ARG; size_t n = g_oswide_log_len < size - 1 ? g_oswide_log_len : (size_t)size - 1; memcpy(out, g_oswide_log, n); out[n] = '\0'; return COS_API_OK; }
int cos_vmm_get_stats(uint64_t* total, uint64_t* used, uint64_t* free) { if (!total || !used || !free) return COS_API_INVALID_ARG; *total = cos_mem_get_total(); *used = cos_mem_get_used(); *free = cos_mem_get_free(); return COS_API_OK; }

int cos_fs_read(const char* path, char* out, uint64_t size) { if (!path || !out || size == 0) return COS_API_INVALID_ARG; out[0] = '\0'; oswide_log_append("[fs-read] "); oswide_log_append(path); oswide_log_append("\n"); return COS_API_OK; }
int cos_fs_write(const char* path, const char* data, uint64_t size) { if (!path || !data || size == 0) return COS_API_INVALID_ARG; oswide_log_append("[fs-write] "); oswide_log_append(path); oswide_log_append("\n"); return COS_API_OK; }
int cos_process_spawn(const char* app) { if (!app) return COS_API_INVALID_ARG; oswide_log_append("[proc-spawn] "); oswide_log_append(app); oswide_log_append("\n"); return COS_API_OK; }
int cos_process_list(char* out, uint64_t size) { if (!out || size == 0) return COS_API_INVALID_ARG; const char* text = "pid=1 init\npid=2 shell\n"; size_t n = strlen(text); if (n >= size) n = (size_t)size - 1; memcpy(out, text, n); out[n] = '\0'; return COS_API_OK; }
int cos_task_yield(void) { oswide_log_append("[task-yield]\n"); return COS_API_OK; }
int cos_task_get_count(uint64_t* count) { if (!count) return COS_API_INVALID_ARG; *count = 2; return COS_API_OK; }
int cos_network_set_mode(const char* mode) { if (!mode) return COS_API_INVALID_ARG; oswide_log_append("[net-mode] "); oswide_log_append(mode); oswide_log_append("\n"); return COS_API_OK; }
int cos_power_request(const char* action) {
    if (!action) return COS_API_INVALID_ARG;
    oswide_log_append("[power] "); oswide_log_append(action); oswide_log_append("\n");
    if (strcmp(action, "low-power") == 0 || strcmp(action, "powersave") == 0) {
        g_power_low_power_hint = true;
    } else if (strcmp(action, "performance") == 0 || strcmp(action, "high-performance") == 0) {
        g_power_low_power_hint = false;
    } else if (strcmp(action, "charging") == 0) {
        g_power_charging = true;
    } else if (strcmp(action, "discharging") == 0) {
        g_power_charging = false;
    } else if (strcmp(action, "suspend") == 0 || strcmp(action, "sleep") == 0) {
        (void)acpi_power_suspend_to_state(3u);
    } else if (strcmp(action, "wake") == 0 || strcmp(action, "resume") == 0) {
        (void)acpi_power_resume_from_state(3u);
    }
    return COS_API_OK;
}
int cos_settings_set(const char* key, const char* value) { if (!key || !value) return COS_API_INVALID_ARG; oswide_log_append("[settings-set] "); oswide_log_append(key); oswide_log_append("="); oswide_log_append(value); oswide_log_append("\n"); return COS_API_OK; }
int cos_settings_get(const char* key, char* out, uint64_t size) { if (!key || !out || size == 0) return COS_API_INVALID_ARG; const char* v = "enabled"; size_t n = strlen(v); if (n >= size) n = (size_t)size - 1; memcpy(out, v, n); out[n] = '\0'; return COS_API_OK; }
