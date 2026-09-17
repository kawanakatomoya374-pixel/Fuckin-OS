/*
 * micropython_repl.h - C-OS 4.0.8 alpha MicroPython REPL Interface
 * Terminal-based REPL for MicroPython with command history and completion
 */

#ifndef MICROPYTHON_REPL_H
#define MICROPYTHON_REPL_H

#include "../../include/memory.h"
#include "../../include/string.h"
#include "../../include/serial.h"

/* REPL Functions */
int micropython_repl_init(void);
void micropython_repl_run(void);
void micropython_repl_stop(void);
bool micropython_repl_is_running(void);
int micropython_repl_execute_file(const char* filename);
void micropython_repl_cleanup(void);

#endif /* MICROPYTHON_REPL_H */
