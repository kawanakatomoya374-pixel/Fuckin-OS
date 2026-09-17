/*
 * cos_errors.h - C-OS 4.0.8 alpha Unified Error Code System
 * Provides standardized error codes for all subsystems
 */

#ifndef COS_ERRORS_H
#define COS_ERRORS_H

#include <stdint.h>

// General error codes
typedef enum {
    COS_SUCCESS = 0,
    
    // System errors (0x1000-0x10FF)
    COS_ERR_NOT_INITIALIZED = 0x1000,
    COS_ERR_ALREADY_INITIALIZED = 0x1001,
    COS_ERR_INVALID_STATE = 0x1002,
    COS_ERR_OPERATION_FAILED = 0x1003,
    COS_ERR_TIMEOUT = 0x1004,
    COS_ERR_INTERRUPTED = 0x1005,
    
    // Memory errors (0x1100-0x11FF)
    COS_ERR_NO_MEMORY = 0x1100,
    COS_ERR_INVALID_POINTER = 0x1101,
    COS_ERR_BUFFER_OVERFLOW = 0x1102,
    COS_ERR_BUFFER_UNDERFLOW = 0x1103,
    COS_ERR_MEMORY_CORRUPTION = 0x1104,
    COS_ERR_OUT_OF_BOUNDS = 0x1105,
    COS_ERR_ALLOCATION_FAILED = 0x1106,
    COS_ERR_DOUBLE_FREE = 0x1107,
    
    // Argument errors (0x1200-0x12FF)
    COS_ERR_INVALID_ARGUMENT = 0x1200,
    COS_ERR_NULL_ARGUMENT = 0x1201,
    COS_ERR_INVALID_SIZE = 0x1202,
    COS_ERR_INVALID_HANDLE = 0x1203,
    COS_ERR_INVALID_FLAG = 0x1204,
    COS_ERR_ARGUMENT_OUT_OF_RANGE = 0x1205,
    
    // File system errors (0x1300-0x13FF)
    COS_ERR_FILE_NOT_FOUND = 0x1300,
    COS_ERR_FILE_EXISTS = 0x1301,
    COS_ERR_PATH_NOT_FOUND = 0x1302,
    COS_ERR_PERMISSION_DENIED = 0x1303,
    COS_ERR_FILE_TOO_LARGE = 0x1304,
    COS_ERR_DISK_FULL = 0x1305,
    COS_ERR_INVALID_PATH = 0x1306,
    COS_ERR_NOT_A_FILE = 0x1307,
    COS_ERR_NOT_A_DIRECTORY = 0x1308,
    COS_ERR_FILE_SYSTEM_CORRUPT = 0x1309,
    
    // Network errors (0x1400-0x14FF)
    COS_ERR_NETWORK_UNREACHABLE = 0x1400,
    COS_ERR_CONNECTION_REFUSED = 0x1401,
    COS_ERR_CONNECTION_TIMEOUT = 0x1402,
    COS_ERR_CONNECTION_RESET = 0x1403,
    COS_ERR_HOST_UNREACHABLE = 0x1404,
    
    // AI/ML errors (0x1500-0x15FF)
    COS_ERR_AI_NOT_INITIALIZED = 0x1500,
    COS_ERR_AI_MODEL_NOT_LOADED = 0x1501,
    COS_ERR_AI_TRAINING_FAILED = 0x1502,
    COS_ERR_AI_INFERENCE_FAILED = 0x1503,
    COS_ERR_AI_INVALID_MODEL = 0x1504,
    COS_ERR_AI_INSUFFICIENT_DATA = 0x1505,
    COS_ERR_AI_CONVERGENCE_FAILED = 0x1506,
    
    // Python/MicroPython errors (0x1600-0x16FF)
    COS_ERR_PYTHON_NOT_INITIALIZED = 0x1600,
    COS_ERR_PYTHON_EXECUTION_FAILED = 0x1601,
    COS_ERR_PYTHON_SYNTAX_ERROR = 0x1602,
    COS_ERR_PYTHON_RUNTIME_ERROR = 0x1603,
    COS_ERR_PYTHON_IMPORT_ERROR = 0x1604,
    COS_ERR_PYTHON_NAME_ERROR = 0x1605,
    COS_ERR_PYTHON_TYPE_ERROR = 0x1606,
    COS_ERR_PYTHON_VALUE_ERROR = 0x1607,
    COS_ERR_PYTHON_MEMORY_ERROR = 0x1608,
    COS_ERR_PYTHON_REPL_ERROR = 0x1609,
    COS_ERR_PYTHON_VFS_ERROR = 0x160A,
    COS_ERR_PYTHON_GC_ERROR = 0x160B,
    
    // Security errors (0x1700-0x17FF)
    COS_ERR_SECURITY_VIOLATION = 0x1700,
    COS_ERR_ACCESS_DENIED = 0x1701,
    COS_ERR_AUTHENTICATION_FAILED = 0x1702,
    COS_ERR_AUTHORIZATION_FAILED = 0x1703,
    COS_ERR_PERMISSION_DENIED = 0x1704,
    COS_ERR_SANDBOX_VIOLATION = 0x1705,
    COS_ERR_RESOURCE_LIMIT_EXCEEDED = 0x1706,
    
    // Hardware errors (0x1800-0x18FF)
    COS_ERR_HARDWARE_FAILURE = 0x1800,
    COS_ERR_DEVICE_NOT_FOUND = 0x1801,
    COS_ERR_DEVICE_BUSY = 0x1802,
    COS_ERR_IO_ERROR = 0x1803,
    COS_ERR_DMA_ERROR = 0x1804,
    COS_ERR_INTERRUPT_ERROR = 0x1805,
    
    // GUI errors (0x1900-0x19FF)
    COS_ERR_GUI_NOT_INITIALIZED = 0x1900,
    COS_ERR_WINDOW_NOT_FOUND = 0x1901,
    COS_ERR_INVALID_WINDOW_STATE = 0x1902,
    COS_ERR_GUI_RESOURCE_EXHAUSTED = 0x1903,
    COS_ERR_WIDGET_NOT_FOUND = 0x1904,
    
    // Generic errors (0x1F00-0x1FFF)
    COS_ERR_UNKNOWN = 0x1F00,
    COS_ERR_NOT_IMPLEMENTED = 0x1F01,
    COS_ERR_NOT_SUPPORTED = 0x1F02,
    COS_ERR_INTERNAL_ERROR = 0x1F03,
    COS_ERR_FATAL_ERROR = 0x1F04
} cos_error_t;

