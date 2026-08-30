/*
 * config_manager.c - Unified Configuration Management System
 * C-OS 4.0.8 alpha Configuration Management Implementation
 * 
 * Provides centralized configuration storage and retrieval for all C-OS 4.0.8 alpha components.
 * Implements persistent storage and runtime configuration management.
 */

#include "types.h"
#include "io.h"
#include "serial.h"
#include "storage.h"
#include "sync.h"
#include <stdint.h>
#include <string.h>

extern uint64_t get_timer_ticks(void);
void config_log_error(const char* error);
void config_log_info(const char* message);

// Configuration system definitions
#define CONFIG_STATUS_OK 0
#define CONFIG_STATUS_ERROR -1
#define CONFIG_MAX_ENTRIES 64
#define CONFIG_MAX_KEY_LENGTH 64
#define CONFIG_MAX_VALUE_LENGTH 256

// Configuration entry structure
typedef struct {
    char key[CONFIG_MAX_KEY_LENGTH];
    char value[CONFIG_MAX_VALUE_LENGTH];
    uint64_t timestamp;
    bool persistent;
    bool dirty;
} config_entry_t;

// Configuration statistics
typedef struct {
    uint64_t total_entries;
    uint64_t persistent_entries;
    uint64_t dirty_entries;
    uint64_t read_operations;
    uint64_t write_operations;
    uint64_t saves_to_storage;
} config_stats_t;

// Global configuration state
static config_entry_t config_entries[CONFIG_MAX_ENTRIES];
static int config_entry_count = 0;
static config_stats_t config_stats = {0};
static bool config_initialized = false;
static bool config_not_init_warned = false;

// In-memory persistence snapshot (used when no filesystem backend is available)
static config_entry_t config_storage_snapshot[CONFIG_MAX_ENTRIES];
static int config_storage_snapshot_count = 0;

#define CONFIG_STORAGE_PATH "/config.txt"
#define CONFIG_STORAGE_MAX_BLOB 32768

static char config_storage_blob[CONFIG_STORAGE_MAX_BLOB];

