/*
 * cos_logging.h - C-OS 4.0.8 alpha Unified Logging System
 * Provides common logging macros for all subsystems
 */

#ifndef COS_LOGGING_H
#define COS_LOGGING_H

#include "cos_errors.h"
#include "timer.h"
#include <stdarg.h>

// Log levels
typedef enum {
    COS_LOG_DEBUG = 0,
    COS_LOG_INFO = 1,
    COS_LOG_WARNING = 2,
    COS_LOG_ERROR = 3,
    COS_LOG_CRITICAL = 4
} cos_log_level_t;

// Log entry structure
typedef struct {
    cos_log_level_t level;
    const char* subsystem;
    const char* function;
    const char* file;
    int line;
    const char* message;
    uint64_t timestamp;
} cos_log_entry_t;

// Logging configuration
typedef struct {
    cos_log_level_t min_level;
    bool enable_serial;
    bool enable_file;
    bool enable_buffer;
    size_t buffer_size;
    cos_log_entry_t* buffer;
    size_t buffer_pos;
    bool buffer_full;
} cos_log_config_t;

// Global logging configuration
extern cos_log_config_t g_log_config;

// Logging macros
#define COS_LOG(level, subsystem, msg, ...) do { \
    if (level >= g_log_config.min_level) { \
        cos_log_write(level, subsystem, __func__, __FILE__, __LINE__, msg, ##__VA_ARGS__); \
    } \
} while(0)

#define COS_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "DEBUG", msg, ##__VA_ARGS__)
#define COS_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "INFO", msg, ##__VA_ARGS__)
#define COS_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "WARNING", msg, ##__VA_ARGS__)
#define COS_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "ERROR", msg, ##__VA_ARGS__)
#define COS_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "CRITICAL", msg, ##__VA_ARGS__)

// Subsystem-specific logging macros
#define AI_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "AI", msg, ##__VA_ARGS__)
#define AI_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "AI", msg, ##__VA_ARGS__)
#define AI_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "AI", msg, ##__VA_ARGS__)
#define AI_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "AI", msg, ##__VA_ARGS__)
#define AI_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "AI", msg, ##__VA_ARGS__)

#define PYTHON_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "PYTHON", msg, ##__VA_ARGS__)
#define PYTHON_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "PYTHON", msg, ##__VA_ARGS__)
#define PYTHON_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "PYTHON", msg, ##__VA_ARGS__)
#define PYTHON_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "PYTHON", msg, ##__VA_ARGS__)
#define PYTHON_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "PYTHON", msg, ##__VA_ARGS__)

#define VFS_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "VFS", msg, ##__VA_ARGS__)
#define VFS_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "VFS", msg, ##__VA_ARGS__)
#define VFS_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "VFS", msg, ##__VA_ARGS__)
#define VFS_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "VFS", msg, ##__VA_ARGS__)
#define VFS_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "VFS", msg, ##__VA_ARGS__)

#define GC_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "GC", msg, ##__VA_ARGS__)
#define GC_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "GC", msg, ##__VA_ARGS__)
#define GC_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "GC", msg, ##__VA_ARGS__)
#define GC_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "GC", msg, ##__VA_ARGS__)
#define GC_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "GC", msg, ##__VA_ARGS__)

#define GUI_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "GUI", msg, ##__VA_ARGS__)
#define GUI_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "GUI", msg, ##__VA_ARGS__)
#define GUI_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "GUI", msg, ##__VA_ARGS__)
#define GUI_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "GUI", msg, ##__VA_ARGS__)
#define GUI_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "GUI", msg, ##__VA_ARGS__)

#define KERNEL_LOG_DEBUG(msg, ...) COS_LOG(COS_LOG_DEBUG, "KERNEL", msg, ##__VA_ARGS__)
#define KERNEL_LOG_INFO(msg, ...) COS_LOG(COS_LOG_INFO, "KERNEL", msg, ##__VA_ARGS__)
#define KERNEL_LOG_WARNING(msg, ...) COS_LOG(COS_LOG_WARNING, "KERNEL", msg, ##__VA_ARGS__)
#define KERNEL_LOG_ERROR(msg, ...) COS_LOG(COS_LOG_ERROR, "KERNEL", msg, ##__VA_ARGS__)
#define KERNEL_LOG_CRITICAL(msg, ...) COS_LOG(COS_LOG_CRITICAL, "KERNEL", msg, ##__VA_ARGS__)

// Function entry/exit logging
#define COS_LOG_ENTRY(subsystem) COS_LOG_DEBUG("Entering %s", __func__)
#define COS_LOG_EXIT(subsystem) COS_LOG_DEBUG("Exiting %s", __func__)
#define COS_LOG_ENTRY_PARAM(subsystem, param) COS_LOG_DEBUG("Entering %s with %s", __func__, param)
#define COS_LOG_EXIT_RESULT(subsystem, result) COS_LOG_DEBUG("Exiting %s with result: %d", __func__, result)

// Performance logging
#define COS_LOG_PERF_START(subsystem) uint64_t _perf_start = get_timer_ticks()
#define COS_LOG_PERF_END(subsystem, operation) do { \
    uint64_t _perf_end = get_timer_ticks(); \
    uint64_t _perf_duration = _perf_end - _perf_start; \
    COS_LOG_DEBUG("%s took %llu ticks", operation, _perf_duration); \
} while(0)

// Memory logging
#define COS_LOG_ALLOC(ptr, size, subsystem) COS_LOG_DEBUG("Allocated %zu bytes at %p in %s", size, ptr, subsystem)
#define COS_LOG_FREE(ptr, subsystem) COS_LOG_DEBUG("Freed memory at %p in %s", ptr, subsystem)
#define COS_LOG_REALLOC(old_ptr, new_ptr, size, subsystem) COS_LOG_DEBUG("Reallocated %p -> %p (%zu bytes) in %s", old_ptr, new_ptr, size, subsystem)

// Function prototypes
void cos_log_init(void);
void cos_log_cleanup(void);
void cos_log_set_level(cos_log_level_t level);
void cos_log_enable_serial(bool enable);
void cos_log_enable_file(bool enable);
void cos_log_enable_buffer(bool enable, size_t buffer_size);
void cos_log_write(cos_log_level_t level, const char* subsystem, const char* function, const char* file, int line, const char* message, ...);
void cos_log_flush(void);
cos_log_entry_t* cos_log_get_buffer(size_t* count);
void cos_log_clear_buffer(void);

// Log level strings
extern const char* cos_log_level_strings[];

// Log formatting macros
#define COS_LOG_FMT_LEVEL "[%s]"
#define COS_LOG_FMT_TIMESTAMP "[%llu]"
#define COS_LOG_FMT_SUBSYSTEM "[%s]"
#define COS_LOG_FMT_FUNCTION "%s()"
#define COS_LOG_FMT_FILE_LINE "%s:%d"
#define COS_LOG_FMT_MESSAGE "%s"

// Complete log format
#define COS_LOG_FULL_FORMAT COS_LOG_FMT_TIMESTAMP " " COS_LOG_FMT_LEVEL " " COS_LOG_FMT_SUBSYSTEM " " COS_LOG_FMT_FUNCTION ": " COS_LOG_FMT_MESSAGE

#endif // COS_LOGGING_H
