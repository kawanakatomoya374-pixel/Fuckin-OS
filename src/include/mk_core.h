#ifndef MK_CORE_H
#define MK_CORE_H

#include "types.h"

// Microkernel constants
#define MK_MAGIC 0x4D4B5F43  // "MK_C"
#define MK_VERSION_2_0 0x0200
#define MK_MAX_SERVERS 64
#define MK_MAX_DRIVERS 128
#define MK_MAX_PROCESSES 256
#define MK_MAX_QUEUES 256
#define MK_MAX_SYSCALLS 64
#define MK_KERNEL_PID 1
#define MK_MIN_PID 1000

// Process and server states
#define MK_STATE_INITIALIZING 0
#define MK_STATE_RUNNING 1
#define MK_STATE_SHUTTING_DOWN 2
#define MK_STATE_ERROR 3

#define MK_PROCESS_READY 0
#define MK_PROCESS_RUNNING 1
#define MK_PROCESS_BLOCKED 2
#define MK_PROCESS_TERMINATED 3

#define MK_SERVER_OFFLINE 0
#define MK_SERVER_STARTING 1
#define MK_SERVER_ONLINE 2
#define MK_SERVER_ERROR 3

#define MK_DRIVER_OFFLINE 0
#define MK_DRIVER_INITIALIZING 1
#define MK_DRIVER_RUNNING 2
#define MK_DRIVER_ERROR 3

// Server types
#define MK_SERVER_MEMORY 1
#define MK_SERVER_FILESYSTEM 2
#define MK_SERVER_DEVICE_MANAGER 3
#define MK_SERVER_NETWORK 4
#define MK_SERVER_GUI 5
#define MK_SERVER_AUDIO 6
#define MK_SERVER_MEDIA 7
#define MK_SERVER_SECURITY 8

// Driver types
#define MK_DRIVER_KEYBOARD 1
#define MK_DRIVER_MOUSE 2
#define MK_DRIVER_VGA 3
#define MK_DRIVER_AUDIO 4
#define MK_DRIVER_NETWORK 5
#define MK_DRIVER_STORAGE 6
#define MK_DRIVER_USB 7

// Process types
#define MK_PROCESS_KERNEL 0
#define MK_PROCESS_SERVER 1
#define MK_PROCESS_DRIVER 2
#define MK_PROCESS_USER 3

// Priorities
#define MK_PRIORITY_IDLE 0
#define MK_PRIORITY_LOW 1
#define MK_PRIORITY_NORMAL 2
#define MK_PRIORITY_HIGH 3
#define MK_PRIORITY_REALTIME 4

// Memory constants
#define MK_PROCESS_STACK_SIZE (64 * 1024)  // 64KB
#define MK_PROCESS_HEAP_SIZE (1024 * 1024) // 1MB
#define MK_DEFAULT_QUANTUM 10
#define MK_SUCCESS 0
#define MK_ERROR -1

// System call numbers
#define MK_SYSCALL_SEND 1
#define MK_SYSCALL_RECEIVE 2
#define MK_SYSCALL_ALLOC 3
#define MK_SYSCALL_FREE 4
#define MK_SYSCALL_OPEN 5
#define MK_SYSCALL_READ 6
#define MK_SYSCALL_WRITE 7
#define MK_SYSCALL_CLOSE 8
#define MK_SYSCALL_IOCTL 9
#define MK_SYSCALL_FORK 10
#define MK_SYSCALL_EXEC 11
#define MK_SYSCALL_EXIT 12

// Forward declarations
typedef struct mk_process mk_process_t;
typedef struct mk_server mk_server_t;
typedef struct mk_driver mk_driver_t;
typedef struct mk_state mk_state_t;

// Message structure
typedef struct {
    uint64_t sender_pid;
    uint64_t receiver_pid;
    uint64_t message_type;
    uint64_t length;
    uint8_t data[256];
} mk_message_t;

// Message queue
typedef struct {
    uint64_t pid;
    mk_message_t messages[32];
    uint64_t head;
    uint64_t tail;
    uint64_t count;
} mk_message_queue_t;

// Process structure
struct mk_process {
    uint64_t pid;
    uint64_t ppid;
    uint64_t type;
    char name[32];
    uint64_t state;
    void* entry_point;
    void* stack_base;
    void* heap_base;
    uint64_t priority;
    uint64_t quantum;
    uint64_t time_used;
    uint64_t registers[8]; // EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP
};

// Server structure
struct mk_server {
    uint64_t pid;
    char name[32];
    uint64_t type;
    uint64_t state;
    void (*main_func)(void);
    uint64_t message_count;
    uint64_t error_count;
};

// Driver structure
struct mk_driver {
    uint64_t pid;
    char name[32];
    uint64_t type;
    uint64_t state;
    int (*init_func)(void);
    void (*cleanup_func)(void);
    uint64_t interrupt_count;
    uint64_t error_count;
};

// Microkernel state
struct mk_state {
    uint64_t magic;
    uint64_t version;
    uint64_t state;
    uint64_t current_process;
    uint64_t uptime;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t process_count;
    uint64_t server_count;
    uint64_t driver_count;
};

// Function types
typedef void (*mk_server_main_func_t)(void);
typedef int (*mk_driver_init_func_t)(void);
typedef void (*mk_driver_cleanup_func_t)(void);
typedef void (*mk_syscall_handler_t)(mk_process_t* process, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// Microkernel core functions
void mk_init(void);
void mk_main_loop(void);
mk_state_t* mk_get_state(void);

// Server management
uint64_t mk_create_server(const char* name, uint64_t type, mk_server_main_func_t main_func);
mk_server_t* mk_find_server(const char* name);

// Driver management
uint64_t mk_create_driver(const char* name, uint64_t type, mk_driver_init_func_t init_func);
mk_driver_t* mk_find_driver(const char* name);

// Process management
uint64_t mk_create_process(const char* name, uint64_t type, void* entry_point);

// System call registration
void mk_register_syscall(uint64_t syscall_num, mk_syscall_handler_t handler);

// Scheduling
void mk_yield(void);

#endif // MK_CORE_H