static void config_copy_string(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void config_append_char(char** out, size_t* remaining, char c) {
    if (!out || !*out || !remaining || *remaining <= 1) return;
    **out = c;
    (*out)++;
    (*remaining)--;
    **out = '\0';
}

static void config_append_str(char** out, size_t* remaining, const char* s) {
    if (!s) s = "";
    while (*s && *remaining > 1) {
        config_append_char(out, remaining, *s++);
    }
}

static void config_append_u64(char** out, size_t* remaining, uint64_t value) {
    char tmp[32];
    int pos = 0;
    if (value == 0) {
        config_append_char(out, remaining, '0');
        return;
    }
    while (value > 0 && pos < (int)sizeof(tmp) - 1) {
        tmp[pos++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (pos-- > 0) {
        config_append_char(out, remaining, tmp[pos]);
    }
}

static uint64_t config_parse_u64(const char* s) {
    uint64_t v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10ULL + (uint64_t)(*s - '0');
        ++s;
    }
    return v;
}

static int config_serialize_to_blob(char* blob, size_t blob_size) {
    char* out = blob;
    size_t remaining = blob_size;
    if (!blob || blob_size == 0) return CONFIG_STATUS_ERROR;
    blob[0] = '\0';

    for (int i = 0; i < config_entry_count; ++i) {
        const config_entry_t* e = &config_entries[i];
        if (!e->key[0]) continue;
        config_append_str(&out, &remaining, e->key);
        config_append_char(&out, &remaining, '\t');
        config_append_u64(&out, &remaining, e->timestamp);
        config_append_char(&out, &remaining, '\t');
        config_append_char(&out, &remaining, e->persistent ? '1' : '0');
        config_append_char(&out, &remaining, '\t');
        config_append_str(&out, &remaining, e->value);
        config_append_char(&out, &remaining, '\n');
        if (remaining <= 1) {
            config_log_error("Configuration blob full");
            return CONFIG_STATUS_ERROR;
        }
    }
    return CONFIG_STATUS_OK;
}

static int config_deserialize_from_blob(const char* blob, size_t blob_size) {
    size_t pos = 0;
    if (!blob || blob_size == 0) return CONFIG_STATUS_ERROR;

    while (pos < blob_size && config_entry_count < CONFIG_MAX_ENTRIES) {
        char key[CONFIG_MAX_KEY_LENGTH] = {0};
        char value[CONFIG_MAX_VALUE_LENGTH] = {0};
        char ts_buf[32] = {0};
        char persistent_buf[8] = {0};
        size_t k = 0, t = 0, p = 0, v = 0;
        int stage = 0;

        while (pos < blob_size && blob[pos] == '\n') pos++;
        if (pos >= blob_size || blob[pos] == '\0') break;

        while (pos < blob_size) {
            char c = blob[pos++];
            if (c == '\n' || c == '\0') break;
            if (stage == 0) {
                if (c == '\t') { stage = 1; continue; }
                if (k + 1 < sizeof(key)) key[k++] = c;
            } else if (stage == 1) {
                if (c == '\t') { stage = 2; continue; }
                if (t + 1 < sizeof(ts_buf)) ts_buf[t++] = c;
            } else if (stage == 2) {
                if (c == '\t') { stage = 3; continue; }
                if (p + 1 < sizeof(persistent_buf)) persistent_buf[p++] = c;
            } else {
                if (v + 1 < sizeof(value)) value[v++] = c;
            }
        }

        if (key[0]) {
            config_entry_t* entry = &config_entries[config_entry_count++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->key, sizeof(entry->key), "%s", key);
            snprintf(entry->value, sizeof(entry->value), "%s", value);
            entry->timestamp = config_parse_u64(ts_buf);
            entry->persistent = (persistent_buf[0] == '1');
            entry->dirty = false;
        }
    }
    return CONFIG_STATUS_OK;
}

// Configuration logging functions
void config_log_error(const char* error) {
    serial_puts("[CONFIG] ERROR: ");
    serial_puts(error);
    serial_puts("\n");
}

void config_log_info(const char* message) {
    serial_puts("[CONFIG] INFO: ");
    serial_puts(message);
    serial_puts("\n");
}

// Find configuration entry
static config_entry_t* config_find_entry(const char* key) {
    for (int i = 0; i < config_entry_count; i++) {
        if (strcmp(config_entries[i].key, key) == 0) {
            return &config_entries[i];
        }
    }
    return NULL;
}

// Add configuration entry
static int config_add_entry(const char* key, const char* value, bool persistent) {
    if (config_entry_count >= CONFIG_MAX_ENTRIES) {
        config_log_error("Configuration table full");
        return CONFIG_STATUS_ERROR;
    }
    
    config_entry_t* entry = &config_entries[config_entry_count];
    snprintf(entry->key, sizeof(entry->key), "%s", key ? key : "");
    snprintf(entry->value, sizeof(entry->value), "%s", value ? value : "");
    entry->timestamp = get_timer_ticks();
    entry->persistent = persistent;
    entry->dirty = true;
    
    config_entry_count++;
    config_stats.total_entries++;
    if (persistent) {
        config_stats.persistent_entries++;
    }
    config_stats.dirty_entries++;
    
    config_log_info("Configuration entry added");
    return CONFIG_STATUS_OK;
}

// Update configuration entry
static int config_update_entry(const char* key, const char* value) {
    config_entry_t* entry = config_find_entry(key);
    if (!entry) {
        /* New keys should be persistent so config set survives reboot. */
        return config_add_entry(key, value, true);
    }

    snprintf(entry->value, sizeof(entry->value), "%s", value ? value : "");
    entry->timestamp = get_timer_ticks();
    entry->dirty = true;

    config_stats.write_operations++;
    config_stats.dirty_entries++;

    config_log_info("Configuration entry updated");
    return CONFIG_STATUS_OK;
}

// Save configuration to persistent storage
static int config_save_to_storage(void) {
    config_log_info("Saving configuration to persistent storage");

    config_storage_snapshot_count = 0;
    for (int i = 0; i < config_entry_count && config_storage_snapshot_count < CONFIG_MAX_ENTRIES; i++) {
        config_storage_snapshot[config_storage_snapshot_count] = config_entries[i];
        config_storage_snapshot[config_storage_snapshot_count].dirty = false;
        config_storage_snapshot_count++;
        config_entries[i].dirty = false;
    }

    bool disk_ok = true;
    if (config_serialize_to_blob(config_storage_blob, sizeof(config_storage_blob)) == CONFIG_STATUS_OK) {
        disk_ok = storage_write_file(CONFIG_STORAGE_PATH, config_storage_blob, (uint64_t)strlen(config_storage_blob) + 1ULL);
        if (!disk_ok) {
            config_log_info("Persistent write failed; kept RAM snapshot");
        }
    }

    config_stats.saves_to_storage++;
    if (disk_ok) {
        config_log_info("Configuration saved to storage");
    }
    return CONFIG_STATUS_OK;
}

// Load configuration from persistent storage
static int config_load_from_storage(void) {
    uint64_t read_size = 0;
    config_log_info("Loading configuration from persistent storage");

    memset(config_storage_blob, 0, sizeof(config_storage_blob));
    config_entry_count = 0;

    if (storage_read_file(CONFIG_STORAGE_PATH, config_storage_blob, sizeof(config_storage_blob) - 1, &read_size) && read_size > 0) {
        config_deserialize_from_blob(config_storage_blob, (size_t)read_size);
    } else {
        for (int i = 0; i < config_storage_snapshot_count && config_entry_count < CONFIG_MAX_ENTRIES; i++) {
            config_entries[config_entry_count++] = config_storage_snapshot[i];
        }
    }

    config_stats.total_entries = config_entry_count;
    config_stats.persistent_entries = 0;
    config_stats.dirty_entries = 0;
    for (int i = 0; i < config_entry_count; i++) {
        if (config_entries[i].persistent) config_stats.persistent_entries++;
        if (config_entries[i].dirty) config_stats.dirty_entries++;
    }

    config_log_info("Configuration loaded from storage");
    return CONFIG_STATUS_OK;
}

static volatile bool config_init_in_progress = false;

// Initialize configuration manager
int config_manager_init(void) {
    /* This is called both at boot (once) and on-demand by the shell's
     * "config load"/"load" commands (intentionally - re-reading from
     * disk and rebuilding the table from scratch is the whole point of
     * a reload command). What it must never do is let two calls
     * overlap: without this guard, a reload racing the tail end of
     * boot init (or two cores both reaching their own init call) could
     * interleave memset()/config_add_entry() calls over the same
     * config_entries table. IRQ-disable is enough here (this kernel
     * doesn't run config_manager_init() from more than one core at
     * once in practice) and matches the guard style storage.c and
     * acpi_power.c already use for the same class of problem. */
    uint64_t irq_flags = sync_irq_save();
    if (config_init_in_progress) {
        sync_irq_restore(irq_flags);
        config_log_error("config_manager_init: already in progress, ignoring re-entrant call");
        return CONFIG_STATUS_ERROR;
    }
    config_init_in_progress = true;
    sync_irq_restore(irq_flags);

    config_log_info("Initializing configuration manager");
    
    // Clear configuration table
    memset(config_entries, 0, sizeof(config_entries));
    config_entry_count = 0;
    memset(&config_stats, 0, sizeof(config_stats));
    config_initialized = false;
    config_not_init_warned = false;
    
    // Load persistent configuration
    if (config_load_from_storage() != CONFIG_STATUS_OK) {
        config_log_error("Failed to load persistent configuration");
        config_init_in_progress = false;
        return CONFIG_STATUS_ERROR;
    }
    
    // Add default configuration entries only when they are absent
    if (!config_find_entry("system.name")) config_add_entry("system.name", "C-OS 4.0.8 alpha", true);
    if (!config_find_entry("system.version")) config_add_entry("system.version", "4.0.7", true);
    if (!config_find_entry("system.architecture")) config_add_entry("system.architecture", "x86_64", true);
    if (!config_find_entry("system.hostname")) config_add_entry("system.hostname", "cos", true);
    if (!config_find_entry("system.timezone")) config_add_entry("system.timezone", "UTC", true);
    if (!config_find_entry("system.volume")) config_add_entry("system.volume", "75", true);
    if (!config_find_entry("display.font_resolution")) config_add_entry("display.font_resolution", "12x16", true);
    if (!config_find_entry("display.resolution")) config_add_entry("display.resolution", "1024x768", true);
    if (!config_find_entry("gui.theme_idx")) config_add_entry("gui.theme_idx", "1", true);
    if (!config_find_entry("gui.wallpaper_idx")) config_add_entry("gui.wallpaper_idx", "0", true);
    if (!config_find_entry("gui.dark_mode")) config_add_entry("gui.dark_mode", "0", true);
    if (!config_find_entry("gui.font_scale")) config_add_entry("gui.font_scale", "1", true);
    if (!config_find_entry("gui.font_family")) config_add_entry("gui.font_family", "0", true);
    if (!config_find_entry("gui.window_animations")) config_add_entry("gui.window_animations", "1", true);
    if (!config_find_entry("gui.notifications_enabled")) config_add_entry("gui.notifications_enabled", "1", true);
    if (!config_find_entry("gui.terminal_autoscroll")) config_add_entry("gui.terminal_autoscroll", "1", true);
    if (!config_find_entry("gui.autostart_terminal")) config_add_entry("gui.autostart_terminal", "0", true);
    if (!config_find_entry("gui.autostart_file_manager")) config_add_entry("gui.autostart_file_manager", "0", true);
    if (!config_find_entry("gui.autostart_browser")) config_add_entry("gui.autostart_browser", "0", true);
    if (!config_find_entry("gui.language")) config_add_entry("gui.language", "0", true);
    if (!config_find_entry("keyboard.layout")) config_add_entry("keyboard.layout", "us", true);
    if (!config_find_entry("debug.enabled")) config_add_entry("debug.enabled", "true", true);
    
    config_initialized = true;
    config_log_info("Configuration manager initialized successfully");
    
    config_init_in_progress = false;
    return CONFIG_STATUS_OK;
}

static void config_warn_not_initialized_once(void) {
    if (!config_not_init_warned) {
        config_log_error("Configuration manager not initialized");
        config_not_init_warned = true;
    }
}

// Get configuration value
const char* config_get_string(const char* key) {
    if (!config_initialized) {
        config_warn_not_initialized_once();
        return NULL;
    }
    
    config_entry_t* entry = config_find_entry(key);
    return entry ? entry->value : NULL;
}

// Set configuration value
int config_set_string(const char* key, const char* value) {
    if (!config_initialized) {
        config_warn_not_initialized_once();
        return CONFIG_STATUS_ERROR;
    }
    
    config_stats.write_operations++;
    return config_update_entry(key, value);
}

// Save all dirty configuration
int config_save_all(void) {
    if (!config_initialized) {
        config_warn_not_initialized_once();
        return CONFIG_STATUS_ERROR;
    }
    
    return config_save_to_storage();
}

// Get configuration statistics
config_stats_t* config_get_stats(void) {
    return &config_stats;
}

// Cleanup configuration manager
int config_manager_cleanup(void) {
    config_log_info("Cleaning up configuration manager");
    
    // Save any pending changes
    config_save_all();
    
    // Clear configuration table
    memset(config_entries, 0, sizeof(config_entries));
    config_entry_count = 0;
    config_initialized = false;
    
    config_log_info("Configuration manager cleaned up successfully");
    return CONFIG_STATUS_OK;
}
