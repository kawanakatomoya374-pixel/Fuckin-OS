#ifndef MK_KERNEL_H
#define MK_KERNEL_H

#include "types.h"
#include "mk_core.h"
#include "mk_ipc.h"
#include "mk_memory.h"

// Advanced microkernel constants
#define MK_KERNEL_MAGIC 0x4B524E4C  // "KRNL"
#define MK_KERNEL_VERSION_3_0 0x0300
#define MK_MAX_SUBSYSTEMS 32
#define MK_MAX_SERVICES 64
#define MK_MAX_PROCESSES 1024
#define MK_MAX_THREADS 4096
#define MK_MAX_MODULES 256
#define MK_DEFAULT_QUANTUM 10
#define MK_CONTEXT_SWITCH_TIME 1
#define MK_INTERRUPT_LATENCY 10
#define MK_MAX_INTERRUPT_LATENCY 100
#define MK_MEMORY_ALIGNMENT 16
#define MK_PAGE_SIZE 4096
#define MK_HEAP_SIZE (64 * 1024 * 1024)  // 64MB
#define MK_STACK_SIZE (64 * 1024)  // 64KB

// Security levels
#define MK_SECURITY_LOW 0
#define MK_SECURITY_MEDIUM 1
#define MK_SECURITY_HIGH 2
#define MK_SECURITY_CRITICAL 3

// Subsystem types
#define MK_SUBSYSTEM_MEMORY 1
#define MK_SUBSYSTEM_IPC 2
#define MK_SUBSYSTEM_SCHEDULER 3
#define MK_SUBSYSTEM_SECURITY 4
#define MK_SUBSYSTEM_MODULES 5
#define MK_SUBSYSTEM_FILESYSTEM 6
#define MK_SUBSYSTEM_NETWORK 7
#define MK_SUBSYSTEM_GRAPHICS 8
#define MK_SUBSYSTEM_AUDIO 9
#define MK_SUBSYSTEM_INPUT 10
#define MK_SUBSYSTEM_DISTRIBUTED 11
#define MK_SUBSYSTEM_REALTIME 12
#define MK_SUBSYSTEM_AI 13
#define MK_SUBSYSTEM_QUANTUM 14

// Service types
#define MK_SERVICE_PROCESS_MANAGER 1
#define MK_SERVICE_MEMORY_MANAGER 2
#define MK_SERVICE_FILESYSTEM_MANAGER 3
#define MK_SERVICE_DEVICE_MANAGER 4
#define MK_SERVICE_NETWORK_MANAGER 5
#define MK_SERVICE_SECURITY_MANAGER 6
#define MK_SERVICE_MODULE_MANAGER 7
#define MK_SERVICE_DISTRIBUTED_COORDINATOR 8
#define MK_SERVICE_REALTIME_SCHEDULER 9
#define MK_SERVICE_AI_ENGINE 10
#define MK_SERVICE_QUANTUM_PROCESSOR 11
#define MK_SERVICE_PERFORMANCE_MONITOR 12
#define MK_SERVICE_DEBUG_SERVER 13

// Process types
#define MK_PROCESS_KERNEL 0
#define MK_PROCESS_SYSTEM 1
#define MK_PROCESS_SERVICE 2
#define MK_PROCESS_DRIVER 3
#define MK_PROCESS_USER 4
#define MK_PROCESS_REALTIME 5

// Module types
#define MK_MODULE_CORE 1
#define MK_MODULE_DRIVER 2
#define MK_MODULE_FILESYSTEM 3
#define MK_MODULE_NETWORK 4
#define MK_MODULE_MULTIMEDIA 5
#define MK_MODULE_AI 6
#define MK_MODULE_QUANTUM 7
#define MK_MODULE_CRYPTO 8
#define MK_MODULE_UTILITY 9

// Subsystem states
#define MK_SUBSYSTEM_INITIALIZING 0
#define MK_SUBSYSTEM_RUNNING 1
#define MK_SUBSYSTEM_STOPPING 2
#define MK_SUBSYSTEM_STOPPED 3
#define MK_SUBSYSTEM_ERROR 4

