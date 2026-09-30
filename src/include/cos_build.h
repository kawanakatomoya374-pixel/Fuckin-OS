/*
 * cos_build.h - C-OS 4.0.8 alpha Build Configuration and Feature Flags
 * Centralized build configuration for all subsystems
 */

#ifndef COS_BUILD_H
#define COS_BUILD_H

// Feature flags - enable/disable subsystems
#define FEATURE_AI_ENGINE               1
#define FEATURE_MODERN_UI               1
#define FEATURE_ADVANCED_GUI            1

// Implementation levels
#define IMPLEMENTATION_LEVEL_STUB       0
#define IMPLEMENTATION_LEVEL_SIMULATION 1
#define IMPLEMENTATION_LEVEL_REAL       2

// Current implementation levels for each subsystem
#define AI_ENGINE_IMPLEMENTATION        IMPLEMENTATION_LEVEL_SIMULATION

// Build configuration
#define COS_BUILD_DEBUG                 1
#define COS_BUILD_RELEASE               0
#define COS_BUILD_PROFILE               0

// Memory configuration
#define COS_MEMORY_HEAP_SIZE            (2 * 1024 * 1024)    // 2MB
#define COS_MEMORY_STACK_SIZE           (64 * 1024)          // 64KB
#define COS_MEMORY_PAGE_SIZE            4096                 // 4KB

// AI Engine configuration
#define AI_ENGINE_MAX_MODELS             4
#define AI_ENGINE_MAX_LAYERS            16
#define AI_ENGINE_MAX_NEURONS_PER_LAYER 1024
#define AI_ENGINE_TRAINING_BATCH_SIZE   32
#define AI_ENGINE_LEARNING_RATE         0.001f

// VFS configuration
#define VFS_MAX_OPEN_FILES              32
#define VFS_MAX_MOUNT_POINTS            8
#define VFS_MAX_PATH_LENGTH             4096
#define VFS_MAX_FILENAME_LENGTH         256

// GUI configuration
#define GUI_MAX_WINDOWS                 16
#define GUI_MAX_WIDGETS_PER_WINDOW      64
#define GUI_MAX_EVENTS_PER_FRAME        32
#define GUI_DOUBLE_BUFFER_ENABLED       1

// Logging configuration
#define LOG_MAX_MESSAGE_LENGTH          512
#define LOG_BUFFER_SIZE                 256
#define LOG_ENABLE_SERIAL_OUTPUT        1
#define LOG_ENABLE_FILE_OUTPUT          0

// Error handling configuration
#define ERROR_STACK_TRACE_ENABLED       1
#define ERROR_DETAILED_MESSAGES         1
#define ERROR_RECOVERY_ENABLED          1

// Security configuration
#define SECURITY_SANDBOX_ENABLED        1
#define SECURITY_MEMORY_PROTECTION      1
#define SECURITY_INPUT_VALIDATION       1

// Performance configuration
#define PERFORMANCE_PROFILING_ENABLED   0
#define PERFORMANCE_METRICS_ENABLED     1
#define PERFORMANCE_CACHE_ENABLED        1

// Debug configuration
#define DEBUG_ASSERTIONS_ENABLED        1
#define DEBUG_MEMORY_TRACKING           0
#define DEBUG_VALIDATION_ENABLED        1

// Feature check macros
#define FEATURE_ENABLED(feature)         ((feature) != 0)
#define FEATURE_DISABLED(feature)        ((feature) == 0)

#define IMPLEMENTATION_IS_STUB(level)   ((level) == IMPLEMENTATION_LEVEL_STUB)
#define IMPLEMENTATION_IS_SIMULATION(level) ((level) == IMPLEMENTATION_LEVEL_SIMULATION)
#define IMPLEMENTATION_IS_REAL(level)    ((level) == IMPLEMENTATION_LEVEL_REAL)

// Conditional compilation macros
#if FEATURE_AI_ENGINE
    #define AI_ENGINE_AVAILABLE() 1
#else
    #define AI_ENGINE_AVAILABLE() 0
#endif

#if FEATURE_MODERN_UI
    #define MODERN_UI_AVAILABLE() 1
#else
    #define MODERN_UI_AVAILABLE() 0
#endif

#if FEATURE_ADVANCED_GUI
    #define ADVANCED_GUI_AVAILABLE() 1
#else
    #define ADVANCED_GUI_AVAILABLE() 0
#endif

// Implementation level checks
#define AI_ENGINE_IS_REAL()              IMPLEMENTATION_IS_REAL(AI_ENGINE_IMPLEMENTATION)
#define AI_ENGINE_IS_SIMULATION()        IMPLEMENTATION_IS_SIMULATION(AI_ENGINE_IMPLEMENTATION)
#define AI_ENGINE_IS_STUB()              IMPLEMENTATION_IS_STUB(AI_ENGINE_IMPLEMENTATION)

// Build type checks
#if COS_BUILD_DEBUG
    #define COS_IS_DEBUG_BUILD() 1
    #define COS_IS_RELEASE_BUILD() 0
#else
    #define COS_IS_DEBUG_BUILD() 0
    #define COS_IS_RELEASE_BUILD() 1
#endif

// Platform-specific configuration (64-bit only)
#ifdef __x86_64__
    #define COS_ARCH_X86 0
    #define COS_ARCH_X86_64 1
    #define COS_ARCH_ARM 0
#else
    #define COS_ARCH_X86 0
    #define COS_ARCH_X86_64 0
    #define COS_ARCH_ARM 0
#endif

// Compiler-specific configuration
#ifdef __GNUC__
    #define COS_COMPILER_GCC 1
    #define COS_COMPILER_CLANG 0
    #define COS_COMPILER_MSVC 0
#elif defined(__clang__)
    #define COS_COMPILER_GCC 0
    #define COS_COMPILER_CLANG 1
    #define COS_COMPILER_MSVC 0
#elif defined(_MSC_VER)
    #define COS_COMPILER_GCC 0
    #define COS_COMPILER_CLANG 0
    #define COS_COMPILER_MSVC 1
#else
    #define COS_COMPILER_GCC 0
    #define COS_COMPILER_CLANG 0
    #define COS_COMPILER_MSVC 0
#endif

// Version information
#define COS_VERSION_MAJOR                4
#define COS_VERSION_MINOR                0
#define COS_VERSION_PATCH                4
#define COS_VERSION_STRING               "4.0.5"
#define COS_VERSION_NUMBER               ((COS_VERSION_MAJOR << 16) | (COS_VERSION_MINOR << 8) | COS_VERSION_PATCH)

// Build information
#define COS_BUILD_DATE                   __DATE__
#define COS_BUILD_TIME                   __TIME__
#define COS_BUILD_COMPILER               "GCC"
#define COS_BUILD_PLATFORM               "x86_64"

#endif // COS_BUILD_H
