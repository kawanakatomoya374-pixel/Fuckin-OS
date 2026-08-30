#ifndef SHELL_H
#define SHELL_H

#include "types.h"
#include <stdbool.h>

void shell_init(void);
typedef void (*shell_output_callback_t)(const char* text);

void shell_process(void);
const char* shell_get_cwd(void);
void shell_execute(char* line, bool print_prompt);
void shell_set_output_callback(shell_output_callback_t callback);
const char* shell_get_pipe_input(void);
int shell_complete_command(const char* prefix, char* out, size_t out_size);

int strcmp(const char* s1, const char* s2);

#endif // SHELL_H
