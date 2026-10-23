#ifndef PYTHON_CODE_TO_C_BAREMETAL_H
#define PYTHON_CODE_TO_C_BAREMETAL_H

#include "platform/python_code_to_c_platform.h"
#include "runtime/python_code_to_c_runtime.h"

typedef struct {
    unsigned char *heap;
    size_t heap_size;
    size_t heap_used;
    char *console;
    size_t console_size;
    size_t console_used;
    uint64_t ticks;
} P2C_BaremetalBoard;

typedef P2C_Object* (*P2C_BaremetalProgram)(void);

void p2c_baremetal_uart_write(const char *data, size_t len);
void p2c_baremetal_board_init(P2C_BaremetalBoard *board, void *heap, size_t heap_size,
                              char *console, size_t console_size);
void p2c_baremetal_board_tick(P2C_BaremetalBoard *board, uint64_t elapsed_ms);
P2C_Platform p2c_baremetal_platform(P2C_BaremetalBoard *board);
int p2c_baremetal_run(P2C_BaremetalBoard *board, P2C_BaremetalProgram program);
P2C_Object* p2c_baremetal_hello_program(void);
int p2c_baremetal_entry(void);

#endif