// Error severity levels
typedef enum {
    COS_SEVERITY_INFO = 0,
    COS_SEVERITY_WARNING = 1,
    COS_SEVERITY_ERROR = 2,
    COS_SEVERITY_CRITICAL = 3,
    COS_SEVERITY_FATAL = 4
} cos_severity_t;

// Error context structure
typedef struct {
    cos_error_t error_code;
    cos_severity_t severity;
    const char* subsystem;
    const char* function;
    const char* file;
    int line;
    const char* message;
    uint64_t timestamp;
} cos_error_context_t;

// Global error handling
extern cos_error_context_t g_last_error;

// Error handling macros
#define COS_SET_ERROR(code, severity, subsystem, msg) do { \
    g_last_error.error_code = (code); \
    g_last_error.severity = (severity); \
    g_last_error.subsystem = (subsystem); \
    g_last_error.function = __func__; \
    g_last_error.file = __FILE__; \
    g_last_error.line = __LINE__; \
    g_last_error.message = (msg); \
    g_last_error.timestamp = get_timer_ticks(); \
} while(0)

#define COS_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "UNKNOWN", msg)
#define COS_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "UNKNOWN", msg)
#define COS_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "UNKNOWN", msg)

// Subsystem-specific error macros
#define AI_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "AI", msg)
#define AI_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "AI", msg)
#define AI_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "AI", msg)

#define PYTHON_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "PYTHON", msg)
#define PYTHON_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "PYTHON", msg)
#define PYTHON_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "PYTHON", msg)

#define VFS_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "VFS", msg)
#define VFS_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "VFS", msg)
#define VFS_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "VFS", msg)

#define GC_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "GC", msg)
#define GC_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "GC", msg)
#define GC_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "GC", msg)

#define GUI_ERROR(code, msg) COS_SET_ERROR(code, COS_SEVERITY_ERROR, "GUI", msg)
#define GUI_WARNING(msg) COS_SET_ERROR(COS_ERR_OPERATION_FAILED, COS_SEVERITY_WARNING, "GUI", msg)
#define GUI_FATAL(msg) COS_SET_ERROR(COS_ERR_FATAL_ERROR, COS_SEVERITY_FATAL, "GUI", msg)

// Error utility functions
const char* cos_error_string(cos_error_t error_code);
const char* cos_severity_string(cos_severity_t severity);
void cos_print_error(const cos_error_context_t* error);
void cos_clear_error(void);
cos_error_t cos_get_last_error(void);
const char* cos_get_last_error_string(void);

// Return value checking macros
#define COS_CHECK_RESULT(expr) do { \
    cos_error_t _result = (expr); \
    if (_result != COS_SUCCESS) { \
        return _result; \
    } \
} while(0)

#define COS_CHECK_RESULT_MSG(expr, msg) do { \
    cos_error_t _result = (expr); \
    if (_result != COS_SUCCESS) { \
        COS_ERROR(_result, msg); \
        return _result; \
    } \
} while(0)

#define COS_CHECK_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        COS_ERROR(COS_ERR_NULL_ARGUMENT, "Null pointer argument"); \
        return COS_ERR_NULL_ARGUMENT; \
    } \
} while(0)

#define COS_CHECK_NULL_MSG(ptr, msg) do { \
    if ((ptr) == NULL) { \
        COS_ERROR(COS_ERR_NULL_ARGUMENT, msg); \
        return COS_ERR_NULL_ARGUMENT; \
    } \
} while(0)

#endif // COS_ERRORS_H
