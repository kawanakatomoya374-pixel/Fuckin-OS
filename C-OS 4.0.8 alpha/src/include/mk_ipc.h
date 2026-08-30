#ifndef MK_IPC_H
#define MK_IPC_H

#include "types.h"
#include "mk_core.h"

// IPC constants
#define MK_IPC_MAGIC 0x4950435F  // "IPC_"
#define MK_MAX_QUEUE_MESSAGES 64
#define MK_MAX_MESSAGE_SIZE 256
#define MK_MAX_SHARED_MEMORY 128
#define MK_MAX_SEMAPHORES 256
#define MK_MAX_MUTEXES 256

// IPC return codes
#define MK_IPC_SUCCESS 0
#define MK_IPC_ERROR_INVALID_PID -1
#define MK_IPC_ERROR_NO_QUEUE -2
#define MK_IPC_ERROR_MESSAGE_TOO_LARGE -3
#define MK_IPC_ERROR_QUEUE_FULL -4
#define MK_IPC_ERROR_TIMEOUT -5
#define MK_IPC_ERROR_NOT_FOUND -6
#define MK_IPC_ERROR_INVALID_STATE -7
#define MK_IPC_ERROR_PERMISSION_DENIED -8
#define MK_IPC_ERROR_OVERFLOW -9

// Message types
#define MK_MESSAGE_TYPE_DATA 1
#define MK_MESSAGE_TYPE_CONTROL 2
#define MK_MESSAGE_TYPE_SYNC 3
#define MK_MESSAGE_TYPE_ASYNC 4
#define MK_MESSAGE_TYPE_BROADCAST 5
#define MK_MESSAGE_TYPE_REQUEST 6
#define MK_MESSAGE_TYPE_RESPONSE 7

// Shared memory permissions
#define MK_SHM_READ 0x01
#define MK_SHM_WRITE 0x02
#define MK_SHM_EXECUTE 0x04
#define MK_SHM_PUBLIC 0x80

// Block reasons
#define MK_BLOCK_REASON_RECEIVE 1
#define MK_BLOCK_REASON_SEND 2
#define MK_BLOCK_REASON_SEMAPHORE 3
#define MK_BLOCK_REASON_MUTEX 4
#define MK_BLOCK_REASON_SHARED_MEMORY 5

// Forward declarations - avoid conflicts

#endif // MK_IPC_H