// Service states
#define MK_SERVICE_STOPPED 0
#define MK_SERVICE_STARTING 1
#define MK_SERVICE_RUNNING 2
#define MK_SERVICE_STOPPING 3
#define MK_SERVICE_ERROR 4

// Kernel states
#define MK_KERNEL_INITIALIZING 0
#define MK_KERNEL_RUNNING 1
#define MK_KERNEL_SHUTTING_DOWN 2
#define MK_KERNEL_PANIC 3
#define MK_KERNEL_RECOVERY 4

// Forward declarations
typedef struct mk_kernel_state mk_kernel_state_t;
typedef struct mk_kernel_config mk_kernel_config_t;
typedef struct mk_subsystem mk_subsystem_t;
typedef struct mk_service mk_service_t;
typedef struct mk_security_context mk_security_context_t;
typedef struct mk_module_registry mk_module_registry_t;
typedef struct mk_distributed_manager mk_distributed_manager_t;
typedef struct mk_realtime_scheduler mk_realtime_scheduler_t;
typedef struct mk_resource_manager mk_resource_manager_t;
typedef struct mk_performance_monitor mk_performance_monitor_t;
typedef struct mk_debug_system mk_debug_system_t;

// Function types
typedef void (*mk_subsystem_init_func_t)(void);
typedef void (*mk_subsystem_cleanup_func_t)(void);
typedef void (*mk_service_main_func_t)(void);

// Kernel state structure
struct mk_kernel_state {
    uint64_t magic;
    uint64_t version;
    uint64_t state;
    uint64_t boot_time;
    uint64_t uptime;
    uint64_t total_processes;
    uint64_t active_processes;
    uint64_t total_threads;
    uint64_t active_threads;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t allocated_memory;
    uint64_t kernel_load;
    uint64_t system_load;
    uint64_t interrupt_count;
    uint64_t context_switches;
    uint64_t page_faults;
    uint64_t system_calls;
};

// Kernel configuration structure
struct mk_kernel_config {
    uint64_t max_processes;
    uint64_t max_threads;
    uint64_t max_modules;
    uint64_t max_services;
    uint64_t security_level;
    bool realtime_enabled;
    bool distributed_enabled;
    bool debug_enabled;
    bool performance_monitoring;
    bool auto_recovery;
    bool hot_swapping;
    bool memory_protection;
    bool process_isolation;
    bool resource_limits;
    bool audit_logging;
    uint64_t scheduler_quantum;
    uint64_t context_switch_time;
    uint64_t interrupt_latency;
    uint64_t max_interrupt_latency;
    bool password_policy;
    bool encryption_enabled;
    bool digital_signatures;
    bool secure_boot;
    bool tpm_support;
    bool sandbox_enforcement;
    uint64_t memory_alignment;
    uint64_t page_size;
    uint64_t heap_size;
    uint64_t stack_size;
    bool garbage_collection;
    bool memory_compression;
    bool swap_enabled;
};

// Subsystem structure
struct mk_subsystem {
    char name[32];
    uint64_t type;
    uint64_t state;
    mk_subsystem_init_func_t init_func;
    mk_subsystem_cleanup_func_t cleanup_func;
    uint64_t start_time;
    uint64_t uptime;
    uint64_t resource_usage;
    uint64_t error_count;
    uint64_t restart_count;
    uint64_t priority;
    bool critical;
    bool auto_restart;
    uint64_t dependencies[16];
    uint64_t dependency_count;
};

// Service structure
struct mk_service {
    char name[32];
    uint64_t type;
    uint64_t state;
    mk_service_main_func_t main_func;
    uint64_t priority;
    uint64_t pid;
    uint64_t start_time;
    uint64_t uptime;
    uint64_t resource_usage;
    uint64_t request_count;
    uint64_t error_count;
    uint64_t restart_count;
    bool auto_restart;
    bool critical;
    uint64_t timeout;
    uint64_t max_memory;
    uint64_t max_cpu;
};

