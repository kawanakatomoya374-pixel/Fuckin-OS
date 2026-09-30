#ifndef IPC_H
#define IPC_H

#include "types.h"

#define IPC_MAX_DATA_SIZE        1024
#define IPC_QUEUE_SIZE            64
#define IPC_MAX_PROCESSES        256
#define IPC_WAIT_FOREVER   0xFFFFFFFFULL

#define IPC_MSG_OPEN             0
#define IPC_MSG_CLOSE            1
#define IPC_MSG_READ             2
#define IPC_MSG_WRITE            3
#define IPC_MSG_IOCTL            4
#define IPC_MSG_EVENT            5
#define IPC_MSG_SHM              6
#define IPC_MSG_SIGNAL           7
#define IPC_MSG_RESPONSE         8
#define IPC_MSG_ERROR            9
#define IPC_MSG_ANY     0xFFFFFFFFULL

#define IPC_SUCCESS                  0
#define IPC_ERROR_NOT_INITIALIZED    -1
#define IPC_ERROR_INVALID_PID        -2
#define IPC_ERROR_DATA_TOO_LARGE     -3
#define IPC_ERROR_QUEUE_FULL         -4
#define IPC_ERROR_TIMEOUT            -5
#define IPC_ERROR_PERMISSION_DENIED  -6

#define SERVER_PID_MEMORY     1
#define SERVER_PID_FS         2
#define SERVER_PID_GUI        3
#define SERVER_PID_INPUT      4
#define SERVER_PID_NET        5
#define SERVER_PID_AUDIO      6
#define SERVER_PID_DRIVER     7

#define SERVER_PRIORITY_HIGH     10
#define SERVER_PRIORITY_NORMAL   50
#define SERVER_PRIORITY_LOW      90

typedef struct {
    uint64_t src_pid;
    uint64_t dst_pid;
    uint64_t msg_type;
    uint64_t msg_id;
    uint64_t data_size;
    uint8_t  data[IPC_MAX_DATA_SIZE];
} ipc_message_t;

typedef struct {
    uint64_t original_msg_id;
    uint64_t status;
    uint64_t data_size;
    uint8_t  data[IPC_MAX_DATA_SIZE];
} ipc_response_t;

typedef struct {
    uint64_t signum;
    uint64_t sender_pid;
} ipc_signal_t;

typedef struct {
    uint64_t size;
    uint64_t flags;
} ipc_shm_request_t;

typedef struct {
    uint64_t status;
    uint64_t shm_id;
    uint64_t address;
    uint64_t size;
} ipc_shm_response_t;

typedef struct {
    uint64_t total_messages;
    uint64_t active_processes;
    uint64_t blocked_processes;
    uint64_t total_queue_size;
} ipc_stats_t;

void ipc_init(void);
int ipc_send(uint64_t dst_pid, uint64_t msg_type, const void* data, uint64_t data_size);
int ipc_receive(uint64_t* src_pid, uint64_t* msg_type, void* data, uint64_t* data_size, uint64_t timeout);
int ipc_respond(uint64_t dst_pid, uint64_t original_msg_id, uint64_t status, const void* data, uint64_t data_size);
int ipc_notify(uint64_t dst_pid, uint64_t event_type, const void* data, uint64_t data_size);
int ipc_request_shared_memory(uint64_t size, uint64_t* shm_id);
/* Extension beyond the original API: resolves a shm_id (as returned by
 * ipc_request_shared_memory) to the virtual address it is mapped at in
 * its owning process's own address space. Returns 0 if not found. */
uint64_t ipc_shm_get_address(uint64_t shm_id);
/* Extension: map an existing shm region (created by another process)
 * into the calling process's own address space. Returns IPC_SUCCESS
 * and fills *out_addr, or an IPC_ERROR_* code. */
int ipc_attach_shared_memory(uint64_t shm_id, uint64_t* out_addr);
/* Remove all IPC resources owned by the given process (mailbox + shm). */
void ipc_release_process_resources(uint64_t owner_pid);
void ipc_process_messages(void);
bool ipc_is_initialized(void);
void ipc_get_stats(ipc_stats_t* stats);

#endif // IPC_H
