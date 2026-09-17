/*
 * micropython_integration.h - C-OS 4.0.8 alpha MicroPython Integration System
 * Complete MicroPython integration with REPL, execution, and C-OS 4.0.8 alpha modules
 */

#ifndef MICROPYTHON_INTEGRATION_H
#define MICROPYTHON_INTEGRATION_H

#include "../../include/memory.h"
#include "../../include/string.h"
#include "../../include/serial.h"

/* Integration Functions */
int micropython_integration_init(void);
int micropython_start_repl(void);
int micropython_execute_script(const char* script_path, const char* input, char* output, size_t output_size);
int micropython_execute_string(const char* code, char* output, size_t output_size);
int micropython_stop_repl(void);
bool micropython_is_repl_active(void);
bool micropython_is_execution_active(void);

/* Statistics Functions */
uint64_t micropython_get_total_executions(void);
uint64_t micropython_get_failed_executions(void);
uint64_t micropython_get_last_execution_time(void);
float micropython_get_success_rate(void);
const char* micropython_get_current_script(void);
void micropython_print_statistics(void);

/* Cleanup Functions */
void micropython_integration_cleanup(void);

/* AI Command Functions */
int micropython_execute_ai_command(const char* command, const char* input, char* output, size_t output_size);
int micropython_init_cos_modules(void);

#endif /* MICROPYTHON_INTEGRATION_H */