// Security context structure
struct mk_security_context {
    uint64_t security_level;
    bool encryption_enabled;
    bool authentication_required;
    bool audit_enabled;
    uint64_t policy_count;
    uint64_t threat_count;
    uint64_t violation_count;
    uint64_t last_scan_time;
    uint64_t scan_interval;
    bool sandbox_enabled;
    bool isolation_enabled;
    uint64_t trusted_processes[256];
    uint64_t trusted_process_count;
    uint64_t blacklisted_processes[256];
    uint64_t blacklisted_process_count;
};

// Module registry structure
struct mk_module_registry {
    uint64_t module_count;
    uint64_t loaded_modules;
    uint64_t failed_modules;
    uint64_t hot_swappable_modules;
    bool auto_loading;
    bool dependency_resolution;
    uint64_t load_order[256];
    uint64_t load_order_count;
};

// Distributed manager structure
struct mk_distributed_manager {
    bool enabled;
    uint64_t node_count;
    uint64_t active_nodes;
    uint64_t task_count;
    uint64_t completed_tasks;
    uint64_t failed_tasks;
    uint64_t network_bandwidth;
    uint64_t latency;
    bool load_balancing;
    bool fault_tolerance;
    uint64_t sync_interval;
    uint64_t last_sync_time;
};

// Realtime scheduler structure
struct mk_realtime_scheduler {
    bool enabled;
    uint64_t realtime_task_count;
    uint64_t priority_levels;
    uint64_t quantum_sizes[16];
    uint64_t deadline_misses;
    uint64_t context_switches;
    uint64_t max_latency;
    uint64_t average_latency;
    bool preemptive;
    bool priority_inheritance;
    uint64_t watchdog_timeout;
};

// Resource manager structure
struct mk_resource_manager {
    uint64_t total_memory;
    uint64_t allocated_memory;
    uint64_t free_memory;
    uint64_t total_cpu;
    uint64_t used_cpu;
    uint64_t free_cpu;
    uint64_t total_io;
    uint64_t used_io;
    uint64_t free_io;
    uint64_t total_network;
    uint64_t used_network;
    uint64_t free_network;
    bool resource_limits;
    bool resource_monitoring;
    uint64_t allocation_failures;
    uint64_t deallocation_failures;
};

// Performance monitor structure
struct mk_performance_monitor {
    bool enabled;
    uint64_t cpu_usage;
    uint64_t memory_usage;
    uint64_t io_usage;
    uint64_t network_usage;
    uint64_t context_switch_rate;
    uint64_t interrupt_rate;
    uint64_t system_call_rate;
    uint64_t page_fault_rate;
    uint64_t throughput;
    uint64_t latency;
    uint64_t error_rate;
    uint64_t warning_count;
    uint64_t critical_count;
    uint64_t last_update_time;
};

// Debug system structure
struct mk_debug_system {
    bool enabled;
    uint64_t log_level;
    uint64_t log_count;
    uint64_t error_count;
    uint64_t warning_count;
    uint64_t info_count;
    uint64_t debug_count;
    bool trace_enabled;
    bool profiling_enabled;
    bool memory_debugging;
    bool performance_debugging;
    uint64_t breakpoints[64];
    uint64_t breakpoint_count;
    bool kernel_debugging;
    bool user_debugging;
};

// Kernel functions
void mk_kernel_init(void);
void mk_kernel_main_loop(void);
mk_kernel_state_t* mk_kernel_get_state(void);
mk_kernel_config_t* mk_kernel_get_config(void);

// Subsystem functions
void mk_kernel_register_subsystem(const char* name, uint64_t type, 
                                 mk_subsystem_init_func_t init_func,
                                 mk_subsystem_cleanup_func_t cleanup_func);
mk_subsystem_t* mk_kernel_get_subsystem(const char* name);
int mk_kernel_start_subsystem(const char* name);
int mk_kernel_stop_subsystem(const char* name);
int mk_kernel_restart_subsystem(const char* name);

