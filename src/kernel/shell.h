/**
 * shell.h - C-OS 4.0.8 alpha Shell Header
 */
#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>

typedef void (*shell_output_callback_t)(const char* text);

/* Shell initialization and control */
void shell_init(void);
void shell_execute(char* line, bool print_prompt);
void shell_process(void);
const char* shell_get_cwd(void);
void shell_set_output_callback(shell_output_callback_t callback);

/* Shell state */
extern bool shell_is_running(void);
extern void shell_set_running(bool running);

#endif /* SHELL_H */