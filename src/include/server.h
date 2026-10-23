#ifndef SERVER_H
#define SERVER_H

#include "types.h"

#define MAX_SERVERS      32
#define SERVER_NAME_MAX  64
#define SERVER_STACK_SIZE 8192

typedef enum {
    SERVER_TYPE_SYSTEM = 0,
    SERVER_TYPE_DRIVER = 1,
    SERVER_TYPE_APPLICATION = 2,
    SERVER_TYPE_UNKNOWN = 255
} server_type_t;

typedef void (*server_main_t)(void);

typedef struct {
    uint64_t total_servers;
    uint64_t running_servers;
    uint64_t total_messages;
    uint64_t total_errors;
} server_stats_t;

void server_manager_init(void);
uint64_t server_register(const char* name, server_type_t type, uint64_t priority, server_main_t entry_point);
bool server_start(uint64_t pid);
bool server_stop(uint64_t pid);
bool server_unregister(uint64_t pid);
void server_main_loop(uint64_t server_pid);
void server_handle_message(uint64_t server_pid, const void* msg);

bool server_is_running(uint64_t pid);
const char* server_get_name(uint64_t pid);
void server_get_stats(server_stats_t* stats);
void server_list_servers(void);
bool server_manager_is_initialized(void);

void server_handle_open(uint64_t server_pid, const void* msg);
void server_handle_close(uint64_t server_pid, const void* msg);
void server_handle_read(uint64_t server_pid, const void* msg);
void server_handle_write(uint64_t server_pid, const void* msg);
void server_handle_ioctl(uint64_t server_pid, const void* msg);
void server_handle_event(uint64_t server_pid, const void* msg);

#endif // SERVER_H