// Service functions
void mk_kernel_register_service(const char* name, uint64_t type,
                               mk_service_main_func_t main_func, uint64_t priority);
mk_service_t* mk_kernel_get_service(const char* name);
int mk_kernel_start_service(const char* name);
int mk_kernel_stop_service(const char* name);
int mk_kernel_restart_service(const char* name);

// Module functions
int mk_kernel_load_module(const char* module_name);
int mk_kernel_unload_module(const char* module_name);
int mk_kernel_reload_module(const char* module_name);
bool mk_kernel_is_module_loaded(const char* module_name);

// Security functions
int mk_kernel_set_security_level(uint64_t level);
int mk_kernel_enable_sandbox(void);
int mk_kernel_disable_sandbox(void);
int mk_kernel_add_trusted_process(uint64_t pid);
int mk_kernel_remove_trusted_process(uint64_t pid);
int mk_kernel_add_blacklisted_process(uint64_t pid);
int mk_kernel_remove_blacklisted_process(uint64_t pid);

// Distributed functions
int mk_kernel_enable_distributed(void);
int mk_kernel_disable_distributed(void);
int mk_kernel_add_node(uint64_t node_id);
int mk_kernel_remove_node(uint64_t node_id);
int mk_kernel_distribute_task(uint64_t task_id, uint64_t node_id);

// Realtime functions
int mk_kernel_enable_realtime(void);
int mk_kernel_disable_realtime(void);
int mk_kernel_add_realtime_task(uint64_t task_id, uint64_t priority, uint64_t deadline);
int mk_kernel_remove_realtime_task(uint64_t task_id);
int mk_kernel_set_realtime_priority(uint64_t task_id, uint64_t priority);

// Resource functions
int mk_kernel_set_memory_limit(uint64_t limit);
int mk_kernel_set_cpu_limit(uint64_t limit);
int mk_kernel_set_io_limit(uint64_t limit);
int mk_kernel_set_network_limit(uint64_t limit);
uint64_t mk_kernel_get_memory_usage(void);
uint64_t mk_kernel_get_cpu_usage(void);
uint64_t mk_kernel_get_io_usage(void);
uint64_t mk_kernel_get_network_usage(void);

// Performance functions
void mk_kernel_enable_performance_monitoring(void);
void mk_kernel_disable_performance_monitoring(void);
void mk_kernel_reset_performance_counters(void);
uint64_t mk_kernel_get_cpu_usage(void);
uint64_t mk_kernel_get_memory_usage(void);
uint64_t mk_kernel_get_throughput(void);
uint64_t mk_kernel_get_latency(void);

// Debug functions
void mk_kernel_enable_debug(void);
void mk_kernel_disable_debug(void);
void mk_kernel_set_log_level(uint64_t level);
void mk_kernel_log(uint64_t level, const char* message);
void mk_kernel_log_error(const char* message);
void mk_kernel_log_warning(const char* message);
void mk_kernel_log_info(const char* message);
void mk_kernel_log_debug(const char* message);

// System functions
int mk_kernel_shutdown(void);
int mk_kernel_reboot(void);
int mk_kernel_panic(const char* message);
int mk_kernel_crash_dump(void);
int mk_kernel_system_info(void);

// Configuration functions
int mk_kernel_load_config(const char* config_file);
int mk_kernel_save_config(const char* config_file);
int mk_kernel_set_config_value(const char* key, const char* value);
const char* mk_kernel_get_config_value(const char* key);

// Utility functions
uint64_t mk_kernel_get_timestamp(void);
uint64_t mk_kernel_get_uptime(void);
uint64_t mk_kernel_get_version(void);
const char* mk_kernel_get_version_string(void);
bool mk_kernel_is_debug_mode(void);
bool mk_kernel_is_safe_mode(void);
bool mk_kernel_is_recovery_mode(void);

#endif // MK_KERNEL_H
