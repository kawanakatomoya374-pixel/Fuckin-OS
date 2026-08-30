/**
 * micropython_interface.h - Clean MicroPython interface for C-OS
 * Proper separation between C-OS and MicroPython
 */

#ifndef MICROPYTHON_INTERFACE_H
#define MICROPYTHON_INTERFACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declarations for C-OS functions
void serial_puts(const char* str);
void serial_putc(char c);
char serial_getc(void);
uint64_t get_timer_ticks(void);
void* kmalloc(size_t size);
void kfree(void* ptr);

// MicroPython to C-OS interface
typedef enum {
    MICROPYTHON_OK = 0,
    MICROPYTHON_ERROR = -1,
    MICROPYTHON_SYNTAX_ERROR = -2,
    MICROPYTHON_MEMORY_ERROR = -3
} micropython_result_t;

// Core interface functions
int micropython_init(void);
int micropython_deinit(void);
int micropython_execute(const char* code, char* output, size_t output_size);
int micropython_execute_string(const char* code, char* output, size_t output_size);
int micropython_execute_file(const char* filename, char* output, size_t output_size);
int micropython_execute_script(const char* script_path, const char* input, char* output, size_t output_size);
int micropython_check_syntax(const char* code, char* error_msg, size_t msg_size);
int micropython_start_repl(void);
int micropython_stop_repl(void);
int micropython_integration_init(void);
void micropython_integration_cleanup(void);

// Debug / stepping support for the Python IDE
int micropython_debug_load_source(const char* source, const char* label);
int micropython_debug_load_file(const char* filename);
int micropython_debug_add_breakpoint(uint32_t line);
int micropython_debug_remove_breakpoint(uint32_t line);
int micropython_debug_clear_breakpoints(void);
int micropython_debug_step(char* output, size_t output_size);
int micropython_debug_continue(char* output, size_t output_size);
bool micropython_debug_is_loaded(void);
bool micropython_debug_is_paused(void);
uint32_t micropython_debug_current_line(void);
const char* micropython_debug_current_label(void);

// Status functions
bool micropython_is_initialized(void);
bool micropython_is_repl_active(void);
bool micropython_is_execution_active(void);
uint64_t micropython_get_execution_count(void);
uint64_t micropython_get_total_executions(void);
uint64_t micropython_get_failed_executions(void);
uint64_t micropython_get_last_execution_time(void);
float micropython_get_success_rate(void);
const char* micropython_get_current_script(void);
void micropython_print_statistics(void);

// C-OS specific bridge functions
micropython_result_t micropython_register_cos_module(void);
micropython_result_t micropython_import_cos_module(const char* module_name);
int micropython_execute_ai_command(const char* command, const char* input, char* output, size_t output_size);
int micropython_init_cos_modules(void);

#endif // MICROPYTHON_INTERFACE_H
